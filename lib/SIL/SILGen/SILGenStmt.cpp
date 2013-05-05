//===--- SILGenStmt.cpp - Implements Lowering of ASTs -> SIL for Stmts ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "Scope.h"
#include "Condition.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "swift/AST/AST.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/ADT/OwningPtr.h"

using namespace swift;
using namespace Lowering;

//===--------------------------------------------------------------------===//
// SILGenFunction visit*Stmt implementation
//===--------------------------------------------------------------------===//

/// emitOrDeleteBlock - If there are branches to the specified basic block,
/// emit it per emitBlock.  If there aren't, then just delete the block - it
/// turns out to have not been needed.
static void emitOrDeleteBlock(SILBuilder &B, SILBasicBlock *BB) {
  if (BB->pred_empty()) {
    // If the block is unused, we don't need it; just delete it.
    BB->eraseFromParent();
  } else {
    // Otherwise, continue emitting code in BB.
    B.emitBlock(BB);
  }
}

static SILValue emitConditionValue(SILGenFunction &gen, Expr *E) {
  // Sema forces conditions to have Builtin.i1 type, which guarantees this.
  SILValue V;
  {
    FullExpr Scope(gen.Cleanups);
    V = gen.visit(E).forwardAsSingleValue(gen);
  }
  assert(V.getType().castTo<BuiltinIntegerType>()->getBitWidth() == 1);

  return V;
}

Condition SILGenFunction::emitCondition(SILLocation Loc, Expr *E,
                                bool hasFalseCode, bool invertValue,
                                ArrayRef<SILType> contArgs) {
  assert(B.hasValidInsertionPoint() &&
         "emitting condition at unreachable point");
  
  SILValue V = emitConditionValue(*this, E);
  
  SILBasicBlock *ContBB = new SILBasicBlock(&F, "condition.cont");
  SILBasicBlock *TrueBB = new SILBasicBlock(&F, "if.true");

  for (SILType argTy : contArgs) {
    new (F.getModule()) SILArgument(argTy, ContBB);
  }
  
  SILBasicBlock *FalseBB, *FalseDestBB;
  if (hasFalseCode) {
    FalseBB = FalseDestBB = new SILBasicBlock(&F, "if.false");
  } else {
    FalseBB = nullptr;
    FalseDestBB = ContBB;
  }
  
  if (invertValue)
    B.createCondBranch(Loc, V, FalseDestBB, TrueBB);
  else
    B.createCondBranch(Loc, V, TrueBB, FalseDestBB);
  
  return Condition(TrueBB, FalseBB, ContBB);
}

void SILGenFunction::visitBraceStmt(BraceStmt *S, SGFContext C) {
  // Enter a new scope.
  Scope BraceScope(Cleanups);
  
  for (auto &ESD : S->getElements()) {
    assert(B.hasValidInsertionPoint());
    
    if (Stmt *S = ESD.dyn_cast<Stmt*>()) {
      visit(S);
      
      // If we ever reach an unreachable point, stop emitting statements.
      // This will need revision if we ever add goto.
      if (!B.hasValidInsertionPoint()) return;
    } else if (Expr *E = ESD.dyn_cast<Expr*>()) {
      FullExpr scope(Cleanups);
      visit(E);
    } else
      visit(ESD.get<Decl*>());
  }
}

/// emitAssignStmtRecursive - Used to destructure (potentially) recursive
/// assignments into tuple expressions down to their scalar stores.
static void emitAssignStmtRecursive(AssignStmt *S, RValue &&Src, Expr *Dest,
                                    SILGenFunction &Gen) {
  // If the destination is a tuple, recursively destructure.
  if (TupleExpr *TE = dyn_cast<TupleExpr>(Dest)) {
    SmallVector<RValue, 4> elements;
    std::move(Src).extractElements(elements);
    unsigned EltNo = 0;
    for (Expr *DestElem : TE->getElements()) {
      emitAssignStmtRecursive(S,
                              std::move(elements[EltNo++]),
                              DestElem, Gen);
    }
    return;
  }
  
  // Otherwise, emit the scalar assignment.
  LValue DstLV = SILGenLValue(Gen).visit(Dest);
  Gen.emitAssignToLValue(S, std::move(Src), DstLV);
}


void SILGenFunction::visitAssignStmt(AssignStmt *S, SGFContext C) {
  FullExpr scope(Cleanups);

  // Handle tuple destinations by destructuring them if present.
  return emitAssignStmtRecursive(S, visit(S->getSrc()), S->getDest(), *this);
}

namespace {

/// IndirectReturnInitialization - represents initializing an indirect return
/// value.
class IndirectReturnInitialization : public SingleInitializationBase {
  SILValue address;
public:
  IndirectReturnInitialization(SILValue address)
    : SingleInitializationBase(address.getType().getSwiftRValueType()),
      address(address) {}
  
  SILValue getAddressOrNull() override { return address; }
};

} // end anonymous namespace

void SILGenFunction::emitReturnExpr(SILLocation loc, Expr *ret) {
  SILValue result;
  if (IndirectReturnAddress) {
    // Indirect return of an address-only value.
    FullExpr scope(Cleanups);
    InitializationPtr returnInit(
                       new IndirectReturnInitialization(IndirectReturnAddress));
    emitExprInto(ret, returnInit.get());
    result = emitEmptyTuple(loc);
  } else {
    // SILValue return.
    FullExpr scope(Cleanups);
    result = visit(ret).forwardAsSingleValue(*this);
  }
  Cleanups.emitReturnAndCleanups(loc, result);
}

void SILGenFunction::visitReturnStmt(ReturnStmt *S, SGFContext C) {
  SILValue ArgV;
  if (!S->hasResult())
    // Void return.
    Cleanups.emitReturnAndCleanups(S, emitEmptyTuple(S));
  else
    emitReturnExpr(S, S->getResult());
}

void SILGenFunction::visitIfStmt(IfStmt *S, SGFContext C) {
  Condition Cond = emitCondition(S, S->getCond(), S->getElseStmt() != nullptr);
  
  if (Cond.hasTrue()) {
    Cond.enterTrue(B);
    visit(S->getThenStmt());
    Cond.exitTrue(B);
  }
  
  if (Cond.hasFalse()) {
    assert(S->getElseStmt());
    Cond.enterFalse(B);
    visit(S->getElseStmt());
    Cond.exitFalse(B);
  }
  
  Cond.complete(B);
}

void SILGenFunction::visitWhileStmt(WhileStmt *S, SGFContext C) {
  // Create a new basic block and jump into it.
  SILBasicBlock *LoopBB = new (F.getModule()) SILBasicBlock(&F, "while");
  B.emitBlock(LoopBB);
  
  // Set the destinations for 'break' and 'continue'
  SILBasicBlock *EndBB = new (F.getModule()) SILBasicBlock(&F, "while.end");
  BreakDestStack.emplace_back(EndBB, getCleanupsDepth());
  ContinueDestStack.emplace_back(LoopBB, getCleanupsDepth());
  
  // Evaluate the condition with the false edge leading directly
  // to the continuation block.
  Condition Cond = emitCondition(S, S->getCond(), /*hasFalseCode*/ false);
  
  // If there's a true edge, emit the body in it.
  if (Cond.hasTrue()) {
    Cond.enterTrue(B);
    visit(S->getBody());
    if (B.hasValidInsertionPoint())
      B.createBranch(S, LoopBB);
    Cond.exitTrue(B);
  }
  
  // Complete the conditional execution.
  Cond.complete(B);
  
  emitOrDeleteBlock(B, EndBB);
  BreakDestStack.pop_back();
  ContinueDestStack.pop_back();
}

void SILGenFunction::visitDoWhileStmt(DoWhileStmt *S, SGFContext C) {
  // Create a new basic block and jump into it.
  SILBasicBlock *LoopBB = new (F.getModule()) SILBasicBlock(&F, "dowhile");
  B.emitBlock(LoopBB);
  
  // Set the destinations for 'break' and 'continue'
  SILBasicBlock *EndBB = new (F.getModule()) SILBasicBlock(&F, "dowhile.end");
  SILBasicBlock *CondBB = new (F.getModule()) SILBasicBlock(&F, "dowhile.cond");
  BreakDestStack.emplace_back(EndBB, getCleanupsDepth());
  ContinueDestStack.emplace_back(CondBB, getCleanupsDepth());
  
  // Emit the body, which is always evaluated the first time around.
  visit(S->getBody());

  // Let's not differ from C99 6.8.5.2: "The evaluation of the controlling
  // expression takes place after each execution of the loop body."
  emitOrDeleteBlock(B, CondBB);

  if (B.hasValidInsertionPoint()) {
    // Evaluate the condition with the false edge leading directly
    // to the continuation block.
    Condition Cond = emitCondition(S, S->getCond(), /*hasFalseCode*/ false);
    
    Cond.enterTrue(B);
    if (B.hasValidInsertionPoint())
      B.createBranch(S, LoopBB);
    
    Cond.exitTrue(B);
    // Complete the conditional execution.
    Cond.complete(B);
  }
  
  emitOrDeleteBlock(B, EndBB);
  BreakDestStack.pop_back();
  ContinueDestStack.pop_back();
}

void SILGenFunction::visitForStmt(ForStmt *S, SGFContext C) {
  // Enter a new scope.
  Scope ForScope(Cleanups);
  
  // Emit any local 'var' variables declared in the initializer.
  for (auto D : S->getInitializerVarDecls())
    visit(D);
  
  if (Expr *E = S->getInitializer().dyn_cast<Expr*>()) {
    FullExpr Scope(Cleanups);
    visit(E);
  } else if (Stmt *AS = S->getInitializer().dyn_cast<AssignStmt*>()) {
    visit(AS);
  }
  
  // If we ever reach an unreachable point, stop emitting statements.
  // This will need revision if we ever add goto.
  if (!B.hasValidInsertionPoint()) return;
  
  // Create a new basic block and jump into it.
  SILBasicBlock *LoopBB = new (F.getModule()) SILBasicBlock(&F, "for.condition");
  B.emitBlock(LoopBB);
  
  // Set the destinations for 'break' and 'continue'
  SILBasicBlock *IncBB = new (F.getModule()) SILBasicBlock(&F, "for.inc");
  SILBasicBlock *EndBB = new (F.getModule()) SILBasicBlock(&F, "for.end");
  BreakDestStack.emplace_back(EndBB, getCleanupsDepth());
  ContinueDestStack.emplace_back(IncBB, getCleanupsDepth());
  
  // Evaluate the condition with the false edge leading directly
  // to the continuation block.
  Condition Cond = S->getCond().isNonNull() ?
  emitCondition(S, S->getCond().get(), /*hasFalseCode*/ false) :
  Condition(LoopBB, 0, 0);  // Infinite loop.
  
  // If there's a true edge, emit the body in it.
  if (Cond.hasTrue()) {
    Cond.enterTrue(B);
    visit(S->getBody());
    
    emitOrDeleteBlock(B, IncBB);
    
    if (B.hasValidInsertionPoint() && !S->getIncrement().isNull()) {
      if (Expr *E = S->getIncrement().dyn_cast<Expr*>()) {
        FullExpr Scope(Cleanups);
        visit(E);
      } else if (Stmt *AS = S->getIncrement().dyn_cast<AssignStmt*>()) {
        visit(AS);
      }
    }
    
    if (B.hasValidInsertionPoint())
      B.createBranch(S, LoopBB);
    Cond.exitTrue(B);
  }
  
  // Complete the conditional execution.
  Cond.complete(B);
  
  emitOrDeleteBlock(B, EndBB);
  BreakDestStack.pop_back();
  ContinueDestStack.pop_back();
}

void SILGenFunction::visitForEachStmt(ForEachStmt *S, SGFContext C) {
  // Emit the 'range' variable that we'll be using for iteration.
  Scope OuterForScope(Cleanups);
  visitPatternBindingDecl(S->getRange(), SGFContext());
  
  // If we ever reach an unreachable point, stop emitting statements.
  // This will need revision if we ever add goto.
  if (!B.hasValidInsertionPoint()) return;
  
  // Create a new basic block and jump into it.
  SILBasicBlock *LoopBB = new (F.getModule()) SILBasicBlock(&F, "foreach.cond");
  B.emitBlock(LoopBB);
  
  // Set the destinations for 'break' and 'continue'
  SILBasicBlock *EndBB = new (F.getModule()) SILBasicBlock(&F, "foreach.end");
  BreakDestStack.emplace_back(EndBB, getCleanupsDepth());
  ContinueDestStack.emplace_back(LoopBB, getCleanupsDepth());
  
  Condition Cond = emitCondition(S, S->getRangeEmpty(), /*hasFalseCode=*/false,
                                 /*invertValue=*/true);
  if (Cond.hasTrue()) {
    Cond.enterTrue(B);
    
    // Emit the loop body.
    // The declared variable(s) for the current element are destroyed
    // at the end of each loop iteration.
    {
      Scope InnerForScope(Cleanups);
      visitPatternBindingDecl(S->getElementInit(), SGFContext());
      visit(S->getBody());
    }
    
    // Loop back to the header.
    if (B.hasValidInsertionPoint())
      B.createBranch(S, LoopBB);
    Cond.exitTrue(B);
  }
  
  // Complete the conditional execution.
  Cond.complete(B);
  
  emitOrDeleteBlock(B, EndBB);
  BreakDestStack.pop_back();
  ContinueDestStack.pop_back();
}

void SILGenFunction::visitBreakStmt(BreakStmt *S, SGFContext C) {
  Cleanups.emitBranchAndCleanups(BreakDestStack.back());
}

void SILGenFunction::visitContinueStmt(ContinueStmt *S, SGFContext C) {
  Cleanups.emitBranchAndCleanups(ContinueDestStack.back());
}

void SILGenFunction::visitSwitchStmt(SwitchStmt *S, SGFContext C) {
  Scope OuterSwitchScope(Cleanups);
  
  // Emit and bind the subject variable.
  {
    InitializationPtr initSubject
      = emitLocalVariableWithCleanup(S->getSubjectDecl());
    emitExprInto(S->getSubjectExpr(), initSubject.get());
  }
  
  // Emit the skeleton of the switch. Map cases to blocks so we can handle
  // fallthrough statements.
  CaseStmt *defaultCase = nullptr;
  FallthroughDest::Map caseBodyBlocks;

  for (auto *C : S->getCases()) {
    // The default case is emitted last.
    if (C->isDefault()) {
      defaultCase = C;
      continue;
    }
    
    // Emit the condition for this case.
    SILValue cond = emitConditionValue(*this, C->getConditionExpr());
    
    SILBasicBlock *trueBB = new SILBasicBlock(&F, "switch.case");
    SILBasicBlock *falseBB = new SILBasicBlock(&F, "switch.next");

    B.createCondBranch(C, cond, trueBB, falseBB);
    
    caseBodyBlocks[C] = trueBB;
    
    B.emitBlock(falseBB);
  }
  SILBasicBlock *contBB;
  if (defaultCase) {
    caseBodyBlocks[defaultCase] = B.getInsertionBB();
    contBB = new SILBasicBlock(&F, "switch.cont");
  } else {
    contBB = B.getInsertionBB();
  }
  
  // Emit the case bodies.
  for (auto &caseAndBlock : caseBodyBlocks) {
    CaseStmt *c = caseAndBlock.first;
    SILBasicBlock *bb = caseAndBlock.second;
    
    B.setInsertionPoint(bb);
    Scope CaseScope(Cleanups);
    FallthroughDestStack.emplace_back(caseBodyBlocks, getCleanupsDepth());
    visit(c->getBody());
    FallthroughDestStack.pop_back();
    if (B.hasValidInsertionPoint())
      B.createBranch(c, contBB);
  }
  
  B.setInsertionPoint(contBB);
}

void SILGenFunction::visitCaseStmt(CaseStmt *S, SGFContext C) {
  llvm_unreachable("cases should be lowered as part of switch stmt");
}

void SILGenFunction::visitFallthroughStmt(FallthroughStmt *S, SGFContext C) {
  Cleanups.emitBranchAndCleanups(
           FallthroughDestStack.back().getDestForCase(S->getFallthroughDest()));
}

ManagedValue SILGenFunction::emitMaterializedLoadFromLValue(SILLocation loc,
                                                           LValue const &src) {
  SILValue addr;
  
  assert(src.begin() != src.end() && "lvalue must have at least one component");
  for (auto &component : src) {
    if (component.isPhysical()) {
      addr = component.asPhysical().offset(*this, loc, addr);
    } else {
      addr = component.asLogical()
        .loadAndMaterialize(*this, loc, addr)
        .address;
    }
    assert((addr.getType().isAddress() ||
            addr.getType().hasReferenceSemantics()) &&
           "resolving lvalue component did not give an address "
           "or reference type");
  }
  assert(addr.getType().isAddress() &&
         "resolving lvalue did not give an address");
  return ManagedValue(addr, ManagedValue::LValue);
}

void SILGenFunction::emitAssignToLValue(SILLocation loc,
                                        RValue &&src, LValue const &dest) {
  struct StoreWriteback {
    SILValue base;
    Materialize member;
    LogicalPathComponent const *component;
  };
  SILValue destAddr;
  SmallVector<StoreWriteback, 4> writebacks;

  assert(dest.begin() != dest.end() &&
         "lvalue must have at least one component");
  
  auto component = dest.begin(), next = dest.begin(), end = dest.end();
  ++next;
  
  // Resolve all components up to the last, keeping track of value-type logical
  // properties we need to write back to.
  for (; next != end; component = next, ++next) {
    if (component->isPhysical()) {
      destAddr = component->asPhysical().offset(*this, loc, destAddr);
    } else {
      LogicalPathComponent const &lcomponent = component->asLogical();
      Materialize newDest = lcomponent
        .loadAndMaterialize(*this, loc, destAddr);
      if (!newDest.address.getType().hasReferenceSemantics())
        writebacks.push_back({destAddr, newDest, &lcomponent});
      destAddr = newDest.address;
    }
    
    if (destAddr.getType().hasReferenceSemantics())
      writebacks.clear();
  }
  
  // Write to the tail component.
  if (component->isPhysical()) {
    SILValue finalDestAddr = component->asPhysical().offset(*this, loc, destAddr);
    
    std::move(src).getAsSingleValue(*this)
      .assignInto(*this, loc, finalDestAddr);
  } else {
    component->asLogical().storeRValue(*this, loc,
                                       std::move(src), destAddr);
  }
  
  // Write back through value-type logical properties.
  for (auto wb = writebacks.rbegin(), wend = writebacks.rend();
       wb != wend; ++wb) {
    ManagedValue wbValue = wb->member.consume(*this, loc);
    wb->component->storeRValue(*this, loc,
                               RValue(*this, wbValue), wb->base);
  }
}

//===--------------------------------------------------------------------===//
// SILGenModule visitTopLevelCodeDecl implementation
//===--------------------------------------------------------------------===//

void SILGenModule::visitTopLevelCodeDecl(TopLevelCodeDecl *td) {
  // Emit top-level statements and expressions into the toplevel function until
  // we hit an unreachable point.
  assert(TopLevelSGF && "top-level code in a non-main module!");
  if (!TopLevelSGF->B.hasValidInsertionPoint())
    return;
  
  TopLevelSGF->visit(td->getBody());
}
