//===- CodeCompletion.h - Routines for code completion --------------------===//
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

#ifndef SWIFT_IDE_CODE_COMPLETION_H
#define SWIFT_IDE_CODE_COMPLETION_H

#include "swift/AST/Identifier.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include <functional>
#include <string>
#include <vector>

namespace swift {
class CodeCompletionCallbacksFactory;
class Decl;
class ClangModule;

namespace ide {

class CodeCompletionContext;
class CodeCompletionResultBuilder;

/// \brief A routine to remove code completion tokens from code completion
/// tests.
///
/// \code
/// code-completion-token:
///     '#^' identifier '^#'
/// \endcode
///
/// \param Input test source code
/// \param TokenName names the token which position should be returned in
/// \p CompletionOffset.
/// \param CompletionOffset set to ~0U on error, or to a 0-based byte offset on
/// success
///
/// \returns test source code without any code completion tokens.
std::string removeCodeCompletionTokens(StringRef Input,
                                       StringRef TokenName,
                                       unsigned *CompletionOffset);

/// \brief A structured representation of a code completion string.
class CodeCompletionString {
  friend class CodeCompletionResultBuilder;

public:
  class Chunk {
    friend class CodeCompletionResultBuilder;

  public:
    enum class ChunkKind {
      /// Normal text chunk.
      Text,

      /// The first chunk of an optional substring that continues until
      /// \c NestingLevel decreases.
      OptionalBegin,

      // Punctuation.
      LeftParen,
      RightParen,
      LeftBracket,
      RightBracket,
      LeftAngle,
      RightAngle,
      Dot,
      Comma,
      ExclamationMark,
      QuestionMark,

      /// The first chunk of a substring that describes the parameter for a
      /// generic type.
      GenericParameterBegin,
      /// Generic type parameter name.
      GenericParameterName,

      /// The first chunk of a substring that describes the parameter for a
      /// function call.
      CallParameterBegin,
      /// Function call parameter name.  Can be omitted in the editor buffer.
      CallParameterName,
      /// A colon between parameter name and value.  Should be inserted in the
      /// editor buffer if the preceding CallParameterName was inserted.
      CallParameterColon,
      /// Required parameter type.
      CallParameterType,

      /// A placeholder for \c ! or \c ? in a call to a method found by dynamic
      /// lookup.
      ///
      /// Note that the IDE should not insert any of these characters by
      /// default.
      DynamicLookupMethodCallTail,

      /// Specifies the type of the whole entity that is returned in this code
      /// completion result.  For example, for variable references it is the
      /// variable type, for function calls it is the return type.
      ///
      /// This chunk should not be inserted into the editor buffer.
      TypeAnnotation
    };

    static bool chunkHasText(ChunkKind Kind) {
      return Kind == ChunkKind::Text ||
             Kind == ChunkKind::LeftParen ||
             Kind == ChunkKind::RightParen ||
             Kind == ChunkKind::LeftBracket ||
             Kind == ChunkKind::RightBracket ||
             Kind == ChunkKind::LeftAngle ||
             Kind == ChunkKind::RightAngle ||
             Kind == ChunkKind::Dot ||
             Kind == ChunkKind::Comma ||
             Kind == ChunkKind::ExclamationMark ||
             Kind == ChunkKind::QuestionMark ||
             Kind == ChunkKind::CallParameterName ||
             Kind == ChunkKind::CallParameterColon ||
             Kind == ChunkKind::CallParameterType ||
             Kind == ChunkKind::GenericParameterName ||
             Kind == ChunkKind::DynamicLookupMethodCallTail ||
             Kind == ChunkKind::TypeAnnotation;
    }

  private:
    unsigned Kind : 8;
    unsigned NestingLevel : 8;

    /// \brief If true, then this chunk is an annotation that is included only
    /// for exposition and may not be inserted in the editor buffer.
    unsigned IsAnnotation : 1;

    StringRef Text;

    Chunk(ChunkKind Kind, unsigned NestingLevel, StringRef Text)
        : Kind(unsigned(Kind)), NestingLevel(NestingLevel), IsAnnotation(0),
          Text(Text) {
      assert(chunkHasText(Kind));
    }

    Chunk(ChunkKind Kind, unsigned NestingLevel)
        : Kind(unsigned(Kind)), NestingLevel(NestingLevel), IsAnnotation(0) {
      assert(!chunkHasText(Kind));
    }

    void setIsAnnotation() {
      IsAnnotation = 1;
    }

  public:
    ChunkKind getKind() const {
      return ChunkKind(Kind);
    }

    unsigned getNestingLevel() const {
      return NestingLevel;
    }

    bool isAnnotation() const {
      return IsAnnotation;
    }

    bool hasText() const { return chunkHasText(getKind()); }

    StringRef getText() const {
      assert(hasText());
      return Text;
    }

    static Chunk createWithText(ChunkKind Kind, unsigned NestingLevel,
                                StringRef Text) {
      return Chunk(Kind, NestingLevel, Text);
    }

    static Chunk createSimple(ChunkKind Kind, unsigned NestingLevel) {
      return Chunk(Kind, NestingLevel);
    }
  };

private:
  unsigned NumChunks : 16;

  CodeCompletionString(ArrayRef<Chunk> Chunks);

public:
  ArrayRef<Chunk> getChunks() const {
    return llvm::makeArrayRef(reinterpret_cast<const Chunk *>(this + 1),
                              NumChunks);
  }

  /// Print a debug representation of the code completion string to \p OS.
  void print(raw_ostream &OS) const;
  void dump() const;
};

/// \brief Describes the origin of the code completion result.
///
/// This enum is ordered from the contexts that are "nearest" to the code
/// completion point to "outside" contexts.
enum class SemanticContextKind {
  /// Used in cases when the concept of semantic context is not applicable.
  None,

  /// \brief This is a highly-likely expression-context-specific completion
  /// result.  This description is intentionally vague: this is a catch-all
  /// category for all heuristics for highly-likely results.
  ///
  /// For example, the name of an overridden superclass member inside a nominal
  /// member function has ExpressionSpecific context:
  /// \code
  ///   class Base {
  ///     init() {}
  ///     init(a: Int) {}
  ///     func foo() {}
  ///     func bar() {}
  ///   }
  ///   class Derived {
  ///     init() {
  ///       super. // init() -- ExpressionSpecific
  ///              // init(a: Int) -- Super
  ///     }
  ///
  ///     func foo() {
  ///       super. // foo() -- ExpressionSpecific
  ///              // bar() -- Super
  ///     }
  ///   }
  /// \endcode
  ///
  /// In C-style for loop headers the iteration variable has ExpressionSpecific
  /// context:
  /// \code
  ///   for var foo = 0; #^A^# // foo -- ExpressionSpecific
  /// \endcode
  ExpressionSpecific,

  /// A declaration from the same function.
  Local,

  /// A declaration found in the immediately enclosing nominal decl.
  CurrentNominal,

  /// A declaration found in the superclass of the immediately enclosing
  /// nominal decl.
  Super,

  /// A declaration found in the non-immediately enclosing nominal decl.
  ///
  /// For example, 'Foo' is visible at (1) because of this.
  /// \code
  ///   struct A {
  ///     typealias Foo = Int
  ///     struct B {
  ///       func foo() {
  ///         // (1)
  ///       }
  ///     }
  ///   }
  /// \endcode
  OutsideNominal,

  /// A declaration from the current module.
  CurrentModule,

  /// A declaration imported from other module.
  OtherModule,
};

/// \brief A single code completion result.
class CodeCompletionResult {
  friend class CodeCompletionResultBuilder;

public:
  enum ResultKind {
    Declaration,
    Keyword,
    Pattern
  };

private:
  unsigned Kind : 2;
  unsigned SemanticContext : 3;
  CodeCompletionString *const CompletionString;
  const Decl *AssociatedDecl;

  CodeCompletionResult(ResultKind Kind,
                       SemanticContextKind SemanticContext,
                       CodeCompletionString *CompletionString)
      : Kind(Kind), SemanticContext(unsigned(SemanticContext)),
        CompletionString(CompletionString) {
  }

  CodeCompletionResult(SemanticContextKind SemanticContext,
                       CodeCompletionString *CompletionString,
                       const Decl *AssociatedDecl)
      : CodeCompletionResult(ResultKind::Declaration, SemanticContext,
                             CompletionString) {
    this->AssociatedDecl = AssociatedDecl;
    assert(AssociatedDecl && "should have a decl");
  }

public:
  ResultKind getKind() const { return ResultKind(Kind); }

  SemanticContextKind getSemanticContext() const {
    return SemanticContextKind(SemanticContext);
  }

  const CodeCompletionString *getCompletionString() const {
    return CompletionString;
  }

  const Decl *getAssociatedDecl() const {
    return AssociatedDecl;
  }

  /// Print a debug representation of the code completion result to \p OS.
  void print(raw_ostream &OS) const;
  void dump() const;
};

class CodeCompletionResultBuilder {
  CodeCompletionContext &Context;
  CodeCompletionResult::ResultKind Kind;
  SemanticContextKind SemanticContext;
  const Decl *AssociatedDecl = nullptr;
  unsigned CurrentNestingLevel = 0;
  SmallVector<CodeCompletionString::Chunk, 4> Chunks;
  bool HasLeadingDot = false;

  void addChunkWithText(CodeCompletionString::Chunk::ChunkKind Kind,
                        StringRef Text);

  void addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind Kind,
                              StringRef Text) {
    Chunks.push_back(CodeCompletionString::Chunk::createWithText(
        Kind, CurrentNestingLevel, Text));
  }

  void addSimpleChunk(CodeCompletionString::Chunk::ChunkKind Kind) {
    Chunks.push_back(
        CodeCompletionString::Chunk::createSimple(Kind,
                                                  CurrentNestingLevel));
  }

  CodeCompletionString::Chunk &getLastChunk() {
    return Chunks.back();
  }

  CodeCompletionResult *takeResult();
  void finishResult();

public:
  CodeCompletionResultBuilder(CodeCompletionContext &Context,
                              CodeCompletionResult::ResultKind Kind,
                              SemanticContextKind SemanticContext)
      : Context(Context), Kind(Kind), SemanticContext(SemanticContext) {
  }

  ~CodeCompletionResultBuilder() {
    finishResult();
  }

  void setAssociatedDecl(const Decl *D) {
    assert(Kind == CodeCompletionResult::ResultKind::Declaration);
    AssociatedDecl = D;
  }

  void addTextChunk(StringRef Text) {
    addChunkWithText(CodeCompletionString::Chunk::ChunkKind::Text, Text);
  }

  void addLeftParen() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftParen, "(");
  }

  void addRightParen() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightParen, ")");
  }

  void addLeftBracket() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftBracket, "[");
  }

  void addRightBracket() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightBracket, "]");
  }

  void addLeftAngle() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::LeftAngle, "<");
  }

  void addRightAngle() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::RightAngle, ">");
  }

  void addLeadingDot() {
    HasLeadingDot = true;
    addDot();
  }

  void addDot() {
    addChunkWithTextNoCopy(CodeCompletionString::Chunk::ChunkKind::Dot, ".");
  }

  void addComma() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::Comma, ", ");
  }

  void addExclamationMark() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::ExclamationMark, "!");
  }

  void addQuestionMark() {
    addChunkWithTextNoCopy(
        CodeCompletionString::Chunk::ChunkKind::QuestionMark, "?");
  }


  void addCallParameter(Identifier Name, StringRef Type) {
    CurrentNestingLevel++;
    addSimpleChunk(CodeCompletionString::Chunk::ChunkKind::CallParameterBegin);
    if (!Name.empty()) {
      StringRef NameStr = Name.str();

      // 'self' is a keyword, we can not allow to insert it into the source
      // buffer.
      bool IsAnnotation = (NameStr == "self");

      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallParameterName, NameStr);
      if (IsAnnotation)
        getLastChunk().setIsAnnotation();

      addChunkWithText(
          CodeCompletionString::Chunk::ChunkKind::CallParameterColon, ": ");
      if (IsAnnotation)
        getLastChunk().setIsAnnotation();
    }
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::CallParameterType, Type);
    CurrentNestingLevel--;
  }

  void addGenericParameter(StringRef Name) {
    CurrentNestingLevel++;
    addSimpleChunk(
       CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin);
    addChunkWithText(
      CodeCompletionString::Chunk::ChunkKind::GenericParameterName, Name);
    CurrentNestingLevel--;
  }

  void addDynamicLookupMethodCallTail() {
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::DynamicLookupMethodCallTail,
        "?|!");
    getLastChunk().setIsAnnotation();
  }

  void addTypeAnnotation(StringRef Type) {
    addChunkWithText(
        CodeCompletionString::Chunk::ChunkKind::TypeAnnotation, Type);
    getLastChunk().setIsAnnotation();
  }
};

class CodeCompletionContext {
public:
  struct ClangCacheImpl;

private:
  friend class CodeCompletionResultBuilder;
  llvm::BumpPtrAllocator Allocator;

  /// \brief Per-module code completion result cache.
  ///
  /// These results persist between multiple code completion requests.
  std::unique_ptr<ClangCacheImpl> ClangResultCache;

  /// \brief A set of current completion results, not yet delivered to the
  /// consumer.
  std::vector<CodeCompletionResult *> CurrentCompletionResults;

  enum class ResultDestination {
    CurrentSet,
    ClangCache
  };

  /// \brief Determines where the newly added results will go.
  ResultDestination CurrentDestination = ResultDestination::CurrentSet;

  void addResult(CodeCompletionResult *R, bool HasLeadingDot);

public:
  CodeCompletionContext();
  ~CodeCompletionContext();

  /// \brief Allocate a string owned by the code completion context.
  StringRef copyString(StringRef String);

  /// \brief Return current code completion results.
  MutableArrayRef<CodeCompletionResult *> takeResults();

  /// \brief Sort code completion results in an implementetion-defined order
  /// in place.
  static void sortCompletionResults(
      MutableArrayRef<CodeCompletionResult *> Results);

  /// \brief Clean the cache of Clang completion results.
  void clearClangCache();

  /// \brief Set a function to refill code compltetion cache.
  ///
  /// \param RefillCache function that when called should generate code
  /// completion results for all Clang modules.
  void setCacheClangResults(
      std::function<void(bool NeedLeadingDot)> RefillCache);

  /// \brief If called, the current set of completion results will include
  /// unqualified Clang completion results without a leading dot.
  void includeUnqualifiedClangResults();

  /// \brief If called, the current set of completion results will include
  /// Clang completion results from a specified module.
  void includeQualifiedClangResults(const ClangModule *Module,
                                    bool NeedLeadingDot);
};

/// \brief An abstract base class for consumers of code completion results.
class CodeCompletionConsumer {
public:
  virtual ~CodeCompletionConsumer() {}

  virtual void handleResults(
      MutableArrayRef<CodeCompletionResult *> Results) = 0;
};

/// \brief A code completion result consumer that prints the results to a
/// \c raw_ostream.
class PrintingCodeCompletionConsumer : public CodeCompletionConsumer {
  llvm::raw_ostream &OS;

public:
  PrintingCodeCompletionConsumer(llvm::raw_ostream &OS)
      : OS(OS) {
  }

  void handleResults(
      MutableArrayRef<CodeCompletionResult *> Results) override;
};

/// \brief Create a factory for code completion callbacks.
CodeCompletionCallbacksFactory *
makeCodeCompletionCallbacksFactory(CodeCompletionContext &CompletionContext,
                                   CodeCompletionConsumer &Consumer);

} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_CODE_COMPLETION_H

