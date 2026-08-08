#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/source_graph.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

using ClassId = std::size_t;
using ConstructorId = std::size_t;
using EnumId = std::size_t;
using GenericParameterId = std::size_t;
using FunctionId = std::size_t;
using LambdaId = std::size_t;
using SymbolId = std::size_t;
using TypeAliasId = std::size_t;

enum class GenericConstraintKind {
  None,
  Invalid,
  Ordered,
  Numeric,
  SignedNumeric,
  Integral,
  SignedIntegral,
  UnsignedIntegral,
  FloatingPoint,
};

struct GenericParameterInfo {
  GenericParameterId id = 0;
  Token name;
  bool pack = false;
  bool value = false;
  GenericConstraintKind constraint = GenericConstraintKind::None;
  std::optional<NamePath> constraintName;
};

struct CompileTimeValue {
  enum Kind {
    Unknown,
    UInt64,
    Parameter,
  };

  [[nodiscard]] static CompileTimeValue uint64(std::uint64_t value) {
    return CompileTimeValue{.kind = UInt64, .value = value};
  }

  [[nodiscard]] static CompileTimeValue
  parameter(GenericParameterId parameterId) {
    return CompileTimeValue{.kind = Parameter,
                            .parameterId = parameterId};
  }

  friend bool operator==(const CompileTimeValue &,
                         const CompileTimeValue &) = default;

  Kind kind = Unknown;
  std::uint64_t value = 0;
  GenericParameterId parameterId = 0;
};

enum class ArrayExtentEvaluationError {
  None,
  NonLiteral,
  Overflow,
  Underflow,
  ZeroDivisor,
};

struct ArrayExtentEvaluation {
  std::optional<std::uint64_t> value;
  ArrayExtentEvaluationError error = ArrayExtentEvaluationError::None;
  const Token *token = nullptr;
};

[[nodiscard]] inline ArrayExtentEvaluation
evaluateArrayExtent(const ArrayExtentExpr &expression) {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return {.value = *value};
    }
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (!expression.left || !expression.right) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const ArrayExtentEvaluation left = evaluateArrayExtent(*expression.left);
  if (!left.value) {
    return left;
  }
  const ArrayExtentEvaluation right = evaluateArrayExtent(*expression.right);
  if (!right.value) {
    return right;
  }

  const std::uint64_t lhs = *left.value;
  const std::uint64_t rhs = *right.value;
  switch (expression.token.kind) {
  case TokenKind::PLUS:
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
      return {.error = ArrayExtentEvaluationError::Overflow,
              .token = &expression.token};
    }
    return {.value = lhs + rhs};
  case TokenKind::MINUS:
    if (lhs < rhs) {
      return {.error = ArrayExtentEvaluationError::Underflow,
              .token = &expression.token};
    }
    return {.value = lhs - rhs};
  case TokenKind::STAR:
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
      return {.error = ArrayExtentEvaluationError::Overflow,
              .token = &expression.token};
    }
    return {.value = lhs * rhs};
  case TokenKind::SLASH:
    if (rhs == 0) {
      return {.error = ArrayExtentEvaluationError::ZeroDivisor,
              .token = &expression.token};
    }
    return {.value = lhs / rhs};
  case TokenKind::PERCENT:
    if (rhs == 0) {
      return {.error = ArrayExtentEvaluationError::ZeroDivisor,
              .token = &expression.token};
    }
    return {.value = lhs % rhs};
  default:
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
}

enum class ValueCategory {
  Value,
  Place,
};

enum class AccessMode {
  ReadOnly,
  Mutable,
};

enum class OwnershipKind {
  Value,
  Borrowed,
  Unique,
  Shared,
};

enum class DropKind {
  Trivial,
  Lexical,
};

struct SemanticType {
  enum Kind {
    Unknown,
    Void,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Bool,
    Char,
    StringView,
    NullPtr,
    Array,
    Class,
    Enum,
    Reference,
    UniqueOwner,
    SharedPointer,
    Storage,
    TypeParameter,
    TypePack,
    TypeName,
    Function,
    Lambda,
    Expected,
    Unexpected,
  };

  SemanticType(Kind kind = Unknown) : kind(kind) {}
  SemanticType(Kind kind, std::vector<SemanticType> arguments)
      : kind(kind), arguments(std::move(arguments)) {}

  [[nodiscard]] static SemanticType
  classType(ClassId id, std::vector<SemanticType> arguments = {},
            std::vector<CompileTimeValue> valueArguments = {}) {
    SemanticType type(Class, std::move(arguments));
    type.classId = id;
    type.valueArguments = std::move(valueArguments);
    return type;
  }

  [[nodiscard]] static SemanticType typeParameter(GenericParameterId id) {
    SemanticType type(TypeParameter);
    type.genericParameterId = id;
    return type;
  }

  [[nodiscard]] static SemanticType enumType(EnumId id) {
    SemanticType type(Enum);
    type.enumId = id;
    return type;
  }

  [[nodiscard]] static SemanticType typePack(GenericParameterId id) {
    SemanticType type(TypePack);
    type.genericParameterId = id;
    return type;
  }

  [[nodiscard]] static SemanticType
  concreteTypePack(GenericParameterId id, std::vector<SemanticType> elements) {
    SemanticType type(TypePack, std::move(elements));
    type.genericParameterId = id;
    type.concretePack = true;
    return type;
  }

  [[nodiscard]] static SemanticType typeName(ClassId id) {
    SemanticType type(TypeName);
    type.classId = id;
    return type;
  }

  [[nodiscard]] static SemanticType lambdaType(LambdaId id) {
    SemanticType type(Lambda);
    type.lambdaId = id;
    return type;
  }

  [[nodiscard]] static SemanticType arrayOf(SemanticType element,
                                            std::uint64_t length) {
    SemanticType type(Array, {std::move(element)});
    type.arrayLength = length;
    return type;
  }

  [[nodiscard]] static SemanticType arrayOf(SemanticType element,
                                            CompileTimeValue length) {
    SemanticType type(Array, {std::move(element)});
    if (length.kind == CompileTimeValue::UInt64) {
      type.arrayLength = length.value;
    } else if (length.kind == CompileTimeValue::Parameter) {
      type.arrayLengthParameterId = length.parameterId;
    }
    return type;
  }

  [[nodiscard]] static SemanticType
  referenceTo(SemanticType referent, AccessMode access = AccessMode::ReadOnly) {
    SemanticType type(Reference, {std::move(referent)});
    type.referenceAccess = access;
    return type;
  }

  [[nodiscard]] static SemanticType uniqueOwnerOf(SemanticType pointee) {
    return SemanticType(UniqueOwner, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType sharedPointerTo(SemanticType pointee) {
    return SemanticType(SharedPointer, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType storageOf(SemanticType element) {
    return SemanticType(Storage, {std::move(element)});
  }

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
  std::vector<CompileTimeValue> valueArguments;
  ClassId classId = 0;
  EnumId enumId = 0;
  GenericParameterId genericParameterId = 0;
  LambdaId lambdaId = 0;
  std::uint64_t arrayLength = 0;
  GenericParameterId arrayLengthParameterId = 0;
  AccessMode referenceAccess = AccessMode::ReadOnly;
  bool concretePack = false;
};

struct SemanticTypeTraits {
  OwnershipKind ownership = OwnershipKind::Value;
  DropKind drop = DropKind::Trivial;
  bool copyable = true;
  bool movable = true;
  bool copyAssignable = true;
  bool moveAssignable = true;
  bool containsBorrowedState = false;
};

// Nominal class traits require collected field metadata. Semantic analysis
// records those resolved traits in ExpressionInfo and BindingInfo.
[[nodiscard]] inline SemanticTypeTraits
semanticTraits(const SemanticType &type) {
  SemanticTypeTraits traits;
  switch (type.kind) {
  case SemanticType::Unknown:
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    return traits;
  case SemanticType::Void:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    return traits;
  case SemanticType::Lambda:
    traits.drop = DropKind::Lexical;
    return traits;
  case SemanticType::Reference:
    traits.ownership = OwnershipKind::Borrowed;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.containsBorrowedState = true;
    return traits;
  case SemanticType::Array:
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.drop = element.drop;
      traits.copyable = element.copyable;
      traits.movable = element.movable;
      traits.copyAssignable = element.copyAssignable;
      traits.moveAssignable = element.moveAssignable;
      traits.containsBorrowedState = element.containsBorrowedState;
      return traits;
    }
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    return traits;
  case SemanticType::UniqueOwner:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    return traits;
  case SemanticType::Storage:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    return traits;
  case SemanticType::SharedPointer:
    traits.ownership = OwnershipKind::Shared;
    traits.drop = DropKind::Lexical;
    return traits;
  case SemanticType::Class:
  case SemanticType::TypeParameter:
  case SemanticType::Expected:
  case SemanticType::Unexpected:
    traits.drop = DropKind::Lexical;
    return traits;
  default:
    return traits;
  }
}

struct ExpressionInfo {
  SemanticType type = SemanticType::Unknown;
  ValueCategory category = ValueCategory::Value;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
};

struct BindingInfo {
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
  bool explicitlyMoved = false;
  SymbolId symbol = 0;
  bool staticStorage = false;
  bool internalLinkage = false;
};

struct FunctionInfo {
  FunctionId id = 0;
  SourceUnitId sourceUnit = 0;
  const FunctionDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<GenericParameterInfo> genericParameters;
  bool parameterPack = false;
  ClassId ownerClass = 0;
  bool entryPoint = false;
  bool staticMember = false;
  bool internalLinkage = false;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
};

struct LambdaCaptureInfo {
  Token capture;
  Token declaration;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
};

struct LambdaInfo {
  LambdaId id = 0;
  const Lambda *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<LambdaCaptureInfo> captures;
  SemanticTypeTraits traits{};
};

struct ClassFieldTypeInfo {
  const VariableDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
};

struct StoredReferenceInfo {
  const VariableDecl *field = nullptr;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
};

struct ClassBaseTypeInfo {
  const BaseSpecifier *syntax = nullptr;
  SemanticType type = SemanticType::Unknown;
  bool interface = false;
};

struct ClassTypeInfo {
  ClassId id = 0;
  SourceUnitId sourceUnit = 0;
  const ClassDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  std::vector<GenericParameterInfo> genericParameters;
  std::vector<ClassFieldTypeInfo> fields;
  std::vector<ClassFieldTypeInfo> staticFields;
  std::optional<StoredReferenceInfo> storedReference;
  ClassKind kind = ClassKind::Class;
  std::vector<ClassBaseTypeInfo> bases;
  bool abstract = false;
  bool polymorphic = false;
};

struct EnumConstant {
  bool negative = false;
  std::uint64_t magnitude = 0;

  friend bool operator==(const EnumConstant &, const EnumConstant &) = default;
};

enum class SwitchCaseKind {
  Integer,
  Character,
  Enumerator,
};

struct SwitchCaseValue {
  SwitchCaseKind kind = SwitchCaseKind::Integer;
  SemanticType type = SemanticType::Unknown;
  EnumConstant value;
  EnumId enumOwner = 0;

  friend bool operator==(const SwitchCaseValue &,
                         const SwitchCaseValue &) = default;
};

struct EnumeratorInfo {
  const EnumeratorDecl *declaration = nullptr;
  EnumConstant value;
  bool explicitValue = false;
};

struct EnumTypeInfo {
  EnumId id = 0;
  SourceUnitId sourceUnit = 0;
  const EnumDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<EnumeratorInfo> enumerators;
};

struct ResolvedEnumeratorInfo {
  EnumId owner = 0;
  const EnumeratorDecl *declaration = nullptr;
  EnumConstant value;
};

struct TypeAliasInfo {
  SourceUnitId sourceUnit = 0;
  const TypeAliasDecl *declaration = nullptr;
  std::string qualifiedName;
  SemanticType type = SemanticType::Unknown;
};

enum class SpecialMemberStatus {
  Declared,
  Generated,
  Deleted,
};

enum class ConstructorKind {
  Ordinary,
  Copy,
  Move,
};

enum class BorrowOriginKind {
  None,
  Receiver,
  Argument,
};

struct ConstructorInfo {
  ConstructorId id = 0;
  ClassId owner = 0;
  const ConstructorDecl *declaration = nullptr;
  ConstructorKind kind = ConstructorKind::Ordinary;
  AccessModifier access = AccessModifier::Public;
  std::vector<SemanticType> parameterTypes;
  std::optional<std::size_t> borrowParameter;
  AccessMode borrowAccess = AccessMode::ReadOnly;
};

struct DestructorInfo {
  ClassId owner = 0;
  const DestructorDecl *declaration = nullptr;
  AccessModifier access = AccessModifier::Public;
};

struct ClassLifecycleInfo {
  ClassId id = 0;
  const ClassDecl *declaration = nullptr;
  std::vector<ConstructorInfo> constructors;
  std::optional<ConstructorInfo> declaredCopyConstructor;
  std::optional<ConstructorInfo> declaredMoveConstructor;
  std::optional<DestructorInfo> declaredDestructor;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructor = SpecialMemberStatus::Generated;
  bool requiresActiveDropState = false;
  SemanticTypeTraits traits{};
  bool polymorphic = false;
};

struct ResolvedConstructionInfo {
  ConstructorId constructor = 0;
  const ConstructorDecl *declaration = nullptr;
  SemanticType constructedType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
  ConstructorKind kind = ConstructorKind::Ordinary;
};

enum class ConstructorInitializerTargetKind {
  Field,
  Base,
};

struct ResolvedConstructorInitializerInfo {
  ConstructorInitializerTargetKind kind =
      ConstructorInitializerTargetKind::Field;
  SemanticType targetType = SemanticType::Unknown;
  SymbolId field = 0;
  ConstructorId constructor = 0;
  const ConstructorDecl *declaration = nullptr;
  std::vector<SemanticType> parameterTypes;
  bool storesReference = false;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
};

struct ResolvedClassArguments {
  std::vector<SemanticType> types;
  std::vector<CompileTimeValue> values;
  bool valid = true;
};

enum class IntrinsicKind {
  None,
  NumericTypeParameterConversion,
  NumericAliasConversion,
  Move,
  AllocateUniqueOwner,
  UniqueOwnerBorrow,
  UniqueOwnerBorrowMut,
  UniqueOwnerIsNull,
  AllocateStorage,
  StorageConstruct,
  StorageRead,
  StorageReadMut,
  StorageDestroy,
  StorageRelocate,
  Count,
};

enum class CallDispatch {
  Static,
  Virtual,
};

struct ResolvedCallInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> typeArguments;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
};

struct ResolvedLambdaCallInfo {
  LambdaId lambda = 0;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
};

struct ResolvedOperatorInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  OverloadedOperator kind = OverloadedOperator::Dereference;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
};

enum class SemanticBindingKind {
  None,
  GlobalVariable,
  LocalVariable,
  Parameter,
  Field,
  StaticField,
  LambdaCapture,
};

enum class SemanticCompletionKind {
  Unqualified,
  Namespace,
  Enum,
  Member,
};

enum class SemanticCompletionCandidateKind {
  Namespace,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Function,
  Method,
  Field,
  GlobalVariable,
  LocalVariable,
  Parameter,
  TypeParameter,
  ValueParameter,
};

struct SemanticCompletionCandidateRecord {
  SemanticCompletionCandidateKind kind =
      SemanticCompletionCandidateKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  std::string detail;
  SemanticType type = SemanticType::Unknown;
  bool mutableBinding = false;
  std::size_t scopeDistance = 0;
  FunctionId function = 0;
  ClassId classType = 0;
  EnumId enumType = 0;
  const TypeAliasDecl *typeAlias = nullptr;
  std::vector<SemanticType> parameterTypes;
  bool substitutedCallable = false;
  bool staticMember = false;
};

struct SemanticCompletionContext {
  SemanticCompletionKind kind = SemanticCompletionKind::Unqualified;
  SourceUnitId sourceUnit = 0;
  SourceSpan replacementRange;
  std::string prefix;
  std::vector<SemanticCompletionCandidateRecord> candidates;
};

enum class SemanticOccurrenceKind {
  Expression,
  Binding,
  Symbol,
  InferredType,
  Function,
  ClassType,
  EnumType,
  TypeAlias,
  Constructor,
  Destructor,
  SelectedCall,
  SelectedConstruction,
};

enum class SymbolKind {
  Namespace,
  NamespaceAlias,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Constructor,
  Destructor,
  Function,
  Method,
  Operator,
  Field,
  GlobalVariable,
  LocalVariable,
  Parameter,
  TypeParameter,
  ValueParameter,
  LambdaCapture,
};

enum class OccurrenceRole : std::uint32_t {
  None = 0,
  Declaration = 1U << 0U,
  Definition = 1U << 1U,
  Reference = 1U << 2U,
  Read = 1U << 3U,
  Write = 1U << 4U,
  Call = 1U << 5U,
  TypeUse = 1U << 6U,
};

[[nodiscard]] constexpr OccurrenceRole operator|(OccurrenceRole left,
                                                 OccurrenceRole right) {
  return static_cast<OccurrenceRole>(static_cast<std::uint32_t>(left) |
                                     static_cast<std::uint32_t>(right));
}

constexpr OccurrenceRole &operator|=(OccurrenceRole &left,
                                     OccurrenceRole right) {
  left = left | right;
  return left;
}

[[nodiscard]] constexpr bool hasRole(OccurrenceRole roles,
                                     OccurrenceRole role) {
  return (static_cast<std::uint32_t>(roles) &
          static_cast<std::uint32_t>(role)) != 0;
}

struct SymbolRecord {
  SymbolId id = 0;
  SymbolKind kind = SymbolKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  SourceUnitId sourceUnit = 0;
  SourceSpan nameSpan;
  SourceSpan declarationSpan;
  std::optional<SourceSpan> definitionSpan;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  AccessModifier access = AccessModifier::Public;
  bool mutableBinding = false;
  bool defaultLibrary = false;
  bool staticMember = false;
  bool internalLinkage = false;
};

struct SemanticOccurrence {
  SourceUnitId sourceUnit = 0;
  SourceSpan span;
  SemanticOccurrenceKind kind = SemanticOccurrenceKind::Expression;
  SymbolId symbol = 0;
  OccurrenceRole roles = OccurrenceRole::None;
  std::string name;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  AccessMode access = AccessMode::ReadOnly;
  bool mutableBinding = false;
  SemanticBindingKind bindingKind = SemanticBindingKind::None;
  const FunctionDecl *function = nullptr;
  const ClassDecl *classType = nullptr;
  const EnumDecl *enumType = nullptr;
  const TypeAliasDecl *typeAlias = nullptr;
  const ConstructorDecl *constructor = nullptr;
  const DestructorDecl *destructor = nullptr;
  std::optional<ResolvedCallInfo> selectedCall;
  std::optional<ResolvedConstructionInfo> selectedConstruction;
  bool staticMember = false;
};

class SemanticDatabase {
public:
  [[nodiscard]] const std::vector<SymbolRecord> &symbols() const {
    return symbolRecords;
  }

  [[nodiscard]] const SymbolRecord *findSymbol(SymbolId id) const {
    return id == 0 || id > symbolRecords.size() ? nullptr
                                                : &symbolRecords[id - 1];
  }

  [[nodiscard]] std::vector<const SemanticOccurrence *>
  occurrencesForSymbol(SymbolId id) const {
    std::vector<const SemanticOccurrence *> result;
    if (id == 0) {
      return result;
    }
    for (const auto &[_, unitOccurrences] : occurrencesByUnit) {
      for (const SemanticOccurrence &occurrence : unitOccurrences) {
        if (occurrence.symbol == id) {
          result.push_back(&occurrence);
        }
      }
    }
    return result;
  }

  [[nodiscard]] const std::vector<SemanticOccurrence> &
  occurrences(SourceUnitId sourceUnit) const {
    static const std::vector<SemanticOccurrence> empty;
    const auto found = occurrencesByUnit.find(sourceUnit);
    return found == occurrencesByUnit.end() ? empty : found->second;
  }

  [[nodiscard]] const SemanticOccurrence *
  findOccurrence(SourceUnitId sourceUnit, std::size_t byteOffset) const {
    const std::vector<SemanticOccurrence> &unitOccurrences =
        occurrences(sourceUnit);
    const auto after = std::upper_bound(
        unitOccurrences.begin(), unitOccurrences.end(), byteOffset,
        [](std::size_t offset, const SemanticOccurrence &occurrence) {
          return offset < occurrence.span.start;
        });
    if (after == unitOccurrences.begin()) {
      return nullptr;
    }

    auto current = after;
    --current;
    const std::size_t candidateStart = current->span.start;
    const SemanticOccurrence *best = nullptr;
    while (true) {
      if (current->span.start != candidateStart) {
        break;
      }
      if (current->span.start <= byteOffset && byteOffset < current->span.end &&
          (best == nullptr || priority(current->kind) > priority(best->kind))) {
        best = &*current;
      }
      if (current == unitOccurrences.begin()) {
        break;
      }
      --current;
    }
    return best;
  }

  [[nodiscard]] const SymbolRecord *findSymbolAt(SourceUnitId sourceUnit,
                                                 std::size_t byteOffset) const {
    const SemanticOccurrence *occurrence =
        findOccurrence(sourceUnit, byteOffset);
    return occurrence == nullptr ? nullptr : findSymbol(occurrence->symbol);
  }

private:
  friend class SemanticModel;

  struct DeclarationKey {
    SourceUnitId sourceUnit = 0;
    std::size_t start = 0;
    std::size_t end = 0;

    friend bool operator==(const DeclarationKey &,
                           const DeclarationKey &) = default;
  };

  struct DeclarationKeyHash {
    std::size_t operator()(const DeclarationKey &key) const {
      std::size_t result = std::hash<std::size_t>{}(key.sourceUnit);
      result ^= std::hash<std::size_t>{}(key.start) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      result ^= std::hash<std::size_t>{}(key.end) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      return result;
    }
  };

  static int priority(SemanticOccurrenceKind kind) {
    switch (kind) {
    case SemanticOccurrenceKind::SelectedCall:
    case SemanticOccurrenceKind::SelectedConstruction:
      return 4;
    case SemanticOccurrenceKind::Function:
    case SemanticOccurrenceKind::Symbol:
    case SemanticOccurrenceKind::ClassType:
    case SemanticOccurrenceKind::EnumType:
    case SemanticOccurrenceKind::TypeAlias:
    case SemanticOccurrenceKind::Constructor:
    case SemanticOccurrenceKind::Destructor:
      return 3;
    case SemanticOccurrenceKind::Binding:
    case SemanticOccurrenceKind::InferredType:
      return 2;
    case SemanticOccurrenceKind::Expression:
      return 1;
    }
    return 0;
  }

  void clear() {
    symbolRecords.clear();
    symbolsByDeclaration.clear();
    occurrencesByUnit.clear();
  }

  SymbolId recordSymbol(SymbolRecord symbol) {
    if (symbol.sourceUnit == 0 ||
        symbol.nameSpan.end <= symbol.nameSpan.start) {
      return 0;
    }
    const DeclarationKey key{symbol.sourceUnit, symbol.nameSpan.start,
                             symbol.nameSpan.end};
    if (const auto found = symbolsByDeclaration.find(key);
        found != symbolsByDeclaration.end()) {
      SymbolRecord &existing = symbolRecords[found->second - 1];
      if (existing.qualifiedName.empty()) {
        existing.qualifiedName = std::move(symbol.qualifiedName);
      }
      if (existing.type == SemanticType::Unknown &&
          symbol.type != SemanticType::Unknown) {
        existing.type = std::move(symbol.type);
        existing.traits = symbol.traits;
      }
      if (symbol.definitionSpan) {
        existing.definitionSpan = std::move(symbol.definitionSpan);
      }
      existing.access = symbol.access;
      existing.mutableBinding = symbol.mutableBinding;
      existing.defaultLibrary =
          existing.defaultLibrary || symbol.defaultLibrary;
      existing.staticMember = existing.staticMember || symbol.staticMember;
      existing.internalLinkage =
          existing.internalLinkage || symbol.internalLinkage;
      return found->second;
    }

    symbol.id = symbolRecords.size() + 1;
    const SymbolId id = symbol.id;
    symbolRecords.emplace_back(std::move(symbol));
    symbolsByDeclaration.emplace(key, id);
    return id;
  }

  [[nodiscard]] SymbolId symbolForDeclaration(SourceUnitId sourceUnit,
                                              const SourceSpan &span) const {
    const auto found = symbolsByDeclaration.find(
        DeclarationKey{sourceUnit, span.start, span.end});
    return found == symbolsByDeclaration.end() ? 0 : found->second;
  }

  void record(SemanticOccurrence occurrence) {
    if (occurrence.sourceUnit == 0 ||
        occurrence.span.end <= occurrence.span.start) {
      return;
    }
    occurrencesByUnit[occurrence.sourceUnit].push_back(std::move(occurrence));
  }

  void finalize() {
    for (auto &[_, unitOccurrences] : occurrencesByUnit) {
      std::stable_sort(
          unitOccurrences.begin(), unitOccurrences.end(),
          [](const SemanticOccurrence &left, const SemanticOccurrence &right) {
            if (left.span.start != right.span.start) {
              return left.span.start < right.span.start;
            }
            if (left.span.end != right.span.end) {
              return left.span.end < right.span.end;
            }
            return priority(left.kind) < priority(right.kind);
          });
      std::vector<SemanticOccurrence> compacted;
      compacted.reserve(unitOccurrences.size());
      for (SemanticOccurrence &occurrence : unitOccurrences) {
        if (!compacted.empty() && occurrence.symbol != 0 &&
            compacted.back().span.start == occurrence.span.start &&
            compacted.back().span.end == occurrence.span.end &&
            compacted.back().kind == occurrence.kind &&
            compacted.back().symbol == occurrence.symbol) {
          compacted.back().roles |= occurrence.roles;
          continue;
        }
        compacted.emplace_back(std::move(occurrence));
      }
      unitOccurrences = std::move(compacted);
    }
  }

  std::vector<SymbolRecord> symbolRecords;
  std::unordered_map<DeclarationKey, SymbolId, DeclarationKeyHash>
      symbolsByDeclaration;
  std::unordered_map<SourceUnitId, std::vector<SemanticOccurrence>>
      occurrencesByUnit;
};

[[nodiscard]] inline ExpressionInfo
makeExpressionInfo(SemanticType type,
                   ValueCategory category = ValueCategory::Value,
                   AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return ExpressionInfo{.type = std::move(type),
                        .category = category,
                        .access = access,
                        .traits = traits};
}

[[nodiscard]] inline BindingInfo
makeBindingInfo(SemanticType type, AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return BindingInfo{
      .type = std::move(type), .access = access, .traits = traits};
}

using TypeSubstitution = std::unordered_map<GenericParameterId, SemanticType>;
using ValueSubstitution =
    std::unordered_map<GenericParameterId, CompileTimeValue>;

struct GenericSubstitution {
  TypeSubstitution types;
  ValueSubstitution values;
};

using SemanticDiagnostic = Diagnostic;

class SemanticModel {
public:
  // AST identities remain valid while the analyzed Program is alive.
  [[nodiscard]] const ExpressionInfo *
  findExpression(const Expr &expression) const {
    const auto found = expressions.find(&expression);
    return found == expressions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] ExpressionInfo expressionInfo(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? makeExpressionInfo(SemanticType::Unknown) : *info;
  }

  [[nodiscard]] const SemanticType *findType(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? nullptr : &info->type;
  }

  [[nodiscard]] const CompileTimeValue *
  findArrayExtent(const ArrayExtentExpr &extent) const {
    const auto found = arrayExtents.find(&extent);
    return found == arrayExtents.end() ? nullptr : &found->second;
  }

  [[nodiscard]] SemanticType typeOf(const Expr &expression) const {
    const SemanticType *type = findType(expression);
    return type == nullptr ? SemanticType::Unknown : *type;
  }

  [[nodiscard]] bool hasType(const Expr &expression) const {
    return expressions.contains(&expression);
  }

  [[nodiscard]] std::size_t expressionCount() const {
    return expressions.size();
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const VariableDecl &declaration) const {
    const auto found = variableBindings.find(&declaration);
    return found == variableBindings.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const Parameter &parameter) const {
    const auto found = parameterBindings.find(&parameter);
    return found == parameterBindings.end() ? nullptr : &found->second;
  }

  [[nodiscard]] std::size_t bindingCount() const {
    return variableBindings.size() + parameterBindings.size();
  }

  [[nodiscard]] const FunctionInfo *
  findFunction(const FunctionDecl &declaration) const {
    const auto found = functions.find(&declaration);
    return found == functions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const FunctionInfo *findFunction(FunctionId id) const {
    const auto found = functionsById.find(id);
    return found == functionsById.end() || found->second == nullptr
               ? nullptr
               : findFunction(*found->second);
  }

  [[nodiscard]] const LambdaInfo *findLambda(const Lambda &declaration) const {
    const auto found = lambdas.find(&declaration);
    return found == lambdas.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const LambdaInfo *findLambda(LambdaId id) const {
    const auto found = lambdasById.find(id);
    return found == lambdasById.end() || found->second == nullptr
               ? nullptr
               : findLambda(*found->second);
  }

  [[nodiscard]] const ClassTypeInfo *
  findClassType(const ClassDecl &declaration) const {
    const auto found = classTypes.find(&declaration);
    return found == classTypes.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ClassTypeInfo *findClassType(ClassId id) const {
    const auto found = classTypesById.find(id);
    return found == classTypesById.end() || found->second == nullptr
               ? nullptr
               : findClassType(*found->second);
  }

  [[nodiscard]] const TypeAliasInfo *
  findTypeAlias(const TypeAliasDecl &declaration) const {
    const auto found = typeAliases.find(&declaration);
    return found == typeAliases.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const EnumTypeInfo *
  findEnumType(const EnumDecl &declaration) const {
    const auto found = enumTypes.find(&declaration);
    return found == enumTypes.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const EnumTypeInfo *findEnumType(EnumId id) const {
    const auto found = enumTypesById.find(id);
    return found == enumTypesById.end() || found->second == nullptr
               ? nullptr
               : findEnumType(*found->second);
  }

  [[nodiscard]] const ResolvedEnumeratorInfo *
  findEnumerator(const QualifiedName &expression) const {
    const auto found = enumerators.find(&expression);
    return found == enumerators.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const SwitchCaseValue *
  findSwitchCase(const Expr &expression) const {
    const auto found = switchCases.find(&expression);
    return found == switchCases.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedCallInfo *findCall(const Call &call) const {
    const auto found = calls.find(&call);
    return found == calls.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedLambdaCallInfo *
  findLambdaCall(const Call &call) const {
    const auto found = lambdaCalls.find(&call);
    return found == lambdaCalls.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedOperatorInfo *
  findOperator(const Expr &expression) const {
    const auto found = operators.find(&expression);
    return found == operators.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedOperatorInfo *
  findContextualConversion(const Expr &expression) const {
    const auto found = contextualConversions.find(&expression);
    return found == contextualConversions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ClassLifecycleInfo *
  findClassLifecycle(const ClassDecl &declaration) const {
    const auto found = classLifecycles.find(&declaration);
    return found == classLifecycles.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedConstructionInfo *
  findConstruction(const Expr &expression) const {
    const auto found = constructions.find(&expression);
    return found == constructions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedConstructorInitializerInfo *
  findConstructorInitializer(const ConstructorInitializer &initializer) const {
    const auto found = constructorInitializers.find(&initializer);
    return found == constructorInitializers.end() ? nullptr : &found->second;
  }

  [[nodiscard]] SymbolId findResolvedSymbol(const Expr &expression) const {
    const auto found = resolvedSymbols.find(&expression);
    return found == resolvedSymbols.end() ? 0 : found->second;
  }

  [[nodiscard]] std::size_t functionCount() const { return functions.size(); }

  [[nodiscard]] std::size_t lambdaCount() const { return lambdas.size(); }

  [[nodiscard]] std::size_t resolvedCallCount() const { return calls.size(); }

  [[nodiscard]] std::size_t classLifecycleCount() const {
    return classLifecycles.size();
  }

  [[nodiscard]] std::size_t resolvedConstructionCount() const {
    return constructions.size();
  }

  [[nodiscard]] const SemanticDatabase &database() const {
    return semanticDatabase;
  }

  [[nodiscard]] const std::optional<SemanticCompletionContext> &
  completionContext() const {
    return completion;
  }

  [[nodiscard]] const GenericParameterInfo *
  findGenericParameter(GenericParameterId id) const {
    const auto find =
        [id](const auto &records) -> const GenericParameterInfo * {
      for (const auto &[_, record] : records) {
        const auto parameter = std::find_if(
            record.genericParameters.begin(), record.genericParameters.end(),
            [id](const GenericParameterInfo &candidate) {
              return candidate.id == id;
            });
        if (parameter != record.genericParameters.end()) {
          return &*parameter;
        }
      }
      return nullptr;
    };
    if (const GenericParameterInfo *parameter = find(functions)) {
      return parameter;
    }
    return find(classTypes);
  }

  [[nodiscard]] const ConstructorInfo *
  findConstructor(const ConstructorDecl &declaration) const {
    for (const auto &[_, lifecycle] : classLifecycles) {
      const auto found = std::find_if(
          lifecycle.constructors.begin(), lifecycle.constructors.end(),
          [&declaration](const ConstructorInfo &candidate) {
            return candidate.declaration == &declaration;
          });
      if (found != lifecycle.constructors.end()) {
        return &*found;
      }
      if (lifecycle.declaredCopyConstructor &&
          lifecycle.declaredCopyConstructor->declaration == &declaration) {
        return &*lifecycle.declaredCopyConstructor;
      }
      if (lifecycle.declaredMoveConstructor &&
          lifecycle.declaredMoveConstructor->declaration == &declaration) {
        return &*lifecycle.declaredMoveConstructor;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const DestructorInfo *
  findDestructor(const DestructorDecl &declaration) const {
    for (const auto &[_, lifecycle] : classLifecycles) {
      if (lifecycle.declaredDestructor &&
          lifecycle.declaredDestructor->declaration == &declaration) {
        return &*lifecycle.declaredDestructor;
      }
    }
    return nullptr;
  }

private:
  friend class SemanticVisitor;

  void clear() {
    expressions.clear();
    arrayExtents.clear();
    variableBindings.clear();
    parameterBindings.clear();
    functions.clear();
    functionsById.clear();
    lambdas.clear();
    lambdasById.clear();
    classTypes.clear();
    classTypesById.clear();
    typeAliases.clear();
    enumTypes.clear();
    enumTypesById.clear();
    enumerators.clear();
    switchCases.clear();
    calls.clear();
    lambdaCalls.clear();
    operators.clear();
    contextualConversions.clear();
    classLifecycles.clear();
    constructions.clear();
    constructorInitializers.clear();
    resolvedSymbols.clear();
    semanticDatabase.clear();
    completion.reset();
  }

  void record(const Expr &expression, ExpressionInfo info) {
    expressions.insert_or_assign(&expression, std::move(info));
  }

  void record(const ArrayExtentExpr &extent, CompileTimeValue value) {
    arrayExtents.insert_or_assign(&extent, value);
  }

  void record(const VariableDecl &declaration, BindingInfo info) {
    variableBindings.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Parameter &parameter, BindingInfo info) {
    parameterBindings.insert_or_assign(&parameter, std::move(info));
  }

  void recordExplicitMove(const VariableDecl &declaration) {
    if (auto binding = variableBindings.find(&declaration);
        binding != variableBindings.end()) {
      binding->second.explicitlyMoved = true;
    }
  }

  void recordExplicitMove(const Parameter &parameter) {
    if (auto binding = parameterBindings.find(&parameter);
        binding != parameterBindings.end()) {
      binding->second.explicitlyMoved = true;
    }
  }

  void record(const FunctionDecl &declaration, FunctionInfo info) {
    const auto [found, _] =
        functions.insert_or_assign(&declaration, std::move(info));
    functionsById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const Lambda &declaration, LambdaInfo info) {
    const auto [found, _] =
        lambdas.insert_or_assign(&declaration, std::move(info));
    lambdasById.insert_or_assign(found->second.id, &declaration);
  }

  void recordClassType(const ClassDecl &declaration, ClassTypeInfo info) {
    const auto [found, _] =
        classTypes.insert_or_assign(&declaration, std::move(info));
    classTypesById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const TypeAliasDecl &declaration, TypeAliasInfo info) {
    typeAliases.insert_or_assign(&declaration, std::move(info));
  }

  void recordEnumType(const EnumDecl &declaration, EnumTypeInfo info) {
    const auto [found, _] =
        enumTypes.insert_or_assign(&declaration, std::move(info));
    enumTypesById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const QualifiedName &expression, ResolvedEnumeratorInfo info) {
    enumerators.insert_or_assign(&expression, std::move(info));
  }

  void recordSwitchCase(const Expr &expression, SwitchCaseValue value) {
    switchCases.insert_or_assign(&expression, std::move(value));
  }

  void record(const Call &call, ResolvedCallInfo info) {
    calls.insert_or_assign(&call, std::move(info));
  }

  void recordLambdaCall(const Call &call, ResolvedLambdaCallInfo info) {
    lambdaCalls.insert_or_assign(&call, std::move(info));
  }

  void recordOperator(const Expr &expression, ResolvedOperatorInfo info) {
    operators.insert_or_assign(&expression, std::move(info));
  }

  void recordContextualConversion(const Expr &expression,
                                  ResolvedOperatorInfo info) {
    contextualConversions.insert_or_assign(&expression, std::move(info));
  }

  void record(const ClassDecl &declaration, ClassLifecycleInfo info) {
    classLifecycles.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Expr &expression, ResolvedConstructionInfo info) {
    constructions.insert_or_assign(&expression, std::move(info));
  }

  void record(const ConstructorInitializer &initializer,
              ResolvedConstructorInitializerInfo info) {
    constructorInitializers.insert_or_assign(&initializer, std::move(info));
  }

  void recordResolvedSymbol(const Expr &expression, SymbolId symbol) {
    if (symbol != 0) {
      resolvedSymbols.insert_or_assign(&expression, symbol);
    }
  }

  SymbolId recordSymbol(SymbolRecord symbol) {
    return semanticDatabase.recordSymbol(std::move(symbol));
  }

  [[nodiscard]] SymbolId symbolForDeclaration(SourceUnitId sourceUnit,
                                              const SourceSpan &span) const {
    return semanticDatabase.symbolForDeclaration(sourceUnit, span);
  }

  void recordOccurrence(SemanticOccurrence occurrence) {
    semanticDatabase.record(std::move(occurrence));
  }

  void recordCompletion(SemanticCompletionContext context) {
    completion = std::move(context);
  }

  void finalizeOccurrences() { semanticDatabase.finalize(); }

  std::unordered_map<const Expr *, ExpressionInfo> expressions;
  std::unordered_map<const ArrayExtentExpr *, CompileTimeValue> arrayExtents;
  std::unordered_map<const VariableDecl *, BindingInfo> variableBindings;
  std::unordered_map<const Parameter *, BindingInfo> parameterBindings;
  std::unordered_map<const FunctionDecl *, FunctionInfo> functions;
  std::unordered_map<FunctionId, const FunctionDecl *> functionsById;
  std::unordered_map<const Lambda *, LambdaInfo> lambdas;
  std::unordered_map<LambdaId, const Lambda *> lambdasById;
  std::unordered_map<const ClassDecl *, ClassTypeInfo> classTypes;
  std::unordered_map<ClassId, const ClassDecl *> classTypesById;
  std::unordered_map<const TypeAliasDecl *, TypeAliasInfo> typeAliases;
  std::unordered_map<const EnumDecl *, EnumTypeInfo> enumTypes;
  std::unordered_map<EnumId, const EnumDecl *> enumTypesById;
  std::unordered_map<const QualifiedName *, ResolvedEnumeratorInfo>
      enumerators;
  std::unordered_map<const Expr *, SwitchCaseValue> switchCases;
  std::unordered_map<const Call *, ResolvedCallInfo> calls;
  std::unordered_map<const Call *, ResolvedLambdaCallInfo> lambdaCalls;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> operators;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> contextualConversions;
  std::unordered_map<const ClassDecl *, ClassLifecycleInfo> classLifecycles;
  std::unordered_map<const Expr *, ResolvedConstructionInfo> constructions;
  std::unordered_map<const ConstructorInitializer *,
                     ResolvedConstructorInitializerInfo>
      constructorInitializers;
  std::unordered_map<const Expr *, SymbolId> resolvedSymbols;
  SemanticDatabase semanticDatabase;
  std::optional<SemanticCompletionContext> completion;
};

class SemanticTypePrinter {
public:
  explicit SemanticTypePrinter(const SemanticModel &semantics)
      : semantics(semantics) {}

  [[nodiscard]] std::string print(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Unknown:
      return "unknown";
    case SemanticType::Void:
      return "void";
    case SemanticType::Int8:
      return "int8_t";
    case SemanticType::Int16:
      return "int16_t";
    case SemanticType::Int32:
      return "int32_t";
    case SemanticType::Int64:
      return "int64_t";
    case SemanticType::UInt8:
      return "uint8_t";
    case SemanticType::UInt16:
      return "uint16_t";
    case SemanticType::UInt32:
      return "uint32_t";
    case SemanticType::UInt64:
      return "uint64_t";
    case SemanticType::Float:
      return "float";
    case SemanticType::Bool:
      return "bool";
    case SemanticType::Char:
      return "char";
    case SemanticType::StringView:
      return "std::string_view";
    case SemanticType::NullPtr:
      return "nullptr_t";
    case SemanticType::Array:
      if (type.arguments.size() == 1) {
        return print(type.arguments.front()) + "[" + arrayExtent(type) + "]";
      }
      return "array";
    case SemanticType::Class:
      return classType(type);
    case SemanticType::Enum:
      if (const EnumTypeInfo *info = semantics.findEnumType(type.enumId)) {
        return info->qualifiedName;
      }
      return "unknown enum";
    case SemanticType::Reference:
      if (type.arguments.size() == 1) {
        return (type.referenceAccess == AccessMode::Mutable ? "mut " : "") +
               print(type.arguments.front()) + "&";
      }
      return "reference";
    case SemanticType::UniqueOwner:
      return unaryType("gti_internal::unique_owner", type);
    case SemanticType::SharedPointer:
      return unaryType("std::shared_ptr", type);
    case SemanticType::Storage:
      return unaryType("gti_internal::storage", type);
    case SemanticType::TypeParameter:
      return genericParameter(type.genericParameterId, false);
    case SemanticType::TypePack:
      return genericParameter(type.genericParameterId, true);
    case SemanticType::TypeName:
      if (const ClassTypeInfo *info = semantics.findClassType(type.classId)) {
        return info->qualifiedName;
      }
      return "type";
    case SemanticType::Function:
      return "function";
    case SemanticType::Lambda:
      return "lambda";
    case SemanticType::Expected:
      if (type.arguments.size() == 2) {
        return "expected<" + print(type.arguments[0]) + ", " +
               print(type.arguments[1]) + ">";
      }
      return "expected";
    case SemanticType::Unexpected:
      return unaryType("unexpected", type);
    }
    return "unknown";
  }

private:
  [[nodiscard]] std::string classType(const SemanticType &type) const {
    const ClassTypeInfo *info = semantics.findClassType(type.classId);
    if (info == nullptr) {
      return "unknown class";
    }
    std::string result = info->qualifiedName;
    if (type.arguments.empty() && type.valueArguments.empty()) {
      return result;
    }
    result += '<';
    bool first = true;
    for (const SemanticType &argument : type.arguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += print(argument);
    }
    for (const CompileTimeValue &argument : type.valueArguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += value(argument);
    }
    result += '>';
    return result;
  }

  [[nodiscard]] std::string unaryType(std::string_view name,
                                      const SemanticType &type) const {
    return type.arguments.size() == 1
               ? std::string(name) + '<' + print(type.arguments.front()) + '>'
               : std::string(name);
  }

  [[nodiscard]] std::string arrayExtent(const SemanticType &type) const {
    return type.arrayLengthParameterId == 0
               ? std::to_string(type.arrayLength)
               : genericParameter(type.arrayLengthParameterId, false);
  }

  [[nodiscard]] std::string value(const CompileTimeValue &value) const {
    if (value.kind == CompileTimeValue::UInt64) {
      return std::to_string(value.value);
    }
    if (value.kind == CompileTimeValue::Parameter) {
      return genericParameter(value.parameterId, false);
    }
    return "unknown value";
  }

  [[nodiscard]] std::string genericParameter(GenericParameterId id,
                                             bool pack) const {
    const GenericParameterInfo *parameter = semantics.findGenericParameter(id);
    if (parameter == nullptr) {
      return pack ? "type pack" : "type parameter";
    }
    return parameter->name.lexeme + (pack ? "..." : "");
  }

  const SemanticModel &semantics;
};

struct SemanticInstanceAnalysis {
  SemanticModel model;
  std::vector<SemanticDiagnostic> diagnostics;

  [[nodiscard]] bool valid() const { return diagnostics.empty(); }
};

class SemanticVisitor final : public ExprVisitor, public StmtVisitor {
public:
  explicit SemanticVisitor(TargetInfo target = TargetInfo::host(),
                           const SourceGraph *sourceGraph = nullptr)
      : target(std::move(target)), sourceGraph(sourceGraph) {}

  bool check(const Program &program) {
    diagnostics.clear();
    scopes.clear();
    namespaces.clear();
    namespaceToolingSymbols.clear();
    namespaceAliases.clear();
    typeAliasIds.clear();
    typeAliases.clear();
    namespaceSymbols.clear();
    classIds.clear();
    enumIds.clear();
    visibleNamespaces.clear();
    visibleNamespaceAliases.clear();
    visibleTypeAliasIds.clear();
    visibleNamespaceSymbols.clear();
    internalNamespaceSymbols.clear();
    visibleClassIds.clear();
    visibleEnumIds.clear();
    classDeclIds.clear();
    functionGenericParameters.clear();
    genericConstraints.clear();
    classes.clear();
    enums.clear();
    typeParameterScopes.clear();
    valueParameterScopes.clear();
    typePackScopes.clear();
    instanceTypeSubstitution.clear();
    instanceValueSubstitution.clear();
    nextGenericParameterId = 1;
    nextFunctionId = 1;
    nextLambdaId = 1;
    nextConstructorId = 1;
    currentNamespace.clear();
    currentSourceUnit = 0;
    predeclaredVariables.clear();
    semanticModel.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    analyzingCallCallee = false;
    currentStaticMemberFunction = false;
    allowPackTypeReference = false;
    receiverStorageBorrowed = false;
    instanceClassContextActive = false;
    contextualInitializerType.reset();
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    destructorDepth = 0;
    functionDepth = 0;
    loopDepth = 0;
    switchDepth = 0;
    lambdaDepth = 0;
    lambdaUncapturedLocals.clear();
    currentReturnType = SemanticType::Unknown;

    registerNamespaces(program.declarations(), {});
    registerNamespaceAliases(program.declarations(), {});
    registerTypeAliases(program.declarations(), {});
    registerEnums(program.declarations(), {});
    registerClasses(program.declarations(), {});
    resolveTypeAliases();
    resolveClassInheritance();
    registerFunctionGenericParameters(program.declarations(), {}, false);
    registerNamespaceSymbols(program.declarations(), {});
    collectClassMembers(program.declarations(), {});
    resolveInheritedMembers();
    validateStoredReferenceContracts();
    recordClassTypes();
    recordClassLifecycles();
    beginScope();
    analyze(program.declarations());
    endScope();
    semanticModel.finalizeOccurrences();
    return !hadError();
  }

  bool check(const Expr &expr) {
    diagnostics.clear();
    scopes.clear();
    namespaces.clear();
    namespaceToolingSymbols.clear();
    namespaceAliases.clear();
    typeAliasIds.clear();
    typeAliases.clear();
    namespaceSymbols.clear();
    classIds.clear();
    enumIds.clear();
    visibleNamespaces.clear();
    visibleNamespaceAliases.clear();
    visibleTypeAliasIds.clear();
    visibleNamespaceSymbols.clear();
    visibleClassIds.clear();
    visibleEnumIds.clear();
    classDeclIds.clear();
    functionGenericParameters.clear();
    genericConstraints.clear();
    classes.clear();
    enums.clear();
    typeParameterScopes.clear();
    valueParameterScopes.clear();
    typePackScopes.clear();
    instanceTypeSubstitution.clear();
    instanceValueSubstitution.clear();
    nextGenericParameterId = 1;
    nextFunctionId = 1;
    nextLambdaId = 1;
    nextConstructorId = 1;
    currentNamespace.clear();
    currentSourceUnit = 0;
    predeclaredVariables.clear();
    semanticModel.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    analyzingCallCallee = false;
    currentStaticMemberFunction = false;
    allowPackTypeReference = false;
    receiverStorageBorrowed = false;
    instanceClassContextActive = false;
    contextualInitializerType.reset();
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    destructorDepth = 0;
    functionDepth = 0;
    loopDepth = 0;
    switchDepth = 0;
    lambdaDepth = 0;
    lambdaUncapturedLocals.clear();
    beginScope();
    analyze(expr);
    endScope();
    semanticModel.finalizeOccurrences();
    return !hadError();
  }

  [[nodiscard]] bool hadError() const { return !diagnostics.empty(); }

  [[nodiscard]] const std::vector<SemanticDiagnostic> &errors() const {
    return diagnostics;
  }

  [[nodiscard]] SemanticType expressionType() const { return currentType; }

  [[nodiscard]] const SemanticModel &model() const { return semanticModel; }

  [[nodiscard]] SemanticTypeTraits traitsFor(const SemanticType &type) const {
    return typeTraits(type);
  }

  [[nodiscard]] SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId functionId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments) const {
    SemanticVisitor instance = *this;
    const FunctionInfo *function = semanticModel.findFunction(functionId);
    if (function == nullptr || function->declaration == nullptr) {
      return {.model = semanticModel};
    }

    instance.prepareInstanceAnalysis();
    if (!instance.prepareInstanceContext(
            *function, classTypeArguments, classValueArguments,
            functionTypeArguments)) {
      return {.model = std::move(instance.semanticModel),
              .diagnostics = std::move(instance.diagnostics)};
    }
    function->declaration->accept(instance);
    instance.finishInstanceContext(*function);
    return {.model = std::move(instance.semanticModel),
            .diagnostics = std::move(instance.diagnostics)};
  }

  [[nodiscard]] SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId constructorId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) const {
    SemanticVisitor instance = *this;
    const ConstructorInfo *constructor = nullptr;
    for (const ClassInfo &owner : instance.classes) {
      const auto found =
          std::find_if(owner.constructors.begin(), owner.constructors.end(),
                       [constructorId](const ConstructorInfo &candidate) {
                         return candidate.id == constructorId;
                       });
      if (found != owner.constructors.end()) {
        constructor = &*found;
        break;
      }
    }
    if (constructor == nullptr || constructor->declaration == nullptr) {
      return {.model = semanticModel};
    }

    instance.prepareInstanceAnalysis();
    if (!instance.prepareClassInstanceContext(
            constructor->owner, classTypeArguments, classValueArguments)) {
      return {.model = std::move(instance.semanticModel),
              .diagnostics = std::move(instance.diagnostics)};
    }
    constructor->declaration->accept(instance);
    instance.finishClassInstanceContext();
    return {.model = std::move(instance.semanticModel),
            .diagnostics = std::move(instance.diagnostics)};
  }

  [[nodiscard]] SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId classId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) const {
    SemanticVisitor instance = *this;
    const ClassTypeInfo *classType = semanticModel.findClassType(classId);
    const ClassLifecycleInfo *lifecycle =
        classType == nullptr || classType->declaration == nullptr
            ? nullptr
            : semanticModel.findClassLifecycle(*classType->declaration);
    if (lifecycle == nullptr || !lifecycle->declaredDestructor ||
        lifecycle->declaredDestructor->declaration == nullptr) {
      return {.model = semanticModel};
    }

    instance.prepareInstanceAnalysis();
    if (!instance.prepareClassInstanceContext(
            classId, classTypeArguments, classValueArguments)) {
      return {.model = std::move(instance.semanticModel),
              .diagnostics = std::move(instance.diagnostics)};
    }
    lifecycle->declaredDestructor->declaration->accept(instance);
    instance.finishClassInstanceContext();
    return {.model = std::move(instance.semanticModel),
            .diagnostics = std::move(instance.diagnostics)};
  }

  [[nodiscard]] SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId classId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) const {
    SemanticVisitor instance = *this;
    const ClassTypeInfo *classType = semanticModel.findClassType(classId);
    if (classType == nullptr || classType->declaration == nullptr) {
      return {.model = semanticModel};
    }

    instance.prepareInstanceAnalysis();
    if (!instance.prepareClassInstanceContext(
            classId, classTypeArguments, classValueArguments)) {
      return {.model = std::move(instance.semanticModel),
              .diagnostics = std::move(instance.diagnostics)};
    }
    for (const ClassFieldTypeInfo &field : classType->fields) {
      if (field.declaration != nullptr) {
        field.declaration->accept(instance);
      }
    }
    for (const ClassFieldTypeInfo &field : classType->staticFields) {
      if (field.declaration != nullptr) {
        field.declaration->accept(instance);
      }
    }
    instance.finishClassInstanceContext();
    return {.model = std::move(instance.semanticModel),
            .diagnostics = std::move(instance.diagnostics)};
  }

  void visitAccessSpecifierDecl(const AccessSpecifierDecl &) override {}

  void visitEnumDecl(const EnumDecl &stmt) override {
    const EnumTypeInfo *info = semanticModel.findEnumType(stmt);
    const SemanticType type = info == nullptr
                                  ? SemanticType::Unknown
                                  : SemanticType::enumType(info->id);
    const SymbolId symbol = recordToolingSymbol(
        stmt.name(), SymbolKind::Enum,
        info == nullptr ? stmt.name().lexeme : info->qualifiedName, type);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::EnumType,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = type,
         .enumType = &stmt});
    for (const EnumeratorDecl &enumerator : stmt.enumerators()) {
      const SymbolId enumeratorSymbol = recordToolingSymbol(
          enumerator.name, SymbolKind::Enumerator,
          (info == nullptr ? stmt.name().lexeme : info->qualifiedName) +
              "::" + enumerator.name.lexeme,
          type);
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(enumerator.name),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = enumeratorSymbol,
           .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
           .name = enumerator.name.lexeme,
           .type = type});
    }
  }

  void visitBlockStmt(const BlockStmt &stmt) override {
    beginScope();
    analyze(stmt.statements());
    endScope();
  }

  void visitClassDecl(const ClassDecl &stmt) override {
    const auto registered = classDeclIds.find(&stmt);
    if (registered == classDeclIds.end()) {
      return;
    }

    const std::optional<ClassId> enclosingClass = currentClass;
    currentClass = registered->second;
    const ClassInfo &info = classInfo(*currentClass);
    const SemanticType type = SemanticType::classType(info.id);
    const SymbolId symbol = recordToolingSymbol(
        stmt.name(),
        stmt.kind() == ClassKind::Struct ? SymbolKind::Struct
                                         : SymbolKind::Class,
        qualifiedName(info.namespaceScope, stmt.name().lexeme), type);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::ClassType,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = type,
         .classType = &stmt});
    beginTypeParameterScope(info.genericParameters);
    for (const BaseSpecifier &base : stmt.bases()) {
      validateType(base.type);
      validateReferencePlacement(base.type, false, "base type");
    }
    if (info.constructors.empty()) {
      for (const FieldInfo &field : info.fields) {
        const auto member = info.members.find(field.declaration->name().lexeme);
        const bool storedReference =
            member != info.members.end() &&
            member->second.symbol.type.kind == SemanticType::Reference;
        if (!field.declaration->initializer() && !storedReference) {
          report(field.declaration->name(),
                 "Class and struct fields must have an initializer or be "
                 "initialized by a constructor.");
        }
      }
    }
    beginScope();
    for (const auto &[name, member] : info.members) {
      scopes.back().emplace(name, member.symbol);
    }
    analyze(stmt.members());
    endScope();
    endTypeParameterScope();
    currentClass = enclosingClass;
  }

  void visitConditionalStmt(const ConditionalStmt &stmt) override {
    if (const StmtList *branch = stmt.activeBranch(target)) {
      analyze(*branch);
    }
  }

  void visitConstructorDecl(const ConstructorDecl &stmt) override {
    if (!currentClass) {
      report(stmt.name(),
             "Constructors can only be declared in a class or struct.");
      return;
    }

    ClassInfo &owner = classInfo(*currentClass);
    const ConstructorInfo *constructorInfo =
        semanticModel.findConstructor(stmt);
    const SemanticType ownerType = SemanticType::classType(owner.id);
    const SymbolId symbol = recordToolingSymbol(
        stmt.name(), SymbolKind::Constructor,
        qualifiedName(owner.namespaceScope,
                      owner.name.lexeme + "::" + stmt.name().lexeme),
        ownerType, false, true, constructorAccess(owner, &stmt));
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::Constructor,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = ownerType,
         .constructor = &stmt});
    for (const Parameter &parameter : stmt.parameters()) {
      validateType(parameter.type);
      validateReferencePlacement(parameter.type, true, "constructor parameter",
                                 constructorInfo != nullptr &&
                                     constructorInfo->kind ==
                                         ConstructorKind::Move);
      const SemanticType parameterType = typeOf(parameter);
      if (nestsBorrowedState(parameterType)) {
        report(parameter.name,
               "Constructor parameters cannot nest borrowed state in the "
               "current lifetime model.",
               "GTI-S2045");
      }
      semanticModel.record(
          parameter,
          bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly));
      recordBindingOccurrence(parameter.name, parameterType,
                              parameter.mutability == Mutability::Mutable,
                              SemanticBindingKind::Parameter);
      if (parameterType == SemanticType::Void) {
        report(parameter.type.name.last(),
               "Constructor parameters cannot have type void.");
      }
    }

    if (constructorInfo == nullptr ||
        constructorInfo->kind != ConstructorKind::Ordinary || !stmt.body()) {
      return;
    }

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    const bool enclosingReceiverStorageBorrowed = receiverStorageBorrowed;
    currentReturnType = SemanticType::Void;
    currentReceiverMutability = ReceiverMutability::Mutable;
    receiverStorageBorrowed = false;
    ++functionDepth;
    ++constructorDepth;
    beginScope();

    for (const Parameter &parameter : stmt.parameters()) {
      if (!parameter.name.lexeme.empty()) {
        declare(parameter.name, typeOf(parameter),
                parameter.mutability == Mutability::Mutable,
                SemanticBindingKind::Parameter, nullptr, &parameter);
      }
    }

    std::unordered_set<std::string> initializedFields;
    std::optional<std::size_t> previousFieldIndex;
    const ClassBaseTypeInfo *base = concreteBase(owner);
    bool initializedBase = false;
    bool sawFieldInitializer = false;
    for (const ConstructorInitializer &initializer : stmt.initializers()) {
      const Token &target = initializer.target.name.last();
      std::optional<std::size_t> fieldIndex;
      const VariableDecl *field = nullptr;
      if (initializer.target.name.segments.size() == 1 &&
          initializer.target.arguments.empty()) {
        for (std::size_t index = 0; index < owner.fields.size(); ++index) {
          if (owner.fields[index].declaration->name().lexeme == target.lexeme) {
            fieldIndex = index;
            field = owner.fields[index].declaration;
            break;
          }
        }
      }

      if (field == nullptr && base != nullptr &&
          typeOf(initializer.target, owner.namespaceScope) == base->type) {
        validateType(initializer.target);
        if (initializedBase) {
          report(target,
                 "Base '" + typeSpelling(base->type) +
                     "' is initialized more than once.",
                 "GTI-S2040");
        }
        if (sawFieldInitializer) {
          report(target,
                 "The state-bearing base initializer must precede field "
                 "initializers.",
                 "GTI-S2040");
        }
        initializedBase = true;
        analyzeBaseConstructorInitializer(initializer, base->type);
        continue;
      }

      sawFieldInitializer = true;
      const auto inserted = initializedFields.insert(target.lexeme);
      if (!inserted.second) {
        report(target, "Constructor field '" + target.lexeme +
                           "' is initialized more than once.");
      }
      if (field != nullptr) {
        const auto member = owner.members.find(target.lexeme);
        if (member != owner.members.end()) {
          const SymbolId symbol = toolingSymbolFor(member->second.symbol);
          semanticModel.recordOccurrence(
              {.sourceUnit = currentSourceUnit,
               .span = tokenSpan(target),
               .kind = SemanticOccurrenceKind::Symbol,
               .symbol = symbol,
               .roles = OccurrenceRole::Reference | OccurrenceRole::Write,
               .name = target.lexeme,
               .type = member->second.symbol.type,
               .traits = typeTraits(member->second.symbol.type),
               .access = member->second.symbol.assignable
                             ? AccessMode::Mutable
                             : AccessMode::ReadOnly,
               .mutableBinding = member->second.symbol.assignable,
               .bindingKind = SemanticBindingKind::Field});
          semanticModel.record(
              initializer,
              ResolvedConstructorInitializerInfo{
                  .kind = ConstructorInitializerTargetKind::Field,
                  .targetType = member->second.symbol.type,
                  .field = symbol,
                  .storesReference = member->second.symbol.type.kind ==
                                     SemanticType::Reference,
                  .borrowAccess = member->second.symbol.type.referenceAccess});
        }
      }
      if (field == nullptr) {
        report(target,
               "Unknown constructor initializer target '" +
                   typeRefSpelling(initializer.target) +
                   "'; expected an immediate field" +
                   (base == nullptr ? std::string(".")
                                    : " or the direct base '" +
                                          typeSpelling(base->type) + "'."));
      } else {
        if (previousFieldIndex && *fieldIndex < *previousFieldIndex) {
          report(target,
                 "Constructor initializers must follow field declaration "
                 "order.");
        }
        previousFieldIndex = fieldIndex;
      }

      const bool enclosingConstructorInitializer =
          analyzingConstructorInitializer;
      analyzingConstructorInitializer = true;
      const SemanticType fieldType =
          field == nullptr ? SemanticType::Unknown
                           : typeOf(field->type(), owner.namespaceScope);
      SemanticType valueType = SemanticType::Unknown;
      if (initializer.arguments.size() != 1) {
        report(target,
               "A field constructor initializer requires exactly one "
               "argument.",
               "GTI-S2012");
        for (const ExprPtr &argument : initializer.arguments) {
          valueType = analyze(argument);
        }
      } else {
        const SemanticType initializerTarget =
            fieldType.kind == SemanticType::Reference &&
                    fieldType.arguments.size() == 1
                ? fieldType.arguments.front()
                : fieldType;
        valueType = field == nullptr
                        ? analyze(initializer.arguments.front())
                        : analyzeInitializer(initializer.arguments.front(),
                                             initializerTarget);
      }
      analyzingConstructorInitializer = enclosingConstructorInitializer;
      if (field != nullptr && initializer.arguments.size() == 1 &&
          fieldType.kind == SemanticType::Reference) {
        validateReferenceBinding(fieldType, valueType,
                                 initializer.arguments.front());
      } else if (field != nullptr && initializer.arguments.size() == 1 &&
                 !isOwnershipAssignable(fieldType, valueType,
                                        initializer.arguments.front())) {
        report(expressionToken(initializer.arguments.front()),
               "Cannot initialize field '" + target.lexeme + "' of type '" +
                   typeSpelling(fieldType) + "' with a value of type '" +
                   typeSpelling(valueType) + "'.",
               "GTI-S2003");
      }
    }

    if (base != nullptr && !initializedBase &&
        !baseHasAccessibleDefaultConstructor(*base, owner.id)) {
      report(stmt.name(),
             "Constructor must explicitly initialize base '" +
                 typeSpelling(base->type) +
                 "' because it has no accessible default constructor.",
             "GTI-S2040");
    }

    for (const FieldInfo &field : owner.fields) {
      if (!field.declaration->initializer() &&
          !initializedFields.contains(field.declaration->name().lexeme)) {
        report(field.declaration->name(),
               "Field must be initialized by this constructor.");
      }
    }

    analyze(stmt.body()->statements());
    endScope();
    --constructorDepth;
    --functionDepth;
    currentReceiverMutability = enclosingReceiverMutability;
    receiverStorageBorrowed = enclosingReceiverStorageBorrowed;
    currentReturnType = enclosingReturnType;
  }

  void visitDestructorDecl(const DestructorDecl &stmt) override {
    if (!currentClass) {
      report(stmt.tilde(),
             "Destructors can only be declared in a class or struct.",
             "GTI-S2021");
      return;
    }

    const ClassInfo &owner = classInfo(*currentClass);
    const SymbolId symbol = recordToolingSymbol(
        stmt.name(), SymbolKind::Destructor,
        qualifiedName(owner.namespaceScope,
                      owner.name.lexeme + "::~" + stmt.name().lexeme),
        SemanticType::classType(*currentClass), false, true,
        AccessModifier::Public);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::Destructor,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = SemanticType::classType(*currentClass),
         .destructor = &stmt});

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    const bool enclosingReceiverStorageBorrowed = receiverStorageBorrowed;
    currentReturnType = SemanticType::Void;
    currentReceiverMutability = ReceiverMutability::Mutable;
    receiverStorageBorrowed = false;
    ++functionDepth;
    ++destructorDepth;
    beginScope();

    analyze(stmt.body()->statements());

    endScope();
    --destructorDepth;
    --functionDepth;
    currentReceiverMutability = enclosingReceiverMutability;
    receiverStorageBorrowed = enclosingReceiverStorageBorrowed;
    currentReturnType = enclosingReturnType;
  }

  void visitEmptyStmt(const EmptyStmt &) override {}

  void visitExpressionStmt(const ExpressionStmt &stmt) override {
    const SemanticType resultType = analyze(stmt.expression());
    const Call *call = directCall(stmt.expression());

    if (stmt.discardAttribute()) {
      if (call == nullptr) {
        report(*stmt.discardAttribute(),
               "'[[discard]]' can only be applied to a function call.");
      } else if (resultType == SemanticType::Void) {
        report(*stmt.discardAttribute(),
               "'[[discard]]' cannot be applied to a void function call.");
      }
      return;
    }

    if (call != nullptr && resultType != SemanticType::Void &&
        resultType != SemanticType::Unknown) {
      report(call->paren(),
             "Function return value must be used or explicitly discarded "
             "with '[[discard]]'.",
             "GTI-S2009");
    }
  }

  void visitForStmt(const ForStmt &stmt) override {
    beginScope();
    analyze(stmt.initializer());
    if (stmt.condition()) {
      const SemanticType conditionType = analyze(stmt.condition());
      requireBool(stmt.condition(), conditionType,
                  expressionToken(stmt.condition()),
                  "For-loop condition must be bool.");
    }
    const ScopeStack beforeLoop = scopes;
    ++loopDepth;
    analyze(stmt.body());
    analyze(stmt.increment());
    --loopDepth;
    const ScopeStack afterIteration = scopes;
    scopes = beforeLoop;
    mergeValueStates(beforeLoop, beforeLoop, afterIteration);
    endScope();
  }

  void visitRangeForStmt(const RangeForStmt &stmt) override {
    analyze(stmt.lowered());
  }

  void visitFunctionDecl(const FunctionDecl &stmt) override {
    const std::vector<GenericParameterInfo> &genericParameters =
        genericParametersFor(stmt);
    beginTypeParameterScope(genericParameters);
    const FunctionInfo *functionInfo = semanticModel.findFunction(stmt);
    const Token &functionToken =
        stmt.operatorName() ? stmt.operatorName()->symbol : stmt.name();
    const SymbolId functionSymbol = recordFunctionSymbol(stmt);
    OccurrenceRole functionRoles = OccurrenceRole::Declaration;
    if (stmt.body()) {
      functionRoles |= OccurrenceRole::Definition;
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(functionToken),
         .kind = SemanticOccurrenceKind::Function,
         .symbol = functionSymbol,
         .roles = functionRoles,
         .name = stmt.operatorName() ? std::string(operatorSourceSpelling(
                                           stmt.operatorName()->kind))
                                     : stmt.name().lexeme,
         .type = functionInfo == nullptr ? SemanticType::Unknown
                                         : functionInfo->returnType,
         .function = &stmt});
    const bool isEntryPoint =
        functionInfo != nullptr && functionInfo->entryPoint;
    if (sourceGraph != nullptr && currentSourceUnit != 0 &&
        currentSourceUnit != sourceGraph->entryUnit() &&
        currentNamespace.empty() && !currentClass && functionDepth == 0 &&
        stmt.name().lexeme == "main") {
      report(stmt.name(),
             "The main entry point can only be declared in the entry source "
             "file.",
             "GTI-S2025");
    }
    if (!genericParameters.empty() && currentNamespace.empty() &&
        !currentClass && stmt.name().lexeme == "main") {
      report(stmt.name(), "The main entry point cannot be generic.");
    }
    validateRuntimeBinding(stmt);
    const bool enclosingPackTypeReference = allowPackTypeReference;
    allowPackTypeReference = true;
    validateType(stmt.returnType());
    allowPackTypeReference = enclosingPackTypeReference;
    const bool methodDeclaration = currentClass && functionDepth == 0;
    if ((stmt.isVirtual() || stmt.isOverride() || stmt.isPure()) &&
        !methodDeclaration) {
      const Token &location =
          stmt.isVirtual() ? *stmt.virtualKeyword()
                           : (stmt.isOverride() ? *stmt.overrideKeyword()
                                                : stmt.pureSpecifier()->equal);
      report(location,
             "Polymorphic specifiers are valid only on class, struct, or "
             "interface methods.",
             "GTI-S2042");
    }
    if (stmt.isStatic() && !methodDeclaration && currentNamespace.empty() &&
        stmt.name().lexeme == "main") {
      report(*stmt.staticKeyword(),
             "The main entry point cannot have internal static linkage.",
             "GTI-S2039");
    }
    validateFunctionPacks(stmt, genericParameters);
    validateOperatorDeclaration(stmt, methodDeclaration);
    validateReferencePlacement(stmt.returnType(), methodDeclaration,
                               methodDeclaration ? "method return type"
                                                 : "function return type");
    const SemanticType declaredReturnType =
        typeOf(stmt.returnType(), stmt.returnMutability());
    if (typeTraits(declaredReturnType).containsBorrowedState &&
        (!methodDeclaration || stmt.isStatic())) {
      report(stmt.name(),
             "A value carrying a stored reference may only be returned from "
             "an instance method and must borrow from that method's receiver.",
             "GTI-S2045");
    }
    if (stmt.returnMutability() == Mutability::Mutable) {
      if (!stmt.returnType().reference) {
        report(stmt.name(),
               "A mutable function return must be a reference type.",
               "GTI-S2017");
      } else if (!methodDeclaration) {
        report(stmt.name(),
               "Mutable reference returns are currently limited to class "
               "and struct methods.",
               "GTI-S2017");
      } else if (stmt.isStatic()) {
        report(stmt.name(),
               "Static methods cannot return a mutable reference in the "
               "current lifetime model.",
               "GTI-S2017");
      } else if (stmt.receiverMutability() != ReceiverMutability::Mutable) {
        report(stmt.name(),
               "A mutable reference return requires a mutable receiver.",
               "GTI-S2017");
      }
    }
    for (const Parameter &parameter : stmt.parameters()) {
      const bool enclosingParameterPackTypeReference = allowPackTypeReference;
      allowPackTypeReference = true;
      validateType(parameter.type);
      allowPackTypeReference = enclosingParameterPackTypeReference;
      validateReferencePlacement(parameter.type, true, "function parameter");
      const SemanticType declaredType = typeOf(parameter);
      const SemanticType parameterType =
          parameter.pack && declaredType.kind == SemanticType::TypeParameter
              ? SemanticType::typePack(declaredType.genericParameterId)
              : declaredType;
      if (nestsBorrowedState(parameterType)) {
        report(parameter.name,
               "Function parameters cannot nest borrowed state in the "
               "current lifetime model.",
               "GTI-S2045");
      }
      semanticModel.record(
          parameter,
          bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly));
      recordBindingOccurrence(parameter.name, parameterType,
                              parameter.mutability == Mutability::Mutable,
                              SemanticBindingKind::Parameter);
      if (declaredType == SemanticType::Void) {
        report(parameter.type.name.last(),
               "Function parameters cannot have type void.");
      }
    }
    if (isEntryPoint) {
      const SemanticType returnType =
          typeOf(stmt.returnType(), stmt.returnMutability());
      const bool invalidReturnType = returnType != SemanticType::Unknown &&
                                     returnType != SemanticType::Int32;
      if (invalidReturnType || !stmt.parameters().empty() || !stmt.body()) {
        report(stmt.name(),
               "The main entry point currently requires a definition with "
               "signature 'int main()'.",
               "GTI-S2032");
      }
    }
    if (!stmt.body()) {
      endTypeParameterScope();
      return;
    }

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    const bool enclosingReceiverStorageBorrowed = receiverStorageBorrowed;
    const bool enclosingStaticMemberFunction = currentStaticMemberFunction;
    if (currentClass && functionDepth == 0) {
      currentReceiverMutability = stmt.receiverMutability();
      currentStaticMemberFunction = stmt.isStatic();
    }
    receiverStorageBorrowed = false;
    currentReturnType = typeOf(stmt.returnType(), stmt.returnMutability());
    ++functionDepth;
    beginScope();

    for (const Parameter &parameter : stmt.parameters()) {
      if (!parameter.name.lexeme.empty()) {
        const SemanticType declaredType = typeOf(parameter);
        declare(parameter.name,
                parameter.pack &&
                        declaredType.kind == SemanticType::TypeParameter
                    ? SemanticType::typePack(declaredType.genericParameterId)
                    : declaredType,
                parameter.mutability == Mutability::Mutable,
                SemanticBindingKind::Parameter, nullptr, &parameter);
      }
    }

    // A function body owns the parameter scope, so do not add another scope.
    analyze(stmt.body()->statements());
    if (currentReturnType != SemanticType::Void &&
        currentReturnType != SemanticType::Unknown && !isEntryPoint &&
        summarizeFlow(stmt.body()->statements()).canFallThrough) {
      const Token &location =
          stmt.operatorName() ? stmt.operatorName()->keyword : stmt.name();
      report(location,
             "Non-void function can reach the end without returning a value "
             "of type '" +
                 typeSpelling(currentReturnType) + "'.",
             "GTI-S2031");
    }
    endScope();
    --functionDepth;
    currentReceiverMutability = enclosingReceiverMutability;
    receiverStorageBorrowed = enclosingReceiverStorageBorrowed;
    currentStaticMemberFunction = enclosingStaticMemberFunction;
    currentReturnType = enclosingReturnType;
    endTypeParameterScope();
  }

  void visitIfStmt(const IfStmt &stmt) override {
    const SemanticType conditionType = analyze(stmt.condition());
    requireBool(stmt.condition(), conditionType,
                expressionToken(stmt.condition()),
                "If condition must be bool.");
    const ScopeStack beforeBranches = scopes;
    analyze(stmt.thenBranch());
    const ScopeStack thenScopes = scopes;
    scopes = beforeBranches;
    if (stmt.elseBranch()) {
      analyze(stmt.elseBranch());
    }
    const ScopeStack elseScopes = scopes;
    scopes = beforeBranches;
    mergeValueStates(beforeBranches, thenScopes, elseScopes);
  }

  void visitLoopControlStmt(const LoopControlStmt &stmt) override {
    const bool isBreak = stmt.keyword().kind == TokenKind::BREAK;
    if ((isBreak && loopDepth == 0 && switchDepth == 0) ||
        (!isBreak && loopDepth == 0)) {
      report(stmt.keyword(),
             isBreak ? "'break' can only be used inside a loop or switch."
                     : "'continue' can only be used inside a loop.",
             "GTI-S2010");
    }
  }

  void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) override {
    recordQualifiedPathUses(stmt.target(), true);
    const SymbolId symbol =
        recordToolingSymbol(stmt.name(), SymbolKind::NamespaceAlias,
                            qualifiedName(currentNamespace, stmt.name().lexeme),
                            SemanticType::Unknown);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme});
  }

  void visitNamespaceDecl(const NamespaceDecl &stmt) override {
    const SymbolId symbol =
        recordToolingSymbol(stmt.name(), SymbolKind::Namespace,
                            qualifiedName(currentNamespace, stmt.name().lexeme),
                            SemanticType::Unknown);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme});
    currentNamespace.emplace_back(stmt.name().lexeme);
    analyze(stmt.declarations());
    currentNamespace.pop_back();
  }

  void visitReturnStmt(const ReturnStmt &stmt) override {
    if (functionDepth == 0) {
      report(stmt.keyword(), "Cannot return from outside a function.");
      return;
    }
    if (constructorDepth > 0) {
      if (stmt.value()) {
        analyze(stmt.value());
      }
      report(stmt.keyword(), "Constructors cannot contain return statements.");
      return;
    }
    if (destructorDepth > 0) {
      if (stmt.value()) {
        analyze(stmt.value());
      }
      report(stmt.keyword(), "Destructors cannot contain return statements.",
             "GTI-S2021");
      return;
    }
    if (currentReturnType == SemanticType::Void) {
      if (stmt.value()) {
        analyze(stmt.value());
        report(stmt.keyword(), "Void function cannot return a value.");
      }
      return;
    }
    if (!stmt.value()) {
      if (isExpectedVoid(currentReturnType)) {
        return;
      }
      report(stmt.keyword(), "A value is required for this return type.");
      return;
    }

    if (currentReturnType.kind == SemanticType::Reference) {
      if (currentReturnType.arguments.size() != 1) {
        analyze(stmt.value());
        return;
      }
      const SemanticType valueType =
          analyzeInitializer(stmt.value(), currentReturnType.arguments[0]);
      if (currentClass) {
        validateReferenceReturn(currentReturnType, valueType, stmt.value());
      }
      return;
    }

    const SemanticType valueType =
        analyzeInitializer(stmt.value(), currentReturnType);
    if (typeTraits(currentReturnType).containsBorrowedState) {
      validateStoredBorrowReturn(currentReturnType, valueType, stmt.value());
    }
    if (!isOwnershipAssignable(currentReturnType, valueType, stmt.value())) {
      report(expressionToken(stmt.value()),
             "Cannot return a value of type '" + typeSpelling(valueType) +
                 "' from a function returning '" +
                 typeSpelling(currentReturnType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(currentReturnType) &&
          currentReturnType == valueType) {
        diagnostics.back().hints.emplace_back(
            "Return std::move(owner) to transfer ownership.");
      }
    }
  }

  void visitSwitchStmt(const SwitchStmt &stmt) override {
    const SemanticType subjectType = analyze(stmt.expression());
    const bool validSubject = isInteger(subjectType) ||
                              subjectType == SemanticType::Char ||
                              subjectType.kind == SemanticType::Enum;
    if (subjectType != SemanticType::Unknown && !validSubject) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2037", DiagnosticPhase::Semantics, stmt.keyword(),
          "Switch expression must have an integer, char, or scoped enum "
          "type; found '" +
              typeSpelling(subjectType) + "'.");
      if (subjectType == SemanticType::Bool) {
        diagnostic.hints.emplace_back(
            "Use 'if' for boolean branching; switch is reserved for "
            "multi-value selection.");
      }
      diagnostics.emplace_back(std::move(diagnostic));
    }

    const ScopeStack beforeSwitch = scopes;
    std::vector<std::pair<SwitchCaseValue, const SwitchLabel *>> seenCases;
    const SwitchLabel *firstDefault = nullptr;
    std::vector<std::pair<ScopeStack, FlowSummary>> armResults;
    bool hasDefault = false;

    ++switchDepth;
    for (const SwitchArm &arm : stmt.arms()) {
      for (const SwitchLabel &label : arm.labels) {
        if (label.isDefault()) {
          hasDefault = true;
          if (firstDefault == nullptr) {
            firstDefault = &label;
          } else {
            Diagnostic diagnostic = makeDiagnostic(
                "GTI-S2037", DiagnosticPhase::Semantics, label.keyword,
                "Switch may contain only one 'default' label.");
            diagnostic.related.push_back(
                {tokenSpan(firstDefault->keyword),
                 "First 'default' label declared here."});
            diagnostics.emplace_back(std::move(diagnostic));
          }
          continue;
        }

        scopes = beforeSwitch;
        const SemanticType caseType = analyze(label.value);
        scopes = beforeSwitch;
        if (subjectType == SemanticType::Unknown ||
            caseType == SemanticType::Unknown || !validSubject) {
          continue;
        }
        if (caseType != subjectType) {
          Diagnostic diagnostic =
              makeDiagnostic("GTI-S2037", DiagnosticPhase::Semantics,
                             expressionToken(label.value),
                             "Switch case type '" + typeSpelling(caseType) +
                                 "' does not exactly match subject type '" +
                                 typeSpelling(subjectType) + "'.");
          diagnostic.hints.emplace_back(
              "GTI switch labels do not perform implicit conversions; use "
              "an explicit conversion when appropriate.");
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }

        const std::optional<SwitchCaseValue> value =
            switchCaseConstant(label.value.get(), subjectType);
        if (!value) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2037", DiagnosticPhase::Semantics,
              expressionToken(label.value),
              "Switch case must be a compile-time integer or character "
              "constant, or a scoped enumerator.");
          diagnostic.hints.emplace_back(
              "Use a literal, an explicitly converted integer literal, or "
              "an enumerator from the switch enum.");
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }

        const auto duplicate = std::find_if(
            seenCases.begin(), seenCases.end(),
            [&](const auto &candidate) { return candidate.first == *value; });
        if (duplicate != seenCases.end()) {
          Diagnostic diagnostic =
              makeDiagnostic("GTI-S2037", DiagnosticPhase::Semantics,
                             label.keyword, "Duplicate switch case value.");
          diagnostic.related.push_back(
              {tokenSpan(duplicate->second->keyword),
               "First matching case label declared here."});
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }
        seenCases.emplace_back(*value, &label);
        semanticModel.recordSwitchCase(*label.value, *value);
      }

      scopes = beforeSwitch;
      beginScope();
      analyze(arm.statements);
      endScope();
      const FlowSummary flow = summarizeFlow(arm.statements);
      armResults.emplace_back(scopes, flow);
      if (flow.canFallThrough) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2037", DiagnosticPhase::Semantics,
            arm.labels.empty() ? stmt.keyword() : arm.labels.back().keyword,
            "Switch arm can reach its boundary without an explicit "
            "terminator.");
        diagnostic.hints.emplace_back(
            "End every switch arm with 'break', 'return', or 'continue' "
            "when the switch is inside a loop.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
    --switchDepth;

    std::vector<ScopeStack> exitStates;
    if (!hasDefault) {
      exitStates.push_back(beforeSwitch);
    }
    for (auto &[armScopes, flow] : armResults) {
      if (flow.canFallThrough || flow.breaksEnclosingControl) {
        exitStates.push_back(std::move(armScopes));
      }
    }
    if (exitStates.empty()) {
      scopes = beforeSwitch;
      return;
    }
    ScopeStack merged = std::move(exitStates.front());
    for (std::size_t index = 1; index < exitStates.size(); ++index) {
      scopes = beforeSwitch;
      mergeValueStates(beforeSwitch, merged, exitStates[index]);
      merged = scopes;
    }
    scopes = std::move(merged);
  }

  void visitTypeAliasDecl(const TypeAliasDecl &stmt) override {
    const TypeAliasInfo *info = semanticModel.findTypeAlias(stmt);
    const SemanticType type =
        info == nullptr ? SemanticType::Unknown : info->type;
    const SymbolId symbol = recordToolingSymbol(
        stmt.name(), SymbolKind::TypeAlias,
        info == nullptr ? qualifiedName(currentNamespace, stmt.name().lexeme)
                        : info->qualifiedName,
        type);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::TypeAlias,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = type,
         .typeAlias = &stmt});
  }

  void visitVariableDecl(const VariableDecl &stmt) override {
    if (stmt.type().name.last().kind == TokenKind::AUTO) {
      analyzeInferredVariable(stmt);
      return;
    }
    validateType(stmt.type());
    const bool localReference = functionDepth > 0;
    const bool instanceField =
        currentClass && functionDepth == 0 && !stmt.isStatic();
    validateReferencePlacement(
        stmt.type(), localReference || instanceField,
        localReference ? "local binding"
                       : (instanceField ? "instance field" : "storage"));
    const SemanticType declaredType =
        typeOf(stmt.type(),
               stmt.isMutable() ? Mutability::Mutable : Mutability::Immutable);
    const SemanticBindingKind bindingKind =
        currentClass && functionDepth == 0
            ? (stmt.isStatic() ? SemanticBindingKind::StaticField
                               : SemanticBindingKind::Field)
            : (functionDepth > 0 ? SemanticBindingKind::LocalVariable
                                 : SemanticBindingKind::GlobalVariable);
    const bool internalLinkage = stmt.isStatic() && !currentClass;
    const SymbolId symbol = recordBindingOccurrence(
        stmt.name(), declaredType, stmt.isMutable(), bindingKind,
        bindingKind == SemanticBindingKind::StaticField, internalLinkage);
    BindingInfo info =
        bindingInfo(declaredType, stmt.isMutable() ? AccessMode::Mutable
                                                   : AccessMode::ReadOnly);
    info.symbol = symbol;
    info.staticStorage = stmt.isStatic();
    info.internalLinkage = internalLinkage;
    semanticModel.record(stmt, std::move(info));
    const bool globalStorage =
        functionDepth == 0 && (!currentClass || stmt.isStatic());
    SemanticType initializerType = SemanticType::Unknown;
    if (declaredType == SemanticType::Void) {
      report(stmt.type().name.last(), "Variables cannot have type void.");
    } else if (declaredType.kind == SemanticType::UniqueOwner &&
               globalStorage) {
      report(stmt.name(),
             "Compiler-private unique owners can only be local bindings, "
             "function values, or class fields.",
             "GTI-S2018");
    } else if (declaredType.kind == SemanticType::Storage && globalStorage) {
      report(stmt.name(),
             "Compiler-private storage can only be used as a local binding or "
             "class field.",
             "GTI-S2019");
    } else if (declaredType.kind == SemanticType::Storage &&
               functionDepth > 0 && !stmt.initializer()) {
      report(stmt.name(),
             "Compiler-private storage bindings require an allocation "
             "initializer.",
             "GTI-S2019");
    } else if (declaredType.kind == SemanticType::UniqueOwner &&
               !stmt.initializer()) {
      report(stmt.name(),
             "Compiler-private unique owner bindings require an initializer.",
             "GTI-S2018");
    } else if (typeTraits(declaredType).ownership == OwnershipKind::Unique &&
               globalStorage) {
      report(stmt.name(),
             "Unique owners can only be local bindings, function values, or "
             "class fields.",
             "GTI-S2018");
    } else if (typeTraits(declaredType).containsBorrowedState &&
               globalStorage) {
      report(stmt.name(),
             "Values carrying stored references cannot have global or static "
             "storage duration.",
             "GTI-S2045");
    } else if (instanceField && declaredType.kind != SemanticType::Reference &&
               typeTraits(declaredType).containsBorrowedState) {
      report(stmt.name(),
             "Borrowed state cannot be nested in another field in the current "
             "lifetime model.",
             "GTI-S2045");
    } else if (declaredType.kind == SemanticType::Reference &&
               !stmt.initializer() && !instanceField) {
      report(stmt.name(), "Reference bindings require an initializer.",
             "GTI-S2017");
    } else if (!stmt.initializer()) {
      const bool field = currentClass && functionDepth == 0 && !stmt.isStatic();
      if (stmt.isStatic() && currentClass) {
        report(stmt.name(),
               "Static class and struct fields require an in-class "
               "initializer.",
               "GTI-S2039");
      } else if (!field && declaredType.kind == SemanticType::Array) {
        report(stmt.name(),
               "Fixed array variables require an initializer; use '{}' to "
               "default-initialize every element.",
               "GTI-S2015");
      } else if (!field && declaredType.kind == SemanticType::Class) {
        report(stmt.name(),
               "Class and struct variables require explicit construction.");
      } else if (!field && declaredType.kind == SemanticType::Enum) {
        report(stmt.name(),
               "Scoped enum variables require an explicit enumerator "
               "initializer.",
               "GTI-S2036");
      } else if (!field && !stmt.isMutable()) {
        report(stmt.name(), "Immutable variable must have an initializer.");
      }
    }
    if (stmt.initializer()) {
      const bool enclosingFieldInitializer = analyzingFieldInitializer;
      analyzingFieldInitializer = currentClass && functionDepth == 0;
      const bool directInitializer = dynamic_cast<const DirectInitializer *>(
                                         stmt.initializer().get()) != nullptr;
      const SemanticType expectedInitializer =
          declaredType.kind == SemanticType::Reference &&
                  declaredType.arguments.size() == 1 && !directInitializer
              ? declaredType.arguments[0]
              : declaredType;
      initializerType =
          analyzeInitializer(stmt.initializer(), expectedInitializer);
      analyzingFieldInitializer = enclosingFieldInitializer;
    }

    if (!predeclaredVariables.contains(&stmt)) {
      if (functionDepth == 0 && !currentClass) {
        declareNamespaceSymbol(currentNamespace, stmt.name(), declaredType,
                               stmt.isMutable(), stmt.isStatic(), symbol);
      } else {
        declare(stmt.name(), declaredType, stmt.isMutable(), bindingKind,
                &stmt);
      }
    }

    if (stmt.initializer() && declaredType.kind == SemanticType::Reference &&
        dynamic_cast<const DirectInitializer *>(stmt.initializer().get()) ==
            nullptr) {
      validateReferenceBinding(declaredType, initializerType,
                               stmt.initializer());
      if (declaredType.arguments.size() == 1 &&
          !isDirectOwnerType(declaredType.arguments[0])) {
        recordRetainedBorrow(stmt.initializer());
      }
    } else if (stmt.initializer()) {
      if (typeTraits(declaredType).containsBorrowedState) {
        validateStoredBorrowInitialization(declaredType, stmt.initializer());
        recordRetainedBorrow(stmt.initializer());
      }
      if (!isOwnershipAssignable(declaredType, initializerType,
                                 stmt.initializer())) {
        report(expressionToken(stmt.initializer()),
               "Cannot initialize '" + stmt.name().lexeme + "' of type '" +
                   typeSpelling(declaredType) + "' with a value of type '" +
                   typeSpelling(initializerType) + "'.",
               "GTI-S2003");
        if (isMoveOnlyOwnerType(declaredType) &&
            declaredType == initializerType) {
          diagnostics.back().hints.emplace_back(
              "Move-only owners cannot be copied; transfer ownership "
              "explicitly with std::move(owner).");
        }
      }
    }
  }

  void visitWhileStmt(const WhileStmt &stmt) override {
    const SemanticType conditionType = analyze(stmt.condition());
    requireBool(stmt.condition(), conditionType,
                expressionToken(stmt.condition()),
                "While condition must be bool.");
    const ScopeStack beforeLoop = scopes;
    ++loopDepth;
    analyze(stmt.body());
    --loopDepth;
    const ScopeStack afterIteration = scopes;
    scopes = beforeLoop;
    mergeValueStates(beforeLoop, beforeLoop, afterIteration);
  }

  void visitAssignExpr(const Assign &expr) override {
    const bool qualified = expr.path().segments.size() > 1;
    if (qualified) {
      recordQualifiedPathUses(expr.path());
    }
    const Symbol *symbol =
        qualified ? resolveQualified(expr.path()) : resolve(expr.name());
    if (symbol == nullptr) {
      if (!qualified && resolveValueParameter(expr.name())) {
        analyze(expr.value());
        report(expr.name(),
               "Cannot assign to compile-time value parameter '" +
                   expr.name().lexeme + "'.",
               "GTI-S2026");
        currentType = SemanticType::UInt64;
        return;
      }
      if (!qualified && !reportMissingLambdaCapture(expr.name()) &&
          !reportInvisibleSymbol(expr.name(), expr.name().lexeme,
                                 resolveGlobally(expr.name()))) {
        report(expr.name(), "Undefined variable '" + expr.name().lexeme + "'.",
               "GTI-S2001");
      } else if (qualified) {
        report(expr.name(),
               "Undefined qualified assignment target '" +
                   pathSpelling(expr.path()) + "'.",
               "GTI-S2001");
      }
      currentType = analyze(expr.value());
      return;
    }
    if (qualified && symbol->ownerClass != 0 &&
        symbol->access == AccessModifier::Private &&
        currentClass != symbol->ownerClass) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, expr.name(),
          "Static member '" + expr.name().lexeme + "' of '" +
              classInfo(symbol->ownerClass).name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(symbol->declaration), "Static member declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    const SemanticType targetType =
        symbol->type.kind == SemanticType::Reference &&
                symbol->type.arguments.size() == 1
            ? symbol->type.arguments[0]
            : symbol->type;
    if (expr.oper().kind != TokenKind::EQUAL &&
        symbol->valueState != ValueState::Available) {
      reportUnavailableValue(expr.name(), *symbol);
    }
    const SemanticType valueType = analyzeInitializer(expr.value(), targetType);
    const auto *moveCall = dynamic_cast<const Call *>(expr.value().get());
    const Variable *movedSource =
        moveCall != nullptr &&
                intrinsicKind(moveCall->callee()) == IntrinsicKind::Move &&
                moveCall->arguments().size() == 1
            ? movedVariable(moveCall->arguments().front())
            : nullptr;
    const bool directSelfMove =
        movedSource != nullptr &&
        movedSource->name().lexeme == expr.name().lexeme;
    if (directSelfMove) {
      report(expr.name(), "Cannot move a binding into itself.", "GTI-S2018");
    }
    if (!symbol->assignable) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2002", DiagnosticPhase::Semantics, expr.name(),
                         symbol->lambdaCapture
                             ? "Cannot assign to immutable lambda capture '" +
                                   expr.name().lexeme + "'."
                             : "Cannot assign to immutable binding '" +
                                   expr.name().lexeme + "'.");
      if (!symbol->declaration.lexeme.empty()) {
        diagnostic.related.push_back(
            {tokenSpan(symbol->declaration), "Binding declared here."});
      }
      diagnostic.hints.emplace_back(
          symbol->lambdaCapture
              ? "Lambda captures are value snapshots and remain immutable."
              : "Bindings are immutable by default; add 'mut' to the "
                "declaration if mutation is required.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else if (symbol->ownerClass != 0 && !symbol->staticMember &&
               currentReceiverMutability != ReceiverMutability::Mutable) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    } else if (symbol->ownerClass != 0 && !symbol->staticMember &&
               receiverStorageBorrowed) {
      report(expr.name(),
             "Cannot mutate 'this' while a reference borrowed from its "
             "move-only storage may still be live.",
             "GTI-S2017");
    } else if (symbol->borrowedStorage) {
      report(expr.name(),
             isMoveOnlyOwnerType(symbol->type)
                 ? "Cannot replace move-only storage while a reference "
                   "borrowed from it may still be live."
                 : "Cannot replace storage while a retained borrow from it "
                   "may still be live.",
             "GTI-S2017");
    }
    const bool valueAssignable =
        isOwnershipAssignment(targetType, valueType, expr.value());
    if (!valueAssignable) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to '" + expr.name().lexeme + "' of type '" +
                 typeSpelling(targetType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(targetType) && targetType == valueType) {
        diagnostics.back().hints.emplace_back(
            "Move-only owners cannot be copied; use std::move(owner) to "
            "transfer "
            "ownership.");
      }
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((targetType != SemanticType::Unknown && !isNumeric(targetType)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    if (expr.oper().kind == TokenKind::EQUAL && symbol->assignable &&
        valueAssignable && !directSelfMove && tracksValueState(*symbol)) {
      if (!qualified) {
        if (Symbol *target = resolveMutable(expr.name())) {
          target->valueState = ValueState::Available;
        }
      }
    }
    currentType = targetType;
  }

  void visitArrayInitializerExpr(const ArrayInitializer &expr) override {
    if (!contextualInitializerType ||
        contextualInitializerType->kind != SemanticType::Array ||
        contextualInitializerType->arguments.size() != 1) {
      for (const ExprPtr &element : expr.elements()) {
        analyze(element);
      }
      if (contextualInitializerType &&
          contextualInitializerType->kind == SemanticType::Class) {
        report(expr.brace(),
               "Class construction does not use '= {...}'; place the braces "
               "directly after the binding name as 'Type name{arguments};'.",
               "GTI-S2038");
        currentType = SemanticType::Unknown;
        return;
      }
      report(expr.brace(),
             "Array initializer requires a fixed array type from its context.",
             "GTI-S2015");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType arrayType = *contextualInitializerType;
    const SemanticType elementType = arrayType.arguments[0];
    if (arrayType.arrayLengthParameterId == 0 && !expr.elements().empty() &&
        expr.elements().size() != arrayType.arrayLength) {
      report(expr.brace(),
             "Fixed array initializer provides " +
                 std::to_string(expr.elements().size()) + " elements but '" +
                 typeSpelling(arrayType) + "' requires exactly " +
                 std::to_string(arrayType.arrayLength) + ".",
             "GTI-S2015");
    }
    if (expr.elements().empty() &&
        (arrayType.arrayLengthParameterId != 0 ||
         arrayType.arrayLength != 0) &&
        !isDefaultInitializable(elementType)) {
      report(expr.brace(),
             "Empty initialization of '" + typeSpelling(arrayType) +
                 "' requires a default-initializable element type.",
             "GTI-S2015");
    }

    for (const ExprPtr &element : expr.elements()) {
      const SemanticType valueType = analyzeInitializer(element, elementType);
      if (!isAssignable(elementType, valueType, element.get())) {
        report(expressionToken(element),
               "Cannot initialize array element of type '" +
                   typeSpelling(elementType) + "' with a value of type '" +
                   typeSpelling(valueType) + "'.",
               "GTI-S2003");
      }
    }
    currentType = arrayType;
  }

  void visitBinaryExpr(const Binary &expr) override {
    const SemanticType leftType = analyze(expr.left());
    const SemanticType rightType = analyze(expr.right());

    if (leftType.kind == SemanticType::Class &&
        (expr.oper().kind == TokenKind::EQUAL_EQUAL ||
         expr.oper().kind == TokenKind::BANG_EQUAL)) {
      const OverloadedOperator kind = expr.oper().kind == TokenKind::EQUAL_EQUAL
                                          ? OverloadedOperator::Equal
                                          : OverloadedOperator::NotEqual;
      const std::optional<FunctionCandidate> selected =
          resolveOperator(expr, kind, expr.left(), leftType, expr.oper(),
                          std::span<const SemanticType>(&rightType, 1),
                          std::span<const ExprPtr>(&expr.right(), 1));
      if (selected && expr.oper().generated &&
          (selected->parameterTypes.size() != 1 ||
           selected->parameterTypes.front().kind != SemanticType::Reference ||
           selected->parameterTypes.front().referenceAccess !=
               AccessMode::ReadOnly)) {
        report(expr.oper(),
               "Range iterator operator!= must accept its sentinel by "
               "read-only reference.",
               "GTI-S2022");
      }
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }

    switch (expr.oper().kind) {
    case TokenKind::COMMA:
      currentType = rightType;
      return;
    case TokenKind::EQUAL_EQUAL:
    case TokenKind::BANG_EQUAL:
      if (!isComparable(leftType, rightType, expr.left().get(),
                        expr.right().get())) {
        report(expr.oper(), "Equality operands have incompatible types.");
      }
      currentType = SemanticType::Bool;
      return;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
      requireOrdered(leftType, rightType, expr.oper());
      if (isInteger(leftType) && isInteger(rightType) &&
          numericResult(leftType, rightType, expr.left().get(),
                        expr.right().get()) == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      currentType = SemanticType::Bool;
      return;
    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::STAR:
    case TokenKind::SLASH:
      requireNumeric(leftType, rightType, expr.oper());
      currentType = numericResult(leftType, rightType, expr.left().get(),
                                  expr.right().get());
      if (isInteger(leftType) && isInteger(rightType) &&
          currentType == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      return;
    case TokenKind::PERCENT:
    case TokenKind::AMPERSAND:
    case TokenKind::CARET:
    case TokenKind::PIPE:
      requireInteger(leftType, rightType, expr.oper());
      if (!isIntegral(leftType) || !isIntegral(rightType)) {
        currentType = SemanticType::Unknown;
        return;
      }
      currentType = numericResult(leftType, rightType, expr.left().get(),
                                  expr.right().get());
      if (isInteger(leftType) && isInteger(rightType) &&
          currentType == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      if (expr.oper().kind == TokenKind::PERCENT) {
        if (const std::optional<IntegerConstant> divisor =
                integerConstant(expr.right().get());
            divisor && divisor->magnitude == 0) {
          report(expr.oper(), "Modulo divisor cannot be zero.");
        }
      }
      return;
    case TokenKind::SHIFT_LEFT:
    case TokenKind::SHIFT_RIGHT:
      requireInteger(leftType, rightType, expr.oper());
      if (!isIntegral(leftType) || !isIntegral(rightType)) {
        currentType = SemanticType::Unknown;
        return;
      }
      currentType = promotedInteger(leftType);
      validateShiftCount(currentType, expr.right().get(), expr.oper());
      return;
    default:
      currentType = SemanticType::Unknown;
    }
  }

  void visitCallExpr(const Call &expr) override {
    if (const IntrinsicKind intrinsic = intrinsicKind(expr.callee());
        intrinsic != IntrinsicKind::None) {
      analyzeIntrinsicCall(expr, intrinsic);
      return;
    }
    if (const auto *callee =
            dynamic_cast<const Variable *>(expr.callee().get())) {
      if (const std::optional<SemanticType> target =
              resolveTypeParameter(NamePath(callee->name()))) {
        analyzeTypeParameterConversion(expr, *target);
        return;
      }
    }
    if (const std::optional<TypeAliasId> aliasId =
            typeAliasForCallee(expr.callee())) {
      const RegisteredTypeAlias &alias = typeAliases[*aliasId - 1];
      if (alias.resolution != TypeAliasResolution::Resolved) {
        for (const ExprPtr &argument : expr.arguments()) {
          analyze(argument);
        }
        currentType = SemanticType::Unknown;
        return;
      }
      if (!expr.typeArguments().empty()) {
        for (const TypeRef &argument : expr.typeArguments()) {
          validateType(argument);
        }
        report(expr.paren(),
               "Type alias '" + alias.qualifiedName +
                   "' does not take generic arguments.",
               "GTI-S2030");
      }
      if (isNumeric(alias.type)) {
        analyzeAliasConversion(expr, alias.type, alias.qualifiedName);
        return;
      }

      std::vector<SemanticType> argumentTypes;
      argumentTypes.reserve(expr.arguments().size());
      for (const ExprPtr &argument : expr.arguments()) {
        argumentTypes.emplace_back(analyze(argument));
      }
      if (alias.type.kind == SemanticType::Class) {
        analyzeConstructorCall(expr, alias.type.classId, alias.type.arguments,
                               alias.type.valueArguments, argumentTypes,
                               expr.arguments(), expr.paren());
      } else {
        report(expressionToken(expr.callee()),
               "Type alias '" + alias.qualifiedName +
                   "' does not name a constructible class or numeric type.",
               "GTI-S2030");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    const bool enclosingCallCallee = analyzingCallCallee;
    analyzingCallCallee = true;
    const SemanticType calleeType = analyze(expr.callee());
    analyzingCallCallee = enclosingCallCallee;

    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (calleeType.kind == SemanticType::Lambda) {
      analyzeLambdaCall(expr, calleeType, argumentTypes);
      return;
    }

    bool hasLambdaArgument = false;
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      if (argumentTypes[index].kind != SemanticType::Lambda) {
        continue;
      }
      hasLambdaArgument = true;
      report(expressionToken(expr.arguments()[index]),
             "Lambda values are lexical and cannot be passed to another "
             "function yet.",
             "GTI-S2027");
    }
    if (hasLambdaArgument) {
      currentType = SemanticType::Unknown;
      return;
    }

    if (calleeType.kind == SemanticType::Class) {
      if (!expr.typeArguments().empty()) {
        for (const TypeRef &argument : expr.typeArguments()) {
          validateType(argument);
        }
        report(expr.paren(),
               "operator() calls do not take explicit type arguments.",
               "GTI-S2022");
      }
      const std::optional<FunctionCandidate> selected = resolveOperator(
          expr, OverloadedOperator::Call, expr.callee(), calleeType,
          expr.paren(), argumentTypes, expr.arguments());
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }

    if (calleeType.kind == SemanticType::TypeName) {
      const ResolvedClassArguments genericArguments =
          resolveClassArguments(calleeType.classId, expr.typeArguments(),
                                expr.paren());
      analyzeConstructorCall(expr, calleeType.classId,
                             genericArguments.types,
                             genericArguments.values, argumentTypes,
                             expr.arguments(), expr.paren());
      return;
    }

    std::vector<SemanticType> explicitTypeArguments;
    explicitTypeArguments.reserve(expr.typeArguments().size());
    for (const TypeRef &argument : expr.typeArguments()) {
      if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
        report(argument.name.last(),
               "Function value generic arguments are not supported yet.",
               "GTI-S2026");
        explicitTypeArguments.emplace_back(SemanticType::Unknown);
        continue;
      }
      validateType(argument);
      const SemanticType argumentType = typeOf(argument);
      if (argumentType == SemanticType::Void) {
        report(argument.name.last(), "Generic type arguments cannot be void.");
      }
      explicitTypeArguments.emplace_back(argumentType);
    }

    if (const auto *member =
            dynamic_cast<const Get *>(expr.callee().get())) {
      const SemanticType *objectType =
          semanticModel.findType(*member->object());
      if (objectType != nullptr && objectType->kind == SemanticType::Expected) {
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "Expected member functions do not take generic arguments.");
        }
        analyzeExpectedMemberCall(*member, *objectType, argumentTypes,
                                  expr.arguments(), expr.paren());
        return;
      }
      if (objectType != nullptr && objectType->kind == SemanticType::Array) {
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "Fixed array member functions do not take generic arguments.");
        }
        analyzeArrayMemberCall(*member, argumentTypes, expr.paren());
        return;
      }
      if (objectType != nullptr &&
          objectType->kind == SemanticType::StringView) {
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "String-view member functions do not take generic arguments.",
                 "GTI-S2035");
        }
        analyzeStringViewMemberCall(*member, argumentTypes, expr.paren());
        return;
      }
    }

    const std::optional<Symbol> callee = resolveExpressionSymbol(expr.callee());

    if (calleeType != SemanticType::Unknown &&
        calleeType != SemanticType::Function) {
      report(expr.paren(), "Can only call functions.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (!callee || callee->type != SemanticType::Function ||
        callee->overloads.empty()) {
      currentType = SemanticType::Unknown;
      return;
    }

    if (hasPackExpansion(argumentTypes) &&
        std::none_of(callee->overloads.begin(), callee->overloads.end(),
                     [](const FunctionCandidate &candidate) {
                       return candidate.parameterPack;
                     })) {
      report(expressionToken(expr.arguments().back()),
             "A parameter pack can only be forwarded to another variadic "
             "function or method.",
             "GTI-S2023");
      currentType = SemanticType::Unknown;
      return;
    }

    if (callee->overloads.size() == 1) {
      const FunctionCandidate &candidate = callee->overloads.front();
      FunctionCandidate resolved = candidate;
      bool valid = applyFunctionTypeArguments(resolved, explicitTypeArguments,
                                              argumentTypes, expr.arguments(),
                                              expr.paren());
      std::vector<SemanticType> resolvedTypeArguments;
      FunctionCandidate trial;
      if (tryInstantiateFunction(candidate, explicitTypeArguments,
                                 argumentTypes, trial, resolvedTypeArguments)) {
        resolved = std::move(trial);
      }

      if (resolved.parameterPack) {
        valid = false;
      } else if (argumentTypes.size() != resolved.parameterTypes.size()) {
        report(
            expr.paren(),
            "Function expects " +
                std::to_string(resolved.parameterTypes.size()) + " argument" +
                (resolved.parameterTypes.size() == 1 ? "" : "s") +
                " but received " + std::to_string(argumentTypes.size()) + ".",
            "GTI-S2005");
        valid = false;
      } else {
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (argumentTypes[index] != SemanticType::Unknown &&
              resolved.parameterTypes[index] != SemanticType::Unknown &&
              !callArgumentMatches(resolved.parameterTypes[index],
                                   argumentTypes[index],
                                   expr.arguments()[index])) {
            reportCallArgumentMismatch(index, resolved.parameterTypes[index],
                                       argumentTypes[index],
                                       expr.arguments()[index], "Function");
            valid = false;
          }
        }
      }

      validateSelectedFunction(candidate, expr.callee(), expr.paren());
      if (valid) {
        recordResolvedCall(expr, resolved, resolvedTypeArguments);
      }
      currentType = callExpressionType(resolved.returnType);
      return;
    }

    struct ViableOverload {
      FunctionCandidate function;
      std::vector<SemanticType> typeArguments;
    };
    std::vector<ViableOverload> viable;
    std::vector<ConstraintFailure> constraintFailures;
    const bool mutableReceiver = callReceiverIsMutable(expr.callee());
    bool rejectedMutableReceiver = false;
    for (const FunctionCandidate &candidate : callee->overloads) {
      if (!acceptsArgumentShape(candidate, argumentTypes)) {
        continue;
      }
      FunctionCandidate resolved;
      std::vector<SemanticType> resolvedTypeArguments;
      ConstraintFailure constraintFailure;
      if (!tryInstantiateFunction(candidate, explicitTypeArguments,
                                  argumentTypes, resolved,
                                  resolvedTypeArguments, &constraintFailure)) {
        if (constraintFailure.constraint != GenericConstraintKind::None) {
          constraintFailures.emplace_back(std::move(constraintFailure));
        }
        continue;
      }
      bool exact = true;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (argumentTypes[index] != SemanticType::Unknown &&
            resolved.parameterTypes[index] != SemanticType::Unknown &&
            !callArgumentMatches(resolved.parameterTypes[index],
                                 argumentTypes[index],
                                 expr.arguments()[index])) {
          exact = false;
          break;
        }
      }
      if (!exact) {
        continue;
      }
      if (resolved.ownerClass != 0 && !resolved.staticMember &&
          resolved.receiverMutability == ReceiverMutability::Mutable &&
          !mutableReceiver) {
        rejectedMutableReceiver = true;
        continue;
      }
      viable.push_back({std::move(resolved), std::move(resolvedTypeArguments)});
    }

    if (mutableReceiver &&
        std::any_of(viable.begin(), viable.end(),
                    [](const ViableOverload &candidate) {
                      return candidate.function.ownerClass != 0 &&
                             !candidate.function.staticMember &&
                             candidate.function.receiverMutability ==
                                 ReceiverMutability::Mutable;
                    })) {
      std::erase_if(viable, [](const ViableOverload &candidate) {
        return candidate.function.ownerClass != 0 &&
               !candidate.function.staticMember &&
               candidate.function.receiverMutability ==
                   ReceiverMutability::ReadOnly;
      });
    }

    const bool hasUnknownArgument = std::any_of(
        argumentTypes.begin(), argumentTypes.end(),
        [](const SemanticType &type) { return type == SemanticType::Unknown; });
    if (viable.size() != 1) {
      if (!hasUnknownArgument) {
        if (viable.empty() && rejectedMutableReceiver) {
          report(expr.paren(), "Mutable method requires a mutable receiver.");
          currentType = SemanticType::Unknown;
          return;
        }
        if (viable.empty() && constraintFailures.size() == 1) {
          reportConstraintFailure(expr.paren(), constraintFailures.front());
          currentType = SemanticType::Unknown;
          return;
        }
        std::vector<const FunctionCandidate *> exactMatches;
        exactMatches.reserve(viable.size());
        for (const ViableOverload &match : viable) {
          exactMatches.emplace_back(&match.function);
        }
        reportOverloadResolutionFailure(expr, *callee, argumentTypes,
                                        exactMatches);
      }
      currentType = SemanticType::Unknown;
      return;
    }

    validateSelectedFunction(viable.front().function, expr.callee(),
                             expr.paren());
    recordResolvedCall(expr, viable.front().function,
                       viable.front().typeArguments);
    currentType = callExpressionType(viable.front().function.returnType);
  }

  void analyzeTypeParameterConversion(const Call &expr,
                                      const SemanticType &targetType) {
    bool valid = true;
    if (!expr.typeArguments().empty()) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "A generic type-parameter conversion does not take type "
             "arguments.",
             "GTI-S2029");
      valid = false;
    }
    if (expr.arguments().size() != 1) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      report(expr.paren(),
             "A numeric conversion requires exactly one value argument.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType valueType = analyze(expr.arguments().front());
    if (!isNumeric(targetType)) {
      report(expressionToken(expr.callee()),
             "Generic conversion target '" + typeSpelling(targetType) +
                 "' must satisfy std::numeric.",
             "GTI-S2029");
      valid = false;
    }
    if (valueType != SemanticType::Unknown && !isNumeric(valueType)) {
      report(expressionToken(expr.arguments().front()),
             "Cannot explicitly convert '" + typeSpelling(valueType) +
                 "' to '" + typeSpelling(targetType) +
                 "'; numeric conversions require a numeric value.",
             "GTI-S2014");
      valid = false;
    }

    currentType = valid ? targetType : SemanticType::Unknown;
    if (valid) {
      semanticModel.record(
          expr,
          ResolvedCallInfo{.returnType = targetType,
                           .parameterTypes = {valueType},
                           .typeArguments = {targetType},
                           .intrinsic =
                               IntrinsicKind::NumericTypeParameterConversion});
    }
  }

  void analyzeAliasConversion(const Call &expr, const SemanticType &targetType,
                              const std::string &aliasName) {
    if (expr.arguments().size() != 1) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      report(expr.paren(),
             "A numeric conversion requires exactly one value argument.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType valueType = analyze(expr.arguments().front());
    if (valueType != SemanticType::Unknown && !isNumeric(valueType)) {
      report(expressionToken(expr.arguments().front()),
             "Cannot explicitly convert '" + typeSpelling(valueType) +
                 "' to '" + aliasName +
                 "'; numeric conversions require a numeric value.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (isInteger(targetType) && isInteger(valueType)) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(expr.arguments().front().get());
          constant && !integerFits(targetType, *constant)) {
        report(expressionToken(expr.arguments().front()),
               "Integer value is outside the range of '" + aliasName + "'.",
               "GTI-S2014");
      }
    }

    currentType = targetType;
    semanticModel.record(
        expr,
        ResolvedCallInfo{.returnType = targetType,
                         .parameterTypes = {valueType},
                         .intrinsic = IntrinsicKind::NumericAliasConversion});
  }

  void visitConversionExpr(const Conversion &expr) override {
    validateType(expr.targetType());
    const SemanticType targetType = typeOf(expr.targetType());
    const SemanticType valueType = analyze(expr.value());
    if (!isNumeric(targetType)) {
      report(expr.targetType().name.last(),
             "Explicit conversions currently require a numeric target type.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (valueType != SemanticType::Unknown && !isNumeric(valueType)) {
      report(expressionToken(expr.value()),
             "Cannot explicitly convert '" + typeSpelling(valueType) +
                 "' to '" + typeSpelling(targetType) +
                 "'; numeric conversions require a numeric value.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (isInteger(targetType) && isInteger(valueType)) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(expr.value().get());
          constant && !integerFits(targetType, *constant)) {
        report(expressionToken(expr.value()),
               "Integer value is outside the range of '" +
                   typeSpelling(targetType) + "'.",
               "GTI-S2014");
      }
    }
    currentType = targetType;
  }

  void visitDirectInitializerExpr(const DirectInitializer &expr) override {
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (!contextualInitializerType) {
      report(expr.brace(),
             "'auto' cannot use direct brace initialization because the "
             "constructed class type is not known; use 'auto name = "
             "Type(arguments)'.",
             "GTI-S2028");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType targetType = *contextualInitializerType;
    if (targetType == SemanticType::Unknown) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (targetType.kind != SemanticType::Class) {
      if (targetType.kind == SemanticType::Reference) {
        report(expr.brace(),
               "Direct brace construction cannot initialize a reference; "
               "bind an existing addressable value with '='.",
               "GTI-S2038");
      } else if (targetType.kind == SemanticType::Array) {
        report(expr.brace(),
               "Direct brace construction is limited to classes and "
               "structs; initialize a fixed array with '= {...}'.",
               "GTI-S2038");
      } else if (targetType.kind == SemanticType::Enum) {
        report(expr.brace(),
               "Scoped enums require an explicit enumerator initializer "
               "with '='.",
               "GTI-S2038");
      } else {
        report(expr.brace(),
               "Direct brace construction is limited to classes and "
               "structs.",
               "GTI-S2038");
      }
      currentType = SemanticType::Unknown;
      return;
    }

    analyzeConstructorCall(expr, targetType.classId, targetType.arguments,
                           targetType.valueArguments, argumentTypes,
                           expr.arguments(), expr.brace());
  }

  void visitDereferenceSetExpr(const DereferenceSet &expr) override {
    const SemanticType ownerType = analyze(expr.object());
    SemanticType valueTarget = SemanticType::Unknown;
    bool mutableTarget = isMutableObject(expr.object());
    if (ownerType.kind == SemanticType::Class) {
      const std::optional<FunctionCandidate> selected =
          resolveOperator(expr, OverloadedOperator::Dereference, expr.object(),
                          ownerType, expr.dereference());
      if (selected) {
        valueTarget = callExpressionType(selected->returnType);
        mutableTarget =
            selected->returnType.kind == SemanticType::Reference &&
            selected->returnType.referenceAccess == AccessMode::Mutable;
      }
    } else if (ownerType != SemanticType::Unknown) {
      report(expr.dereference(),
             "Dereference assignment requires an owning value or a class "
             "defining operator*.",
             "GTI-S2022");
    }

    const SemanticType valueType =
        analyzeInitializer(expr.value(), valueTarget);
    if (!mutableTarget) {
      report(expr.dereference(),
             "Dereference assignment requires mutable access.", "GTI-S2002");
    }
    if (!isAssignable(valueTarget, valueType, expr.value().get())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' through a dereference of type '" +
                 typeSpelling(valueTarget) + "'.",
             "GTI-S2003");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((valueTarget != SemanticType::Unknown && !isNumeric(valueTarget)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = valueTarget;
  }

  void visitGetExpr(const Get &expr) override {
    SemanticType objectType = analyze(expr.object());
    if (expr.access().kind == TokenKind::ARROW) {
      objectType =
          arrowTargetType(expr, expr.object(), objectType, expr.access());
      if (objectType == SemanticType::Unknown) {
        currentType = SemanticType::Unknown;
        return;
      }
    }
    if (expr.name().completion) {
      const ExpressionInfo *receiver =
          semanticModel.findExpression(*expr.object());
      const bool mutableReceiver = receiver != nullptr &&
                                   receiver->category == ValueCategory::Place &&
                                   receiver->access == AccessMode::Mutable;
      captureMemberCompletion(expr.name(), objectType, mutableReceiver);
      currentType = SemanticType::Unknown;
      return;
    }
    if (objectType.kind == SemanticType::Array) {
      if (expr.name().lexeme == "size") {
        if (!analyzingCallCallee) {
          report(expr.name(),
                 "Function names must be called; function values are not "
                 "supported yet.");
        }
        currentType = SemanticType::Function;
      } else {
        report(expr.name(),
               "Unknown fixed array member '" + expr.name().lexeme + "'.",
               "GTI-S2016");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    if (objectType.kind == SemanticType::StringView) {
      if (expr.name().lexeme == "size" || expr.name().lexeme == "empty") {
        if (!analyzingCallCallee) {
          report(expr.name(),
                 "Function names must be called; function values are not "
                 "supported yet.");
        }
        currentType = SemanticType::Function;
      } else {
        report(expr.name(),
               "Unknown std::string_view member '" + expr.name().lexeme + "'.",
               "GTI-S2035");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    if (objectType.kind == SemanticType::Expected) {
      if (expr.name().lexeme == "has_value" || expr.name().lexeme == "value" ||
          expr.name().lexeme == "error" || expr.name().lexeme == "value_or") {
        if (!analyzingCallCallee) {
          report(expr.name(),
                 "Function names must be called; function values are not "
                 "supported yet.");
        }
        currentType = SemanticType::Function;
      } else {
        report(expr.name(), "Unknown expected member '" + expr.name().lexeme +
                                "'.");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    const MemberInfo *member = resolveMember(objectType, expr.name());
    if (member == nullptr) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (member->symbol.staticMember) {
      report(expr.name(),
             "Static member '" + expr.name().lexeme +
                 "' must be accessed through its class or struct name.",
             "GTI-S2039");
      currentType = SemanticType::Unknown;
      return;
    }
    const SemanticType memberType =
        substituteSymbol(member->symbol, objectType).type;
    currentType = memberType.kind == SemanticType::Reference &&
                          memberType.arguments.size() == 1
                      ? memberType.arguments.front()
                      : memberType;
    if (currentType == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name(),
             "Function names must be called; function values are not "
             "supported yet.");
    }
  }

  void visitGroupingExpr(const Grouping &expr) override {
    currentType = analyze(expr.expression());
  }

  void visitIndexExpr(const Index &expr) override {
    const SemanticType objectType = analyze(expr.object());
    const SemanticType indexType = analyze(expr.index());
    if (objectType.kind == SemanticType::StringView) {
      currentType = analyzeStringViewIndexAfterOperands(
          expr.object(), expr.index(), indexType, expr.bracket());
      return;
    }
    if (objectType.kind == SemanticType::Class) {
      const std::optional<FunctionCandidate> selected = resolveOperator(
          expr, OverloadedOperator::Subscript, expr.object(), objectType,
          expr.bracket(), std::span<const SemanticType>(&indexType, 1),
          std::span<const ExprPtr>(&expr.index(), 1));
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }
    currentType = analyzeArrayIndexAfterOperands(objectType, expr.index(),
                                                 indexType, expr.bracket());
  }

  void visitIndexSetExpr(const IndexSet &expr) override {
    const SemanticType objectType = analyze(expr.object());
    const SemanticType indexType = analyze(expr.index());
    if (objectType.kind == SemanticType::StringView) {
      const SemanticType elementType = analyzeStringViewIndexAfterOperands(
          expr.object(), expr.index(), indexType, expr.bracket());
      const SemanticType valueType =
          analyzeInitializer(expr.value(), elementType);
      report(expr.bracket(),
             "Cannot assign through std::string_view; character access is "
             "read-only.",
             "GTI-S2035");
      if (!isAssignable(elementType, valueType, expr.value().get())) {
        report(expressionToken(expr.value()),
               "Cannot assign a value of type '" + typeSpelling(valueType) +
                   "' to a string-view character of type 'char'.",
               "GTI-S2003");
      }
      currentType = elementType;
      return;
    }
    SemanticType elementType = SemanticType::Unknown;
    const ResolvedOperatorInfo *resolvedOperator = nullptr;
    if (objectType.kind == SemanticType::Class) {
      const std::optional<FunctionCandidate> selected = resolveOperator(
          expr, OverloadedOperator::Subscript, expr.object(), objectType,
          expr.bracket(), std::span<const SemanticType>(&indexType, 1),
          std::span<const ExprPtr>(&expr.index(), 1));
      if (selected) {
        elementType = callExpressionType(selected->returnType);
        resolvedOperator = semanticModel.findOperator(expr);
      }
    } else {
      elementType = analyzeArrayIndexAfterOperands(objectType, expr.index(),
                                                   indexType, expr.bracket());
    }
    const SemanticType valueType =
        analyzeInitializer(expr.value(), elementType);
    const bool mutableElement =
        resolvedOperator != nullptr
            ? resolvedOperator->returnType.kind == SemanticType::Reference &&
                  resolvedOperator->returnType.referenceAccess ==
                      AccessMode::Mutable
            : isMutableObject(expr.object());
    if (!mutableElement) {
      if (objectType.kind == SemanticType::Class) {
        report(expr.bracket(),
               "operator[] does not provide mutable element access.",
               "GTI-S2002");
      } else if (!reportReceiverRestrictedArrayField(expr.object())) {
        report(expr.bracket(),
               "Cannot assign through an immutable fixed array binding.",
               "GTI-S2002");
      }
    }
    if (!isAssignable(elementType, valueType, expr.value().get())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to an array element of type '" + typeSpelling(elementType) +
                 "'.",
             "GTI-S2003");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((elementType != SemanticType::Unknown && !isNumeric(elementType)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = elementType;
  }

  void visitLambdaExpr(const Lambda &expr) override {
    const LambdaId id = nextLambdaId++;
    if (expr.returnType().name.last().kind == TokenKind::AUTO) {
      report(expr.returnType().name.last(),
             "Lambda return types must be explicit; 'auto' return deduction "
             "is not supported.",
             "GTI-S2027");
    } else {
      validateType(expr.returnType());
    }
    const SemanticType returnType = typeOf(expr.returnType());
    if (returnType.kind == SemanticType::Reference) {
      report(expr.returnType().name.last(),
             "Lambda reference returns require an escape-aware lifetime model "
             "and are not supported yet.",
             "GTI-S2027");
    }

    std::vector<SemanticType> parameterTypes;
    parameterTypes.reserve(expr.parameters().size());
    for (const Parameter &parameter : expr.parameters()) {
      validateType(parameter.type);
      validateReferencePlacement(parameter.type, true, "lambda parameter");
      const SemanticType parameterType = typeOf(parameter);
      semanticModel.record(
          parameter,
          bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly));
      recordBindingOccurrence(parameter.name, parameterType,
                              parameter.mutability == Mutability::Mutable,
                              SemanticBindingKind::Parameter);
      if (parameterType == SemanticType::Void) {
        report(parameter.type.name.last(),
               "Lambda parameters cannot have type void.", "GTI-S2027");
      }
      if (parameter.pack) {
        report(*parameter.pack,
               "Variadic lambda parameters are not supported yet.",
               "GTI-S2027");
      }
      parameterTypes.push_back(parameterType);
    }

    // Lambda analysis is isolated, so transfer the enclosing stack instead of
    // deep-copying every local symbol before restoring it unchanged.
    ScopeStack enclosingScopes = std::move(scopes);
    const auto findLocal = [&](const Token &name) -> const Symbol * {
      for (auto scope = enclosingScopes.rbegin();
           scope != enclosingScopes.rend(); ++scope) {
        if (const auto found = scope->find(name.lexeme);
            found != scope->end()) {
          return &found->second;
        }
      }
      return nullptr;
    };

    std::unordered_set<std::string> capturedNames;
    std::vector<LambdaCaptureInfo> captures;
    Scope captureScope;
    SemanticTypeTraits lambdaTraits;
    lambdaTraits.drop = DropKind::Trivial;
    for (const LambdaCapture &capture : expr.captures()) {
      if (!capturedNames.insert(capture.name.lexeme).second) {
        report(capture.name,
               "Lambda capture '" + capture.name.lexeme +
                   "' is listed more than once.",
               "GTI-S2027");
        continue;
      }
      const Symbol *source = findLocal(capture.name);
      if (source == nullptr || source->type == SemanticType::Function ||
          source->type.kind == SemanticType::TypeName ||
          source->ownerClass != 0) {
        report(capture.name,
               "Lambda captures must name a local value binding; '" +
                   capture.name.lexeme + "' is not capturable.",
               "GTI-S2027");
        continue;
      }

      const SemanticTypeTraits traits = typeTraits(source->type);
      if (source->type.kind == SemanticType::Reference) {
        report(capture.name,
               "Lambda capture '" + capture.name.lexeme +
                   "' is a reference; reference captures are not supported.",
               "GTI-S2027");
      }
      if (!traits.copyable) {
        report(capture.name,
               "Lambda capture '" + capture.name.lexeme +
                   "' is not copyable; move captures require explicit "
                   "ownership-transfer syntax that is not available yet.",
               "GTI-S2027");
      }
      if (source->valueState != ValueState::Available) {
        report(capture.name,
               "Lambda capture '" + capture.name.lexeme +
                   "' is not available because it has been moved.",
               "GTI-S2018");
      }

      if (traits.ownership == OwnershipKind::Shared) {
        lambdaTraits.ownership = OwnershipKind::Shared;
      }
      if (traits.drop == DropKind::Lexical) {
        lambdaTraits.drop = DropKind::Lexical;
      }
      lambdaTraits.copyable = lambdaTraits.copyable && traits.copyable;
      lambdaTraits.movable = lambdaTraits.movable && traits.movable;
      captures.push_back({.capture = capture.name,
                          .declaration = source->declaration,
                          .type = source->type,
                          .traits = traits});
      recordBindingOccurrence(capture.name, source->type, false,
                              SemanticBindingKind::LambdaCapture);
      captureScope.emplace(
          capture.name.lexeme,
          Symbol{.type = source->type,
                 .sourceUnit = source->sourceUnit,
                 .assignable = false,
                 .declaration = capture.name,
                 .lambdaCapture = true,
                 .bindingKind = SemanticBindingKind::LambdaCapture});
    }

    std::unordered_map<std::string, Token> unavailableLocals;
    for (auto scope = enclosingScopes.rbegin(); scope != enclosingScopes.rend();
         ++scope) {
      for (const auto &[name, symbol] : *scope) {
        if (capturedNames.contains(name) || symbol.ownerClass != 0 ||
            symbol.type == SemanticType::Function ||
            symbol.type.kind == SemanticType::TypeName) {
          continue;
        }
        unavailableLocals.try_emplace(name, symbol.declaration);
      }
    }

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    const bool enclosingReceiverStorageBorrowed = receiverStorageBorrowed;
    const std::size_t enclosingLoopDepth = loopDepth;
    const std::size_t enclosingSwitchDepth = switchDepth;
    const std::size_t enclosingConstructorDepth = constructorDepth;
    const std::size_t enclosingDestructorDepth = destructorDepth;
    const bool enclosingAnalyzingCallCallee = analyzingCallCallee;
    const std::optional<SemanticType> enclosingInitializerType =
        contextualInitializerType;
    scopes.clear();
    scopes.push_back(std::move(captureScope));
    currentReturnType = returnType;
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    receiverStorageBorrowed = false;
    analyzingCallCallee = false;
    contextualInitializerType.reset();
    loopDepth = 0;
    switchDepth = 0;
    constructorDepth = 0;
    destructorDepth = 0;
    ++functionDepth;
    ++lambdaDepth;
    lambdaUncapturedLocals.push_back(std::move(unavailableLocals));

    for (const Parameter &parameter : expr.parameters()) {
      if (!parameter.name.lexeme.empty()) {
        declare(parameter.name, typeOf(parameter),
                parameter.mutability == Mutability::Mutable,
                SemanticBindingKind::Parameter, nullptr, &parameter);
      }
    }
    analyze(expr.body());
    if (returnType != SemanticType::Void &&
        returnType != SemanticType::Unknown &&
        summarizeFlow(expr.body()).canFallThrough) {
      report(expr.arrow(),
             "Non-void lambda can reach the end without returning a value "
             "of type '" +
                 typeSpelling(returnType) + "'.",
             "GTI-S2031");
    }

    lambdaUncapturedLocals.pop_back();
    --lambdaDepth;
    --functionDepth;
    destructorDepth = enclosingDestructorDepth;
    constructorDepth = enclosingConstructorDepth;
    loopDepth = enclosingLoopDepth;
    switchDepth = enclosingSwitchDepth;
    contextualInitializerType = enclosingInitializerType;
    analyzingCallCallee = enclosingAnalyzingCallCallee;
    receiverStorageBorrowed = enclosingReceiverStorageBorrowed;
    currentReceiverMutability = enclosingReceiverMutability;
    currentReturnType = enclosingReturnType;
    scopes = std::move(enclosingScopes);

    semanticModel.record(expr,
                         LambdaInfo{.id = id,
                                    .declaration = &expr,
                                    .returnType = returnType,
                                    .parameterTypes = std::move(parameterTypes),
                                    .captures = std::move(captures),
                                    .traits = lambdaTraits});
    currentType = SemanticType::lambdaType(id);
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    currentType = literalType(expr.value());
  }

  void visitLogicalExpr(const Logical &expr) override {
    const SemanticType leftType = analyze(expr.left());
    const SemanticType rightType = analyze(expr.right());
    requireBool(expr.left(), leftType, expr.oper(),
                "Logical operands must be bool.");
    requireBool(expr.right(), rightType, expr.oper(),
                "Logical operands must be bool.");
    currentType = SemanticType::Bool;
  }

  void visitPackExpansionExpr(const PackExpansion &expr) override {
    const Symbol *symbol = resolve(expr.name());
    if (symbol == nullptr) {
      report(expr.name(),
             "Undefined parameter pack '" + expr.name().lexeme + "'.",
             "GTI-S2001");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->type.kind != SemanticType::TypePack) {
      report(expr.ellipsis(),
             "'" + expr.name().lexeme + "' is not a parameter pack.",
             "GTI-S2023");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->valueState != ValueState::Available) {
      reportUnavailableValue(expr.name(), *symbol);
    } else if (packRequiresMove(symbol->type)) {
      if (Symbol *pack = resolveMutable(expr.name())) {
        pack->valueState = ValueState::Moved;
      }
    }
    currentType = symbol->type;
  }

  void visitPostfixExpr(const Postfix &expr) override {
    const SemanticType type = analyze(
        expr.expression(), OccurrenceRole::Reference | OccurrenceRole::Read |
                               OccurrenceRole::Write);
    if (type != SemanticType::Unknown && !isNumeric(type)) {
      report(expr.oper(), "Increment and decrement require a numeric value.");
    }
    if (!isMutableTarget(expr.expression())) {
      report(expr.oper(), "Increment and decrement require an assignable value.");
    }
    currentType = type;
  }

  void visitQualifiedNameExpr(const QualifiedName &expr) override {
    if (expr.name().last().completion) {
      captureQualifiedCompletion(expr.name());
      currentType = SemanticType::Unknown;
      return;
    }
    recordQualifiedPathUses(expr.name());
    const Symbol *symbol = resolveQualified(expr.name());
    if (symbol == nullptr) {
      if (expr.name().segments.size() >= 2) {
        const NamePath ownerPath(std::vector<Token>(
            expr.name().segments.begin(), expr.name().segments.end() - 1));
        if (const std::optional<ClassId> classId =
                resolveClassPath(ownerPath, currentNamespace)) {
          const ClassInfo &owner = classInfo(*classId);
          const auto member = owner.members.find(expr.name().last().lexeme);
          if (member != owner.members.end() &&
              !member->second.symbol.staticMember) {
            report(expr.name().last(),
                   "Instance member '" + expr.name().last().lexeme +
                       "' requires an object.",
                   "GTI-S2039");
          } else {
            report(expr.name().last(),
                   "Unknown static member '" + expr.name().last().lexeme +
                       "' on '" + owner.name.lexeme + "'.",
                   "GTI-S2039");
          }
          currentType = SemanticType::Unknown;
          return;
        }
        if (resolveEnumPath(ownerPath, currentNamespace)) {
          report(expr.name().last(),
                 "Unknown enumerator '" + expr.name().last().lexeme +
                     "' in scoped enum '" + pathSpelling(ownerPath) + "'.",
                 "GTI-S2036");
          currentType = SemanticType::Unknown;
          return;
        }
      }
      if (!reportInvisibleSymbol(expr.name().last(), pathSpelling(expr.name()),
                                 resolveQualifiedGlobally(expr.name()))) {
        report(expr.name().last(),
               "Undefined qualified name '" + pathSpelling(expr.name()) + "'.");
      }
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->ownerClass != 0 && symbol->type != SemanticType::Function &&
        symbol->access == AccessModifier::Private &&
        currentClass != symbol->ownerClass) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, expr.name().last(),
          "Static member '" + expr.name().last().lexeme + "' of '" +
              classInfo(symbol->ownerClass).name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(symbol->declaration), "Static member declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (const EnumeratorRecord *enumerator = resolveEnumerator(expr.name())) {
      const SemanticType &type = enumerator->symbol.type;
      semanticModel.record(
          expr, ResolvedEnumeratorInfo{.owner = type.enumId,
                                       .declaration = enumerator->declaration,
                                       .value = enumerator->value});
    }
    if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name().last(),
             "Function names must be called; function values are not "
             "supported yet.");
    }
    currentType = symbol->type;
  }

  void visitThisExpr(const This &expr) override {
    if (analyzingConstructorInitializer) {
      report(expr.keyword(),
             "Cannot use 'this' in a constructor initializer expression.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (lambdaDepth > 0) {
      report(expr.keyword(),
             "Lambdas cannot capture 'this' yet; class borrows require an "
             "explicit lifetime design.",
             "GTI-S2027");
      currentType = SemanticType::Unknown;
      return;
    }
    if (!currentClass || functionDepth == 0) {
      report(expr.keyword(),
             "Cannot use 'this' outside a class or struct method.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (currentStaticMemberFunction) {
      report(expr.keyword(), "Static methods do not have a 'this' object.",
             "GTI-S2039");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = openClassType(*currentClass);
  }

  void visitSetExpr(const Set &expr) override {
    SemanticType objectType = analyze(expr.object());
    bool mutableReceiver = isMutableObject(expr.object());
    if (expr.access().kind == TokenKind::ARROW) {
      objectType =
          arrowTargetType(expr, expr.object(), objectType, expr.access());
      if (objectType == SemanticType::Unknown) {
        analyze(expr.value());
        currentType = SemanticType::Unknown;
        return;
      }
      if (const ResolvedOperatorInfo *resolved =
              semanticModel.findOperator(expr)) {
        mutableReceiver =
            resolved->returnType.kind == SemanticType::Reference &&
            resolved->returnType.referenceAccess == AccessMode::Mutable;
      }
    }

    const MemberInfo *member = resolveMember(objectType, expr.name());
    if (member == nullptr) {
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }
    if (member->symbol.staticMember) {
      analyze(expr.value());
      report(expr.name(),
             "Static member '" + expr.name().lexeme +
                 "' must be accessed through its class or struct name.",
             "GTI-S2039");
      currentType = SemanticType::Unknown;
      return;
    }
    const Symbol resolvedMember = substituteSymbol(member->symbol, objectType);
    const SemanticType valueType =
        analyzeInitializer(expr.value(), resolvedMember.type);
    if (resolvedMember.type == SemanticType::Function) {
      report(expr.name(), "Methods are not assignable.");
    } else if (!resolvedMember.assignable) {
      report(expr.name(), "Member is immutable.");
    } else if (!mutableReceiver) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    } else if (const Variable *owner = directStorageVariable(expr.object());
               owner != nullptr && hasRetainedBorrow(*owner)) {
      report(expr.name(),
             "Cannot mutate storage while a retained borrow from it may "
             "still be live.",
             "GTI-S2017");
    } else if (receiverStorageBorrowed &&
               isReceiverDerivedBorrow(expr.object())) {
      report(expr.name(),
             "Cannot mutate 'this' while a reference borrowed from its "
             "move-only storage may still be live.",
             "GTI-S2017");
    }
    if (!isOwnershipAssignment(resolvedMember.type, valueType, expr.value())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to member '" + expr.name().lexeme + "' of type '" +
                 typeSpelling(resolvedMember.type) + "'.",
             "GTI-S2003");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((resolvedMember.type != SemanticType::Unknown &&
          !isNumeric(resolvedMember.type)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = resolvedMember.type;
  }

  void visitUnaryExpr(const Unary &expr) override {
    if (expr.oper().kind == TokenKind::MINUS) {
      if (const auto *literal =
              dynamic_cast<const LiteralExpr *>(expr.right().get());
          literal != nullptr) {
        const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
        if (magnitude != nullptr &&
            *magnitude == (std::uint64_t{1} << 63U)) {
          currentType = SemanticType::Int64;
          return;
        }
      }
    }
    const bool mutating = expr.oper().kind == TokenKind::PLUS_PLUS ||
                          expr.oper().kind == TokenKind::MINUS_MINUS;
    const SemanticType rightType =
        mutating ? analyze(expr.right(), OccurrenceRole::Reference |
                                             OccurrenceRole::Read |
                                             OccurrenceRole::Write)
                 : analyze(expr.right());

    if (expr.oper().kind == TokenKind::PLUS_PLUS &&
        rightType.kind == SemanticType::Class) {
      const std::optional<FunctionCandidate> selected =
          resolveOperator(expr, OverloadedOperator::PreIncrement, expr.right(),
                          rightType, expr.oper());
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }

    if (expr.oper().kind == TokenKind::STAR) {
      if (rightType.kind == SemanticType::Class) {
        const std::optional<FunctionCandidate> selected =
            resolveOperator(expr, OverloadedOperator::Dereference, expr.right(),
                            rightType, expr.oper());
        currentType = selected ? callExpressionType(selected->returnType)
                               : SemanticType::Unknown;
      } else {
        report(expr.oper(), "Dereference requires a class defining operator*.",
               "GTI-S2022");
        currentType = SemanticType::Unknown;
      }
      return;
    }

    if (expr.oper().kind == TokenKind::BANG) {
      requireBool(expr.right(), rightType, expr.oper(),
                  "Logical negation requires bool.");
      currentType = SemanticType::Bool;
      return;
    }

    if (expr.oper().kind == TokenKind::TILDE) {
      if (rightType != SemanticType::Unknown && !isIntegral(rightType)) {
        report(expr.oper(), "Bitwise complement requires an integer operand.");
        currentType = SemanticType::Unknown;
      } else {
        currentType = promotedInteger(rightType);
      }
      return;
    }

    if (rightType != SemanticType::Unknown &&
        (expr.oper().kind == TokenKind::MINUS
             ? !isSignedNumeric(rightType) && !isUnsignedInteger(rightType)
             : !isNumeric(rightType))) {
      report(expr.oper(), expr.oper().kind == TokenKind::MINUS
                              ? "Unary '-' requires a signed numeric value."
                              : "Unary operator requires a numeric value.");
    }
    if ((expr.oper().kind == TokenKind::PLUS_PLUS ||
         expr.oper().kind == TokenKind::MINUS_MINUS) &&
        !isMutableTarget(expr.right())) {
      report(expr.oper(), "Increment and decrement require an assignable value.");
    }
    if ((expr.oper().kind == TokenKind::PLUS ||
         expr.oper().kind == TokenKind::MINUS) &&
        (rightType == SemanticType::Int8 ||
         rightType == SemanticType::Int16 ||
         rightType == SemanticType::UInt8 ||
         rightType == SemanticType::UInt16)) {
      currentType = SemanticType::Int32;
      return;
    }
    if (expr.oper().kind == TokenKind::MINUS && isUnsignedInteger(rightType)) {
      report(expr.oper(),
             "Unary '-' cannot be applied to an unsigned integer.");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = rightType;
  }

  void visitUnexpectedExpr(const Unexpected &expr) override {
    currentType = SemanticType(
        SemanticType::Unexpected, {analyze(expr.error())});
  }

  void visitVariableExpr(const Variable &expr) override {
    if (expr.name().completion) {
      captureUnqualifiedCompletion(expr.name());
      currentType = SemanticType::Unknown;
      return;
    }
    const Symbol *symbol = resolve(expr.name());
    if (symbol == nullptr) {
      if (resolveValueParameter(expr.name())) {
        currentType = SemanticType::UInt64;
        return;
      }
      if (!reportMissingLambdaCapture(expr.name()) &&
          !reportInvisibleSymbol(expr.name(), expr.name().lexeme,
                                 resolveGlobally(expr.name()))) {
        report(expr.name(), "Undefined name '" + expr.name().lexeme + "'.",
               "GTI-S2001");
      }
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name(),
             "Function names must be called; function values are not "
             "supported yet.");
    }
    if (symbol->type.kind == SemanticType::TypePack) {
      report(expr.name(),
             "Parameter pack '" + expr.name().lexeme +
                 "' must be expanded as the final call argument with '...'.",
             "GTI-S2023");
    }
    if (symbol->ownerClass != 0 &&
        (analyzingFieldInitializer || analyzingConstructorInitializer)) {
      report(expr.name(), analyzingConstructorInitializer
                              ? "Class and struct members cannot be referenced "
                                "from constructor initializer expressions."
                              : "Class and struct members cannot be referenced "
                                "from field initializers yet.");
    }
    if (symbol->ownerClass != 0 && symbol->access == AccessModifier::Private &&
        currentClass != symbol->ownerClass) {
      const ClassInfo &declaringOwner = classInfo(symbol->ownerClass);
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, expr.name(),
                         "Member '" + expr.name().lexeme + "' of '" +
                             declaringOwner.name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(symbol->declaration), "Member declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (currentStaticMemberFunction && symbol->ownerClass != 0 &&
        !symbol->staticMember) {
      report(expr.name(),
             "Static methods cannot access instance member '" +
                 expr.name().lexeme + "' without an object.",
             "GTI-S2039");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->valueState != ValueState::Available) {
      reportUnavailableValue(expr.name(), *symbol);
    }
    currentType = symbol->type.kind == SemanticType::Reference &&
                          symbol->type.arguments.size() == 1
                      ? symbol->type.arguments[0]
                      : symbol->type;
  }

private:
  enum class ValueState {
    Available,
    Moved,
    MaybeMoved,
  };

  struct FunctionCandidate {
    FunctionId id = 0;
    SourceUnitId sourceUnit = 0;
    const FunctionDecl *declaration = nullptr;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::vector<GenericParameterInfo> genericParameters;
    bool parameterPack = false;
    ClassId ownerClass = 0;
    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
    AccessModifier access = AccessModifier::Public;
    bool staticMember = false;
    bool internalLinkage = false;
    bool virtualMethod = false;
    bool pureVirtual = false;
    bool overrideMethod = false;
    std::vector<FunctionId> virtualRoots;
    SemanticType dispatchOwner = SemanticType::Unknown;
  };

  struct AnalyzedCallArgument {
    SemanticType type = SemanticType::Unknown;
    const ExprPtr *expression = nullptr;
    bool forwardedPackElement = false;
  };

  struct ConstraintFailure {
    Token parameter;
    SemanticType argument = SemanticType::Unknown;
    GenericConstraintKind constraint = GenericConstraintKind::None;
    std::optional<NamePath> constraintName;
  };

  struct Symbol {
    SemanticType type = SemanticType::Unknown;
    SourceUnitId sourceUnit = 0;
    bool assignable = false;
    ValueState valueState = ValueState::Available;
    std::vector<FunctionCandidate> overloads;
    ClassId ownerClass = 0;
    Token declaration;
    const VariableDecl *variableDeclaration = nullptr;
    const Parameter *parameterDeclaration = nullptr;
    bool borrowedStorage = false;
    bool lambdaCapture = false;
    SemanticBindingKind bindingKind = SemanticBindingKind::None;
    std::string qualifiedName;
    bool staticMember = false;
    bool internalLinkage = false;
    SymbolId toolingSymbol = 0;
    AccessModifier access = AccessModifier::Public;
  };

  struct ResolvedFieldUse {
    const Token *use = nullptr;
    const Symbol *symbol = nullptr;
  };

  using Scope = std::unordered_map<std::string, Symbol>;
  using ScopeStack = std::vector<Scope>;

  struct MemberInfo {
    Symbol symbol;
    AccessModifier access = AccessModifier::Private;
  };

  struct FieldInfo {
    const VariableDecl *declaration = nullptr;
  };

  struct ClassInfo {
    ClassId id = 0;
    SourceUnitId sourceUnit = 0;
    const ClassDecl *declaration = nullptr;
    Token name;
    ClassKind kind = ClassKind::Class;
    std::vector<std::string> namespaceScope;
    std::vector<GenericParameterInfo> genericParameters;
    std::unordered_map<std::string, MemberInfo> members;
    std::vector<FieldInfo> fields;
    std::vector<FieldInfo> staticFields;
    std::vector<ConstructorInfo> constructors;
    std::optional<ConstructorInfo> copyConstructor;
    std::optional<ConstructorInfo> moveConstructor;
    std::optional<DestructorInfo> destructor;
    std::optional<StoredReferenceInfo> storedReference;
    std::vector<ClassBaseTypeInfo> bases;
    bool abstract = false;
    bool polymorphic = false;
  };

  struct EnumeratorRecord {
    const EnumeratorDecl *declaration = nullptr;
    EnumConstant value;
    Symbol symbol;
  };

  struct EnumInfo {
    EnumId id = 0;
    SourceUnitId sourceUnit = 0;
    const EnumDecl *declaration = nullptr;
    Token name;
    std::vector<std::string> namespaceScope;
    SemanticType underlyingType = SemanticType::Int32;
    std::unordered_map<std::string, EnumeratorRecord> enumerators;
  };

  struct NamespaceAliasInfo {
    std::string target;
    SourceUnitId sourceUnit = 0;
  };

  struct ResolvedNamespaceSegment {
    std::string declaration;
    std::string target;
  };

  enum class TypeAliasResolution {
    Unresolved,
    Resolving,
    Resolved,
    Invalid,
  };

  struct RegisteredTypeAlias {
    SourceUnitId sourceUnit = 0;
    const TypeAliasDecl *declaration = nullptr;
    std::string qualifiedName;
    std::vector<std::string> namespaceScope;
    SemanticType type = SemanticType::Unknown;
    TypeAliasResolution resolution = TypeAliasResolution::Unresolved;
  };

  struct FlowSummary {
    bool canFallThrough = true;
    bool breaksEnclosingControl = false;
  };

  [[nodiscard]] static std::optional<bool>
  constantBoolean(const ExprPtr &expression) {
    if (const auto *literal =
            dynamic_cast<const LiteralExpr *>(expression.get())) {
      if (const bool *value = std::get_if<bool>(&literal->value())) {
        return *value;
      }
      return std::nullopt;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return constantBoolean(grouping->expression());
    }
    return std::nullopt;
  }

  [[nodiscard]] FlowSummary summarizeFlow(const StmtList &statements) const {
    FlowSummary result;
    for (const StmtPtr &statement : statements) {
      if (!result.canFallThrough) {
        break;
      }
      const FlowSummary next = summarizeFlow(statement.get());
      result.canFallThrough = next.canFallThrough;
      result.breaksEnclosingControl =
          result.breaksEnclosingControl || next.breaksEnclosingControl;
    }
    return result;
  }

  [[nodiscard]] FlowSummary summarizeFlow(const Stmt *statement) const {
    if (statement == nullptr) {
      return {};
    }
    if (dynamic_cast<const ReturnStmt *>(statement) != nullptr) {
      return {.canFallThrough = false};
    }
    if (const auto *control =
            dynamic_cast<const LoopControlStmt *>(statement)) {
      return {.canFallThrough = false,
              .breaksEnclosingControl =
                  control->keyword().kind == TokenKind::BREAK};
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(statement)) {
      return summarizeFlow(block->statements());
    }
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(statement)) {
      const StmtList *branch = conditional->activeBranch(target);
      return branch == nullptr ? FlowSummary{} : summarizeFlow(*branch);
    }
    if (const auto *ifStatement = dynamic_cast<const IfStmt *>(statement)) {
      if (const std::optional<bool> condition =
              constantBoolean(ifStatement->condition())) {
        return *condition ? summarizeFlow(ifStatement->thenBranch().get())
                          : summarizeFlow(ifStatement->elseBranch().get());
      }
      const FlowSummary thenFlow =
          summarizeFlow(ifStatement->thenBranch().get());
      const FlowSummary elseFlow =
          summarizeFlow(ifStatement->elseBranch().get());
      return {.canFallThrough =
                  thenFlow.canFallThrough || elseFlow.canFallThrough,
              .breaksEnclosingControl = thenFlow.breaksEnclosingControl ||
                                        elseFlow.breaksEnclosingControl};
    }
    if (const auto *switchStatement =
            dynamic_cast<const SwitchStmt *>(statement)) {
      bool hasDefault = false;
      bool reachesAfterSwitch = false;
      for (const SwitchArm &arm : switchStatement->arms()) {
        for (const SwitchLabel &label : arm.labels) {
          hasDefault = hasDefault || label.isDefault();
        }
        const FlowSummary armFlow = summarizeFlow(arm.statements);
        reachesAfterSwitch = reachesAfterSwitch || armFlow.canFallThrough ||
                             armFlow.breaksEnclosingControl;
      }
      return {.canFallThrough = !hasDefault || reachesAfterSwitch};
    }
    if (const auto *forStatement = dynamic_cast<const ForStmt *>(statement)) {
      const FlowSummary body = summarizeFlow(forStatement->body().get());
      const bool repeatsForever =
          !forStatement->condition() ||
          constantBoolean(forStatement->condition()) == true;
      return {.canFallThrough = !repeatsForever || body.breaksEnclosingControl};
    }
    if (const auto *rangeFor = dynamic_cast<const RangeForStmt *>(statement)) {
      return summarizeFlow(rangeFor->lowered().get());
    }
    if (const auto *whileStatement =
            dynamic_cast<const WhileStmt *>(statement)) {
      const FlowSummary body = summarizeFlow(whileStatement->body().get());
      return {.canFallThrough =
                  constantBoolean(whileStatement->condition()) != true ||
                  body.breaksEnclosingControl};
    }
    return {};
  }

  void analyzeInferredVariable(const VariableDecl &declaration) {
    const TypeRef &type = declaration.type();
    const bool local = functionDepth > 0;
    if (!local) {
      report(type.name.last(), "'auto' inference is limited to local bindings.",
             "GTI-S2028");
    }
    if (!type.arguments.empty() || !type.arrayExtents.empty()) {
      report(type.name.last(),
             "'auto' cannot have generic arguments or array extents.",
             "GTI-S2028");
    }
    if (type.reference && !declaration.isRangeBinding() &&
        !type.name.last().generated) {
      report(type.name.last(),
             "'auto&' inference is limited to range-for element bindings.",
             "GTI-S2028");
    }
    if (!declaration.initializer()) {
      report(declaration.name(), "An 'auto' binding requires an initializer.",
             "GTI-S2028");
    }

    const SemanticType initializerType =
        declaration.initializer() ? analyze(declaration.initializer())
                                  : SemanticType::Unknown;
    SemanticType inferredType = initializerType;
    if (initializerType == SemanticType::Void ||
        initializerType == SemanticType::Function ||
        initializerType.kind == SemanticType::TypeName ||
        initializerType.kind == SemanticType::Unexpected) {
      report(declaration.name(),
             "'auto' requires an initializer with a complete value type.",
             "GTI-S2028");
      inferredType = SemanticType::Unknown;
    }
    if (!type.reference && inferredType.kind == SemanticType::Reference) {
      report(declaration.name(),
             "'auto' does not infer references; declare the reference type "
             "explicitly.",
             "GTI-S2028");
      inferredType = SemanticType::Unknown;
    }
    if (type.reference && inferredType != SemanticType::Unknown) {
      inferredType = SemanticType::referenceTo(
          initializerType,
          declaration.isMutable() ? AccessMode::Mutable : AccessMode::ReadOnly);
      if (declaration.initializer()) {
        validateReferenceBinding(inferredType, initializerType,
                                 declaration.initializer());
        if (!isDirectOwnerType(initializerType)) {
          recordRetainedBorrow(declaration.initializer());
        }
      }
    }
    if (inferredType.kind == SemanticType::Lambda && declaration.isMutable()) {
      report(declaration.name(),
             "Lambda bindings are immutable; captured snapshots cannot be "
             "made mutable with 'mut auto'.",
             "GTI-S2027");
    }
    if (!type.reference && declaration.initializer() &&
        !isOwnershipAssignable(inferredType, initializerType,
                               declaration.initializer())) {
      report(expressionToken(declaration.initializer()),
             "Cannot initialize inferred binding '" +
                 declaration.name().lexeme + "' by copying move-only type '" +
                 typeSpelling(inferredType) + "'.",
             "GTI-S2003");
      diagnostics.back().hints.emplace_back(
          "Move-only owners cannot be copied; transfer ownership explicitly "
          "with std::move(owner).");
    }
    if (!type.reference && declaration.initializer() &&
        typeTraits(inferredType).containsBorrowedState) {
      validateStoredBorrowInitialization(inferredType,
                                         declaration.initializer());
      recordRetainedBorrow(declaration.initializer());
    }

    const AccessMode access =
        declaration.isMutable() ? AccessMode::Mutable : AccessMode::ReadOnly;
    semanticModel.record(declaration, bindingInfo(inferredType, access));
    recordBindingOccurrence(declaration.name(), inferredType,
                            declaration.isMutable(),
                            local ? SemanticBindingKind::LocalVariable
                                  : SemanticBindingKind::GlobalVariable);
    if (!type.name.last().generated) {
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(type.name.last()),
           .kind = SemanticOccurrenceKind::InferredType,
           .name = type.name.last().lexeme,
           .type = inferredType,
           .traits = typeTraits(inferredType),
           .access = access,
           .mutableBinding = declaration.isMutable()});
    }
    if (!predeclaredVariables.contains(&declaration)) {
      if (local) {
        declare(declaration.name(), inferredType,
                declaration.isMutable() &&
                    inferredType.kind != SemanticType::Lambda,
                SemanticBindingKind::LocalVariable, &declaration);
      } else {
        declareNamespaceSymbol(currentNamespace, declaration.name(),
                               inferredType, declaration.isMutable());
      }
    }
  }

  bool reportMissingLambdaCapture(const Token &use) {
    if (lambdaUncapturedLocals.empty()) {
      return false;
    }
    const auto found = lambdaUncapturedLocals.back().find(use.lexeme);
    if (found == lambdaUncapturedLocals.back().end()) {
      return false;
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2027", DiagnosticPhase::Semantics, use,
        "Local binding '" + use.lexeme + "' is not captured by this lambda.");
    if (!found->second.lexeme.empty()) {
      diagnostic.related.push_back(
          {tokenSpan(found->second), "Local binding declared here."});
    }
    diagnostic.hints.emplace_back("Add '" + use.lexeme +
                                  "' to the lambda capture list.");
    diagnostics.push_back(std::move(diagnostic));
    return true;
  }

  [[nodiscard]] SemanticTypeTraits typeTraits(const SemanticType &type) const {
    std::unordered_set<ClassId> visiting;
    return typeTraits(type, visiting);
  }

  [[nodiscard]] SemanticTypeTraits
  typeTraits(const SemanticType &type,
             std::unordered_set<ClassId> &visiting) const {
    if (type.kind == SemanticType::Array && type.arguments.size() == 1) {
      SemanticTypeTraits traits;
      const SemanticTypeTraits element =
          typeTraits(type.arguments[0], visiting);
      traits.ownership = element.ownership;
      traits.drop = element.drop;
      traits.copyable = element.copyable;
      traits.movable = element.movable;
      traits.copyAssignable = element.copyAssignable;
      traits.moveAssignable = element.moveAssignable;
      traits.containsBorrowedState = element.containsBorrowedState;
      return traits;
    }

    if (type.kind == SemanticType::Lambda) {
      const LambdaInfo *lambda = semanticModel.findLambda(type.lambdaId);
      return lambda == nullptr ? semanticTraits(type) : lambda->traits;
    }

    if (type.kind == SemanticType::Expected ||
        type.kind == SemanticType::Unexpected) {
      SemanticTypeTraits traits = semanticTraits(type);
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        const SemanticType &argument = type.arguments[index];
        if (type.kind == SemanticType::Expected && index == 0 &&
            argument == SemanticType::Void) {
          continue;
        }
        const SemanticTypeTraits argumentTraits =
            typeTraits(argument, visiting);
        if (argumentTraits.ownership == OwnershipKind::Unique ||
            (traits.ownership == OwnershipKind::Value &&
             argumentTraits.ownership == OwnershipKind::Shared)) {
          traits.ownership = argumentTraits.ownership;
        }
        traits.copyable = traits.copyable && argumentTraits.copyable;
        traits.movable = traits.movable && argumentTraits.movable;
        traits.copyAssignable =
            traits.copyAssignable && argumentTraits.copyAssignable;
        traits.moveAssignable =
            traits.moveAssignable && argumentTraits.moveAssignable;
        traits.containsBorrowedState = traits.containsBorrowedState ||
                                       argumentTraits.containsBorrowedState;
      }
      return traits;
    }

    if (type.kind != SemanticType::Class || type.classId == 0 ||
        type.classId > classes.size()) {
      return semanticTraits(type);
    }

    SemanticTypeTraits traits;
    traits.drop = DropKind::Lexical;
    if (!visiting.insert(type.classId).second) {
      traits.copyable = false;
      traits.movable = false;
      traits.copyAssignable = false;
      traits.moveAssignable = false;
      return traits;
    }

    const ClassInfo &owner = classInfo(type.classId);
    const GenericSubstitution substitution = classSubstitution(type);
    const auto mergeTraits = [&](const SemanticTypeTraits &component) {
      if (component.ownership == OwnershipKind::Unique ||
          (traits.ownership == OwnershipKind::Value &&
           component.ownership == OwnershipKind::Shared)) {
        traits.ownership = component.ownership;
      } else if (traits.ownership == OwnershipKind::Value &&
                 component.ownership == OwnershipKind::Borrowed) {
        traits.ownership = OwnershipKind::Borrowed;
      }
      if (component.drop == DropKind::Lexical) {
        traits.drop = DropKind::Lexical;
      }
      traits.copyable = traits.copyable && component.copyable;
      traits.movable = traits.movable && component.movable;
      traits.copyAssignable = traits.copyAssignable && component.copyAssignable;
      traits.moveAssignable = traits.moveAssignable && component.moveAssignable;
      traits.containsBorrowedState =
          traits.containsBorrowedState || component.containsBorrowedState;
    };
    if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
      mergeTraits(
          typeTraits(substituteType(base->type, substitution), visiting));
    }
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member == owner.members.end()) {
        continue;
      }
      const SemanticType fieldType =
          substituteType(member->second.symbol.type, substitution);
      mergeTraits(typeTraits(fieldType, visiting));
    }
    if (owner.destructor) {
      traits.copyable = false;
      traits.copyAssignable = false;
    }
    if (owner.storedReference) {
      traits.ownership = traits.ownership == OwnershipKind::Value
                             ? OwnershipKind::Borrowed
                             : traits.ownership;
      traits.copyable = false;
      traits.copyAssignable = false;
      traits.moveAssignable = false;
      traits.containsBorrowedState = true;
    }
    if (owner.copyConstructor &&
        owner.copyConstructor->declaration != nullptr) {
      const auto &specifier = owner.copyConstructor->declaration->specifier();
      if (specifier && specifier->kind == SpecialMemberSpecifierKind::Deleted) {
        traits.copyable = false;
      }
    }
    if (owner.moveConstructor &&
        owner.moveConstructor->declaration != nullptr) {
      const auto &specifier = owner.moveConstructor->declaration->specifier();
      if (specifier && specifier->kind == SpecialMemberSpecifierKind::Deleted) {
        traits.movable = false;
      }
    }
    visiting.erase(type.classId);
    return traits;
  }

  [[nodiscard]] ExpressionInfo
  expressionInfo(SemanticType type,
                 ValueCategory category = ValueCategory::Value,
                 AccessMode access = AccessMode::ReadOnly) const {
    const SemanticTypeTraits traits = typeTraits(type);
    return ExpressionInfo{.type = std::move(type),
                          .category = category,
                          .access = access,
                          .traits = traits};
  }

  [[nodiscard]] BindingInfo
  bindingInfo(SemanticType type,
              AccessMode access = AccessMode::ReadOnly) const {
    const SemanticTypeTraits traits = typeTraits(type);
    return BindingInfo{
        .type = std::move(type), .access = access, .traits = traits};
  }

  [[nodiscard]] bool isMoveOnlyOwnerType(const SemanticType &type) const {
    const SemanticTypeTraits traits = typeTraits(type);
    return !traits.copyable && traits.movable;
  }

  [[nodiscard]] bool
  isDirectStoredReferenceType(const SemanticType &type) const {
    const ClassInfo *owner = classInfo(type);
    return owner != nullptr && owner->storedReference.has_value();
  }

  [[nodiscard]] AccessMode borrowAccess(const SemanticType &type) const {
    if (type.kind == SemanticType::Reference) {
      return type.referenceAccess;
    }
    const ClassInfo *owner = classInfo(type);
    return owner != nullptr && owner->storedReference
               ? owner->storedReference->access
               : AccessMode::ReadOnly;
  }

  [[nodiscard]] bool nestsBorrowedState(const SemanticType &type) const {
    if (type.kind == SemanticType::Reference) {
      return !type.arguments.empty() &&
             typeTraits(type.arguments.front()).containsBorrowedState;
    }
    return typeTraits(type).containsBorrowedState &&
           !isDirectStoredReferenceType(type);
  }

  [[nodiscard]] static bool isDirectOwnerType(const SemanticType &type) {
    return type.kind == SemanticType::UniqueOwner ||
           type.kind == SemanticType::Storage;
  }

  [[nodiscard]] static IntrinsicKind intrinsicKind(const ExprPtr &callee) {
    const auto *qualified = dynamic_cast<const QualifiedName *>(callee.get());
    if (qualified == nullptr || qualified->name().segments.size() != 2) {
      return IntrinsicKind::None;
    }
    const std::string &owner = qualified->name().segments[0].lexeme;
    const std::string &name = qualified->name().segments[1].lexeme;
    if (owner == "std") {
      if (name == "move") {
        return IntrinsicKind::Move;
      }
      return IntrinsicKind::None;
    }
    if (owner == "gti_internal") {
      if (name == "allocate_unique_owner") {
        return IntrinsicKind::AllocateUniqueOwner;
      }
      if (name == "unique_owner_borrow") {
        return IntrinsicKind::UniqueOwnerBorrow;
      }
      if (name == "unique_owner_borrow_mut") {
        return IntrinsicKind::UniqueOwnerBorrowMut;
      }
      if (name == "unique_owner_is_null") {
        return IntrinsicKind::UniqueOwnerIsNull;
      }
      if (name == "allocate_storage") {
        return IntrinsicKind::AllocateStorage;
      }
      if (name == "storage_construct") {
        return IntrinsicKind::StorageConstruct;
      }
      if (name == "storage_read") {
        return IntrinsicKind::StorageRead;
      }
      if (name == "storage_read_mut") {
        return IntrinsicKind::StorageReadMut;
      }
      if (name == "storage_destroy") {
        return IntrinsicKind::StorageDestroy;
      }
      if (name == "storage_relocate") {
        return IntrinsicKind::StorageRelocate;
      }
    }
    return IntrinsicKind::None;
  }

  [[nodiscard]] static const Variable *
  movedVariable(const ExprPtr &expression) {
    const Expr *candidate = expression.get();
    while (const auto *grouping = dynamic_cast<const Grouping *>(candidate)) {
      candidate = grouping->expression().get();
    }
    return dynamic_cast<const Variable *>(candidate);
  }

  [[nodiscard]] const ExprPtr *
  storedBorrowSource(const ExprPtr &expression) const {
    if (!expression) {
      return nullptr;
    }
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol != nullptr &&
          (symbol->type.kind == SemanticType::Reference ||
           typeTraits(symbol->type).containsBorrowedState) &&
          symbol->variableDeclaration != nullptr &&
          symbol->variableDeclaration->initializer()) {
        return &symbol->variableDeclaration->initializer();
      }
      return nullptr;
    }
    const ResolvedConstructionInfo *construction =
        semanticModel.findConstruction(*expression);
    if (construction != nullptr &&
        construction->borrowOrigin == BorrowOriginKind::Argument) {
      if (const auto *call = dynamic_cast<const Call *>(expression.get())) {
        return construction->borrowArgument < call->arguments().size()
                   ? &call->arguments()[construction->borrowArgument]
                   : nullptr;
      }
      if (const auto *initializer =
              dynamic_cast<const DirectInitializer *>(expression.get())) {
        return construction->borrowArgument < initializer->arguments().size()
                   ? &initializer->arguments()[construction->borrowArgument]
                   : nullptr;
      }
    }
    const auto *call = dynamic_cast<const Call *>(expression.get());
    if (call == nullptr) {
      return nullptr;
    }
    if (const ResolvedOperatorInfo *resolved =
            semanticModel.findOperator(*call);
        resolved != nullptr && resolved->kind == OverloadedOperator::Call &&
        (resolved->returnType.kind == SemanticType::Reference ||
         typeTraits(resolved->returnType).containsBorrowedState)) {
      return &call->callee();
    }
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved == nullptr) {
      return nullptr;
    }
    if (resolved->borrowOrigin == BorrowOriginKind::Argument) {
      return resolved->borrowArgument < call->arguments().size()
                 ? &call->arguments()[resolved->borrowArgument]
                 : nullptr;
    }
    if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
      if (const auto *member =
              dynamic_cast<const Get *>(call->callee().get())) {
        return &member->object();
      }
    }
    return nullptr;
  }

  [[nodiscard]] const Variable *
  borrowedOwnerVariable(const ExprPtr &expression) const {
    std::unordered_set<const Expr *> visiting;
    return borrowedOwnerVariable(expression, visiting);
  }

  [[nodiscard]] const Variable *
  borrowedOwnerVariable(const ExprPtr &expression,
                        std::unordered_set<const Expr *> &visiting) const {
    if (!expression || !visiting.insert(expression.get()).second) {
      return nullptr;
    }
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      if (const ExprPtr *source = storedBorrowSource(expression)) {
        if (const Variable *owner = borrowedOwnerVariable(*source, visiting)) {
          return owner;
        }
      }
      return variable;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return borrowedOwnerVariable(grouping->expression(), visiting);
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expression.get());
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return borrowedOwnerVariable(binary->right(), visiting);
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return borrowedOwnerVariable(index->object(), visiting);
    }
    if (const auto *member = dynamic_cast<const Get *>(expression.get())) {
      return borrowedOwnerVariable(member->object(), visiting);
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expression.get());
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      return borrowedOwnerVariable(unary->right(), visiting);
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return borrowedOwnerVariable(*source, visiting);
    }
    return nullptr;
  }

  [[nodiscard]] const Variable *
  directStorageVariable(const ExprPtr &expression) const {
    if (!expression) {
      return nullptr;
    }
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      return variable;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return directStorageVariable(grouping->expression());
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expression.get());
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return directStorageVariable(binary->right());
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return directStorageVariable(index->object());
    }
    if (const auto *member = dynamic_cast<const Get *>(expression.get())) {
      return directStorageVariable(member->object());
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expression.get());
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      return directStorageVariable(unary->right());
    }
    return nullptr;
  }

  [[nodiscard]] bool hasRetainedBorrow(const Variable &variable) const {
    const Symbol *symbol = resolve(variable.name());
    return symbol != nullptr && symbol->borrowedStorage;
  }

  void recordRetainedBorrow(const ExprPtr &expression) {
    if (!hasStableBorrowStorage(expression)) {
      return;
    }
    const ExpressionInfo *info =
        expression ? semanticModel.findExpression(*expression) : nullptr;
    const bool retainedStoredBorrow =
        info != nullptr && info->traits.containsBorrowedState;
    const Variable *owner = borrowedOwnerVariable(expression);
    if (owner == nullptr) {
      if (currentClass && isReceiverDerivedBorrow(expression) &&
          (retainedStoredBorrow ||
           isMoveOnlyOwnerType(openClassType(*currentClass)))) {
        receiverStorageBorrowed = true;
      }
      return;
    }
    Symbol *symbol = resolveMutable(owner->name());
    if (symbol != nullptr &&
        (retainedStoredBorrow || isMoveOnlyOwnerType(symbol->type))) {
      symbol->borrowedStorage = true;
    }
  }

  void analyzeIntrinsicCall(const Call &expr, IntrinsicKind intrinsic) {
    if (intrinsic == IntrinsicKind::AllocateUniqueOwner ||
        intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
        intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
        intrinsic == IntrinsicKind::UniqueOwnerIsNull) {
      analyzeUniqueOwnerIntrinsicCall(expr, intrinsic);
      return;
    }
    if (intrinsic != IntrinsicKind::Move) {
      analyzeStorageIntrinsicCall(expr, intrinsic);
      return;
    }

    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty()) {
      report(expr.paren(), "std::move does not take type arguments.",
             "GTI-S2018");
    }
    if (expr.arguments().size() != 1) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      report(expr.paren(), "std::move expects exactly one value.", "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }

    const ExprPtr &argument = expr.arguments().front();
    const SemanticType valueType = analyze(argument);
    const Variable *variable = movedVariable(argument);
    if (variable == nullptr) {
      const std::optional<Symbol> symbol = resolveExpressionSymbol(argument);
      if (symbol &&
          symbol->bindingKind == SemanticBindingKind::GlobalVariable) {
        report(expressionToken(argument),
               "std::move cannot consume a global binding because its move "
               "state is not locally provable.",
               "GTI-S2018");
      } else if (const ExpressionInfo *info =
                     semanticModel.findExpression(*argument);
                 info != nullptr && info->category == ValueCategory::Place) {
        report(expressionToken(argument),
               "std::move cannot partially move a field, indexed element, or "
               "borrowed place until place-aware initialization tracking is "
               "available.",
               "GTI-S2018");
      } else {
        report(expressionToken(argument),
               "std::move requires a named local value or by-value parameter; "
               "temporaries are already values.",
               "GTI-S2018");
      }
      currentType = SemanticType::Unknown;
      return;
    }
    const Symbol *symbol = resolve(variable->name());
    if (symbol == nullptr) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->type.kind == SemanticType::Reference) {
      report(variable->name(),
             "std::move cannot consume a reference; move the owning value "
             "instead.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (!typeTraits(symbol->type).movable) {
      report(variable->name(),
             "std::move requires a movable value, but type '" +
                 typeSpelling(symbol->type) + "' is not movable.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->bindingKind == SemanticBindingKind::Field) {
      report(variable->name(),
             "std::move cannot partially move field '" +
                 variable->name().lexeme +
                 "' until place-aware initialization tracking is available.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->bindingKind == SemanticBindingKind::GlobalVariable) {
      report(variable->name(),
             "std::move cannot consume global binding '" +
                 variable->name().lexeme +
                 "' because its move state is not locally provable.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->bindingKind == SemanticBindingKind::LambdaCapture) {
      report(variable->name(),
             "std::move cannot consume immutable lambda capture '" +
                 variable->name().lexeme + "'.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->bindingKind != SemanticBindingKind::LocalVariable &&
        symbol->bindingKind != SemanticBindingKind::Parameter) {
      report(variable->name(),
             "std::move requires a named local value or by-value parameter.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->valueState != ValueState::Available) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->borrowedStorage) {
      report(variable->name(),
             "Cannot move storage while a reference borrowed from it may "
             "still be live.",
             "GTI-S2017");
      currentType = SemanticType::Unknown;
      return;
    }
    Symbol *mutableSymbol = resolveMutable(variable->name());
    if (mutableSymbol == nullptr) {
      currentType = SemanticType::Unknown;
      return;
    }
    mutableSymbol->valueState = ValueState::Moved;
    if (mutableSymbol->variableDeclaration != nullptr) {
      semanticModel.recordExplicitMove(*mutableSymbol->variableDeclaration);
    } else if (mutableSymbol->parameterDeclaration != nullptr) {
      semanticModel.recordExplicitMove(*mutableSymbol->parameterDeclaration);
    }
    currentType = valueType;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = {valueType},
                               .intrinsic = IntrinsicKind::Move,
                               .borrowOrigin =
                                   typeTraits(currentType).containsBorrowedState
                                       ? BorrowOriginKind::Argument
                                       : BorrowOriginKind::None,
                               .borrowArgument = 0,
                               .borrowAccess = borrowAccess(currentType)});
  }

  void analyzeUniqueOwnerIntrinsicCall(const Call &expr,
                                       IntrinsicKind intrinsic) {
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (intrinsic == IntrinsicKind::AllocateUniqueOwner) {
      if (expr.typeArguments().size() != 1) {
        for (const TypeRef &argument : expr.typeArguments()) {
          validateType(argument);
        }
        report(expr.paren(),
               "gti_internal::allocate_unique_owner<T> requires exactly one "
               "pointee type.",
               "GTI-S2018");
        currentType = SemanticType::Unknown;
        return;
      }

      const TypeRef &pointeeRef = expr.typeArguments().front();
      validateType(pointeeRef);
      validateReferencePlacement(pointeeRef, false, "allocated type");
      const SemanticType pointeeType = typeOf(pointeeRef);
      if (pointeeType.kind != SemanticType::Class &&
          pointeeType.kind != SemanticType::TypeParameter) {
        report(pointeeRef.name.last(),
               "Compiler-private unique allocation currently requires a "
               "class, struct, or generic object type.",
               "GTI-S2018");
      } else if (pointeeType.kind == SemanticType::Class) {
        if (const std::optional<std::vector<AnalyzedCallArgument>> arguments =
                concreteCallArguments(argumentTypes, expr.arguments())) {
          analyzeConstructorCallArguments(
              expr, pointeeType.classId, pointeeType.arguments,
              pointeeType.valueArguments, *arguments, expr.paren());
        }
      }

      currentType = SemanticType::uniqueOwnerOf(pointeeType);
      semanticModel.record(
          expr,
          ResolvedCallInfo{.returnType = currentType,
                           .parameterTypes = argumentTypes,
                           .typeArguments = {pointeeType},
                           .intrinsic = IntrinsicKind::AllocateUniqueOwner});
      return;
    }

    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty()) {
      report(expr.paren(),
             "Unique-owner operations infer their pointee type and do not "
             "take explicit type arguments.",
             "GTI-S2018");
    }
    if (argumentTypes.size() != 1) {
      report(expr.paren(),
             uniqueOwnerIntrinsicName(intrinsic) +
                 " expects exactly one unique owner.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (argumentTypes.front().kind != SemanticType::UniqueOwner ||
        argumentTypes.front().arguments.size() != 1) {
      if (argumentTypes.front() != SemanticType::Unknown) {
        report(expressionToken(expr.arguments().front()),
               uniqueOwnerIntrinsicName(intrinsic) +
                   " requires gti_internal::unique_owner<T>.",
               "GTI-S2018");
      }
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType ownerType = argumentTypes.front();
    const SemanticType pointeeType = ownerType.arguments.front();
    const bool borrows = intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
                         intrinsic == IntrinsicKind::UniqueOwnerBorrowMut;
    if (borrows) {
      const ExpressionInfo *owner =
          semanticModel.findExpression(*expr.arguments().front());
      if (owner == nullptr || owner->category != ValueCategory::Place) {
        report(expressionToken(expr.arguments().front()),
               uniqueOwnerIntrinsicName(intrinsic) +
                   " requires stable unique-owner storage.",
               "GTI-S2018");
      } else if (intrinsic == IntrinsicKind::UniqueOwnerBorrowMut &&
                 owner->access != AccessMode::Mutable) {
        report(expressionToken(expr.arguments().front()),
               "unique_owner_borrow_mut requires mutable unique-owner "
               "storage.",
               "GTI-S2018");
      }
    }

    currentType = intrinsic == IntrinsicKind::UniqueOwnerIsNull
                      ? SemanticType::Bool
                      : pointeeType;
    semanticModel.record(
        expr,
        ResolvedCallInfo{
            .returnType =
                borrows ? SemanticType::referenceTo(
                              pointeeType,
                              intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly)
                        : currentType,
            .parameterTypes = std::move(argumentTypes),
            .typeArguments = {pointeeType},
            .intrinsic = intrinsic,
            .borrowOrigin =
                borrows ? BorrowOriginKind::Argument : BorrowOriginKind::None,
            .borrowArgument = 0,
            .borrowAccess = intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly});
  }

  void analyzeStorageIntrinsicCall(const Call &expr, IntrinsicKind intrinsic) {
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (intrinsic == IntrinsicKind::AllocateStorage) {
      analyzeStorageAllocation(expr, argumentTypes);
      return;
    }

    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty()) {
      report(expr.paren(),
             "Storage operations infer their element type from the storage "
             "argument and do not take explicit type arguments.",
             "GTI-S2019");
    }

    const std::size_t expectedArguments =
        intrinsic == IntrinsicKind::StorageRelocate ? 3 : 2;
    if (intrinsic == IntrinsicKind::StorageConstruct) {
      if (expr.arguments().size() != 3) {
        reportStorageArity(expr, intrinsic, 3);
      }
    } else if (expr.arguments().size() != expectedArguments) {
      reportStorageArity(expr, intrinsic, expectedArguments);
    }

    if (argumentTypes.empty() ||
        argumentTypes.front().kind != SemanticType::Storage ||
        argumentTypes.front().arguments.size() != 1) {
      if (!argumentTypes.empty() &&
          argumentTypes.front() != SemanticType::Unknown) {
        report(expressionToken(expr.arguments().front()),
               storageIntrinsicName(intrinsic) +
                   " requires gti_internal::storage<T> as its first argument.",
               "GTI-S2019");
      }
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType storageType = argumentTypes.front();
    const SemanticType elementType = storageType.arguments.front();
    const bool mutatesFirst = intrinsic == IntrinsicKind::StorageConstruct ||
                              intrinsic == IntrinsicKind::StorageReadMut ||
                              intrinsic == IntrinsicKind::StorageDestroy ||
                              intrinsic == IntrinsicKind::StorageRelocate;
    if (mutatesFirst) {
      requireMutableStorage(expr.arguments().front(),
                            storageIntrinsicName(intrinsic));
    }

    if (intrinsic == IntrinsicKind::StorageConstruct) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      if (argumentTypes.size() >= 3 &&
          argumentTypes[2] != SemanticType::Unknown &&
          argumentTypes[2] != elementType) {
        report(expressionToken(expr.arguments()[2]),
               "Storage element has type '" + typeSpelling(argumentTypes[2]) +
                   "' but this storage contains '" + typeSpelling(elementType) +
                   "'.",
               "GTI-S2019");
      }
      currentType = SemanticType::Void;
    } else if (intrinsic == IntrinsicKind::StorageRead ||
               intrinsic == IntrinsicKind::StorageReadMut) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      currentType = elementType;
    } else if (intrinsic == IntrinsicKind::StorageDestroy) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      currentType = SemanticType::Void;
    } else {
      if (argumentTypes.size() >= 2 &&
          argumentTypes[1] != SemanticType::Unknown) {
        if (argumentTypes[1] != storageType) {
          report(expressionToken(expr.arguments()[1]),
                 "storage_relocate requires source and destination storage to "
                 "have the same element type.",
                 "GTI-S2019");
        } else {
          requireMutableStorage(expr.arguments()[1], "storage_relocate");
        }
      }
      if (argumentTypes.size() >= 3) {
        requireStorageIndex(expr.arguments()[2], argumentTypes[2], intrinsic);
      }
      currentType = SemanticType::Void;
    }

    const bool borrowsStorage = intrinsic == IntrinsicKind::StorageRead ||
                                intrinsic == IntrinsicKind::StorageReadMut;
    semanticModel.record(
        expr,
        ResolvedCallInfo{
            .returnType = borrowsStorage
                              ? SemanticType::referenceTo(
                                    elementType,
                                    intrinsic == IntrinsicKind::StorageReadMut
                                        ? AccessMode::Mutable
                                        : AccessMode::ReadOnly)
                              : currentType,
            .parameterTypes = std::move(argumentTypes),
            .typeArguments = {elementType},
            .intrinsic = intrinsic,
            .borrowOrigin = borrowsStorage ? BorrowOriginKind::Argument
                                           : BorrowOriginKind::None,
            .borrowArgument = 0,
            .borrowAccess = intrinsic == IntrinsicKind::StorageReadMut
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly});
  }

  void
  analyzeStorageAllocation(const Call &expr,
                           const std::vector<SemanticType> &argumentTypes) {
    if (expr.typeArguments().size() != 1) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "gti_internal::allocate_storage<T> requires exactly one element "
             "type.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }

    const TypeRef &elementRef = expr.typeArguments().front();
    validateType(elementRef);
    validateReferencePlacement(elementRef, false, "storage element type");
    const SemanticType elementType = typeOf(elementRef);
    if (!isStorageElementType(elementType)) {
      report(elementRef.name.last(),
             "Compiler-private storage requires a concrete value element type.",
             "GTI-S2019");
    }
    if (argumentTypes.size() != 1) {
      reportStorageArity(expr, IntrinsicKind::AllocateStorage, 1);
    } else {
      requireStorageIndex(expr.arguments().front(), argumentTypes.front(),
                          IntrinsicKind::AllocateStorage);
    }

    currentType = SemanticType::storageOf(elementType);
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = argumentTypes,
                               .typeArguments = {elementType},
                               .intrinsic = IntrinsicKind::AllocateStorage});
  }

  [[nodiscard]] static bool isStorageElementType(const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Unknown:
    case SemanticType::Void:
    case SemanticType::NullPtr:
    case SemanticType::Reference:
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::TypeName:
    case SemanticType::Function:
    case SemanticType::Lambda:
    case SemanticType::Unexpected:
      return false;
    default:
      return true;
    }
  }

  void requireMutableStorage(const ExprPtr &argument,
                             std::string_view operation) {
    const ExpressionInfo *info =
        argument ? semanticModel.findExpression(*argument) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place ||
        info->access != AccessMode::Mutable) {
      report(expressionToken(argument),
             std::string(operation) + " requires mutable storage.",
             "GTI-S2019");
    }
    if (const Variable *owner = borrowedOwnerVariable(argument)) {
      const Symbol *symbol = resolve(owner->name());
      if (symbol != nullptr && symbol->borrowedStorage) {
        report(expressionToken(argument),
               std::string(operation) +
                   " cannot mutate storage while a reference borrowed from "
                   "it may still be live.",
               "GTI-S2017");
      }
    } else if (receiverStorageBorrowed && isReceiverDerivedBorrow(argument)) {
      report(expressionToken(argument),
             std::string(operation) +
                 " cannot mutate receiver storage while a reference borrowed "
                 "from it may still be live.",
             "GTI-S2017");
    }
  }

  void requireStorageIndex(const ExprPtr &argument, const SemanticType &type,
                           IntrinsicKind intrinsic) {
    if (type != SemanticType::Unknown && type != SemanticType::UInt64) {
      report(expressionToken(argument),
             storageIntrinsicName(intrinsic) +
                 " requires a uint64_t index or count.",
             "GTI-S2019");
    }
  }

  void reportStorageArity(const Call &expr, IntrinsicKind intrinsic,
                          std::size_t expected) {
    report(expr.paren(),
           storageIntrinsicName(intrinsic) + " expects " +
               std::to_string(expected) + " argument" +
               (expected == 1 ? "." : "s."),
           "GTI-S2019");
  }

  [[nodiscard]] static std::string
  uniqueOwnerIntrinsicName(IntrinsicKind intrinsic) {
    switch (intrinsic) {
    case IntrinsicKind::AllocateUniqueOwner:
      return "allocate_unique_owner";
    case IntrinsicKind::UniqueOwnerBorrow:
      return "unique_owner_borrow";
    case IntrinsicKind::UniqueOwnerBorrowMut:
      return "unique_owner_borrow_mut";
    case IntrinsicKind::UniqueOwnerIsNull:
      return "unique_owner_is_null";
    default:
      return "unique-owner operation";
    }
  }

  [[nodiscard]] static std::string
  storageIntrinsicName(IntrinsicKind intrinsic) {
    switch (intrinsic) {
    case IntrinsicKind::AllocateStorage:
      return "allocate_storage";
    case IntrinsicKind::StorageConstruct:
      return "storage_construct";
    case IntrinsicKind::StorageRead:
      return "storage_read";
    case IntrinsicKind::StorageReadMut:
      return "storage_read_mut";
    case IntrinsicKind::StorageDestroy:
      return "storage_destroy";
    case IntrinsicKind::StorageRelocate:
      return "storage_relocate";
    default:
      return "storage operation";
    }
  }

  [[nodiscard]] std::optional<TypeAliasId>
  typeAliasForCallee(const ExprPtr &callee) const {
    if (const auto *variable = dynamic_cast<const Variable *>(callee.get())) {
      return resolveTypeAliasPath(NamePath(variable->name()), currentNamespace);
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(callee.get())) {
      return resolveTypeAliasPath(qualified->name(), currentNamespace);
    }
    return std::nullopt;
  }

  [[nodiscard]] static const Call *directCall(const ExprPtr &expression) {
    const Expr *candidate = expression.get();
    while (const auto *grouping = dynamic_cast<const Grouping *>(candidate)) {
      candidate = grouping->expression().get();
    }
    return dynamic_cast<const Call *>(candidate);
  }

  void analyzeLambdaCall(const Call &call, const SemanticType &calleeType,
                         const std::vector<SemanticType> &argumentTypes) {
    const LambdaInfo *lambda = semanticModel.findLambda(calleeType.lambdaId);
    if (lambda == nullptr) {
      report(call.paren(), "Unknown lambda value.", "GTI-S2027");
      currentType = SemanticType::Unknown;
      return;
    }
    bool valid = true;
    if (!call.typeArguments().empty()) {
      report(call.paren(), "Lambdas do not take explicit generic arguments.",
             "GTI-S2027");
      valid = false;
    }
    if (argumentTypes.size() != lambda->parameterTypes.size()) {
      report(call.paren(),
             "Lambda expects " + std::to_string(lambda->parameterTypes.size()) +
                 " argument" + (lambda->parameterTypes.size() == 1 ? "" : "s") +
                 " but received " + std::to_string(argumentTypes.size()) + ".",
             "GTI-S2005");
      valid = false;
    }
    const std::size_t count =
        std::min(argumentTypes.size(), lambda->parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
      if (argumentTypes[index].kind == SemanticType::Lambda) {
        report(expressionToken(call.arguments()[index]),
               "Lambda values cannot be passed to another lambda yet.",
               "GTI-S2027");
        valid = false;
        continue;
      }
      if (argumentTypes[index] != SemanticType::Unknown &&
          lambda->parameterTypes[index] != SemanticType::Unknown &&
          !callArgumentMatches(lambda->parameterTypes[index],
                               argumentTypes[index], call.arguments()[index])) {
        reportCallArgumentMismatch(index, lambda->parameterTypes[index],
                                   argumentTypes[index],
                                   call.arguments()[index], "Lambda");
        valid = false;
      }
    }
    if (valid) {
      semanticModel.recordLambdaCall(
          call,
          ResolvedLambdaCallInfo{.lambda = lambda->id,
                                 .returnType = lambda->returnType,
                                 .parameterTypes = lambda->parameterTypes});
    }
    currentType = callExpressionType(lambda->returnType);
  }

  [[nodiscard]] static const GenericParameterInfo *
  findGenericParameter(const std::vector<GenericParameterInfo> &parameters,
                       GenericParameterId id) {
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.id == id) {
        return &parameter;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static const GenericParameterInfo *
  packGenericParameter(const FunctionCandidate &function) {
    const auto found = std::find_if(
        function.genericParameters.begin(), function.genericParameters.end(),
        [](const GenericParameterInfo &parameter) { return parameter.pack; });
    return found == function.genericParameters.end() ? nullptr : &*found;
  }

  [[nodiscard]] static std::size_t
  fixedParameterCount(const FunctionCandidate &function) {
    return function.parameterPack && !function.parameterTypes.empty()
               ? function.parameterTypes.size() - 1
               : function.parameterTypes.size();
  }

  [[nodiscard]] static bool
  hasPackExpansion(const std::vector<SemanticType> &arguments) {
    return !arguments.empty() &&
           arguments.back().kind == SemanticType::TypePack;
  }

  [[nodiscard]] bool
  appendConcreteCallArgument(const SemanticType &type,
                             const ExprPtr &expression,
                             std::vector<AnalyzedCallArgument> &result) const {
    if (type.kind != SemanticType::TypePack) {
      result.push_back({.type = type, .expression = &expression});
      return true;
    }
    if (!type.concretePack) {
      return false;
    }
    for (const SemanticType &element : type.arguments) {
      if (element.kind == SemanticType::TypePack) {
        if (!appendConcreteCallArgument(element, expression, result)) {
          return false;
        }
      } else {
        result.push_back({.type = element,
                          .expression = &expression,
                          .forwardedPackElement = true});
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<std::vector<AnalyzedCallArgument>>
  concreteCallArguments(const std::vector<SemanticType> &types,
                        const ExprList &expressions) const {
    if (types.size() != expressions.size()) {
      return std::nullopt;
    }
    std::vector<AnalyzedCallArgument> result;
    for (std::size_t index = 0; index < types.size(); ++index) {
      if (!appendConcreteCallArgument(types[index], expressions[index],
                                      result)) {
        return std::nullopt;
      }
    }
    return result;
  }

  [[nodiscard]] static bool
  acceptsArgumentShape(const FunctionCandidate &function,
                       const std::vector<SemanticType> &arguments) {
    if (!function.parameterPack) {
      return !hasPackExpansion(arguments) &&
             arguments.size() == function.parameterTypes.size();
    }
    const std::size_t fixed = fixedParameterCount(function);
    if (hasPackExpansion(arguments)) {
      return arguments.size() >= fixed + 1;
    }
    return arguments.size() >= fixed;
  }

  [[nodiscard]] bool
  satisfiesConstraint(const SemanticType &argument,
                      GenericConstraintKind constraint) const {
    if (argument == SemanticType::Unknown ||
        constraint == GenericConstraintKind::None ||
        constraint == GenericConstraintKind::Invalid) {
      return true;
    }
    if (argument.kind == SemanticType::TypeParameter ||
        argument.kind == SemanticType::TypePack) {
      const auto found = genericConstraints.find(argument.genericParameterId);
      const GenericConstraintKind actual = found == genericConstraints.end()
                                               ? GenericConstraintKind::None
                                               : found->second;
      return constraintImplies(actual, constraint);
    }
    switch (constraint) {
    case GenericConstraintKind::Ordered:
    case GenericConstraintKind::Numeric:
      return isInteger(argument) || argument == SemanticType::Float;
    case GenericConstraintKind::SignedNumeric:
      return isSignedInteger(argument) || argument == SemanticType::Float;
    case GenericConstraintKind::Integral:
      return isInteger(argument);
    case GenericConstraintKind::SignedIntegral:
      return isSignedInteger(argument);
    case GenericConstraintKind::UnsignedIntegral:
      return isUnsignedInteger(argument);
    case GenericConstraintKind::FloatingPoint:
      return argument == SemanticType::Float;
    case GenericConstraintKind::None:
    case GenericConstraintKind::Invalid:
      return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<ConstraintFailure>
  firstConstraintFailure(const std::vector<GenericParameterInfo> &parameters,
                         const std::vector<SemanticType> &arguments) const {
    std::size_t argumentIndex = 0;
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.value) {
        continue;
      }
      const std::size_t end =
          parameter.pack ? arguments.size() : argumentIndex + 1;
      while (argumentIndex < end && argumentIndex < arguments.size()) {
        if (!satisfiesConstraint(arguments[argumentIndex],
                                 parameter.constraint)) {
          return ConstraintFailure{.parameter = parameter.name,
                                   .argument = arguments[argumentIndex],
                                   .constraint = parameter.constraint,
                                   .constraintName = parameter.constraintName};
        }
        ++argumentIndex;
      }
    }
    return std::nullopt;
  }

  void reportConstraintFailure(const Token &site,
                               const ConstraintFailure &failure) {
    const std::string constraint = failure.constraintName
                                       ? pathSpelling(*failure.constraintName)
                                       : "the declared constraint";
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2029", DiagnosticPhase::Semantics, site,
        "Type '" + typeSpelling(failure.argument) +
            "' does not satisfy generic constraint '" + constraint +
            "' for parameter '" + failure.parameter.lexeme + "'.");
    if (failure.constraintName) {
      diagnostic.related.push_back({tokenSpan(failure.constraintName->last()),
                                    "Constraint on generic parameter '" +
                                        failure.parameter.lexeme +
                                        "' is declared here."});
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  bool applyVariadicFunctionTypeArguments(
      FunctionCandidate &function,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren) {
    FunctionCandidate resolved;
    std::vector<SemanticType> resolvedTypeArguments;
    ConstraintFailure constraintFailure;
    if (tryInstantiateFunction(function, explicitTypeArguments, argumentTypes,
                               resolved, resolvedTypeArguments,
                               &constraintFailure)) {
      function = std::move(resolved);
      return true;
    }
    if (constraintFailure.constraint != GenericConstraintKind::None) {
      reportConstraintFailure(paren, constraintFailure);
      return false;
    }

    bool valid = true;
    const GenericParameterInfo *pack = packGenericParameter(function);
    const std::size_t fixedGenerics =
        pack == nullptr ? function.genericParameters.size()
                        : function.genericParameters.size() - 1;
    if (pack == nullptr) {
      report(paren,
             "A parameter pack requires a matching final generic type pack.",
             "GTI-S2023");
      return false;
    }
    if (!explicitTypeArguments.empty() &&
        explicitTypeArguments.size() < fixedGenerics) {
      report(paren,
             "Variadic function calls must explicitly provide every fixed "
             "generic type before the inferred pack.",
             "GTI-S2023");
      valid = false;
    }
    if (!acceptsArgumentShape(function, argumentTypes)) {
      report(paren,
             "Variadic function expects at least " +
                 std::to_string(fixedParameterCount(function)) +
                 " fixed argument" +
                 (fixedParameterCount(function) == 1 ? "" : "s") +
                 " before its parameter pack.",
             "GTI-S2005");
      valid = false;
    }

    TypeSubstitution substitution;
    if (!explicitTypeArguments.empty()) {
      const std::size_t count =
          std::min(fixedGenerics, explicitTypeArguments.size());
      for (std::size_t index = 0; index < count; ++index) {
        substitution.emplace(function.genericParameters[index].id,
                             explicitTypeArguments[index]);
      }
    } else {
      const std::size_t count =
          std::min(fixedParameterCount(function), argumentTypes.size());
      for (std::size_t index = 0; index < count; ++index) {
        valid = inferTypeArguments(function.parameterTypes[index],
                                   argumentTypes[index],
                                   function.genericParameters, substitution,
                                   expressionToken(arguments[index])) &&
                valid;
      }
    }
    for (std::size_t index = 0; index < fixedGenerics; ++index) {
      const GenericParameterInfo &parameter = function.genericParameters[index];
      if (!substitution.contains(parameter.id)) {
        report(paren,
               "Cannot infer generic type parameter '" + parameter.name.lexeme +
                   "'; provide it explicitly before the type pack.",
               "GTI-S2023");
        valid = false;
      }
    }

    if (!explicitTypeArguments.empty() &&
        explicitTypeArguments.size() > fixedGenerics &&
        !hasPackExpansion(argumentTypes)) {
      const std::size_t explicitPackSize =
          explicitTypeArguments.size() - fixedGenerics;
      const std::size_t valuePackSize =
          argumentTypes.size() >= fixedParameterCount(function)
              ? argumentTypes.size() - fixedParameterCount(function)
              : 0;
      if (explicitPackSize != valuePackSize) {
        report(paren,
               "Explicit variadic type arguments must match the number of "
               "expanded value arguments.",
               "GTI-S2023");
        valid = false;
      }
    }
    const std::size_t fixedValues = fixedParameterCount(function);
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      const SemanticType &argument = argumentTypes[index];
      if (argument == SemanticType::Void) {
        report(paren, "Variadic type packs cannot contain void.", "GTI-S2023");
        valid = false;
      }
    }
    return valid;
  }

  bool inferTypeArguments(const SemanticType &pattern,
                          const SemanticType &argument,
                          const std::vector<GenericParameterInfo> &parameters,
                          TypeSubstitution &substitution,
                          const Token &argumentToken) {
    if (pattern.kind == SemanticType::Reference &&
        pattern.arguments.size() == 1) {
      return inferTypeArguments(pattern.arguments[0], argument, parameters,
                                substitution, argumentToken);
    }
    if (pattern.kind == SemanticType::TypeParameter &&
        findGenericParameter(parameters, pattern.genericParameterId) !=
            nullptr) {
      if (argument == SemanticType::Unknown) {
        return true;
      }
      const auto found = substitution.find(pattern.genericParameterId);
      if (found == substitution.end()) {
        substitution.emplace(pattern.genericParameterId, argument);
        return true;
      }
      if (found->second != argument) {
        const GenericParameterInfo *parameter =
            findGenericParameter(parameters, pattern.genericParameterId);
        report(argumentToken, "Conflicting types inferred for generic type "
                              "parameter '" +
                                  parameter->name.lexeme + "'.");
        return false;
      }
      return true;
    }

    if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
        pattern.arrayLength != argument.arrayLength ||
        pattern.arrayLengthParameterId != argument.arrayLengthParameterId ||
        pattern.valueArguments != argument.valueArguments ||
        pattern.arguments.size() != argument.arguments.size()) {
      return true;
    }
    bool valid = true;
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      valid = inferTypeArguments(pattern.arguments[index],
                                 argument.arguments[index], parameters,
                                 substitution, argumentToken) &&
              valid;
    }
    return valid;
  }

  bool applyFunctionTypeArguments(
      FunctionCandidate &function,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren) {
    if (function.parameterPack) {
      return applyVariadicFunctionTypeArguments(
          function, explicitTypeArguments, argumentTypes, arguments, paren);
    }
    bool valid = true;
    if (function.genericParameters.empty()) {
      if (!explicitTypeArguments.empty()) {
        report(paren, "Non-generic functions do not take generic arguments.");
        valid = false;
      }
      return valid;
    }

    TypeSubstitution substitution;
    if (!explicitTypeArguments.empty()) {
      if (explicitTypeArguments.size() != function.genericParameters.size()) {
        report(
            paren,
            "Generic function called with the wrong number of type arguments.");
        valid = false;
      }
      const std::size_t count = std::min(explicitTypeArguments.size(),
                                         function.genericParameters.size());
      for (std::size_t index = 0; index < count; ++index) {
        substitution.emplace(function.genericParameters[index].id,
                             explicitTypeArguments[index]);
      }
    } else {
      const std::size_t count =
          std::min(argumentTypes.size(), function.parameterTypes.size());
      for (std::size_t index = 0; index < count; ++index) {
        valid = inferTypeArguments(function.parameterTypes[index],
                                   argumentTypes[index],
                                   function.genericParameters, substitution,
                                   expressionToken(arguments[index])) &&
                valid;
      }
    }

    for (const GenericParameterInfo &parameter : function.genericParameters) {
      if (!substitution.contains(parameter.id)) {
        report(paren, "Cannot infer generic type parameter '" +
                          parameter.name.lexeme +
                          "'; provide explicit type arguments.");
        valid = false;
      }
    }
    std::vector<SemanticType> resolvedTypeArguments;
    resolvedTypeArguments.reserve(function.genericParameters.size());
    for (const GenericParameterInfo &parameter : function.genericParameters) {
      if (const auto found = substitution.find(parameter.id);
          found != substitution.end()) {
        resolvedTypeArguments.emplace_back(found->second);
      }
    }
    if (resolvedTypeArguments.size() == function.genericParameters.size()) {
      if (const std::optional<ConstraintFailure> failure =
              firstConstraintFailure(function.genericParameters,
                                     resolvedTypeArguments)) {
        reportConstraintFailure(paren, *failure);
        valid = false;
      }
    }
    function.returnType = substituteType(function.returnType, substitution);
    for (SemanticType &parameter : function.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
    return valid;
  }

  [[nodiscard]] static bool
  tryInferTypeArguments(const SemanticType &pattern,
                        const SemanticType &argument,
                        const std::vector<GenericParameterInfo> &parameters,
                        TypeSubstitution &substitution) {
    if (pattern.kind == SemanticType::Reference &&
        pattern.arguments.size() == 1) {
      return tryInferTypeArguments(pattern.arguments[0], argument, parameters,
                                   substitution);
    }
    if (pattern.kind == SemanticType::TypeParameter &&
        findGenericParameter(parameters, pattern.genericParameterId) !=
            nullptr) {
      if (argument == SemanticType::Unknown) {
        return true;
      }
      const auto found = substitution.find(pattern.genericParameterId);
      if (found == substitution.end()) {
        substitution.emplace(pattern.genericParameterId, argument);
        return true;
      }
      return found->second == argument;
    }

    if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
        pattern.arrayLength != argument.arrayLength ||
        pattern.arrayLengthParameterId != argument.arrayLengthParameterId ||
        pattern.valueArguments != argument.valueArguments ||
        pattern.arguments.size() != argument.arguments.size()) {
      return true;
    }
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      if (!tryInferTypeArguments(pattern.arguments[index],
                                 argument.arguments[index], parameters,
                                 substitution)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool
  tryInstantiateFunction(const FunctionCandidate &candidate,
                         const std::vector<SemanticType> &explicitTypeArguments,
                         const std::vector<SemanticType> &argumentTypes,
                         FunctionCandidate &resolved,
                         std::vector<SemanticType> &resolvedTypeArguments,
                         ConstraintFailure *constraintFailure = nullptr) const {
    resolved = candidate;
    resolvedTypeArguments.clear();
    if (candidate.genericParameters.empty()) {
      return explicitTypeArguments.empty() &&
             acceptsArgumentShape(candidate, argumentTypes);
    }

    TypeSubstitution substitution;
    const GenericParameterInfo *pack = packGenericParameter(candidate);
    const std::size_t fixedGenerics =
        pack == nullptr ? candidate.genericParameters.size()
                        : candidate.genericParameters.size() - 1;
    if (candidate.parameterPack != (pack != nullptr) ||
        !acceptsArgumentShape(candidate, argumentTypes)) {
      return false;
    }
    if (!explicitTypeArguments.empty()) {
      if (pack == nullptr &&
          explicitTypeArguments.size() != candidate.genericParameters.size()) {
        return false;
      }
      if (pack != nullptr && explicitTypeArguments.size() < fixedGenerics) {
        return false;
      }
      for (std::size_t index = 0;
           index < std::min(fixedGenerics, explicitTypeArguments.size());
           ++index) {
        substitution.emplace(candidate.genericParameters[index].id,
                             explicitTypeArguments[index]);
      }
    } else {
      const std::size_t count =
          std::min(fixedParameterCount(candidate), argumentTypes.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (!tryInferTypeArguments(candidate.parameterTypes[index],
                                   argumentTypes[index],
                                   candidate.genericParameters, substitution)) {
          return false;
        }
      }
    }

    for (std::size_t index = 0; index < fixedGenerics; ++index) {
      const GenericParameterInfo &parameter =
          candidate.genericParameters[index];
      const auto found = substitution.find(parameter.id);
      if (found == substitution.end()) {
        return false;
      }
      resolvedTypeArguments.emplace_back(found->second);
    }

    std::vector<SemanticType> packTypes;
    std::vector<SemanticType> resolvedPackParameters;
    if (pack != nullptr) {
      const std::size_t fixedValues = fixedParameterCount(candidate);
      if (hasPackExpansion(argumentTypes)) {
        if (explicitTypeArguments.size() > fixedGenerics) {
          return false;
        }
        resolvedPackParameters.assign(
            argumentTypes.begin() + static_cast<std::ptrdiff_t>(fixedValues),
            argumentTypes.end());
        for (const SemanticType &argument : resolvedPackParameters) {
          if (argument.kind == SemanticType::TypePack &&
              argument.concretePack) {
            packTypes.insert(packTypes.end(), argument.arguments.begin(),
                             argument.arguments.end());
          } else {
            packTypes.emplace_back(argument);
          }
        }
      } else if (explicitTypeArguments.size() > fixedGenerics) {
        packTypes.assign(explicitTypeArguments.begin() +
                             static_cast<std::ptrdiff_t>(fixedGenerics),
                         explicitTypeArguments.end());
        if (packTypes.size() != argumentTypes.size() - fixedValues) {
          return false;
        }
        resolvedPackParameters = packTypes;
      } else {
        packTypes.assign(argumentTypes.begin() +
                             static_cast<std::ptrdiff_t>(fixedValues),
                         argumentTypes.end());
        resolvedPackParameters = packTypes;
      }
      resolvedTypeArguments.insert(resolvedTypeArguments.end(),
                                   packTypes.begin(), packTypes.end());
    }
    if (std::any_of(resolvedTypeArguments.begin(), resolvedTypeArguments.end(),
                    [](const SemanticType &argument) {
                      return argument == SemanticType::Void;
                    })) {
      return false;
    }
    if (const std::optional<ConstraintFailure> failure = firstConstraintFailure(
            candidate.genericParameters, resolvedTypeArguments)) {
      if (constraintFailure != nullptr) {
        *constraintFailure = *failure;
      }
      return false;
    }
    resolved.returnType = substituteType(resolved.returnType, substitution);
    std::vector<SemanticType> resolvedParameters;
    resolvedParameters.reserve(fixedParameterCount(candidate) +
                               packTypes.size());
    for (std::size_t index = 0; index < fixedParameterCount(candidate);
         ++index) {
      resolvedParameters.emplace_back(
          substituteType(candidate.parameterTypes[index], substitution));
    }
    resolvedParameters.insert(resolvedParameters.end(),
                              resolvedPackParameters.begin(),
                              resolvedPackParameters.end());
    resolved.parameterTypes = std::move(resolvedParameters);
    resolved.parameterPack = false;
    return true;
  }

  [[nodiscard]] static std::string callableSpelling(const ExprPtr &callee) {
    if (const auto *variable = dynamic_cast<const Variable *>(callee.get())) {
      return variable->name().lexeme;
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(callee.get())) {
      return pathSpelling(qualified->name());
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      return member->name().lexeme;
    }
    return "function";
  }

  [[nodiscard]] static std::string typeRefSpelling(const TypeRef &type) {
    std::string result = pathSpelling(type.name);
    if (!type.arguments.empty()) {
      result += '<';
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        if (index != 0) {
          result += ", ";
        }
        result += typeRefSpelling(type.arguments[index]);
      }
      result += '>';
    }
    for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
      result += '[' +
                (extent ? arrayExtentSpelling(*extent) : std::string("?")) +
                ']';
    }
    if (type.reference) {
      result += type.reference->lexeme;
    }
    return result;
  }

  [[nodiscard]] static std::string
  functionSignatureSpelling(const FunctionCandidate &function) {
    if (function.declaration == nullptr) {
      return "function";
    }
    const FunctionDecl &declaration = *function.declaration;
    std::string result = declaration.operatorName()
                             ? std::string(operatorSourceSpelling(
                                   declaration.operatorName()->kind))
                             : declaration.name().lexeme;
    if (!declaration.genericParameters().empty()) {
      result += '<';
      for (std::size_t index = 0;
           index < declaration.genericParameters().size(); ++index) {
        if (index != 0) {
          result += ", ";
        }
        if (declaration.genericParameters()[index].constraint) {
          result +=
              pathSpelling(*declaration.genericParameters()[index].constraint) +
              " ";
        }
        result += declaration.genericParameters()[index].name.lexeme;
        if (declaration.genericParameters()[index].pack) {
          result += "...";
        }
      }
      result += '>';
    }
    result += '(';
    for (std::size_t index = 0; index < declaration.parameters().size();
         ++index) {
      if (index != 0) {
        result += ", ";
      }
      result += typeRefSpelling(declaration.parameters()[index].type);
      if (declaration.parameters()[index].pack) {
        result += "...";
      }
    }
    result += ')';
    if (declaration.receiverMutability() == ReceiverMutability::Mutable) {
      result += " mut";
    }
    return result;
  }

  [[nodiscard]] static std::string
  constructorSignatureSpelling(const ConstructorInfo &constructor) {
    if (constructor.declaration == nullptr) {
      return "generated default constructor";
    }
    std::string result = constructor.declaration->name().lexeme + '(';
    for (std::size_t index = 0;
         index < constructor.declaration->parameters().size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      const Parameter &parameter = constructor.declaration->parameters()[index];
      if (parameter.mutability == Mutability::Mutable &&
          parameter.type.reference) {
        result += "mut ";
      }
      result += typeRefSpelling(parameter.type);
    }
    result += ')';
    return result;
  }

  void validateSelectedFunction(const FunctionCandidate &function,
                                const ExprPtr &callee, const Token &paren) {
    if (function.ownerClass != 0 &&
        function.access == AccessModifier::Private &&
        currentClass != function.ownerClass) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, paren,
          "Method '" + function.declaration->name().lexeme + "' of '" +
              classInfo(function.ownerClass).name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(function.declaration->name()), "Method declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }

    if (function.staticMember) {
      // Object-qualified static access is diagnosed while resolving Get.
      return;
    }

    if (function.receiverMutability != ReceiverMutability::Mutable) {
      return;
    }
    bool mutableReceiver =
        currentReceiverMutability == ReceiverMutability::Mutable;
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      mutableReceiver = memberReceiverIsMutable(*member);
    }
    if (!mutableReceiver) {
      report(paren, "Mutable method requires a mutable receiver.");
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      if (const Variable *owner = directStorageVariable(member->object())) {
        const Symbol *symbol = resolve(owner->name());
        if (symbol != nullptr && symbol->borrowedStorage) {
          report(paren,
                 isMoveOnlyOwnerType(symbol->type)
                     ? "Mutable method cannot use move-only storage while a "
                       "reference borrowed from it may still be live."
                     : "Mutable method cannot use storage while a retained "
                       "borrow from it may still be live.",
                 "GTI-S2017");
        }
      } else if (receiverStorageBorrowed &&
                 isReceiverDerivedBorrow(member->object())) {
        report(paren,
               "Mutable method cannot use receiver storage while a reference "
               "borrowed from it may still be live.",
               "GTI-S2017");
      }
    }
  }

  [[nodiscard]] std::optional<FunctionCandidate> resolveOperator(
      const Expr &site, OverloadedOperator kind, const ExprPtr &receiver,
      const SemanticType &receiverType, const Token &token,
      std::span<const SemanticType> argumentTypes = {},
      std::span<const ExprPtr> arguments = {}, bool contextual = false) {
    const ClassInfo *owner = classInfo(receiverType);
    if (owner == nullptr) {
      return std::nullopt;
    }

    const std::string memberName(operatorFunctionName(kind));
    const auto found = owner->members.find(memberName);
    if (found == owner->members.end() ||
        found->second.symbol.type != SemanticType::Function) {
      report(token,
             "Type '" + typeSpelling(receiverType) + "' does not define " +
                 std::string(operatorSourceSpelling(kind)) + ".",
             "GTI-S2022");
      return std::nullopt;
    }

    const Symbol overloadSet =
        substituteSymbol(found->second.symbol, receiverType);
    const bool mutableReceiver = isMutableObject(receiver);
    bool rejectedMutableReceiver = false;
    std::vector<FunctionCandidate> viable;
    for (const FunctionCandidate &candidate : overloadSet.overloads) {
      if (candidate.parameterTypes.size() != argumentTypes.size() ||
          arguments.size() != argumentTypes.size()) {
        continue;
      }
      bool exact = true;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (argumentTypes[index] != SemanticType::Unknown &&
            candidate.parameterTypes[index] != SemanticType::Unknown &&
            !callArgumentMatches(candidate.parameterTypes[index],
                                 argumentTypes[index], arguments[index])) {
          exact = false;
          break;
        }
      }
      if (!exact) {
        continue;
      }
      if (candidate.receiverMutability == ReceiverMutability::Mutable &&
          !mutableReceiver) {
        rejectedMutableReceiver = true;
        continue;
      }
      viable.emplace_back(candidate);
    }

    if (mutableReceiver && std::any_of(viable.begin(), viable.end(),
                                       [](const FunctionCandidate &candidate) {
                                         return candidate.receiverMutability ==
                                                ReceiverMutability::Mutable;
                                       })) {
      std::erase_if(viable, [](const FunctionCandidate &candidate) {
        return candidate.receiverMutability == ReceiverMutability::ReadOnly;
      });
    }

    if (viable.size() != 1) {
      if (viable.empty() && rejectedMutableReceiver) {
        report(token,
               std::string(operatorSourceSpelling(kind)) +
                   " requires a mutable receiver.",
               "GTI-S2022");
      } else if (viable.empty()) {
        std::string received;
        if (!argumentTypes.empty()) {
          received = " for argument types (";
          for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
            if (index != 0) {
              received += ", ";
            }
            received += typeSpelling(argumentTypes[index]);
          }
          received += ')';
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2022", DiagnosticPhase::Semantics, token,
            "No exact overload of " +
                std::string(operatorSourceSpelling(kind)) + received + ".");
        for (const FunctionCandidate &candidate : overloadSet.overloads) {
          if (candidate.declaration != nullptr) {
            diagnostic.related.push_back(
                {tokenSpan(candidate.declaration->operatorName()->keyword),
                 "Candidate: " + functionSignatureSpelling(candidate)});
          }
        }
        diagnostics.emplace_back(std::move(diagnostic));
      } else {
        report(token,
               "Use of " + std::string(operatorSourceSpelling(kind)) +
                   " is ambiguous.",
               "GTI-S2022");
      }
      return std::nullopt;
    }

    const FunctionCandidate &selected = viable.front();
    if (selected.access == AccessModifier::Private &&
        currentClass != selected.ownerClass) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, token,
                         std::string(operatorSourceSpelling(kind)) + " of '" +
                             owner->name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(selected.declaration->operatorName()->keyword),
           "Operator declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (selected.receiverMutability == ReceiverMutability::Mutable) {
      if (const Variable *ownerVariable = directStorageVariable(receiver)) {
        const Symbol *symbol = resolve(ownerVariable->name());
        if (symbol != nullptr && symbol->borrowedStorage) {
          report(token,
                 isMoveOnlyOwnerType(symbol->type)
                     ? "Mutable operator cannot use move-only storage while a "
                       "reference borrowed from it may still be live."
                     : "Mutable operator cannot use storage while a retained "
                       "borrow from it may still be live.",
                 "GTI-S2017");
        }
      } else if (receiverStorageBorrowed && isReceiverDerivedBorrow(receiver)) {
        report(token,
               "Mutable operator cannot use receiver storage while a reference "
               "borrowed from it may still be live.",
               "GTI-S2017");
      }
    }

    ResolvedOperatorInfo resolved{.function = selected.id,
                                  .declaration = selected.declaration,
                                  .dispatch = selected.virtualMethod
                                                  ? CallDispatch::Virtual
                                                  : CallDispatch::Static,
                                  .dispatchOwner = selected.dispatchOwner,
                                  .kind = kind,
                                  .returnType = selected.returnType,
                                  .parameterTypes = selected.parameterTypes};
    if (contextual) {
      semanticModel.recordContextualConversion(site, std::move(resolved));
    } else {
      semanticModel.recordOperator(site, std::move(resolved));
    }
    return selected;
  }

  SemanticType arrowTargetType(const Expr &site, const ExprPtr &receiver,
                               const SemanticType &receiverType,
                               const Token &token) {
    if (receiverType.kind == SemanticType::Class) {
      const std::optional<FunctionCandidate> selected = resolveOperator(
          site, OverloadedOperator::Arrow, receiver, receiverType, token);
      if (!selected || selected->returnType.kind != SemanticType::Reference ||
          selected->returnType.arguments.size() != 1) {
        return SemanticType::Unknown;
      }
      return selected->returnType.arguments[0];
    }
    if (receiverType != SemanticType::Unknown) {
      report(token, "Operator '->' requires a class defining operator->.",
             "GTI-S2022");
    }
    return SemanticType::Unknown;
  }

  void recordResolvedCall(const Call &call, const FunctionCandidate &function,
                          std::vector<SemanticType> typeArguments) {
    const bool borrowsReceiver =
        function.ownerClass != 0 && !function.staticMember &&
        (function.returnType.kind == SemanticType::Reference ||
         typeTraits(function.returnType).containsBorrowedState);
    ResolvedCallInfo resolved{
        .function = function.id,
        .declaration = function.declaration,
        .returnType = function.returnType,
        .parameterTypes = function.parameterTypes,
        .typeArguments = std::move(typeArguments),
        .borrowOrigin = borrowsReceiver ? BorrowOriginKind::Receiver
                                        : BorrowOriginKind::None,
        .borrowAccess = borrowAccess(function.returnType),
        .dispatch = function.virtualMethod ? CallDispatch::Virtual
                                           : CallDispatch::Static,
        .dispatchOwner = function.dispatchOwner};
    semanticModel.record(call, resolved);
    const Token token = callableToken(call.callee());
    if (token.generated) {
      return;
    }
    const SymbolId symbol = resolved.declaration == nullptr
                                ? 0
                                : recordFunctionSymbol(*resolved.declaration);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(token),
         .kind = SemanticOccurrenceKind::SelectedCall,
         .symbol = symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::Call,
         .name = token.lexeme,
         .type = resolved.returnType,
         .traits = typeTraits(resolved.returnType),
         .function = resolved.declaration,
         .selectedCall = std::move(resolved)});
  }

  [[nodiscard]] static SemanticType
  callExpressionType(const SemanticType &returnType) {
    if (returnType.kind == SemanticType::Reference &&
        returnType.arguments.size() == 1) {
      return returnType.arguments[0];
    }
    return returnType;
  }

  void reportOverloadResolutionFailure(
      const Call &call, const Symbol &overloadSet,
      const std::vector<SemanticType> &argumentTypes,
      const std::vector<const FunctionCandidate *> &exactMatches) {
    const std::string name = callableSpelling(call.callee());
    Diagnostic diagnostic;
    if (exactMatches.empty()) {
      std::string arguments;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (index != 0) {
          arguments += ", ";
        }
        arguments += typeSpelling(argumentTypes[index]);
      }
      diagnostic = makeDiagnostic(
          "GTI-S2012", DiagnosticPhase::Semantics, call.paren(),
          "No overload of '" + name + "' exactly matches argument types (" +
              arguments + ").");
      diagnostic.hints.emplace_back(
          "Function calls do not perform implicit conversions; convert an "
          "argument explicitly with syntax such as 'uint64_t(value)'.");
      for (const FunctionCandidate &candidate : overloadSet.overloads) {
        if (candidate.declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->name()),
               "Candidate: " + functionSignatureSpelling(candidate)});
        }
      }
    } else {
      diagnostic =
          makeDiagnostic("GTI-S2013", DiagnosticPhase::Semantics, call.paren(),
                         "Call to '" + name + "' is ambiguous; " +
                             std::to_string(exactMatches.size()) +
                             " overloads exactly match.");
      for (const FunctionCandidate *candidate : exactMatches) {
        if (candidate != nullptr && candidate->declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(candidate->declaration->name()),
               "Exact candidate: " + functionSignatureSpelling(*candidate)});
        }
      }
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  [[nodiscard]] bool
  callArgumentMatches(const SemanticType &parameter,
                      const SemanticType &argument, const ExprPtr &expression,
                      bool allowValueAssignment = false) const {
    if (parameter == SemanticType::Unknown ||
        argument == SemanticType::Unknown) {
      return true;
    }
    if (parameter.kind != SemanticType::Reference) {
      if (parameter == argument && expression &&
          parameter.kind != SemanticType::TypePack) {
        const ExpressionInfo *info = semanticModel.findExpression(*expression);
        if (info != nullptr) {
          const SemanticTypeTraits traits = typeTraits(parameter);
          if ((info->category == ValueCategory::Value && !traits.movable) ||
              (info->category == ValueCategory::Place && !traits.copyable)) {
            return false;
          }
        }
      }
      return allowValueAssignment
                 ? isAssignable(parameter, argument, expression.get())
                 : parameter == argument;
    }
    if (parameter.arguments.size() != 1 || parameter.arguments[0] != argument ||
        !expression) {
      return false;
    }
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    if (info == nullptr || info->category != ValueCategory::Place) {
      return false;
    }
    return parameter.referenceAccess != AccessMode::Mutable ||
           info->access == AccessMode::Mutable;
  }

  void reportCallArgumentMismatch(std::size_t index,
                                  const SemanticType &parameter,
                                  const SemanticType &argument,
                                  const ExprPtr &expression,
                                  std::string_view callable) {
    const std::string argumentLabel =
        callable == "Function" ? "Argument "
                               : std::string(callable) + " argument ";
    if (parameter.kind == SemanticType::Reference &&
        parameter.arguments.size() == 1 && parameter.arguments[0] == argument &&
        expression) {
      const ExpressionInfo *info = semanticModel.findExpression(*expression);
      if (info == nullptr || info->category != ValueCategory::Place) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " cannot bind a reference to a temporary.",
               "GTI-S2017");
        return;
      }
      if (parameter.referenceAccess == AccessMode::Mutable &&
          info->access != AccessMode::Mutable) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " requires a mutable value for parameter '" +
                   typeSpelling(parameter) + "'.",
               "GTI-S2017");
        return;
      }
    }
    if (isMoveOnlyOwnerType(parameter) && parameter == argument) {
      const ClassInfo *owner = classInfo(parameter);
      if (owner != nullptr && owner->copyConstructor &&
          owner->copyConstructor->declaration != nullptr &&
          owner->copyConstructor->declaration->specifier() &&
          owner->copyConstructor->declaration->specifier()->kind ==
              SpecialMemberSpecifierKind::Deleted) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " would call the deleted copy constructor of '" +
                   typeSpelling(parameter) + "'.",
               "GTI-S2018");
        return;
      }
      std::string copyDescription =
          " would copy a move-only value; use std::move(owner) to ";
      if (typeTraits(parameter).ownership == OwnershipKind::Unique) {
        copyDescription =
            " would copy a unique owner; use std::move(owner) to ";
      } else if (parameter.kind == SemanticType::Storage) {
        copyDescription = " would copy compiler-private storage; use "
                          "std::move(owner) to ";
      }
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) + copyDescription +
                 "transfer ownership.",
             "GTI-S2018");
      return;
    }
    if (parameter == argument && parameter.kind != SemanticType::TypePack &&
        expression) {
      const ExpressionInfo *info = semanticModel.findExpression(*expression);
      const SemanticTypeTraits traits = typeTraits(parameter);
      if (info != nullptr && info->category == ValueCategory::Place &&
          !traits.copyable) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " requires copying type '" + typeSpelling(parameter) +
                   "', but copy construction is unavailable.",
               "GTI-S2018");
        return;
      }
      if (info != nullptr && info->category == ValueCategory::Value &&
          !traits.movable) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " requires moving type '" + typeSpelling(parameter) +
                   "', but move construction is unavailable.",
               "GTI-S2018");
        return;
      }
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2003", DiagnosticPhase::Semantics, expressionToken(expression),
        argumentLabel + std::to_string(index + 1) + " has type '" +
            typeSpelling(argument) + "' but the parameter requires '" +
            typeSpelling(parameter) + "'.");
    if (parameter.kind != SemanticType::Reference) {
      diagnostic.hints.emplace_back(
          "Calls require exact argument types; use an explicit "
          "conversion such as '" +
          typeSpelling(parameter) +
          "(value)' when the conversion is intentional.");
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  ResolvedClassArguments
  resolveClassArguments(ClassId classId,
                        const std::vector<TypeRef> &arguments,
                        const Token &site) {
    ResolvedClassArguments result;
    if (classId == 0 || classId > classes.size()) {
      result.valid = false;
      return result;
    }

    const ClassInfo &owner = classInfo(classId);
    result.types.reserve(genericTypeParameterCount(owner.genericParameters));
    result.values.reserve(
        genericValueParameterCount(owner.genericParameters));
    if (arguments.size() != owner.genericParameters.size()) {
      const bool typeOnly =
          genericValueParameterCount(owner.genericParameters) == 0;
      report(site, "Type '" + owner.name.lexeme + "' requires " +
                       std::to_string(owner.genericParameters.size()) +
                       (typeOnly ? " generic type argument"
                                 : " generic argument") +
                       (owner.genericParameters.size() == 1 ? "." : "s."),
             "GTI-S2026");
      result.valid = false;
    }

    const std::size_t count =
        std::min(arguments.size(), owner.genericParameters.size());
    for (std::size_t index = 0; index < count; ++index) {
      const GenericParameterInfo &parameter = owner.genericParameters[index];
      const TypeRef &argument = arguments[index];
      if (parameter.value) {
        const std::optional<CompileTimeValue> value =
            genericValueArgument(argument);
        if (!value) {
          report(argument.name.last(),
                 "Generic parameter '" + parameter.name.lexeme +
                     "' requires a uint64_t compile-time value.",
                 "GTI-S2026");
          result.values.emplace_back();
          result.valid = false;
        } else {
          result.values.emplace_back(*value);
        }
        continue;
      }

      if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
        report(argument.name.last(),
               "Generic parameter '" + parameter.name.lexeme +
                   "' requires a type argument.",
               "GTI-S2026");
        result.types.emplace_back(SemanticType::Unknown);
        result.valid = false;
        continue;
      }
      validateType(argument);
      const SemanticType type = typeOf(argument);
      if (type == SemanticType::Void) {
        report(argument.name.last(), "Generic type arguments cannot be void.");
        result.valid = false;
      } else if (!satisfiesConstraint(type, parameter.constraint)) {
        reportConstraintFailure(
            argument.name.last(),
            ConstraintFailure{.parameter = parameter.name,
                              .argument = type,
                              .constraint = parameter.constraint,
                              .constraintName = parameter.constraintName});
        result.valid = false;
      }
      result.types.emplace_back(type);
    }
    return result;
  }

  void
  analyzeConstructorCall(const Expr &construction, ClassId classId,
                         const std::vector<SemanticType> &typeArguments,
                         const std::vector<CompileTimeValue> &valueArguments,
                         const std::vector<SemanticType> &argumentTypes,
                         const ExprList &arguments, const Token &paren) {
    std::vector<AnalyzedCallArgument> analyzedArguments;
    analyzedArguments.reserve(argumentTypes.size());
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      analyzedArguments.push_back({.type = argumentTypes[index],
                                   .expression = index < arguments.size()
                                                     ? &arguments[index]
                                                     : nullptr});
    }
    analyzeConstructorCallArguments(construction, classId, typeArguments,
                                    valueArguments, analyzedArguments, paren);
  }

  [[nodiscard]] bool
  forwardedPackArgumentMatches(const SemanticType &parameter,
                               const SemanticType &argument) const {
    if (parameter == SemanticType::Unknown ||
        argument == SemanticType::Unknown) {
      return true;
    }
    if (parameter.kind != SemanticType::Reference) {
      return parameter == argument;
    }
    return parameter.arguments.size() == 1 &&
           parameter.arguments.front() == argument &&
           parameter.referenceAccess == AccessMode::ReadOnly;
  }

  void analyzeConstructorCallArguments(
      const Expr &construction, ClassId classId,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments,
      const std::vector<AnalyzedCallArgument> &arguments, const Token &paren) {
    if (classId == 0 || classId > classes.size()) {
      currentType = SemanticType::Unknown;
      return;
    }

    const ClassInfo &owner = classInfo(classId);
    GenericSubstitution substitution;
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter : owner.genericParameters) {
      if (parameter.value) {
        if (valueIndex < valueArguments.size()) {
          substitution.values.emplace(parameter.id,
                                      valueArguments[valueIndex++]);
        }
      } else if (typeIndex < typeArguments.size()) {
        substitution.types.emplace(parameter.id, typeArguments[typeIndex++]);
      }
    }
    const SemanticType constructedType = SemanticType::classType(
        classId, typeArguments, valueArguments);
    if (owner.abstract) {
      report(paren,
             "Cannot construct abstract " +
                 std::string(classKindSpelling(owner.kind)) + " '" +
                 owner.name.lexeme + "'.",
             "GTI-S2044");
    }

    struct ViableConstructor {
      const ConstructorInfo *constructor = nullptr;
      std::vector<SemanticType> parameterTypes;
      bool generatedDefault = false;
      ConstructorKind kind = ConstructorKind::Ordinary;
    };
    std::vector<ViableConstructor> viable;
    for (const ConstructorInfo &constructor : owner.constructors) {
      if (constructor.parameterTypes.size() != arguments.size()) {
        continue;
      }
      std::vector<SemanticType> parameterTypes;
      parameterTypes.reserve(constructor.parameterTypes.size());
      bool exact = true;
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType parameterType =
            substituteType(constructor.parameterTypes[index], substitution);
        parameterTypes.emplace_back(parameterType);
        const AnalyzedCallArgument &argument = arguments[index];
        const bool matches =
            argument.forwardedPackElement
                ? forwardedPackArgumentMatches(parameterType, argument.type)
                : argument.expression != nullptr &&
                      callArgumentMatches(parameterType, argument.type,
                                          *argument.expression);
        if (!matches) {
          exact = false;
          break;
        }
      }
      if (exact) {
        viable.push_back({&constructor, std::move(parameterTypes), false,
                          ConstructorKind::Ordinary});
      }
    }

    if (arguments.empty() && defaultConstructor(owner) == nullptr &&
        classCanGenerateDefaultConstructor(owner)) {
      viable.push_back({nullptr, {}, true, ConstructorKind::Ordinary});
    }

    std::optional<ConstructorKind> attemptedSpecial;
    if (arguments.size() == 1 && !arguments.front().forwardedPackElement &&
        arguments.front().expression != nullptr &&
        arguments.front().type == constructedType) {
      const ExpressionInfo *argumentInfo =
          semanticModel.findExpression(**arguments.front().expression);
      if (argumentInfo != nullptr) {
        const bool moving = argumentInfo->category == ValueCategory::Value;
        attemptedSpecial =
            moving ? ConstructorKind::Move : ConstructorKind::Copy;
        const SemanticTypeTraits traits = typeTraits(constructedType);
        const bool available = moving ? traits.movable : traits.copyable;
        if (available) {
          const std::optional<ConstructorInfo> &declared =
              moving ? owner.moveConstructor : owner.copyConstructor;
          std::vector<SemanticType> parameterTypes;
          if (declared && !declared->parameterTypes.empty()) {
            parameterTypes.push_back(
                substituteType(declared->parameterTypes.front(), substitution));
          } else {
            parameterTypes.push_back(
                SemanticType::referenceTo(constructedType));
          }
          viable.push_back({declared ? &*declared : nullptr,
                            std::move(parameterTypes), false,
                            *attemptedSpecial});
        }
      }
    }

    const bool hasUnknownArgument =
        std::any_of(arguments.begin(), arguments.end(),
                    [](const AnalyzedCallArgument &value) {
                      return value.type == SemanticType::Unknown;
                    });
    if (viable.size() != 1) {
      if (!hasUnknownArgument) {
        if (viable.empty() && attemptedSpecial) {
          const bool moving = *attemptedSpecial == ConstructorKind::Move;
          const std::optional<ConstructorInfo> &declared =
              moving ? owner.moveConstructor : owner.copyConstructor;
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2020", DiagnosticPhase::Semantics, paren,
              std::string(moving ? "Move" : "Copy") + " construction of '" +
                  owner.name.lexeme + "' is deleted.");
          if (declared && declared->declaration != nullptr) {
            diagnostic.related.push_back(
                {tokenSpan(declared->declaration->name()),
                 std::string(moving ? "Move" : "Copy") +
                     " constructor policy declared here."});
          }
          diagnostic.hints.emplace_back(
              moving ? "Remove std::move only if the type remains copyable."
                     : "Transfer the value with std::move only if the type "
                       "remains movable.");
          diagnostics.emplace_back(std::move(diagnostic));
          currentType = constructedType;
          return;
        }
        std::string argumentsSpelling;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
          if (index != 0) {
            argumentsSpelling += ", ";
          }
          argumentsSpelling += typeSpelling(arguments[index].type);
        }
        Diagnostic diagnostic = makeDiagnostic(
            viable.empty() ? "GTI-S2012" : "GTI-S2013",
            DiagnosticPhase::Semantics, paren,
            viable.empty()
                ? "No constructor of '" + owner.name.lexeme +
                      "' exactly matches argument types (" + argumentsSpelling +
                      ")."
                : "Construction of '" + owner.name.lexeme +
                      "' is ambiguous; multiple constructors exactly match.");
        diagnostic.hints.emplace_back(
            "Constructor calls do not perform implicit conversions; convert "
            "an argument explicitly with syntax such as 'uint64_t(value)'.");
        for (const ConstructorInfo &candidate : owner.constructors) {
          if (candidate.declaration != nullptr) {
            diagnostic.related.push_back(
                {tokenSpan(candidate.declaration->name()),
                 "Candidate: " + constructorSignatureSpelling(candidate)});
          }
        }
        diagnostics.emplace_back(std::move(diagnostic));
      }
      currentType = constructedType;
      return;
    }

    const ViableConstructor &selected = viable.front();
    if (selected.constructor != nullptr &&
        selected.constructor->access == AccessModifier::Private &&
        currentClass != classId) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, paren,
          "Constructor of '" + owner.name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(selected.constructor->declaration->name()),
           "Constructor declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    semanticModel.record(
        construction,
        ResolvedConstructionInfo{
            .constructor =
                selected.constructor == nullptr ? 0 : selected.constructor->id,
            .declaration = selected.constructor == nullptr
                               ? nullptr
                               : selected.constructor->declaration,
            .constructedType = constructedType,
            .parameterTypes = selected.parameterTypes,
            .borrowOrigin =
                selected.constructor != nullptr &&
                        selected.constructor->borrowParameter
                    ? BorrowOriginKind::Argument
                    : (selected.kind != ConstructorKind::Ordinary &&
                               typeTraits(constructedType).containsBorrowedState
                           ? BorrowOriginKind::Argument
                           : BorrowOriginKind::None),
            .borrowArgument = selected.constructor != nullptr &&
                                      selected.constructor->borrowParameter
                                  ? *selected.constructor->borrowParameter
                                  : 0,
            .borrowAccess = selected.constructor == nullptr
                                ? AccessMode::ReadOnly
                                : selected.constructor->borrowAccess,
            .generatedDefault = selected.generatedDefault,
            .kind = selected.kind});
    if (const auto *call = dynamic_cast<const Call *>(&construction)) {
      const ResolvedConstructionInfo *resolved =
          semanticModel.findConstruction(construction);
      const Token token = callableToken(call->callee());
      if (resolved != nullptr) {
        SymbolId symbol = 0;
        if (resolved->declaration != nullptr) {
          symbol = symbolForDeclaration(resolved->declaration->name());
          if (symbol == 0) {
            symbol = recordToolingSymbol(
                resolved->declaration->name(), SymbolKind::Constructor,
                qualifiedName(owner.namespaceScope,
                              owner.name.lexeme + "::" + owner.name.lexeme),
                constructedType, false, true,
                constructorAccess(owner, resolved->declaration));
          }
        } else if (owner.declaration != nullptr) {
          symbol = symbolForDeclaration(owner.declaration->name());
          if (symbol == 0) {
            symbol = recordToolingSymbol(
                owner.declaration->name(),
                owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                                : SymbolKind::Class,
                qualifiedName(owner.namespaceScope, owner.name.lexeme),
                SemanticType::classType(owner.id));
          }
        }
        semanticModel.recordOccurrence(
            {.sourceUnit = currentSourceUnit,
             .span = tokenSpan(token),
             .kind = SemanticOccurrenceKind::SelectedConstruction,
             .symbol = symbol,
             .roles = OccurrenceRole::Reference | OccurrenceRole::Call |
                      OccurrenceRole::TypeUse,
             .name = token.lexeme,
             .type = constructedType,
             .traits = typeTraits(constructedType),
             .constructor = resolved->declaration,
             .selectedConstruction = *resolved});
      }
    }
    currentType = constructedType;
  }

  void analyzeExpectedMemberCall(
      const Get &member, const SemanticType &expected,
      const std::vector<SemanticType> &argumentTypes,
      const ExprList &arguments, const Token &paren) {
    const auto requireArity = [&](std::size_t expectedCount) {
      if (argumentTypes.size() != expectedCount) {
        report(paren, "Expected member called with the wrong number of arguments.");
        return false;
      }
      return true;
    };

    if (member.name().lexeme == "has_value") {
      requireArity(0);
      currentType = SemanticType::Bool;
      return;
    }
    if (member.name().lexeme == "value") {
      requireArity(0);
      currentType = expected.arguments[0];
      return;
    }
    if (member.name().lexeme == "error") {
      requireArity(0);
      currentType = expected.arguments[1];
      return;
    }
    if (member.name().lexeme == "value_or") {
      if (expected.arguments[0] == SemanticType::Void) {
        report(member.name(),
               "expected<void, E> does not provide 'value_or'.");
        currentType = SemanticType::Unknown;
        return;
      }
      if (requireArity(1) && argumentTypes[0] != SemanticType::Unknown &&
          argumentTypes[0] != expected.arguments[0]) {
        report(member.name(),
               "value_or fallback must exactly match the value type.");
      }
      currentType = expected.arguments[0];
    }
  }

  void analyzeArrayMemberCall(const Get &member,
                              const std::vector<SemanticType> &argumentTypes,
                              const Token &paren) {
    if (member.name().lexeme != "size") {
      currentType = SemanticType::Unknown;
      return;
    }
    if (!argumentTypes.empty()) {
      report(paren, "Fixed array 'size' expects no arguments.", "GTI-S2016");
    }
    currentType = SemanticType::UInt64;
  }

  void
  analyzeStringViewMemberCall(const Get &member,
                              const std::vector<SemanticType> &argumentTypes,
                              const Token &paren) {
    if (member.name().lexeme != "size" && member.name().lexeme != "empty") {
      currentType = SemanticType::Unknown;
      return;
    }
    if (!argumentTypes.empty()) {
      report(paren,
             "std::string_view '" + member.name().lexeme +
                 "' expects no arguments.",
             "GTI-S2035");
    }
    currentType = member.name().lexeme == "size" ? SemanticType::UInt64
                                                 : SemanticType::Bool;
  }

  SemanticType analyzeStringViewIndexAfterOperands(
      const ExprPtr &object, const ExprPtr &index,
      const SemanticType &indexType, const Token &) {
    if (indexType != SemanticType::Unknown && !isInteger(indexType)) {
      report(expressionToken(index),
             "std::string_view index must have an integer type, not '" +
                 typeSpelling(indexType) + "'.",
             "GTI-S2035");
    }
    const auto *literal =
        object ? dynamic_cast<const LiteralExpr *>(object.get()) : nullptr;
    const auto *text = literal == nullptr
                           ? nullptr
                           : std::get_if<std::string>(&literal->value());
    if (text != nullptr) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(index.get());
          constant &&
          (constant->negative || constant->magnitude >= text->size())) {
        report(expressionToken(index),
               "String-view index is outside the valid range [0, " +
                   std::to_string(text->size()) + ").",
               "GTI-S2035");
      }
    }
    return SemanticType::Char;
  }

  SemanticType analyzeArrayIndexAfterOperands(const SemanticType &objectType,
                                              const ExprPtr &index,
                                              const SemanticType &indexType,
                                              const Token &bracket) {
    if (objectType != SemanticType::Unknown &&
        (objectType.kind != SemanticType::Array ||
         objectType.arguments.size() != 1)) {
      report(bracket, "Indexing requires a fixed array value.", "GTI-S2016");
      return SemanticType::Unknown;
    }
    if (indexType != SemanticType::Unknown && !isInteger(indexType)) {
      report(expressionToken(index),
             "Fixed array index must have an integer type, not '" +
                 typeSpelling(indexType) + "'.",
             "GTI-S2016");
    }
    if (objectType.kind != SemanticType::Array ||
        objectType.arguments.size() != 1) {
      return SemanticType::Unknown;
    }
    if (objectType.arrayLengthParameterId == 0) {
      if (const std::optional<IntegerConstant> constant =
            integerConstant(index.get());
          constant && (constant->negative ||
                       constant->magnitude >= objectType.arrayLength)) {
        report(expressionToken(index),
               "Fixed array index is outside the valid range [0, " +
                   std::to_string(objectType.arrayLength) + ").",
               "GTI-S2016");
      }
    }
    return objectType.arguments[0];
  }

  [[nodiscard]] bool isDefaultInitializable(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::StringView:
    case SemanticType::TypeParameter:
      return true;
    case SemanticType::Array:
      return (type.arrayLengthParameterId == 0 && type.arrayLength == 0) ||
             (type.arguments.size() == 1 &&
              isDefaultInitializable(type.arguments[0]));
    case SemanticType::Class: {
      const ClassInfo *owner = classInfo(type);
      if (owner == nullptr || owner->abstract) {
        return false;
      }
      if (const ConstructorInfo *constructor = defaultConstructor(*owner)) {
        return constructor->access == AccessModifier::Public ||
               currentClass == owner->id;
      }
      return classCanGenerateDefaultConstructor(*owner);
    }
    default:
      return false;
    }
  }

  [[nodiscard]] static std::size_t genericTypeParameterCount(
      const std::vector<GenericParameterInfo> &parameters) {
    return static_cast<std::size_t>(std::count_if(
        parameters.begin(), parameters.end(),
        [](const GenericParameterInfo &parameter) { return !parameter.value; }));
  }

  [[nodiscard]] static std::size_t genericValueParameterCount(
      const std::vector<GenericParameterInfo> &parameters) {
    return parameters.size() - genericTypeParameterCount(parameters);
  }

  [[nodiscard]] std::optional<CompileTimeValue>
  genericValueArgument(const TypeRef &argument) const {
    if (argument.name.segments.size() != 1 || !argument.arguments.empty() ||
        !argument.arrayExtents.empty() || argument.reference) {
      return std::nullopt;
    }
    const Token &token = argument.name.last();
    if (token.kind == TokenKind::INT_LITERAL) {
      if (const auto *value = std::get_if<std::uint64_t>(&token.literal)) {
        return CompileTimeValue::uint64(*value);
      }
      return std::nullopt;
    }
    if (token.kind == TokenKind::IDENTIFIER) {
      return resolveValueParameter(token);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<CompileTimeValue>
  resolvedArrayExtent(const ArrayExtentExprPtr &extent) const {
    if (!extent) {
      return std::nullopt;
    }
    const ArrayExtentEvaluation evaluation = evaluateArrayExtent(*extent);
    if (evaluation.value) {
      return CompileTimeValue::uint64(*evaluation.value);
    }
    if (extent->isAtom() && extent->token.kind == TokenKind::IDENTIFIER) {
      return resolveValueParameter(extent->token);
    }
    return std::nullopt;
  }

  void validateArrayExtent(const ArrayExtentExprPtr &extent) {
    if (!extent) {
      return;
    }
    const ArrayExtentEvaluation evaluation = evaluateArrayExtent(*extent);
    if (evaluation.value) {
      semanticModel.record(*extent,
                           CompileTimeValue::uint64(*evaluation.value));
      return;
    }

    const Token &location =
        evaluation.token == nullptr ? extent->token : *evaluation.token;
    if (evaluation.error == ArrayExtentEvaluationError::NonLiteral &&
        location.kind == TokenKind::IDENTIFIER) {
      const std::optional<CompileTimeValue> value =
          resolveValueParameter(location);
      if (value) {
        recordGenericParameterUse(location, *value, OccurrenceRole::Read);
        if (extent->isAtom()) {
          semanticModel.record(*extent, *value);
          return;
        }
        report(location,
               "Array extent arithmetic currently requires integer literals; "
               "a uint64_t value generic parameter may only be used as the "
               "complete extent.",
               "GTI-S2026");
        return;
      }
      report(location,
             "Fixed array extent '" + location.lexeme +
                 "' is not an in-scope uint64_t value generic parameter.",
             "GTI-S2026");
      return;
    }

    switch (evaluation.error) {
    case ArrayExtentEvaluationError::Overflow:
      report(location, "Fixed array extent arithmetic overflows uint64_t.",
             "GTI-S2026");
      return;
    case ArrayExtentEvaluationError::Underflow:
      report(location,
             "Fixed array extent arithmetic cannot produce a negative value.",
             "GTI-S2026");
      return;
    case ArrayExtentEvaluationError::ZeroDivisor:
      report(location,
             "Fixed array extent arithmetic cannot divide or take modulo by "
             "zero.",
             "GTI-S2026");
      return;
    case ArrayExtentEvaluationError::None:
    case ArrayExtentEvaluationError::NonLiteral:
      report(location,
             "Fixed array extent must be an integer constant expression or "
             "uint64_t value generic parameter.",
             "GTI-S2026");
      return;
    }
  }

  void validateType(const TypeRef &type) {
    if (type.name.last().kind == TokenKind::AUTO) {
      report(type.name.last(),
             "'auto' is only valid for an initialized local binding.",
             "GTI-S2028");
      return;
    }
    recordTypeUse(type);
    for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
      validateArrayExtent(extent);
    }
    if (!allowPackTypeReference && isActiveTypePack(type)) {
      report(type.name.last(),
             "A generic type pack can only appear in its matching final "
             "parameter-pack declaration.",
             "GTI-S2023");
    }
    if (isGtiInternalUniqueOwner(type)) {
      if (type.arguments.size() != 1) {
        report(type.name.last(),
               "gti_internal::unique_owner<T> requires exactly one pointee "
               "type.",
               "GTI-S2018");
      } else {
        if (type.arguments[0].genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
          report(type.arguments[0].name.last(),
                 "gti_internal::unique_owner requires a type argument.",
                 "GTI-S2026");
          return;
        }
        validateType(type.arguments[0]);
        const SemanticType pointee = typeOf(type.arguments[0]);
        if (pointee == SemanticType::Void ||
            pointee.kind == SemanticType::Reference ||
            pointee.kind == SemanticType::Array) {
          report(type.arguments[0].name.last(),
                 "Compiler-private unique ownership requires a concrete "
                 "non-reference object type.",
                 "GTI-S2018");
        }
      }
      if (!type.arrayExtents.empty()) {
        report(type.name.last(),
               "Fixed arrays of compiler-private unique owners are not "
               "supported.",
               "GTI-S2018");
      }
      return;
    }
    if (isGtiInternalStorage(type)) {
      if (type.arguments.size() != 1) {
        report(type.name.last(),
               "gti_internal::storage<T> requires exactly one element type.",
               "GTI-S2019");
      } else {
        if (type.arguments.front().genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
          report(type.arguments.front().name.last(),
                 "gti_internal::storage requires a type argument.",
                 "GTI-S2026");
          return;
        }
        validateType(type.arguments.front());
        if (!isStorageElementType(typeOf(type.arguments.front()))) {
          report(type.arguments.front().name.last(),
                 "Compiler-private storage requires a concrete value element "
                 "type.",
                 "GTI-S2019");
        }
      }
      if (!type.arrayExtents.empty()) {
        report(type.name.last(),
               "Fixed arrays of compiler-private storage are not supported.",
               "GTI-S2019");
      }
      return;
    }
    if (isGtiInternalTextView(type)) {
      if (!type.arguments.empty()) {
        report(type.name.last(),
               "gti_internal::text_view does not take generic arguments.",
               "GTI-S2034");
      }
      return;
    }
    if (!type.arrayExtents.empty() &&
        baseTypeOf(type, currentNamespace) == SemanticType::Void) {
      report(type.name.last(), "Fixed array elements cannot have type void.",
             "GTI-S2015");
    }
    if (type.name.last().kind == TokenKind::EXPECTED &&
        (type.arguments.size() != 2 ||
         typeOf(type.arguments[1]) == SemanticType::Void)) {
      report(type.name.last(),
             "expected<T, E> requires a non-void error type.");
    }
    if (type.name.last().kind == TokenKind::EXPECTED) {
      for (const TypeRef &argument : type.arguments) {
        validateType(argument);
      }
    }
    if (type.name.last().kind != TokenKind::IDENTIFIER) {
      return;
    }
    if (resolveTypeParameter(type.name)) {
      if (!type.arguments.empty()) {
        report(type.name.last(),
               "Generic type parameters cannot take generic arguments.");
      }
      return;
    }

    if (resolveTypeAliasPath(type.name, currentNamespace)) {
      if (!type.arguments.empty()) {
        report(type.name.last(),
               "Type alias '" + pathSpelling(type.name) +
                   "' does not take generic arguments.",
               "GTI-S2030");
        for (const TypeRef &argument : type.arguments) {
          if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
            validateType(argument);
          }
        }
      }
      return;
    }

    if (resolveEnumPath(type.name, currentNamespace)) {
      if (!type.arguments.empty()) {
        report(type.name.last(),
               "Scoped enum type '" + pathSpelling(type.name) +
                   "' does not take generic arguments.",
               "GTI-S2036");
      }
      return;
    }

    const std::optional<ClassId> classId =
        resolveClassPath(type.name, currentNamespace);
    if (!classId) {
      if (type.name.segments.size() == 1 &&
          type.name.last().lexeme == "string") {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2033", DiagnosticPhase::Semantics, type.name.last(),
            "'string' is no longer a built-in type; use 'std::string_view' "
            "for non-owning text.");
        diagnostic.fixes.push_back(
            {tokenSpan(type.name.last()), "std::string_view",
             "Replace 'string' with 'std::string_view'."});
        diagnostic.hints.push_back(
            "An owning std::string type is not available yet.");
        diagnostics.emplace_back(std::move(diagnostic));
        return;
      }
      if (const std::optional<TypeAliasId> globalAlias =
              resolveTypeAliasPathGlobally(type.name, currentNamespace)) {
        const RegisteredTypeAlias &declaration = typeAliases[*globalAlias - 1];
        if (reportInvisibleDeclaration(
                type.name.last(), pathSpelling(type.name),
                declaration.declaration->name(), declaration.sourceUnit)) {
          return;
        }
      }
      const std::optional<ClassId> globalClass =
          resolveClassPathGlobally(type.name, currentNamespace);
      if (globalClass) {
        const ClassInfo &declaration = classInfo(*globalClass);
        if (reportInvisibleDeclaration(
                type.name.last(), pathSpelling(type.name), declaration.name,
                declaration.sourceUnit)) {
          return;
        }
      }
      const std::optional<EnumId> globalEnum =
          resolveEnumPathGlobally(type.name, currentNamespace);
      if (globalEnum && *globalEnum != 0 && *globalEnum <= enums.size()) {
        const EnumInfo &declaration = enums[*globalEnum - 1];
        if (reportInvisibleDeclaration(
                type.name.last(), pathSpelling(type.name), declaration.name,
                declaration.sourceUnit)) {
          return;
        }
      }
      report(type.name.last(),
             "Unknown type '" + pathSpelling(type.name) + "'.");
      return;
    }
    const ClassInfo &declaration = classInfo(*classId);
    const std::size_t expectedArguments = declaration.genericParameters.size();
    if (type.arguments.size() != expectedArguments) {
      const bool typeOnly =
          genericValueParameterCount(declaration.genericParameters) == 0;
      report(type.name.last(),
             "Type '" + pathSpelling(type.name) + "' requires " +
                 std::to_string(expectedArguments) +
                 (typeOnly ? " generic type argument"
                           : " generic argument") +
                 (expectedArguments == 1 ? "." : "s."));
    }
    const std::size_t count =
        std::min(type.arguments.size(), declaration.genericParameters.size());
    for (std::size_t index = 0; index < count; ++index) {
      const GenericParameterInfo &parameter =
          declaration.genericParameters[index];
      const TypeRef &argument = type.arguments[index];
      if (parameter.value) {
        const std::optional<CompileTimeValue> value =
            genericValueArgument(argument);
        if (!value) {
          report(argument.name.last(),
                 "Generic parameter '" + parameter.name.lexeme +
                     "' requires a uint64_t compile-time value.",
                 "GTI-S2026");
        } else if (argument.name.last().kind == TokenKind::IDENTIFIER) {
          recordGenericParameterUse(argument.name.last(), *value,
                                    OccurrenceRole::Read);
        }
        continue;
      }
      if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
        report(argument.name.last(),
               "Generic parameter '" + parameter.name.lexeme +
                   "' requires a type argument.",
               "GTI-S2026");
        continue;
      }
      validateType(argument);
      const SemanticType argumentType = typeOf(argument);
      if (argumentType == SemanticType::Void) {
        report(argument.name.last(), "Generic type arguments cannot be void.");
      } else if (!satisfiesConstraint(argumentType, parameter.constraint)) {
        reportConstraintFailure(
            argument.name.last(),
            ConstraintFailure{.parameter = parameter.name,
                              .argument = argumentType,
                              .constraint = parameter.constraint,
                              .constraintName = parameter.constraintName});
      }
    }
    for (std::size_t index = count; index < type.arguments.size(); ++index) {
      const TypeRef &argument = type.arguments[index];
      if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
        validateType(argument);
      }
    }
  }

  [[nodiscard]] static bool isGtiInternalUniqueOwner(const TypeRef &type) {
    return type.name.segments.size() == 2 &&
           type.name.segments[0].lexeme == "gti_internal" &&
           type.name.segments[1].lexeme == "unique_owner";
  }

  [[nodiscard]] static bool isGtiInternalStorage(const TypeRef &type) {
    return type.name.segments.size() == 2 &&
           type.name.segments[0].lexeme == "gti_internal" &&
           type.name.segments[1].lexeme == "storage";
  }

  [[nodiscard]] static bool isGtiInternalTextView(const TypeRef &type) {
    return type.name.segments.size() == 2 &&
           type.name.segments[0].lexeme == "gti_internal" &&
           type.name.segments[1].lexeme == "text_view";
  }

  [[nodiscard]] static bool containsReference(const TypeRef &type) {
    if (type.reference) {
      return true;
    }
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [](const TypeRef &argument) { return containsReference(argument); });
  }

  [[nodiscard]] bool isActiveTypePack(const TypeRef &type) const {
    const std::optional<SemanticType> parameter =
        resolveTypeParameter(type.name);
    if (!parameter || parameter->kind != SemanticType::TypeParameter) {
      return false;
    }
    return std::any_of(typePackScopes.rbegin(), typePackScopes.rend(),
                       [&](const auto &scope) {
                         return scope.contains(parameter->genericParameterId);
                       });
  }

  void validateReferencePlacement(const TypeRef &type, bool allowTopLevel,
                                  std::string_view context,
                                  bool allowRvalueReference = false) {
    if (type.reference) {
      if (type.reference->kind == TokenKind::AND && !allowRvalueReference) {
        report(*type.reference,
               "'&&' is currently confined to a class or struct's exact "
               "move constructor policy.",
               "GTI-S2020");
      }
      if (!allowTopLevel) {
        report(*type.reference,
               "References cannot be used as a " + std::string(context) +
                   " yet.",
               "GTI-S2017");
      }
      if (!type.arrayExtents.empty()) {
        report(*type.reference,
               "References to fixed arrays are not supported yet.",
               "GTI-S2017");
      }
      if (baseTypeOf(type, currentNamespace) == SemanticType::Void) {
        report(*type.reference, "References cannot refer to void.",
               "GTI-S2017");
      }
      if (isDirectOwnerType(baseTypeOf(type, currentNamespace))) {
        const bool unique = baseTypeOf(type, currentNamespace).kind ==
                            SemanticType::UniqueOwner;
        report(
            *type.reference,
            unique
                ? "References to compiler-private unique owners are not "
                  "supported; borrow the owned value instead."
                : "References to compiler-private storage are not supported; "
                  "use storage operations instead.",
            "GTI-S2018");
      }
    }
    for (const TypeRef &argument : type.arguments) {
      if (containsReference(argument)) {
        report(argument.reference ? *argument.reference : argument.name.last(),
               "References cannot be nested inside another type yet.",
               "GTI-S2017");
      }
    }
  }

  [[nodiscard]] bool
  isPublicBaseConversion(const SemanticType &derived, const SemanticType &base,
                         std::unordered_set<ClassId> &visited) const {
    if (derived.kind != SemanticType::Class ||
        base.kind != SemanticType::Class || derived.classId == 0 ||
        base.classId == 0 || !visited.insert(derived.classId).second) {
      return false;
    }
    const ClassInfo *owner = classInfo(derived);
    if (owner == nullptr) {
      return false;
    }
    const GenericSubstitution substitution = classSubstitution(derived);
    for (const ClassBaseTypeInfo &declaredBase : owner->bases) {
      const SemanticType actualBase =
          substituteType(declaredBase.type, substitution);
      if (actualBase == base ||
          isPublicBaseConversion(actualBase, base, visited)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool isPublicBaseConversion(const SemanticType &derived,
                                            const SemanticType &base) const {
    std::unordered_set<ClassId> visited;
    return isPublicBaseConversion(derived, base, visited);
  }

  void validateReferenceBinding(const SemanticType &reference,
                                const SemanticType &initializerType,
                                const ExprPtr &initializer) {
    if (reference.arguments.size() != 1) {
      return;
    }
    const SemanticType &referent = reference.arguments[0];
    if (initializerType != SemanticType::Unknown &&
        initializerType != referent &&
        !isPublicBaseConversion(initializerType, referent)) {
      report(expressionToken(initializer),
             "Reference requires an exact '" + typeSpelling(referent) +
                 "' initializer, but received '" +
                 typeSpelling(initializerType) + "'.",
             "GTI-S2017");
      return;
    }
    const ExpressionInfo *info =
        initializer ? semanticModel.findExpression(*initializer) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place) {
      report(expressionToken(initializer),
             "Reference initializer must be an addressable value, not a "
             "temporary.",
             "GTI-S2017");
      return;
    }
    if (!hasStableBorrowStorage(initializer)) {
      report(expressionToken(initializer),
             "Reference initializer is derived from temporary storage that "
             "does not outlive the binding.",
             "GTI-S2017");
      return;
    }
    if (reference.referenceAccess == AccessMode::Mutable &&
        info->access != AccessMode::Mutable) {
      report(expressionToken(initializer),
             "A mutable reference requires a mutable initializer.",
             "GTI-S2017");
    }
  }

  void validateStoredBorrowInitialization(const SemanticType &type,
                                          const ExprPtr &initializer) {
    if (!isDirectStoredReferenceType(type)) {
      report(expressionToken(initializer),
             "Borrowed state cannot be nested inside another stored value in "
             "the current lifetime model.",
             "GTI-S2045");
      return;
    }
    if (!hasStableBorrowStorage(initializer)) {
      report(expressionToken(initializer),
             "Stored-reference value is derived from temporary storage that "
             "does not outlive the binding.",
             "GTI-S2045");
    }
  }

  void validateStoredBorrowReturn(const SemanticType &returnType,
                                  const SemanticType &valueType,
                                  const ExprPtr &value) {
    if (!isDirectStoredReferenceType(returnType)) {
      report(expressionToken(value),
             "A return type cannot nest borrowed state in the current "
             "lifetime model.",
             "GTI-S2045");
      return;
    }
    if (valueType != SemanticType::Unknown && valueType != returnType) {
      return;
    }
    if (!currentClass || currentStaticMemberFunction ||
        !isReceiverDerivedBorrow(value)) {
      report(expressionToken(value),
             "Stored-reference method returns must borrow from 'this'.",
             "GTI-S2045");
    }
  }

  void validateReferenceReturn(const SemanticType &reference,
                               const SemanticType &valueType,
                               const ExprPtr &value) {
    if (reference.arguments.size() != 1) {
      return;
    }
    const SemanticType &referent = reference.arguments[0];
    if (valueType != SemanticType::Unknown && valueType != referent &&
        !isPublicBaseConversion(valueType, referent)) {
      report(expressionToken(value),
             "Reference return requires an exact '" + typeSpelling(referent) +
                 "' value, but received '" + typeSpelling(valueType) + "'.",
             "GTI-S2017");
      return;
    }
    const ExpressionInfo *info =
        value ? semanticModel.findExpression(*value) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place) {
      report(expressionToken(value),
             "Reference return requires an addressable value, not a "
             "temporary.",
             "GTI-S2017");
      return;
    }
    if (reference.referenceAccess == AccessMode::Mutable &&
        info->access != AccessMode::Mutable) {
      report(expressionToken(value),
             "Mutable reference return requires a mutable value.", "GTI-S2017");
      return;
    }
    if (!isReceiverDerivedBorrow(value)) {
      report(expressionToken(value),
             "Method reference returns must borrow from 'this'.", "GTI-S2017");
    }
  }

  [[nodiscard]] bool isReceiverDerivedBorrow(const ExprPtr &expression) const {
    std::unordered_set<const Expr *> visiting;
    return isReceiverDerivedBorrow(expression, visiting);
  }

  [[nodiscard]] bool
  isReceiverDerivedBorrow(const ExprPtr &expression,
                          std::unordered_set<const Expr *> &visiting) const {
    if (!expression || !visiting.insert(expression.get()).second) {
      return false;
    }
    if (dynamic_cast<const This *>(expression.get()) != nullptr) {
      return true;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return isReceiverDerivedBorrow(grouping->expression(), visiting);
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expression.get());
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return isReceiverDerivedBorrow(binary->right(), visiting);
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return isReceiverDerivedBorrow(index->object(), visiting);
    }
    if (const auto *member = dynamic_cast<const Get *>(expression.get())) {
      return isReceiverDerivedBorrow(member->object(), visiting);
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expression.get());
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      return isReceiverDerivedBorrow(unary->right(), visiting);
    }
    if (const auto *call = dynamic_cast<const Call *>(expression.get())) {
      const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
      if (resolved != nullptr &&
          resolved->borrowOrigin == BorrowOriginKind::Receiver &&
          dynamic_cast<const Get *>(call->callee().get()) == nullptr) {
        return currentClass.has_value();
      }
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return isReceiverDerivedBorrow(*source, visiting);
    }
    return false;
  }

  [[nodiscard]] bool hasStableBorrowStorage(const ExprPtr &expression) const {
    std::unordered_set<const Expr *> visiting;
    return hasStableBorrowStorage(expression, visiting);
  }

  [[nodiscard]] bool
  hasStableBorrowStorage(const ExprPtr &expression,
                         std::unordered_set<const Expr *> &visiting) const {
    if (!expression || !visiting.insert(expression.get()).second) {
      return false;
    }
    if (dynamic_cast<const Variable *>(expression.get()) != nullptr) {
      if (const ExprPtr *source = storedBorrowSource(expression)) {
        return hasStableBorrowStorage(*source, visiting);
      }
      return true;
    }
    if (dynamic_cast<const QualifiedName *>(expression.get()) != nullptr ||
        dynamic_cast<const This *>(expression.get()) != nullptr) {
      return true;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return hasStableBorrowStorage(grouping->expression(), visiting);
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expression.get());
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return hasStableBorrowStorage(binary->right(), visiting);
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return hasStableBorrowStorage(index->object(), visiting);
    }
    if (const auto *member = dynamic_cast<const Get *>(expression.get())) {
      return hasStableBorrowStorage(member->object(), visiting);
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expression.get());
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      const ExpressionInfo *owner =
          semanticModel.findExpression(*unary->right());
      return owner != nullptr && owner->category == ValueCategory::Place;
    }
    if (const auto *call = dynamic_cast<const Call *>(expression.get())) {
      const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
      if (resolved != nullptr &&
          resolved->borrowOrigin == BorrowOriginKind::Receiver &&
          dynamic_cast<const Get *>(call->callee().get()) == nullptr) {
        return currentClass.has_value();
      }
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return hasStableBorrowStorage(*source, visiting);
    }
    return false;
  }

  static std::string qualifiedName(const std::vector<std::string> &scope,
                                   std::string_view name) {
    std::string result;
    for (const std::string &segment : scope) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment;
    }
    if (!result.empty()) {
      result += "::";
    }
    result += name;
    return result;
  }

  static std::string pathSpelling(const NamePath &path) {
    std::string result;
    for (const Token &segment : path.segments) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment.lexeme;
    }
    return result;
  }

  [[nodiscard]] static std::optional<GenericConstraintKind>
  standardConstraint(const NamePath &name) {
    if (name.segments.size() != 2 || name.segments[0].lexeme != "std") {
      return std::nullopt;
    }
    const std::string &constraint = name.segments[1].lexeme;
    if (constraint == "ordered") {
      return GenericConstraintKind::Ordered;
    }
    if (constraint == "numeric") {
      return GenericConstraintKind::Numeric;
    }
    if (constraint == "signed_numeric") {
      return GenericConstraintKind::SignedNumeric;
    }
    if (constraint == "integral") {
      return GenericConstraintKind::Integral;
    }
    if (constraint == "signed_integral") {
      return GenericConstraintKind::SignedIntegral;
    }
    if (constraint == "unsigned_integral") {
      return GenericConstraintKind::UnsignedIntegral;
    }
    if (constraint == "floating_point") {
      return GenericConstraintKind::FloatingPoint;
    }
    return std::nullopt;
  }

  std::vector<GenericParameterInfo>
  makeGenericParameters(const std::vector<GenericParameter> &parameters,
                        const Token &declarationName) {
    std::vector<GenericParameterInfo> result;
    result.reserve(parameters.size());
    std::unordered_set<std::string> names;
    bool sawValueParameter = false;
    for (const GenericParameter &parameter : parameters) {
      if (parameter.name.lexeme == declarationName.lexeme) {
        report(parameter.name, "Generic parameter cannot have the same name "
                               "as its declaration.");
        continue;
      }
      if (!names.insert(parameter.name.lexeme).second) {
        report(parameter.name,
               std::string("Duplicate generic ") +
                   (parameter.valueType ? "value" : "type") +
                   " parameter '" + parameter.name.lexeme + "'.");
        continue;
      }
      const bool valueParameter = parameter.valueType.has_value();
      if (valueParameter) {
        sawValueParameter = true;
        if (parameter.valueType->kind != TokenKind::UINT64) {
          report(*parameter.valueType,
                 "Value generic parameters currently require type uint64_t.",
                 "GTI-S2026");
        }
      } else if (sawValueParameter) {
        report(parameter.name,
               "Generic type parameters must appear before value "
               "parameters.",
               "GTI-S2026");
      }
      GenericConstraintKind constraint = GenericConstraintKind::None;
      if (parameter.constraint) {
        if (valueParameter) {
          report(parameter.constraint->last(),
                 "Generic constraints can only apply to type parameters.",
                 "GTI-S2029");
          constraint = GenericConstraintKind::Invalid;
        } else if (const std::optional<GenericConstraintKind> resolved =
                       standardConstraint(*parameter.constraint)) {
          constraint = *resolved;
        } else {
          report(parameter.constraint->last(),
                 "Unknown generic constraint '" +
                     pathSpelling(*parameter.constraint) +
                     "'. Supported constraints are std::ordered, "
                     "std::numeric, std::signed_numeric, std::integral, "
                     "std::signed_integral, std::unsigned_integral, and "
                     "std::floating_point.",
                 "GTI-S2029");
          constraint = GenericConstraintKind::Invalid;
        }
      }
      const GenericParameterId id = nextGenericParameterId++;
      genericConstraints.insert_or_assign(id, constraint);
      result.push_back(
          GenericParameterInfo{.id = id,
                               .name = parameter.name,
                               .pack = parameter.pack.has_value(),
                               .value = valueParameter,
                               .constraint = constraint,
                               .constraintName = parameter.constraint});
      const SemanticType parameterType = valueParameter
                                             ? SemanticType::UInt64
                                             : SemanticType::typeParameter(id);
      const SymbolId symbol = recordToolingSymbol(
          parameter.name,
          valueParameter ? SymbolKind::ValueParameter
                         : SymbolKind::TypeParameter,
          declarationName.lexeme + "::" + parameter.name.lexeme, parameterType);
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(parameter.name),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbol,
           .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
           .name = parameter.name.lexeme,
           .type = parameterType});
    }
    return result;
  }

  [[nodiscard]] const std::vector<GenericParameterInfo> &
  genericParametersFor(const FunctionDecl &function) const {
    static const std::vector<GenericParameterInfo> empty;
    const auto found = functionGenericParameters.find(&function);
    return found == functionGenericParameters.end() ? empty : found->second;
  }

  void
  beginTypeParameterScope(const std::vector<GenericParameterInfo> &parameters) {
    auto &scope = typeParameterScopes.emplace_back();
    auto &values = valueParameterScopes.emplace_back();
    auto &packs = typePackScopes.emplace_back();
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.value) {
        const auto concrete = instanceValueSubstitution.find(parameter.id);
        values.emplace(parameter.name.lexeme,
                       concrete == instanceValueSubstitution.end()
                           ? CompileTimeValue::parameter(parameter.id)
                           : concrete->second);
        continue;
      }
      const auto concrete = instanceTypeSubstitution.find(parameter.id);
      scope.emplace(parameter.name.lexeme,
                    concrete == instanceTypeSubstitution.end()
                        ? SemanticType::typeParameter(parameter.id)
                        : concrete->second);
      if (parameter.pack) {
        packs.insert(parameter.id);
      }
    }
  }

  void endTypeParameterScope() {
    typeParameterScopes.pop_back();
    valueParameterScopes.pop_back();
    typePackScopes.pop_back();
  }

  [[nodiscard]] std::optional<SemanticType>
  resolveTypeParameter(const NamePath &name) const {
    if (name.segments.size() != 1) {
      return std::nullopt;
    }
    for (auto scope = typeParameterScopes.rbegin();
         scope != typeParameterScopes.rend(); ++scope) {
      if (const auto found = scope->find(name.last().lexeme);
          found != scope->end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<CompileTimeValue>
  resolveValueParameter(const Token &name) const {
    for (auto scope = valueParameterScopes.rbegin();
         scope != valueParameterScopes.rend(); ++scope) {
      if (const auto found = scope->find(name.lexeme); found != scope->end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static SemanticType
  substituteType(const SemanticType &type,
                 const GenericSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.types.find(type.genericParameterId);
      return found == substitution.types.end() ? type : found->second;
    }

    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substituteType(argument, substitution);
    }
    for (CompileTimeValue &argument : result.valueArguments) {
      if (argument.kind != CompileTimeValue::Parameter) {
        continue;
      }
      const auto found = substitution.values.find(argument.parameterId);
      if (found != substitution.values.end()) {
        argument = found->second;
      }
    }
    if (result.arrayLengthParameterId != 0) {
      const auto found =
          substitution.values.find(result.arrayLengthParameterId);
      if (found != substitution.values.end() &&
          found->second.kind == CompileTimeValue::UInt64) {
        result.arrayLength = found->second.value;
        result.arrayLengthParameterId = 0;
      }
    }
    return result;
  }

  [[nodiscard]] static SemanticType
  substituteType(const SemanticType &type,
                 const TypeSubstitution &substitution) {
    return substituteType(type,
                          GenericSubstitution{.types = substitution});
  }

  [[nodiscard]] GenericSubstitution
  classSubstitution(const SemanticType &objectType) const {
    GenericSubstitution result;
    if (objectType.kind != SemanticType::Class || objectType.classId == 0 ||
        objectType.classId > classes.size()) {
      return result;
    }
    const ClassInfo &owner = classInfo(objectType.classId);
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter : owner.genericParameters) {
      if (parameter.value) {
        if (valueIndex < objectType.valueArguments.size()) {
          result.values.emplace(parameter.id,
                                objectType.valueArguments[valueIndex++]);
        }
      } else if (typeIndex < objectType.arguments.size()) {
        result.types.emplace(parameter.id, objectType.arguments[typeIndex++]);
      }
    }
    return result;
  }

  [[nodiscard]] SemanticType openClassType(ClassId id) const {
    const ClassInfo &owner = classInfo(id);
    std::vector<SemanticType> arguments;
    std::vector<CompileTimeValue> valueArguments;
    arguments.reserve(genericTypeParameterCount(owner.genericParameters));
    valueArguments.reserve(genericValueParameterCount(owner.genericParameters));
    for (const GenericParameterInfo &parameter : owner.genericParameters) {
      if (parameter.value) {
        const auto concrete = instanceValueSubstitution.find(parameter.id);
        valueArguments.emplace_back(
            concrete == instanceValueSubstitution.end()
                ? CompileTimeValue::parameter(parameter.id)
                : concrete->second);
        continue;
      }
      const auto concrete = instanceTypeSubstitution.find(parameter.id);
      arguments.emplace_back(concrete == instanceTypeSubstitution.end()
                                 ? SemanticType::typeParameter(parameter.id)
                                 : concrete->second);
    }
    return SemanticType::classType(id, std::move(arguments),
                                   std::move(valueArguments));
  }

  void prepareInstanceAnalysis() {
    diagnostics.clear();
    scopes.clear();
    typeParameterScopes.clear();
    valueParameterScopes.clear();
    typePackScopes.clear();
    currentNamespace.clear();
    currentSourceUnit = 0;
    currentClass.reset();
    instanceTypeSubstitution.clear();
    instanceValueSubstitution.clear();
    instanceClassContextActive = false;
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    analyzingCallCallee = false;
    currentStaticMemberFunction = false;
    allowPackTypeReference = false;
    receiverStorageBorrowed = false;
    contextualInitializerType.reset();
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    destructorDepth = 0;
    functionDepth = 0;
    loopDepth = 0;
    switchDepth = 0;
    lambdaDepth = 0;
    lambdaUncapturedLocals.clear();
    currentType = SemanticType::Unknown;
    currentReturnType = SemanticType::Unknown;
  }

  bool
  prepareClassInstanceContext(ClassId classId,
                              const std::vector<SemanticType> &typeArguments,
                              const std::vector<CompileTimeValue>
                                  &valueArguments) {
    if (classId == 0 || classId > classes.size()) {
      return false;
    }
    const ClassInfo &owner = classInfo(classId);
    if (typeArguments.size() !=
            genericTypeParameterCount(owner.genericParameters) ||
        valueArguments.size() !=
            genericValueParameterCount(owner.genericParameters)) {
      return false;
    }
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter : owner.genericParameters) {
      if (parameter.value) {
        instanceValueSubstitution.insert_or_assign(
            parameter.id, valueArguments[valueIndex++]);
      } else {
        instanceTypeSubstitution.insert_or_assign(
            parameter.id, typeArguments[typeIndex++]);
      }
    }
    currentClass = classId;
    currentNamespace = owner.namespaceScope;
    currentSourceUnit = owner.sourceUnit;
    beginTypeParameterScope(owner.genericParameters);
    beginScope();
    const SemanticType concreteClass =
        SemanticType::classType(classId, typeArguments, valueArguments);
    for (const auto &[name, member] : owner.members) {
      scopes.back().emplace(name,
                            substituteSymbol(member.symbol, concreteClass));
    }
    instanceClassContextActive = true;
    return true;
  }

  bool prepareInstanceContext(
      const FunctionInfo &function,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments) {
    if (function.ownerClass != 0 &&
        !prepareClassInstanceContext(function.ownerClass, classTypeArguments,
                                     classValueArguments)) {
      return false;
    }
    if (function.ownerClass == 0) {
      currentNamespace = function.namespaceScope;
      currentSourceUnit = function.sourceUnit;
    }

    const std::size_t fixedGenericCount =
        function.parameterPack && !function.genericParameters.empty()
            ? function.genericParameters.size() - 1
            : function.genericParameters.size();
    if (functionTypeArguments.size() < fixedGenericCount ||
        (!function.parameterPack &&
         functionTypeArguments.size() != fixedGenericCount)) {
      return false;
    }
    for (std::size_t index = 0; index < fixedGenericCount; ++index) {
      instanceTypeSubstitution.insert_or_assign(
          function.genericParameters[index].id, functionTypeArguments[index]);
    }
    if (function.parameterPack && !function.genericParameters.empty()) {
      const GenericParameterInfo &pack = function.genericParameters.back();
      std::vector<SemanticType> elements(
          functionTypeArguments.begin() +
              static_cast<std::ptrdiff_t>(fixedGenericCount),
          functionTypeArguments.end());
      instanceTypeSubstitution.insert_or_assign(
          pack.id,
          SemanticType::concreteTypePack(pack.id, std::move(elements)));
    }
    return true;
  }

  void finishClassInstanceContext() {
    if (!instanceClassContextActive) {
      return;
    }
    endScope();
    endTypeParameterScope();
    instanceClassContextActive = false;
    currentClass.reset();
  }

  void finishInstanceContext(const FunctionInfo &function) {
    if (function.ownerClass != 0) {
      finishClassInstanceContext();
    }
  }

  [[nodiscard]] Symbol substituteSymbol(const Symbol &symbol,
                                        const SemanticType &objectType) const {
    Symbol result = symbol;
    const GenericSubstitution substitution = classSubstitution(objectType);
    result.type = substituteType(result.type, substitution);
    for (FunctionCandidate &overload : result.overloads) {
      overload.returnType = substituteType(overload.returnType, substitution);
      for (SemanticType &parameter : overload.parameterTypes) {
        parameter = substituteType(parameter, substitution);
      }
      overload.dispatchOwner =
          substituteType(overload.dispatchOwner, substitution);
    }
    return result;
  }

  Symbol functionSymbol(const FunctionDecl &function,
                        const std::vector<std::string> &scope) {
    const std::vector<GenericParameterInfo> &genericParameters =
        genericParametersFor(function);
    const FunctionInfo *registered = semanticModel.findFunction(function);
    beginTypeParameterScope(genericParameters);
    FunctionCandidate candidate{
        .sourceUnit = currentSourceUnit,
        .declaration = &function,
        .returnType =
            typeOf(function.returnType(), function.returnMutability(), scope),
        .genericParameters = genericParameters,
        .parameterPack = !function.parameters().empty() &&
                         function.parameters().back().pack.has_value(),
        .receiverMutability = function.receiverMutability()};
    if (registered != nullptr) {
      candidate.id = registered->id;
      candidate.staticMember = registered->staticMember;
      candidate.internalLinkage = registered->internalLinkage;
    }
    candidate.parameterTypes.reserve(function.parameters().size());
    for (const Parameter &parameter : function.parameters()) {
      candidate.parameterTypes.emplace_back(typeOf(parameter, scope));
    }
    Symbol symbol{.type = SemanticType::Function,
                  .sourceUnit = currentSourceUnit,
                  .assignable = false,
                  .overloads = {std::move(candidate)},
                  .declaration = function.name(),
                  .staticMember =
                      registered != nullptr && registered->staticMember,
                  .internalLinkage =
                      registered != nullptr && registered->internalLinkage};
    endTypeParameterScope();
    return symbol;
  }

  void validateFunctionPacks(
      const FunctionDecl &function,
      const std::vector<GenericParameterInfo> &genericParameters) {
    const GenericParameterInfo *pack = nullptr;
    for (std::size_t index = 0; index < genericParameters.size(); ++index) {
      if (!genericParameters[index].pack) {
        continue;
      }
      if (pack != nullptr) {
        report(genericParameters[index].name,
               "A function may declare only one generic type pack.",
               "GTI-S2023");
      }
      pack = &genericParameters[index];
      if (index + 1 != genericParameters.size()) {
        report(genericParameters[index].name,
               "A generic type pack must be the final generic parameter.",
               "GTI-S2023");
      }
    }

    const auto containsPack = [&](const SemanticType &type,
                                  const auto &self) -> bool {
      if (pack != nullptr &&
          (type.kind == SemanticType::TypeParameter ||
           type.kind == SemanticType::TypePack) &&
          type.genericParameterId == pack->id) {
        return true;
      }
      return std::any_of(
          type.arguments.begin(), type.arguments.end(),
          [&](const SemanticType &argument) { return self(argument, self); });
    };

    if (containsPack(typeOf(function.returnType()), containsPack)) {
      report(function.returnType().name.last(),
             "A generic type pack cannot be used as a function return type.",
             "GTI-S2023");
    }

    const Parameter *declaredPack = nullptr;
    for (std::size_t index = 0; index < function.parameters().size(); ++index) {
      const Parameter &parameter = function.parameters()[index];
      const SemanticType parameterType = typeOf(parameter);
      if (!parameter.pack) {
        if (containsPack(parameterType, containsPack)) {
          report(parameter.type.name.last(),
                 "A generic type pack must be expanded as a function "
                 "parameter with '...'.",
                 "GTI-S2023");
        }
        continue;
      }

      if (declaredPack != nullptr) {
        report(*parameter.pack,
               "A function may declare only one parameter pack.", "GTI-S2023");
      }
      declaredPack = &parameter;
      if (index + 1 != function.parameters().size()) {
        report(*parameter.pack,
               "A parameter pack must be the final function parameter.",
               "GTI-S2023");
      }
      if (parameter.name.lexeme.empty()) {
        report(*parameter.pack, "A parameter pack requires a name.",
               "GTI-S2023");
      }
      if (parameter.mutability == Mutability::Mutable) {
        report(*parameter.pack,
               "Parameter packs are immutable; move individual values only "
               "after a future pack iteration feature exists.",
               "GTI-S2023");
      }
      if (parameter.type.reference || !parameter.type.arrayExtents.empty()) {
        report(*parameter.pack,
               "The first variadic layer supports only by-value parameter "
               "packs.",
               "GTI-S2023");
      }
      if (pack == nullptr ||
          (parameterType.kind != SemanticType::TypeParameter &&
           parameterType.kind != SemanticType::TypePack) ||
          parameterType.genericParameterId != pack->id) {
        report(*parameter.pack,
               "A parameter pack type must name the function's generic type "
               "pack.",
               "GTI-S2023");
      }
    }

    if (pack != nullptr && declaredPack == nullptr) {
      report(pack->name,
             "A generic type pack must be bound by a final parameter pack.",
             "GTI-S2023");
    } else if (pack == nullptr && declaredPack != nullptr) {
      report(*declaredPack->pack,
             "A parameter pack requires a matching final generic type pack.",
             "GTI-S2023");
    }
  }

  void validateOperatorDeclaration(const FunctionDecl &function,
                                   bool methodDeclaration) {
    if (!function.operatorName()) {
      return;
    }

    const OperatorName &name = *function.operatorName();
    const auto fail = [&](std::string message) {
      report(name.keyword, std::move(message), "GTI-S2022");
    };
    if (!methodDeclaration) {
      fail("Operator overloads can only be declared as class or struct "
           "members.");
      return;
    }
    if (function.isStatic()) {
      fail("Operator overloads require an object receiver and cannot be "
           "static.");
    }
    if (!function.genericParameters().empty()) {
      fail("Operator overloads cannot declare method type parameters.");
    }
    if (function.runtimeBinding()) {
      fail("Operator overloads cannot be runtime bindings.");
    }

    std::optional<std::size_t> expectedArity = 0;
    switch (name.kind) {
    case OverloadedOperator::Subscript:
    case OverloadedOperator::Equal:
    case OverloadedOperator::NotEqual:
      expectedArity = 1;
      break;
    case OverloadedOperator::Call:
      expectedArity = std::nullopt;
      break;
    case OverloadedOperator::Dereference:
    case OverloadedOperator::Arrow:
    case OverloadedOperator::PreIncrement:
    case OverloadedOperator::ContextualBool:
      break;
    }
    if (expectedArity && function.parameters().size() != *expectedArity) {
      fail(std::string(operatorSourceSpelling(name.kind)) + " expects " +
           std::to_string(*expectedArity) + " parameter" +
           (*expectedArity == 1 ? "." : "s."));
    }

    const SemanticType returnType =
        typeOf(function.returnType(), function.returnMutability());
    if ((name.kind == OverloadedOperator::Equal ||
         name.kind == OverloadedOperator::NotEqual ||
         name.kind == OverloadedOperator::ContextualBool) &&
        returnType != SemanticType::Bool) {
      fail(std::string(operatorSourceSpelling(name.kind)) +
           " must return bool.");
    }
    if ((name.kind == OverloadedOperator::Dereference ||
         name.kind == OverloadedOperator::Arrow) &&
        returnType.kind != SemanticType::Reference) {
      fail(std::string(operatorSourceSpelling(name.kind)) +
           " must return a checked reference.");
    }
    if (name.kind == OverloadedOperator::Subscript &&
        returnType == SemanticType::Void) {
      fail("operator[] must return a value or checked reference.");
    }
    if (name.kind == OverloadedOperator::PreIncrement &&
        returnType != SemanticType::Void) {
      fail("Prefix operator++ must return void in GTI.");
    }
    if (name.kind == OverloadedOperator::PreIncrement &&
        function.receiverMutability() != ReceiverMutability::Mutable) {
      fail("Prefix operator++ must use a mutable receiver.");
    }
    if ((name.kind == OverloadedOperator::Equal ||
         name.kind == OverloadedOperator::NotEqual ||
         name.kind == OverloadedOperator::ContextualBool) &&
        function.receiverMutability() == ReceiverMutability::Mutable) {
      fail(std::string(operatorSourceSpelling(name.kind)) +
           " must use a read-only receiver.");
    }
  }

  void validateRuntimeBinding(const FunctionDecl &function) {
    if (!function.runtimeBinding()) {
      return;
    }

    const RuntimeBinding &binding = *function.runtimeBinding();
    const bool valid =
        binding.name == "stdout.write" &&
        qualifiedName(currentNamespace, function.name().lexeme) ==
            "gti_internal::runtime::write_stdout" &&
        typeOf(function.returnType()) == SemanticType::Void &&
        function.parameters().size() == 1 &&
        typeOf(function.parameters().front().type) ==
            SemanticType::StringView &&
        function.parameters().front().mutability == Mutability::Immutable &&
        function.genericParameters().empty() && !function.body();
    if (!valid) {
      report(binding.attribute,
             "Invalid declaration for runtime binding '" + binding.name +
                 "'.");
    }
  }

  [[nodiscard]] SourceUnitId sourceUnitFor(const Token &token) const {
    return sourceGraph == nullptr
               ? 0
               : sourceGraph->sourceUnitForPath(token.source);
  }

  [[nodiscard]] bool sourceVisible(SourceUnitId declaration) const {
    return sourceGraph == nullptr ||
           sourceGraph->isVisible(currentSourceUnit, declaration);
  }

  template <typename Callback>
  void forEachSourceConsumer(SourceUnitId declaration, Callback callback) {
    if (sourceGraph == nullptr || declaration == 0) {
      return;
    }
    for (const SourceUnit &unit : sourceGraph->sourceUnits()) {
      if (sourceGraph->isVisible(unit.id, declaration)) {
        callback(unit.id);
      }
    }
  }

  void publishNamespace(const std::string &name, SourceUnitId declaration) {
    forEachSourceConsumer(declaration, [&](SourceUnitId consumer) {
      visibleNamespaces[consumer].insert(name);
    });
  }

  void publishNamespaceAlias(const std::string &name,
                             const NamespaceAliasInfo &alias) {
    forEachSourceConsumer(alias.sourceUnit, [&](SourceUnitId consumer) {
      visibleNamespaceAliases[consumer].insert_or_assign(name, alias);
    });
  }

  void publishTypeAlias(const std::string &name, TypeAliasId id,
                        SourceUnitId declaration) {
    forEachSourceConsumer(declaration, [&](SourceUnitId consumer) {
      visibleTypeAliasIds[consumer].insert_or_assign(name, id);
    });
  }

  void publishClass(const std::string &name, ClassId id,
                    SourceUnitId declaration) {
    forEachSourceConsumer(declaration, [&](SourceUnitId consumer) {
      visibleClassIds[consumer].insert_or_assign(name, id);
    });
  }

  void publishEnum(const std::string &name, EnumId id,
                   SourceUnitId declaration) {
    forEachSourceConsumer(declaration, [&](SourceUnitId consumer) {
      visibleEnumIds[consumer].insert_or_assign(name, id);
    });
  }

  void publishNamespaceSymbol(const std::string &name, const Symbol &symbol) {
    if (symbol.internalLinkage && sourceGraph != nullptr &&
        symbol.sourceUnit != 0) {
      visibleNamespaceSymbols[symbol.sourceUnit].insert_or_assign(name, symbol);
      return;
    }
    forEachSourceConsumer(symbol.sourceUnit, [&](SourceUnitId consumer) {
      auto &symbols = visibleNamespaceSymbols[consumer];
      const auto existing = symbols.find(name);
      if (existing == symbols.end()) {
        symbols.emplace(name, symbol);
        return;
      }
      if (existing->second.type != SemanticType::Function ||
          symbol.type != SemanticType::Function) {
        return;
      }
      existing->second.overloads.insert(existing->second.overloads.end(),
                                        symbol.overloads.begin(),
                                        symbol.overloads.end());
    });
  }

  [[nodiscard]] bool namespaceIsVisible(const std::string &name) const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return namespaces.contains(name);
    }
    const auto found = visibleNamespaces.find(currentSourceUnit);
    return found != visibleNamespaces.end() && found->second.contains(name);
  }

  [[nodiscard]] const NamespaceAliasInfo *
  findNamespaceAlias(const std::string &name) const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      const auto found = namespaceAliases.find(name);
      return found == namespaceAliases.end() ? nullptr : &found->second;
    }
    const auto unitAliases = visibleNamespaceAliases.find(currentSourceUnit);
    if (unitAliases == visibleNamespaceAliases.end()) {
      return nullptr;
    }
    const auto found = unitAliases->second.find(name);
    return found == unitAliases->second.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const std::unordered_map<std::string, Symbol> &
  currentNamespaceSymbols() const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return namespaceSymbols;
    }
    const auto found = visibleNamespaceSymbols.find(currentSourceUnit);
    if (found != visibleNamespaceSymbols.end()) {
      return found->second;
    }
    static const std::unordered_map<std::string, Symbol> empty;
    return empty;
  }

  [[nodiscard]] const std::unordered_map<std::string, ClassId> &
  currentClassIds() const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return classIds;
    }
    const auto found = visibleClassIds.find(currentSourceUnit);
    if (found != visibleClassIds.end()) {
      return found->second;
    }
    static const std::unordered_map<std::string, ClassId> empty;
    return empty;
  }

  [[nodiscard]] const std::unordered_map<std::string, EnumId> &
  currentEnumIds() const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return enumIds;
    }
    const auto found = visibleEnumIds.find(currentSourceUnit);
    if (found != visibleEnumIds.end()) {
      return found->second;
    }
    static const std::unordered_map<std::string, EnumId> empty;
    return empty;
  }

  [[nodiscard]] const std::unordered_map<std::string, TypeAliasId> &
  currentTypeAliasIds() const {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return typeAliasIds;
    }
    const auto found = visibleTypeAliasIds.find(currentSourceUnit);
    if (found != visibleTypeAliasIds.end()) {
      return found->second;
    }
    static const std::unordered_map<std::string, TypeAliasId> empty;
    return empty;
  }

  void registerNamespaces(const StmtList &statements,
                          std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaces(*branch, scope);
        }
        continue;
      }
      const auto *namespaceDecl =
          dynamic_cast<const NamespaceDecl *>(statement.get());
      if (namespaceDecl == nullptr) {
        continue;
      }
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      const std::string name =
          qualifiedName(scope, namespaceDecl->name().lexeme);
      namespaces.insert(name);
      const SymbolId symbol =
          recordToolingSymbol(namespaceDecl->name(), SymbolKind::Namespace,
                              name, SemanticType::Unknown);
      namespaceToolingSymbols.try_emplace(name, symbol);
      publishNamespace(name, currentSourceUnit);
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerNamespaces(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }

  void registerNamespaceAliases(const StmtList &statements,
                                std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaceAliases(*branch, scope);
        }
      } else if (const auto *alias =
                     dynamic_cast<const NamespaceAliasDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(alias->name());
        const std::optional<std::string> targetNamespace =
            resolveNamespacePath(alias->target(), scope);
        if (!targetNamespace) {
          report(alias->target().last(), "Unknown namespace in alias target.");
          continue;
        }

        const std::string name = qualifiedName(scope, alias->name().lexeme);
        if (namespaces.contains(name) || namespaceAliases.contains(name)) {
          report(alias->name(), "Duplicate declaration of '" +
                                    alias->name().lexeme + "'.");
          continue;
        }
        NamespaceAliasInfo info{.target = *targetNamespace,
                                .sourceUnit = currentSourceUnit};
        namespaceAliases.emplace(name, info);
        const SymbolId symbol =
            recordToolingSymbol(alias->name(), SymbolKind::NamespaceAlias, name,
                                SemanticType::Unknown);
        namespaceToolingSymbols.try_emplace(name, symbol);
        publishNamespaceAlias(name, info);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerNamespaceAliases(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void registerTypeAliases(const StmtList &statements,
                           std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerTypeAliases(*branch, scope);
        }
      } else if (const auto *alias =
                     dynamic_cast<const TypeAliasDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(alias->name());
        const std::string qualified =
            qualifiedName(scope, alias->name().lexeme);
        if (namespaces.contains(qualified) ||
            namespaceAliases.contains(qualified) ||
            typeAliasIds.contains(qualified)) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2006", DiagnosticPhase::Semantics, alias->name(),
              "Duplicate declaration of '" + alias->name().lexeme + "'.");
          if (const auto existing = typeAliasIds.find(qualified);
              existing != typeAliasIds.end()) {
            diagnostic.related.push_back(
                {tokenSpan(
                     typeAliases[existing->second - 1].declaration->name()),
                 "Previous declaration is here."});
          }
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }

        const TypeAliasId id = typeAliases.size() + 1;
        typeAliasIds.emplace(qualified, id);
        typeAliases.push_back({.sourceUnit = currentSourceUnit,
                               .declaration = alias,
                               .qualifiedName = qualified,
                               .namespaceScope = scope});
        publishTypeAlias(qualified, id, currentSourceUnit);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerTypeAliases(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void resolveTypeAliases() {
    for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
      (void)resolveTypeAlias(id, nullptr);
    }
  }

  void resolveTypeAliasDependencies(const TypeRef &type,
                                    const std::vector<std::string> &scope) {
    if (type.name.last().kind == TokenKind::IDENTIFIER) {
      if (const std::optional<TypeAliasId> dependency =
              resolveTypeAliasPath(type.name, scope)) {
        (void)resolveTypeAlias(*dependency, &type.name.last());
      }
    }
    for (const TypeRef &argument : type.arguments) {
      resolveTypeAliasDependencies(argument, scope);
    }
  }

  [[nodiscard]] SemanticType resolveTypeAlias(TypeAliasId id,
                                              const Token *use) {
    if (id == 0 || id > typeAliases.size()) {
      return SemanticType::Unknown;
    }
    RegisteredTypeAlias &alias = typeAliases[id - 1];
    if (alias.resolution == TypeAliasResolution::Resolved) {
      return alias.type;
    }
    if (alias.resolution == TypeAliasResolution::Invalid) {
      return SemanticType::Unknown;
    }
    if (alias.resolution == TypeAliasResolution::Resolving) {
      const Token &location = use == nullptr ? alias.declaration->name() : *use;
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2030", DiagnosticPhase::Semantics, location,
          "Type alias cycle involving '" + alias.qualifiedName + "'.");
      diagnostic.related.push_back(
          {tokenSpan(alias.declaration->name()),
           "Alias participating in the cycle is declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      alias.resolution = TypeAliasResolution::Invalid;
      return SemanticType::Unknown;
    }

    alias.resolution = TypeAliasResolution::Resolving;
    const SourceUnitId enclosingSourceUnit = currentSourceUnit;
    const std::vector<std::string> enclosingNamespace = currentNamespace;
    currentSourceUnit = alias.sourceUnit;
    currentNamespace = alias.namespaceScope;

    const std::size_t diagnosticsBefore = diagnostics.size();
    const TypeRef &target = alias.declaration->target();
    resolveTypeAliasDependencies(target, alias.namespaceScope);
    bool targetFormValid = true;
    if (target.name.last().kind == TokenKind::AUTO) {
      report(target.name.last(),
             "A type alias requires an explicit target type.", "GTI-S2030");
      targetFormValid = false;
    }
    if (target.reference) {
      report(*target.reference,
             "Reference aliases are not supported; keep '&' explicit at each "
             "borrow site.",
             "GTI-S2030");
      targetFormValid = false;
    }
    if (targetFormValid) {
      validateType(target);
    }
    const SemanticType resolved = typeOf(target, alias.namespaceScope);
    const bool valid = alias.resolution != TypeAliasResolution::Invalid &&
                       diagnostics.size() == diagnosticsBefore &&
                       resolved != SemanticType::Unknown && targetFormValid;
    if (valid) {
      alias.type = resolved;
      alias.resolution = TypeAliasResolution::Resolved;
      semanticModel.record(*alias.declaration,
                           TypeAliasInfo{.sourceUnit = alias.sourceUnit,
                                         .declaration = alias.declaration,
                                         .qualifiedName = alias.qualifiedName,
                                         .type = alias.type});
    } else {
      alias.resolution = TypeAliasResolution::Invalid;
    }

    currentNamespace = enclosingNamespace;
    currentSourceUnit = enclosingSourceUnit;
    return valid ? alias.type : SemanticType::Unknown;
  }

  [[nodiscard]] static SemanticType
  enumUnderlyingType(const std::optional<TypeRef> &type) {
    if (!type) {
      return SemanticType::Int32;
    }
    switch (type->name.last().kind) {
    case TokenKind::INT:
    case TokenKind::INT32:
      return SemanticType::Int32;
    case TokenKind::INT8:
      return SemanticType::Int8;
    case TokenKind::INT16:
      return SemanticType::Int16;
    case TokenKind::INT64:
      return SemanticType::Int64;
    case TokenKind::UINT:
    case TokenKind::UINT32:
      return SemanticType::UInt32;
    case TokenKind::UINT8:
      return SemanticType::UInt8;
    case TokenKind::UINT16:
      return SemanticType::UInt16;
    case TokenKind::UINT64:
      return SemanticType::UInt64;
    default:
      return SemanticType::Unknown;
    }
  }

  [[nodiscard]] static bool
  validEnumUnderlyingSyntax(const TypeRef &type) {
    return type.name.segments.size() == 1 && type.arguments.empty() &&
           type.arrayExtents.empty() && !type.reference &&
           enumUnderlyingType(type) != SemanticType::Unknown;
  }

  [[nodiscard]] static std::optional<EnumConstant>
  nextEnumConstant(EnumConstant value) {
    if (value.negative) {
      if (value.magnitude <= 1) {
        return EnumConstant{};
      }
      --value.magnitude;
      return value;
    }
    if (value.magnitude == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    ++value.magnitude;
    return value;
  }

  void registerEnums(const StmtList &statements,
                     std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerEnums(*branch, scope);
        }
        continue;
      }
      if (const auto *namespaceDecl =
              dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerEnums(namespaceDecl->declarations(), scope);
        scope.pop_back();
        continue;
      }
      const auto *enumDecl = dynamic_cast<const EnumDecl *>(statement.get());
      if (enumDecl == nullptr) {
        continue;
      }

      currentSourceUnit = sourceUnitFor(enumDecl->name());
      const std::string qualified =
          qualifiedName(scope, enumDecl->name().lexeme);
      if (namespaces.contains(qualified) ||
          namespaceAliases.contains(qualified) ||
          typeAliasIds.contains(qualified) || enumIds.contains(qualified)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, enumDecl->name(),
            "Duplicate declaration of '" + enumDecl->name().lexeme + "'.");
        if (const auto existing = enumIds.find(qualified);
            existing != enumIds.end()) {
          diagnostic.related.push_back(
              {tokenSpan(enums[existing->second - 1].name),
               "Previous declaration is here."});
        }
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      SemanticType underlying = enumUnderlyingType(enumDecl->underlyingType());
      if (enumDecl->underlyingType() &&
          !validEnumUnderlyingSyntax(*enumDecl->underlyingType())) {
        report(enumDecl->underlyingType()->name.last(),
               "A scoped enum backing type must be a fixed integral primitive "
               "such as int32_t or uint8_t.",
               "GTI-S2036");
        underlying = SemanticType::Int32;
      }

      const EnumId id = enums.size() + 1;
      enumIds.emplace(qualified, id);
      publishEnum(qualified, id, currentSourceUnit);
      enums.push_back(EnumInfo{.id = id,
                               .sourceUnit = currentSourceUnit,
                               .declaration = enumDecl,
                               .name = enumDecl->name(),
                               .namespaceScope = scope,
                               .underlyingType = underlying});
      EnumInfo &info = enums.back();
      std::vector<EnumeratorInfo> modelEnumerators;
      modelEnumerators.reserve(enumDecl->enumerators().size());

      std::optional<EnumConstant> nextValue = EnumConstant{};
      for (const EnumeratorDecl &enumerator : enumDecl->enumerators()) {
        EnumConstant value = nextValue.value_or(EnumConstant{});
        bool valueKnown = nextValue.has_value();
        if (enumerator.initializer) {
          const std::optional<IntegerConstant> constant =
              integerConstant(enumerator.initializer.get());
          if (!constant) {
            report(expressionToken(enumerator.initializer),
                   "Scoped enum values must currently be signed integer "
                   "literals.",
                   "GTI-S2036");
            valueKnown = false;
          } else {
            value = {.negative = constant->negative,
                     .magnitude = constant->magnitude};
            valueKnown = true;
          }
        } else if (!nextValue) {
          report(enumerator.name,
                 "Implicit scoped enum value cannot be determined from the "
                 "previous enumerator.",
                 "GTI-S2036");
        }

        if (valueKnown &&
            !integerFits(underlying,
                         IntegerConstant{.negative = value.negative,
                                         .magnitude = value.magnitude})) {
          report(enumerator.name,
                 "Scoped enum value does not fit its backing type.",
                 "GTI-S2036");
        }

        const auto [inserted, success] = info.enumerators.emplace(
            enumerator.name.lexeme,
            EnumeratorRecord{
                .declaration = &enumerator,
                .value = value,
                .symbol = Symbol{.type = SemanticType::enumType(id),
                                 .sourceUnit = currentSourceUnit,
                                 .assignable = false,
                                 .declaration = enumerator.name}});
        if (!success) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2006", DiagnosticPhase::Semantics, enumerator.name,
              "Duplicate enumerator '" + enumerator.name.lexeme + "'.");
          diagnostic.related.push_back(
              {tokenSpan(inserted->second.declaration->name),
               "Previous enumerator is here."});
          diagnostics.emplace_back(std::move(diagnostic));
        }
        modelEnumerators.push_back(
            {.declaration = &enumerator,
             .value = value,
             .explicitValue = enumerator.initializer != nullptr});
        nextValue = valueKnown ? nextEnumConstant(value) : std::nullopt;
      }

      semanticModel.recordEnumType(
          *enumDecl,
          EnumTypeInfo{.id = id,
                       .sourceUnit = currentSourceUnit,
                       .declaration = enumDecl,
                       .qualifiedName = qualified,
                       .namespaceScope = scope,
                       .underlyingType = underlying,
                       .enumerators = std::move(modelEnumerators)});
    }
  }

  void registerClasses(const StmtList &statements,
                       std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerClasses(*branch, scope);
        }
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(classDecl->name());
        const std::string qualified =
            qualifiedName(scope, classDecl->name().lexeme);
        if (namespaces.contains(qualified) ||
            namespaceAliases.contains(qualified) ||
            typeAliasIds.contains(qualified) || enumIds.contains(qualified) ||
            classIds.contains(qualified)) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2006", DiagnosticPhase::Semantics, classDecl->name(),
              "Duplicate declaration of '" + classDecl->name().lexeme + "'.");
          if (const auto existing = classIds.find(qualified);
              existing != classIds.end()) {
            diagnostic.related.push_back(
                {tokenSpan(classInfo(existing->second).name),
                 "Previous declaration is here."});
          }
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }

        const ClassId id = classes.size() + 1;
        std::vector<GenericParameterInfo> genericParameters =
            makeGenericParameters(classDecl->genericParameters(),
                                  classDecl->name());
        for (const GenericParameter &parameter :
             classDecl->genericParameters()) {
          if (parameter.pack) {
            report(*parameter.pack,
                   "Generic type packs are currently limited to functions and "
                   "methods.",
                   "GTI-S2023");
          }
        }
        classIds.emplace(qualified, id);
        publishClass(qualified, id, currentSourceUnit);
        classDeclIds.emplace(classDecl, id);
        classes.push_back(
            ClassInfo{.id = id,
                      .sourceUnit = currentSourceUnit,
                      .declaration = classDecl,
                      .name = classDecl->name(),
                      .kind = classDecl->kind(),
                      .namespaceScope = scope,
                      .genericParameters = std::move(genericParameters)});
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerClasses(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  [[nodiscard]] static std::string_view classKindSpelling(ClassKind kind) {
    switch (kind) {
    case ClassKind::Class:
      return "class";
    case ClassKind::Struct:
      return "struct";
    case ClassKind::Interface:
      return "interface";
    }
    return "type";
  }

  void resolveClassInheritance() {
    for (ClassInfo &owner : classes) {
      if (owner.declaration == nullptr) {
        continue;
      }
      currentSourceUnit = owner.sourceUnit;
      beginTypeParameterScope(owner.genericParameters);
      bool hasConcreteBase = false;
      std::unordered_set<ClassId> directBases;
      for (const BaseSpecifier &syntax : owner.declaration->bases()) {
        const Token &location = syntax.type.name.last();
        if (!syntax.access || syntax.access->kind != TokenKind::PUBLIC) {
          Diagnostic diagnostic =
              makeDiagnostic("GTI-S2040", DiagnosticPhase::Semantics,
                             syntax.access ? *syntax.access : location,
                             "Inheritance must be public; write 'public " +
                                 typeRefSpelling(syntax.type) + "'.");
          diagnostic.hints.emplace_back(
              "GTI uses inheritance only for substitutable base contracts; "
              "use a field for private implementation reuse.");
          diagnostics.emplace_back(std::move(diagnostic));
        }

        const SemanticType baseType = typeOf(syntax.type, owner.namespaceScope);
        if (baseType.kind != SemanticType::Class || baseType.classId == 0 ||
            baseType.classId > classes.size()) {
          if (baseType != SemanticType::Unknown) {
            report(location,
                   "Base type must name a class, struct, or interface.",
                   "GTI-S2040");
          }
          continue;
        }
        if (baseType.classId == owner.id) {
          report(location, "A type cannot inherit from itself.", "GTI-S2040");
          continue;
        }
        if (!directBases.insert(baseType.classId).second) {
          report(location,
                 "Base type '" + typeSpelling(baseType) +
                     "' is listed more than once.",
                 "GTI-S2040");
          continue;
        }

        const ClassInfo &base = classInfo(baseType.classId);
        const bool interfaceBase = base.kind == ClassKind::Interface;
        if (owner.kind == ClassKind::Interface && !interfaceBase) {
          report(location,
                 "An interface can inherit only from other interfaces.",
                 "GTI-S2040");
          continue;
        }
        if (!interfaceBase) {
          if (hasConcreteBase) {
            report(location,
                   "A class or struct can have only one state-bearing base; "
                   "additional bases must be interfaces.",
                   "GTI-S2040");
            continue;
          }
          hasConcreteBase = true;
        }
        owner.bases.push_back(
            {.syntax = &syntax, .type = baseType, .interface = interfaceBase});
      }
      endTypeParameterScope();
    }

    std::vector<std::uint8_t> state(classes.size(), 0);
    for (const ClassInfo &owner : classes) {
      detectInheritanceCycle(owner.id, state);
    }
    for (const ClassInfo &owner : classes) {
      std::unordered_set<ClassId> ancestors;
      collectUniqueAncestors(owner, owner.id, ancestors);
    }
  }

  void detectInheritanceCycle(ClassId id, std::vector<std::uint8_t> &state) {
    if (id == 0 || id > classes.size() || state[id - 1] == 2) {
      return;
    }
    if (state[id - 1] == 1) {
      return;
    }
    state[id - 1] = 1;
    const ClassInfo &owner = classInfo(id);
    for (const ClassBaseTypeInfo &base : owner.bases) {
      if (base.type.classId == 0 || base.type.classId > classes.size()) {
        continue;
      }
      if (state[base.type.classId - 1] == 1) {
        const Token &location =
            base.syntax == nullptr ? owner.name : base.syntax->type.name.last();
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2040", DiagnosticPhase::Semantics, location,
            "Inheritance cycle involving '" + owner.name.lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(classInfo(base.type.classId).name),
             "Cyclic base type declared here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      detectInheritanceCycle(base.type.classId, state);
    }
    state[id - 1] = 2;
  }

  void collectUniqueAncestors(const ClassInfo &current, ClassId root,
                              std::unordered_set<ClassId> &ancestors) {
    for (const ClassBaseTypeInfo &base : current.bases) {
      if (base.type.classId == 0 || base.type.classId > classes.size()) {
        continue;
      }
      if (!ancestors.insert(base.type.classId).second) {
        const Token &location = base.syntax == nullptr
                                    ? current.name
                                    : base.syntax->type.name.last();
        report(location,
               "Base '" + classInfo(base.type.classId).name.lexeme +
                   "' is inherited through more than one path; inheritance "
                   "diamonds are not supported.",
               "GTI-S2040");
        continue;
      }
      if (base.type.classId != root) {
        collectUniqueAncestors(classInfo(base.type.classId), root, ancestors);
      }
    }
  }

  void registerFunctionGenericParameters(const StmtList &statements,
                                         std::vector<std::string> scope,
                                         bool classMember) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerFunctionGenericParameters(*branch, scope, classMember);
        }
      } else if (const auto *function =
                     dynamic_cast<const FunctionDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(function->name());
        std::vector<GenericParameterInfo> genericParameters =
            makeGenericParameters(function->genericParameters(),
                                  function->name());
        for (const GenericParameter &parameter :
             function->genericParameters()) {
          if (parameter.valueType) {
            report(*parameter.valueType,
                   "Value generic parameters are currently limited to "
                   "classes and structs.",
                   "GTI-S2026");
          }
        }
        functionGenericParameters.emplace(function,
                                          std::move(genericParameters));
        semanticModel.record(
            *function,
            FunctionInfo{
                .id = nextFunctionId++,
                .sourceUnit = currentSourceUnit,
                .declaration = function,
                .qualifiedName = qualifiedName(scope, function->name().lexeme),
                .entryPoint = !classMember && scope.empty() &&
                              function->name().lexeme == "main" &&
                              (sourceGraph == nullptr ||
                               currentSourceUnit == sourceGraph->entryUnit()),
                .staticMember = classMember && function->isStatic(),
                .internalLinkage = !classMember && function->isStatic()});
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(classDecl->name());
        std::vector<std::string> memberScope = scope;
        memberScope.emplace_back(classDecl->name().lexeme);
        registerFunctionGenericParameters(classDecl->members(),
                                          std::move(memberScope), true);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerFunctionGenericParameters(namespaceDecl->declarations(), scope,
                                          false);
        scope.pop_back();
      }
    }
  }

  void registerNamespaceSymbols(const StmtList &statements,
                                std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaceSymbols(*branch, scope);
        }
      } else if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(function->name());
        Symbol symbol = functionSymbol(*function, scope);
        if (!symbol.overloads.empty()) {
          recordFunctionSignature(*function, symbol.overloads.front(), scope,
                                  0);
        }
        declareNamespaceSymbol(scope, function->name(), std::move(symbol));
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(classDecl->name());
        if (const auto found = classDeclIds.find(classDecl);
            found != classDeclIds.end()) {
          declareNamespaceSymbol(scope, classDecl->name(),
                                 SemanticType::typeName(found->second), false);
        }
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerNamespaceSymbols(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void collectClassMembers(const StmtList &statements,
                           std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          collectClassMembers(*branch, scope);
        }
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(classDecl->name());
        const auto found = classDeclIds.find(classDecl);
        if (found == classDeclIds.end()) {
          continue;
        }
        ClassInfo &info = classInfo(found->second);
        AccessModifier access = info.kind == ClassKind::Class
                                    ? AccessModifier::Private
                                    : AccessModifier::Public;
        beginTypeParameterScope(info.genericParameters);
        collectMembers(classDecl->members(), info, access);
        endTypeParameterScope();
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        currentSourceUnit = sourceUnitFor(namespaceDecl->name());
        scope.emplace_back(namespaceDecl->name().lexeme);
        collectClassMembers(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void refreshFunctionPolymorphism(const FunctionCandidate &candidate) {
    if (candidate.declaration == nullptr) {
      return;
    }
    const FunctionInfo *existing =
        semanticModel.findFunction(*candidate.declaration);
    if (existing == nullptr) {
      return;
    }
    FunctionInfo updated = *existing;
    updated.virtualMethod = candidate.virtualMethod;
    updated.pureVirtual = candidate.pureVirtual;
    updated.overrideMethod = candidate.overrideMethod;
    updated.virtualRoots = candidate.virtualRoots;
    semanticModel.record(*candidate.declaration, std::move(updated));
  }

  void mergeInheritedMember(ClassInfo &owner, const std::string &name,
                            MemberInfo incoming) {
    const auto found = owner.members.find(name);
    if (found == owner.members.end()) {
      owner.members.emplace(name, std::move(incoming));
      return;
    }
    MemberInfo &existing = found->second;
    if (existing.symbol.type != SemanticType::Function ||
        incoming.symbol.type != SemanticType::Function) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2043", DiagnosticPhase::Semantics, owner.name,
                         "Inherited member name '" + name +
                             "' is ambiguous in '" + owner.name.lexeme + "'.");
      diagnostic.related.push_back(
          {tokenSpan(existing.symbol.declaration), "First member is here."});
      diagnostic.related.push_back({tokenSpan(incoming.symbol.declaration),
                                    "Conflicting member is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
    for (FunctionCandidate &candidate : incoming.symbol.overloads) {
      const bool duplicateIdentity = std::any_of(
          existing.symbol.overloads.begin(), existing.symbol.overloads.end(),
          [&](const FunctionCandidate &previous) {
            return previous.id != 0 && previous.id == candidate.id;
          });
      if (!duplicateIdentity) {
        existing.symbol.overloads.emplace_back(std::move(candidate));
      }
    }
  }

  void mergeDeclaredMember(ClassInfo &owner, const std::string &name,
                           MemberInfo local) {
    const auto found = owner.members.find(name);
    if (found == owner.members.end()) {
      for (const FunctionCandidate &candidate : local.symbol.overloads) {
        if (candidate.overrideMethod) {
          report(candidate.declaration->overrideKeyword().value(),
                 "Method marked 'override' has no inherited virtual method "
                 "with the same exact signature.",
                 "GTI-S2042");
        }
        refreshFunctionPolymorphism(candidate);
      }
      owner.members.emplace(name, std::move(local));
      return;
    }

    MemberInfo &inherited = found->second;
    if (inherited.symbol.type != SemanticType::Function ||
        local.symbol.type != SemanticType::Function) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2043", DiagnosticPhase::Semantics, local.symbol.declaration,
          "Member '" + name +
              "' hides an inherited member; inherited data "
              "members cannot be redeclared.");
      diagnostic.related.push_back({tokenSpan(inherited.symbol.declaration),
                                    "Inherited member is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }

    for (FunctionCandidate &candidate : local.symbol.overloads) {
      std::vector<std::size_t> matches;
      for (std::size_t index = 0; index < inherited.symbol.overloads.size();
           ++index) {
        if (sameFunctionSignature(inherited.symbol.overloads[index],
                                  candidate)) {
          matches.push_back(index);
        }
      }

      if (matches.empty()) {
        if (candidate.overrideMethod) {
          report(candidate.declaration->overrideKeyword().value(),
                 "Method marked 'override' has no inherited virtual method "
                 "with the same exact signature.",
                 "GTI-S2042");
        }
        inherited.symbol.overloads.emplace_back(std::move(candidate));
        refreshFunctionPolymorphism(inherited.symbol.overloads.back());
        continue;
      }

      bool validOverride = true;
      std::vector<FunctionId> roots;
      for (std::size_t index : matches) {
        const FunctionCandidate &base = inherited.symbol.overloads[index];
        if (!base.virtualMethod || base.staticMember) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2042", DiagnosticPhase::Semantics,
              candidate.declaration->name(),
              "Method has the same signature as a non-virtual base method; "
              "GTI does not permit accidental method hiding.");
          diagnostic.related.push_back(
              {tokenSpan(base.declaration->name()), "Base method is here."});
          diagnostics.emplace_back(std::move(diagnostic));
          validOverride = false;
        }
        if (candidate.returnType != base.returnType) {
          Diagnostic diagnostic =
              makeDiagnostic("GTI-S2042", DiagnosticPhase::Semantics,
                             candidate.declaration->returnType().name.last(),
                             "Override return type must exactly match '" +
                                 typeSpelling(base.returnType) + "'.");
          diagnostic.related.push_back(
              {tokenSpan(base.declaration->returnType().name.last()),
               "Base return type is declared here."});
          diagnostics.emplace_back(std::move(diagnostic));
          validOverride = false;
        }
        roots.insert(roots.end(), base.virtualRoots.begin(),
                     base.virtualRoots.end());
      }
      if (!candidate.overrideMethod) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2042", DiagnosticPhase::Semantics,
            candidate.declaration->name(),
            "Overriding methods must use the C++-familiar 'override' "
            "specifier.");
        diagnostic.related.push_back(
            {tokenSpan(inherited.symbol.overloads[matches.front()]
                           .declaration->name()),
             "Virtual base method is declared here."});
        diagnostic.hints.emplace_back(
            "Add 'override' after the parameter list.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
      if (validOverride) {
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        candidate.virtualMethod = true;
        candidate.virtualRoots = std::move(roots);
      }

      std::erase_if(inherited.symbol.overloads,
                    [&](const FunctionCandidate &base) {
                      return sameFunctionSignature(base, candidate);
                    });
      inherited.symbol.overloads.emplace_back(std::move(candidate));
      refreshFunctionPolymorphism(inherited.symbol.overloads.back());
    }
    inherited.symbol.declaration = local.symbol.declaration;
    inherited.symbol.ownerClass = owner.id;
  }

  void buildInheritedMembers(ClassId id, std::vector<std::uint8_t> &state) {
    if (id == 0 || id > classes.size() || state[id - 1] == 2) {
      return;
    }
    if (state[id - 1] == 1) {
      return;
    }
    state[id - 1] = 1;
    ClassInfo &owner = classInfo(id);
    std::unordered_map<std::string, MemberInfo> declared =
        std::move(owner.members);
    owner.members.clear();

    for (const ClassBaseTypeInfo &base : owner.bases) {
      buildInheritedMembers(base.type.classId, state);
      const ClassInfo &baseOwner = classInfo(base.type.classId);
      owner.polymorphic = owner.polymorphic || baseOwner.polymorphic;
      for (const auto &[name, member] : baseOwner.members) {
        MemberInfo inherited = member;
        inherited.symbol = substituteSymbol(member.symbol, base.type);
        mergeInheritedMember(owner, name, std::move(inherited));
      }
    }

    for (auto &[name, member] : declared) {
      mergeDeclaredMember(owner, name, std::move(member));
    }

    owner.abstract = owner.kind == ClassKind::Interface;
    for (const auto &[name, member] : owner.members) {
      (void)name;
      if (member.symbol.type != SemanticType::Function) {
        continue;
      }
      for (const FunctionCandidate &candidate : member.symbol.overloads) {
        owner.polymorphic = owner.polymorphic || candidate.virtualMethod;
        owner.abstract = owner.abstract || candidate.pureVirtual;
      }
      for (std::size_t left = 0; left < member.symbol.overloads.size();
           ++left) {
        for (std::size_t right = left + 1;
             right < member.symbol.overloads.size(); ++right) {
          const FunctionCandidate &first = member.symbol.overloads[left];
          const FunctionCandidate &second = member.symbol.overloads[right];
          if (!sameFunctionSignature(first, second)) {
            continue;
          }
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2043", DiagnosticPhase::Semantics, owner.name,
              "Inherited virtual contract '" + name +
                  "' is ambiguous; redeclare one exact method with "
                  "'override' to unify it.");
          diagnostic.related.push_back({tokenSpan(first.declaration->name()),
                                        "First contract is here."});
          diagnostic.related.push_back({tokenSpan(second.declaration->name()),
                                        "Second contract is here."});
          diagnostics.emplace_back(std::move(diagnostic));
        }
      }
    }
    state[id - 1] = 2;
  }

  void resolveInheritedMembers() {
    std::vector<std::uint8_t> state(classes.size(), 0);
    for (const ClassInfo &owner : classes) {
      buildInheritedMembers(owner.id, state);
    }
  }

  [[nodiscard]] static const ClassBaseTypeInfo *
  concreteBase(const ClassInfo &owner) {
    const auto found = std::find_if(
        owner.bases.begin(), owner.bases.end(),
        [](const ClassBaseTypeInfo &base) { return !base.interface; });
    return found == owner.bases.end() ? nullptr : &*found;
  }

  [[nodiscard]] static bool
  fieldsHaveDeclarationInitializers(const ClassInfo &owner) {
    return std::all_of(owner.fields.begin(), owner.fields.end(),
                       [](const FieldInfo &field) {
                         return field.declaration != nullptr &&
                                field.declaration->initializer() != nullptr;
                       });
  }

  [[nodiscard]] static const ConstructorInfo *
  defaultConstructor(const ClassInfo &owner) {
    const auto found =
        std::find_if(owner.constructors.begin(), owner.constructors.end(),
                     [](const ConstructorInfo &constructor) {
                       return constructor.parameterTypes.empty();
                     });
    return found == owner.constructors.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool classCanGenerateDefaultConstructor(
      const ClassInfo &owner, std::unordered_set<ClassId> &visiting) const {
    if (!fieldsHaveDeclarationInitializers(owner)) {
      return false;
    }
    if (!visiting.insert(owner.id).second) {
      return false;
    }
    const ClassBaseTypeInfo *base = concreteBase(owner);
    if (base == nullptr || base->type.classId == 0 ||
        base->type.classId > classes.size()) {
      visiting.erase(owner.id);
      return true;
    }
    const ClassInfo &baseOwner = classInfo(base->type.classId);
    const ConstructorInfo *declared = defaultConstructor(baseOwner);
    const bool available =
        declared != nullptr
            ? declared->access == AccessModifier::Public
            : classCanGenerateDefaultConstructor(baseOwner, visiting);
    visiting.erase(owner.id);
    return available;
  }

  [[nodiscard]] bool
  classCanGenerateDefaultConstructor(const ClassInfo &owner) const {
    std::unordered_set<ClassId> visiting;
    return classCanGenerateDefaultConstructor(owner, visiting);
  }

  [[nodiscard]] bool
  baseHasAccessibleDefaultConstructor(const ClassBaseTypeInfo &base,
                                      ClassId derived) const {
    if (base.type.classId == 0 || base.type.classId > classes.size()) {
      return false;
    }
    const ClassInfo &owner = classInfo(base.type.classId);
    if (const ConstructorInfo *declared = defaultConstructor(owner)) {
      return declared->access == AccessModifier::Public || owner.id == derived;
    }
    return classCanGenerateDefaultConstructor(owner);
  }

  void
  analyzeBaseConstructorInitializer(const ConstructorInitializer &initializer,
                                    const SemanticType &baseType) {
    if (baseType.kind != SemanticType::Class || baseType.classId == 0 ||
        baseType.classId > classes.size()) {
      return;
    }
    const ClassInfo &baseOwner = classInfo(baseType.classId);
    const bool enclosingConstructorInitializer =
        analyzingConstructorInitializer;
    analyzingConstructorInitializer = true;
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(initializer.arguments.size());
    for (const ExprPtr &argument : initializer.arguments) {
      argumentTypes.emplace_back(analyze(argument));
    }
    analyzingConstructorInitializer = enclosingConstructorInitializer;

    const GenericSubstitution substitution = classSubstitution(baseType);
    struct ViableConstructor {
      const ConstructorInfo *constructor = nullptr;
      std::vector<SemanticType> parameterTypes;
      bool generatedDefault = false;
    };
    std::vector<ViableConstructor> viable;
    for (const ConstructorInfo &constructor : baseOwner.constructors) {
      if (constructor.parameterTypes.size() != argumentTypes.size()) {
        continue;
      }
      std::vector<SemanticType> parameterTypes;
      bool exact = true;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        const SemanticType parameter =
            substituteType(constructor.parameterTypes[index], substitution);
        parameterTypes.emplace_back(parameter);
        if (!callArgumentMatches(parameter, argumentTypes[index],
                                 initializer.arguments[index])) {
          exact = false;
          break;
        }
      }
      if (exact) {
        viable.push_back({&constructor, std::move(parameterTypes), false});
      }
    }
    if (initializer.arguments.empty() &&
        defaultConstructor(baseOwner) == nullptr &&
        classCanGenerateDefaultConstructor(baseOwner)) {
      viable.push_back({nullptr, {}, true});
    }

    const Token &location = initializer.target.name.last();
    const bool hasUnknownArgument = std::any_of(
        argumentTypes.begin(), argumentTypes.end(),
        [](const SemanticType &type) { return type == SemanticType::Unknown; });
    if (viable.size() != 1) {
      if (!hasUnknownArgument) {
        std::string arguments;
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (index != 0) {
            arguments += ", ";
          }
          arguments += typeSpelling(argumentTypes[index]);
        }
        Diagnostic diagnostic = makeDiagnostic(
            viable.empty() ? "GTI-S2012" : "GTI-S2013",
            DiagnosticPhase::Semantics, location,
            viable.empty()
                ? "No constructor of base '" + typeSpelling(baseType) +
                      "' exactly matches argument types (" + arguments + ")."
                : "Base construction of '" + typeSpelling(baseType) +
                      "' is ambiguous.");
        for (const ConstructorInfo &candidate : baseOwner.constructors) {
          if (candidate.declaration != nullptr) {
            diagnostic.related.push_back(
                {tokenSpan(candidate.declaration->name()),
                 "Candidate: " + constructorSignatureSpelling(candidate)});
          }
        }
        diagnostics.emplace_back(std::move(diagnostic));
      }
      return;
    }

    const ViableConstructor &selected = viable.front();
    if (selected.constructor != nullptr &&
        selected.constructor->access != AccessModifier::Public) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, location,
          "Constructor of base '" + typeSpelling(baseType) + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(selected.constructor->declaration->name()),
           "Constructor declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    semanticModel.record(
        initializer,
        ResolvedConstructorInitializerInfo{
            .kind = ConstructorInitializerTargetKind::Base,
            .targetType = baseType,
            .constructor =
                selected.constructor == nullptr ? 0 : selected.constructor->id,
            .declaration = selected.constructor == nullptr
                               ? nullptr
                               : selected.constructor->declaration,
            .parameterTypes = selected.parameterTypes,
            .generatedDefault = selected.generatedDefault});
  }

  void validateStoredReferenceContracts() {
    for (ClassInfo &owner : classes) {
      for (const FieldInfo &field : owner.fields) {
        if (field.declaration == nullptr) {
          continue;
        }
        const auto member =
            owner.members.find(field.declaration->name().lexeme);
        if (member == owner.members.end() ||
            member->second.symbol.type.kind != SemanticType::Reference) {
          continue;
        }

        const SemanticType &fieldType = member->second.symbol.type;
        if (owner.storedReference) {
          report(field.declaration->name(),
                 "A class or struct may store only one reference in the "
                 "current lifetime model.",
                 "GTI-S2045");
          continue;
        }
        owner.storedReference =
            StoredReferenceInfo{.field = field.declaration,
                                .type = fieldType,
                                .access = fieldType.referenceAccess};
        if (field.declaration->isMutable()) {
          report(field.declaration->name(),
                 "Stored reference fields must be read-only; mutable stored "
                 "references require exclusive-loan tracking.",
                 "GTI-S2045");
        }
        if (field.declaration->initializer()) {
          report(field.declaration->name(),
                 "A stored reference field must be bound explicitly by every "
                 "constructor.",
                 "GTI-S2045");
        }
      }
    }

    for (ClassInfo &owner : classes) {
      for (const ClassBaseTypeInfo &base : owner.bases) {
        if (!base.interface && typeTraits(base.type).containsBorrowedState) {
          report(base.syntax == nullptr ? owner.name
                                        : base.syntax->type.name.last(),
                 "Borrowed state cannot be inherited in the current lifetime "
                 "model; store one direct reference instead.",
                 "GTI-S2045");
        }
      }
      for (const FieldInfo &field : owner.fields) {
        if (field.declaration == nullptr ||
            field.declaration->type().reference) {
          continue;
        }
        const auto member =
            owner.members.find(field.declaration->name().lexeme);
        if (member != owner.members.end() &&
            typeTraits(member->second.symbol.type).containsBorrowedState) {
          report(field.declaration->name(),
                 "Borrowed state cannot be nested in another field in the "
                 "current lifetime model.",
                 "GTI-S2045");
        }
      }
      if (!owner.storedReference) {
        continue;
      }
      const StoredReferenceInfo &stored = *owner.storedReference;
      if (owner.destructor) {
        report(owner.destructor->declaration->tilde(),
               "A class carrying a stored reference cannot declare a "
               "destructor in the current lifetime model.",
               "GTI-S2045");
      }
      if (owner.constructors.empty()) {
        report(stored.field->name(),
               "A stored reference field requires a declared constructor "
               "that binds it from a reference parameter.",
               "GTI-S2045");
        continue;
      }

      for (ConstructorInfo &constructor : owner.constructors) {
        if (constructor.declaration == nullptr) {
          continue;
        }
        const ConstructorInitializer *binding = nullptr;
        for (const ConstructorInitializer &initializer :
             constructor.declaration->initializers()) {
          if (initializer.target.name.segments.size() == 1 &&
              initializer.target.arguments.empty() &&
              initializer.target.name.last().lexeme ==
                  stored.field->name().lexeme) {
            binding = &initializer;
            break;
          }
        }
        if (binding == nullptr || binding->arguments.size() != 1) {
          report(constructor.declaration->name(),
                 "Every constructor of a stored-reference class must bind "
                 "field '" +
                     stored.field->name().lexeme +
                     "' from exactly one reference parameter.",
                 "GTI-S2045");
          continue;
        }
        const auto *source =
            dynamic_cast<const Variable *>(binding->arguments.front().get());
        if (source == nullptr) {
          report(expressionToken(binding->arguments.front()),
                 "Stored reference field '" + stored.field->name().lexeme +
                     "' must be bound directly from a constructor reference "
                     "parameter.",
                 "GTI-S2045");
          continue;
        }
        const auto parameter = std::find_if(
            constructor.declaration->parameters().begin(),
            constructor.declaration->parameters().end(),
            [&](const Parameter &candidate) {
              return candidate.name.lexeme == source->name().lexeme;
            });
        if (parameter == constructor.declaration->parameters().end()) {
          report(source->name(),
                 "Stored reference field '" + stored.field->name().lexeme +
                     "' must be bound from a constructor parameter.",
                 "GTI-S2045");
          continue;
        }
        const std::size_t parameterIndex =
            static_cast<std::size_t>(std::distance(
                constructor.declaration->parameters().begin(), parameter));
        if (parameterIndex >= constructor.parameterTypes.size() ||
            constructor.parameterTypes[parameterIndex] != stored.type) {
          report(parameter->name,
                 "Stored reference field '" + stored.field->name().lexeme +
                     "' requires a parameter of exact type '" +
                     typeSpelling(stored.type) + "'.",
                 "GTI-S2045");
          continue;
        }
        constructor.borrowParameter = parameterIndex;
        constructor.borrowAccess = stored.access;
      }
    }
  }

  void recordClassLifecycles() {
    for (const ClassInfo &owner : classes) {
      if (owner.declaration == nullptr) {
        continue;
      }
      const SemanticTypeTraits traits = typeTraits(openClassType(owner.id));
      const SpecialMemberStatus defaultStatus =
          defaultConstructor(owner) != nullptr
              ? SpecialMemberStatus::Declared
              : (classCanGenerateDefaultConstructor(owner)
                     ? SpecialMemberStatus::Generated
                     : SpecialMemberStatus::Deleted);
      const auto specialStatus =
          [&](const std::optional<ConstructorInfo> &constructor, bool available,
              std::string_view operation) {
            if (!constructor || constructor->declaration == nullptr ||
                !constructor->declaration->specifier()) {
              return available ? SpecialMemberStatus::Generated
                               : SpecialMemberStatus::Deleted;
            }
            const SpecialMemberSpecifier &specifier =
                *constructor->declaration->specifier();
            if (specifier.kind == SpecialMemberSpecifierKind::Deleted) {
              return SpecialMemberStatus::Deleted;
            }
            if (!available) {
              Diagnostic diagnostic = makeDiagnostic(
                  "GTI-S2020", DiagnosticPhase::Semantics, specifier.keyword,
                  "The defaulted " + std::string(operation) + " of '" +
                      owner.name.lexeme +
                      "' is unavailable because its base, fields, or cleanup "
                      "policy does not support that operation.");
              diagnostic.hints.emplace_back(
                  "Declare this constructor '= delete' or change the "
                  "non-" +
                  std::string(operation == "copy constructor" ? "copyable"
                                                              : "movable") +
                  " component.");
              diagnostics.emplace_back(std::move(diagnostic));
              return SpecialMemberStatus::Deleted;
            }
            return SpecialMemberStatus::Generated;
          };
      const SpecialMemberStatus copyStatus = specialStatus(
          owner.copyConstructor, traits.copyable, "copy constructor");
      const SpecialMemberStatus moveStatus = specialStatus(
          owner.moveConstructor, traits.movable, "move constructor");
      semanticModel.record(
          *owner.declaration,
          ClassLifecycleInfo{
              .id = owner.id,
              .declaration = owner.declaration,
              .constructors = owner.constructors,
              .declaredCopyConstructor = owner.copyConstructor,
              .declaredMoveConstructor = owner.moveConstructor,
              .declaredDestructor = owner.destructor,
              .defaultConstructor = defaultStatus,
              .copyConstructor = copyStatus,
              .moveConstructor = moveStatus,
              .copyAssignment = traits.copyAssignable
                                    ? SpecialMemberStatus::Generated
                                    : SpecialMemberStatus::Deleted,
              .moveAssignment = traits.moveAssignable
                                    ? SpecialMemberStatus::Generated
                                    : SpecialMemberStatus::Deleted,
              .destructor = owner.destructor ? SpecialMemberStatus::Declared
                                             : SpecialMemberStatus::Generated,
              .requiresActiveDropState = owner.destructor.has_value(),
              .traits = traits,
              .polymorphic = owner.polymorphic});
    }
  }

  void collectMembers(const StmtList &members, ClassInfo &owner,
                      AccessModifier &access) {
    for (const StmtPtr &statement : members) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          collectMembers(*branch, owner, access);
        }
        continue;
      }
      if (const auto *specifier =
              dynamic_cast<const AccessSpecifierDecl *>(statement.get())) {
        if (owner.kind == ClassKind::Interface) {
          report(specifier->keyword(),
                 "Interface members are always public; access specifiers are "
                 "not permitted.",
                 "GTI-S2041");
        }
        access = specifier->modifier();
        continue;
      }

      if (const auto *constructor =
              dynamic_cast<const ConstructorDecl *>(statement.get())) {
        if (owner.kind == ClassKind::Interface) {
          report(constructor->name(), "Interfaces cannot declare constructors.",
                 "GTI-S2041");
          continue;
        }
        ConstructorInfo info{.id = nextConstructorId++,
                             .owner = owner.id,
                             .declaration = constructor,
                             .access = access};
        info.parameterTypes.reserve(constructor->parameters().size());
        for (const Parameter &parameter : constructor->parameters()) {
          info.parameterTypes.emplace_back(
              typeOf(parameter, owner.namespaceScope));
        }

        const SemanticType receiverType = openClassType(owner.id);
        if (info.parameterTypes.size() == 1) {
          const SemanticType &parameter = info.parameterTypes.front();
          const Parameter &syntax = constructor->parameters().front();
          const bool receiverReference =
              parameter.kind == SemanticType::Reference &&
              parameter.arguments.size() == 1 &&
              parameter.arguments.front() == receiverType;
          if (receiverReference && syntax.type.reference) {
            info.kind = syntax.type.reference->kind == TokenKind::AND
                            ? ConstructorKind::Move
                            : ConstructorKind::Copy;
          } else if (parameter == receiverType) {
            report(constructor->name(),
                   "A copy or move constructor must take exactly one '" +
                       owner.name.lexeme + "&' or '" + owner.name.lexeme +
                       "&&' parameter.",
                   "GTI-S2020");
            continue;
          }
        }

        if (info.kind != ConstructorKind::Ordinary) {
          const Parameter &parameter = constructor->parameters().front();
          if (parameter.mutability == Mutability::Mutable) {
            report(parameter.name.lexeme.empty() ? parameter.type.name.last()
                                                 : parameter.name,
                   "Copy and move policy parameters do not take 'mut'.",
                   "GTI-S2020");
          }
          if (parameter.pack) {
            report(*parameter.pack,
                   "Copy and move constructors cannot use a parameter pack.",
                   "GTI-S2020");
          }
          if (access != AccessModifier::Public) {
            report(constructor->name(),
                   "Copy and move constructor policies must be public; use "
                   "'= delete' to disable an operation.",
                   "GTI-S2020");
          }
          if (!constructor->initializers().empty()) {
            report(constructor->name(),
                   "Copy and move constructor policies cannot have an "
                   "initializer list.",
                   "GTI-S2020");
          }
          if (!constructor->specifier()) {
            Diagnostic diagnostic = makeDiagnostic(
                "GTI-S2020", DiagnosticPhase::Semantics, constructor->name(),
                "Custom copy and move constructor bodies require "
                "place-aware field moves and are not supported yet.");
            diagnostic.hints.emplace_back(
                info.kind == ConstructorKind::Copy
                    ? "Declare the policy as '" + owner.name.lexeme + "(" +
                          owner.name.lexeme + "&) = default;' or '= delete;'."
                    : "Declare the policy as '" + owner.name.lexeme + "(" +
                          owner.name.lexeme +
                          "&&) = default;' or '= delete;'.");
            diagnostics.emplace_back(std::move(diagnostic));
          }

          std::optional<ConstructorInfo> &declared =
              info.kind == ConstructorKind::Copy ? owner.copyConstructor
                                                 : owner.moveConstructor;
          if (declared) {
            Diagnostic diagnostic = makeDiagnostic(
                "GTI-S2011", DiagnosticPhase::Semantics, constructor->name(),
                std::string("Duplicate ") +
                    (info.kind == ConstructorKind::Copy ? "copy" : "move") +
                    " constructor policy for '" + owner.name.lexeme + "'.");
            diagnostic.related.push_back(
                {tokenSpan(declared->declaration->name()),
                 "Previous policy declaration is here."});
            diagnostics.emplace_back(std::move(diagnostic));
            continue;
          }
          declared = std::move(info);
          continue;
        }

        if (constructor->specifier()) {
          report(constructor->specifier()->equal,
                 "'= default' and '= delete' are currently available only "
                 "for exact copy and move constructor policies.",
                 "GTI-S2020");
        }

        const auto duplicate = std::find_if(
            owner.constructors.begin(), owner.constructors.end(),
            [&](const ConstructorInfo &previous) {
              return previous.parameterTypes == info.parameterTypes;
            });
        if (duplicate != owner.constructors.end()) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2011", DiagnosticPhase::Semantics, constructor->name(),
              "Duplicate constructor overload signature for '" +
                  owner.name.lexeme + "'.");
          diagnostic.related.push_back(
              {tokenSpan(duplicate->declaration->name()),
               "Previous constructor overload is here."});
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }

        owner.constructors.emplace_back(std::move(info));
        continue;
      }

      if (const auto *destructor =
              dynamic_cast<const DestructorDecl *>(statement.get())) {
        if (owner.kind == ClassKind::Interface) {
          report(destructor->tilde(),
                 "Interfaces use compiler-generated polymorphic destruction "
                 "and cannot declare destructors.",
                 "GTI-S2041");
          continue;
        }
        if (destructor->name().lexeme != owner.name.lexeme) {
          report(destructor->name(),
                 "Destructor name must match its class or struct name '" +
                     owner.name.lexeme + "'.",
                 "GTI-S2021");
          continue;
        }
        if (owner.destructor) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2021", DiagnosticPhase::Semantics, destructor->tilde(),
              "A class or struct cannot declare more than one destructor.");
          diagnostic.related.push_back(
              {tokenSpan(owner.destructor->declaration->tilde()),
               "Previous destructor is here."});
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }
        if (access == AccessModifier::Private) {
          report(destructor->tilde(),
                 "Destructors must be public in the current ownership layer.",
                 "GTI-S2021");
        }
        owner.destructor = DestructorInfo{
            .owner = owner.id, .declaration = destructor, .access = access};
        continue;
      }

      const Token *name = nullptr;
      const VariableDecl *field = nullptr;
      const FunctionDecl *function =
          dynamic_cast<const FunctionDecl *>(statement.get());
      Symbol symbol;
      if (function != nullptr) {
        for (const GenericParameter &methodParameter :
             function->genericParameters()) {
          for (const GenericParameterInfo &classParameter :
               owner.genericParameters) {
            if (methodParameter.name.lexeme == classParameter.name.lexeme) {
              report(methodParameter.name,
                     "Method generic type parameters cannot shadow class or "
                     "struct type parameters.");
            }
          }
        }
        name = &function->name();
        symbol = functionSymbol(*function, owner.namespaceScope);
        symbol.staticMember = function->isStatic();
        const bool polymorphic = function->isVirtual() || function->isPure() ||
                                 function->isOverride() ||
                                 owner.kind == ClassKind::Interface;
        if (polymorphic && function->isStatic()) {
          report(function->name(),
                 "Static methods do not have a receiver and cannot be "
                 "virtual.",
                 "GTI-S2042");
        }
        if (polymorphic && !function->genericParameters().empty()) {
          report(function->name(),
                 "Virtual methods cannot declare method-level generic "
                 "parameters.",
                 "GTI-S2042");
        }
        if (function->isPure() && owner.kind != ClassKind::Interface &&
            !function->isVirtual() && !function->isOverride()) {
          report(function->pureSpecifier()->equal,
                 "A pure class method must be declared 'virtual' or "
                 "'override'.",
                 "GTI-S2042");
        }
        if (function->isVirtual() && !function->isPure() && !function->body()) {
          report(function->name(), "A non-pure virtual method requires a body.",
                 "GTI-S2042");
        }
        if (owner.kind == ClassKind::Interface) {
          if (function->body()) {
            report(function->name(),
                   "Interface methods are pure contracts and cannot have a "
                   "body.",
                   "GTI-S2041");
          }
          if (!function->isPure()) {
            report(function->name(), "Interface methods must end with '= 0;'.",
                   "GTI-S2041");
          }
          if (access != AccessModifier::Public) {
            report(function->name(), "Interface methods must be public.",
                   "GTI-S2041");
          }
        }
      } else if (const auto *variable =
                     dynamic_cast<const VariableDecl *>(statement.get())) {
        name = &variable->name();
        field = variable;
        symbol = Symbol{.type = typeOf(variable->type(), variable->mutability(),
                                       owner.namespaceScope),
                        .sourceUnit = owner.sourceUnit,
                        .assignable = variable->isMutable(),
                        .declaration = variable->name(),
                        .bindingKind = variable->isStatic()
                                           ? SemanticBindingKind::StaticField
                                           : SemanticBindingKind::Field,
                        .staticMember = variable->isStatic()};
        predeclaredVariables.insert(variable);
        if (owner.kind == ClassKind::Interface) {
          report(variable->name(),
                 "Interfaces are behavior-only and cannot declare data "
                 "members.",
                 "GTI-S2041");
          continue;
        }
      }
      if (name == nullptr) {
        continue;
      }

      symbol.ownerClass = owner.id;
      symbol.access = access;
      if ((function != nullptr && function->isStatic()) ||
          (field != nullptr && field->isStatic())) {
        if (!owner.genericParameters.empty()) {
          report(*name,
                 "Static members of generic classes and structs require "
                 "qualified generic member paths, which are not supported "
                 "yet.",
                 "GTI-S2039");
        }
      }
      for (FunctionCandidate &overload : symbol.overloads) {
        overload.ownerClass = owner.id;
        overload.dispatchOwner = openClassType(owner.id);
        overload.access = access;
        overload.staticMember = function != nullptr && function->isStatic();
        if (function != nullptr) {
          overload.virtualMethod =
              function->isVirtual() || function->isPure() ||
              function->isOverride() || owner.kind == ClassKind::Interface;
          overload.pureVirtual =
              function->isPure() || owner.kind == ClassKind::Interface;
          overload.overrideMethod = function->isOverride();
          if (overload.virtualMethod && !overload.overrideMethod &&
              overload.id != 0) {
            overload.virtualRoots = {overload.id};
          }
        }
      }
      if (function != nullptr && !symbol.overloads.empty()) {
        recordFunctionSignature(*function, symbol.overloads.front(),
                                owner.namespaceScope, owner.id);
      }
      const auto existing = owner.members.find(name->lexeme);
      if (existing != owner.members.end()) {
        if (appendFunctionOverload(existing->second.symbol, std::move(symbol),
                                   *name)) {
          continue;
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, *name,
            "Duplicate member declaration of '" + name->lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(existing->second.symbol.declaration),
             "Previous member declaration is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      owner.members.emplace(
          name->lexeme,
          MemberInfo{.symbol = std::move(symbol), .access = access});
      if (field != nullptr) {
        (field->isStatic() ? owner.staticFields : owner.fields)
            .push_back(FieldInfo{.declaration = field});
      }
    }
  }

  void recordFunctionSignature(const FunctionDecl &function,
                               const FunctionCandidate &candidate,
                               const std::vector<std::string> &namespaceScope,
                               ClassId ownerClass) {
    const FunctionInfo *registered = semanticModel.findFunction(function);
    if (registered == nullptr) {
      return;
    }
    std::vector<std::string> qualifiedScope = namespaceScope;
    if (ownerClass != 0) {
      qualifiedScope.emplace_back(classInfo(ownerClass).name.lexeme);
    }
    semanticModel.record(
        function, FunctionInfo{.id = registered->id,
                               .sourceUnit = registered->sourceUnit,
                               .declaration = &function,
                               .qualifiedName = qualifiedName(
                                   qualifiedScope, function.name().lexeme),
                               .namespaceScope = namespaceScope,
                               .returnType = candidate.returnType,
                               .parameterTypes = candidate.parameterTypes,
                               .genericParameters = candidate.genericParameters,
                               .parameterPack = candidate.parameterPack,
                               .ownerClass = ownerClass,
                               .entryPoint = registered->entryPoint,
                               .staticMember = registered->staticMember,
                               .internalLinkage = registered->internalLinkage,
                               .virtualMethod = candidate.virtualMethod,
                               .pureVirtual = candidate.pureVirtual,
                               .overrideMethod = candidate.overrideMethod,
                               .virtualRoots = candidate.virtualRoots});
  }

  void recordClassTypes() {
    for (const ClassInfo &owner : classes) {
      if (owner.declaration == nullptr) {
        continue;
      }
      std::vector<ClassFieldTypeInfo> fields;
      std::vector<ClassFieldTypeInfo> staticFields;
      fields.reserve(owner.fields.size());
      for (const FieldInfo &field : owner.fields) {
        if (field.declaration == nullptr) {
          continue;
        }
        const auto member =
            owner.members.find(field.declaration->name().lexeme);
        fields.push_back({.declaration = field.declaration,
                          .type = member == owner.members.end()
                                      ? SemanticType::Unknown
                                      : member->second.symbol.type});
      }
      staticFields.reserve(owner.staticFields.size());
      for (const FieldInfo &field : owner.staticFields) {
        if (field.declaration == nullptr) {
          continue;
        }
        const auto member =
            owner.members.find(field.declaration->name().lexeme);
        staticFields.push_back({.declaration = field.declaration,
                                .type = member == owner.members.end()
                                            ? SemanticType::Unknown
                                            : member->second.symbol.type});
      }
      semanticModel.recordClassType(
          *owner.declaration,
          ClassTypeInfo{.id = owner.id,
                        .sourceUnit = owner.sourceUnit,
                        .declaration = owner.declaration,
                        .qualifiedName = qualifiedName(owner.namespaceScope,
                                                       owner.name.lexeme),
                        .namespaceScope = owner.namespaceScope,
                        .genericParameters = owner.genericParameters,
                        .fields = std::move(fields),
                        .staticFields = std::move(staticFields),
                        .storedReference = owner.storedReference,
                        .kind = owner.kind,
                        .bases = owner.bases,
                        .abstract = owner.abstract,
                        .polymorphic = owner.polymorphic});
    }
  }

  [[nodiscard]] SourceUnitId statementSourceUnit(const Stmt &statement) const {
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(&statement)) {
      return sourceUnitFor(conditional->directive());
    }
    if (const auto *classDecl = dynamic_cast<const ClassDecl *>(&statement)) {
      return sourceUnitFor(classDecl->name());
    }
    if (const auto *function = dynamic_cast<const FunctionDecl *>(&statement)) {
      return sourceUnitFor(function->name());
    }
    if (const auto *alias =
            dynamic_cast<const NamespaceAliasDecl *>(&statement)) {
      return sourceUnitFor(alias->name());
    }
    if (const auto *namespaceDecl =
            dynamic_cast<const NamespaceDecl *>(&statement)) {
      return sourceUnitFor(namespaceDecl->name());
    }
    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&statement)) {
      return sourceUnitFor(alias->name());
    }
    if (const auto *variable = dynamic_cast<const VariableDecl *>(&statement)) {
      return sourceUnitFor(variable->name());
    }
    if (const auto *empty = dynamic_cast<const EmptyStmt *>(&statement)) {
      return sourceUnitFor(empty->semicolon());
    }
    return 0;
  }

  void analyze(const StmtList &statements) {
    for (const StmtPtr &statement : statements) {
      analyze(statement);
    }
  }

  void analyze(const StmtPtr &stmt) {
    if (stmt) {
      const SourceUnitId enclosingSourceUnit = currentSourceUnit;
      const SourceUnitId statementUnit = statementSourceUnit(*stmt);
      if (statementUnit != 0) {
        currentSourceUnit = statementUnit;
      }
      stmt->accept(*this);
      currentSourceUnit = enclosingSourceUnit;
    }
  }

  [[nodiscard]] ExpressionInfo classifyExpression(const Expr &expr,
                                                  SemanticType type) const {
    const auto preserveCategory = [&](const Expr &source) {
      const ExpressionInfo *sourceInfo = semanticModel.findExpression(source);
      return sourceInfo == nullptr
                 ? expressionInfo(std::move(type))
                 : expressionInfo(std::move(type), sourceInfo->category,
                                  sourceInfo->access);
    };

    if (const auto *grouping = dynamic_cast<const Grouping *>(&expr)) {
      return preserveCategory(*grouping->expression());
    }
    if (const auto *binary = dynamic_cast<const Binary *>(&expr);
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return preserveCategory(*binary->right());
    }
    if (const auto *variable = dynamic_cast<const Variable *>(&expr)) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                                symbol->type == SemanticType::TypeName)) {
        return expressionInfo(std::move(type));
      }
      const bool mutableAccess =
          symbol == nullptr ||
          (symbol->type.kind == SemanticType::Reference
               ? symbol->type.referenceAccess == AccessMode::Mutable
               : symbol->assignable &&
                     (symbol->ownerClass == 0 || symbol->staticMember ||
                      currentReceiverMutability ==
                          ReceiverMutability::Mutable));
      return expressionInfo(std::move(type), ValueCategory::Place,
                            mutableAccess ? AccessMode::Mutable
                                          : AccessMode::ReadOnly);
    }
    if (const auto *qualified = dynamic_cast<const QualifiedName *>(&expr)) {
      const Symbol *symbol = resolveQualified(qualified->name());
      if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                                symbol->type == SemanticType::TypeName)) {
        return expressionInfo(std::move(type));
      }
      return expressionInfo(std::move(type), ValueCategory::Place,
                            symbol != nullptr && symbol->assignable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly);
    }
    if (dynamic_cast<const This *>(&expr) != nullptr) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            currentReceiverMutability ==
                                    ReceiverMutability::Mutable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly);
    }
    const bool directOperatorResult =
        dynamic_cast<const Index *>(&expr) != nullptr ||
        dynamic_cast<const Call *>(&expr) != nullptr ||
        (dynamic_cast<const Unary *>(&expr) != nullptr &&
         static_cast<const Unary &>(expr).oper().kind == TokenKind::STAR);
    if (directOperatorResult) {
      if (const ResolvedOperatorInfo *resolved =
              semanticModel.findOperator(expr)) {
        if (resolved->returnType.kind == SemanticType::Reference &&
            resolved->returnType.arguments.size() == 1) {
          return expressionInfo(std::move(type), ValueCategory::Place,
                                resolved->returnType.referenceAccess);
        }
        return expressionInfo(std::move(type));
      }
    }
    if (const auto *index = dynamic_cast<const Index *>(&expr)) {
      const SemanticType *objectType = semanticModel.findType(*index->object());
      if (objectType != nullptr &&
          objectType->kind == SemanticType::StringView) {
        return expressionInfo(std::move(type));
      }
      const ExpressionInfo *objectInfo =
          semanticModel.findExpression(*index->object());
      return expressionInfo(std::move(type), ValueCategory::Place,
                            objectInfo != nullptr &&
                                    objectInfo->category ==
                                        ValueCategory::Place &&
                                    objectInfo->access == AccessMode::Mutable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly);
    }
    if (const auto *unary = dynamic_cast<const Unary *>(&expr);
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      if (const ResolvedOperatorInfo *resolved =
              semanticModel.findOperator(expr);
          resolved != nullptr &&
          resolved->returnType.kind == SemanticType::Reference) {
        return expressionInfo(std::move(type), ValueCategory::Place,
                              resolved->returnType.referenceAccess);
      }
      const ExpressionInfo *ownerInfo =
          semanticModel.findExpression(*unary->right());
      return expressionInfo(std::move(type), ValueCategory::Place,
                            ownerInfo != nullptr &&
                                    ownerInfo->access == AccessMode::Mutable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly);
    }
    if (const auto *get = dynamic_cast<const Get *>(&expr)) {
      if (type == SemanticType::Function) {
        return expressionInfo(std::move(type));
      }
      const SemanticType memberObjectType = memberAccessObjectType(*get);
      const MemberInfo *member = findMember(memberObjectType, get->name());
      const ExpressionInfo *objectInfo =
          semanticModel.findExpression(*get->object());
      const bool mutableAccess =
          member == nullptr ||
          (member->symbol.type.kind == SemanticType::Reference
               ? member->symbol.type.referenceAccess == AccessMode::Mutable
               : member->symbol.assignable &&
                     (get->access().kind == TokenKind::ARROW
                          ? memberReceiverIsMutable(*get)
                          : objectInfo != nullptr &&
                                objectInfo->category == ValueCategory::Place &&
                                objectInfo->access == AccessMode::Mutable));
      return expressionInfo(std::move(type), ValueCategory::Place,
                            mutableAccess ? AccessMode::Mutable
                                          : AccessMode::ReadOnly);
    }
    if (const auto *call = dynamic_cast<const Call *>(&expr)) {
      const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
      if (resolved != nullptr &&
          resolved->returnType.kind == SemanticType::Reference &&
          resolved->returnType.arguments.size() == 1) {
        return expressionInfo(std::move(type), ValueCategory::Place,
                              resolved->returnType.referenceAccess);
      }
    }
    return expressionInfo(std::move(type));
  }

  SemanticType analyze(const Expr &expr) {
    return analyze(expr, OccurrenceRole::Reference | OccurrenceRole::Read);
  }

  SemanticType analyze(const Expr &expr, OccurrenceRole requestedRoles) {
    expr.accept(*this);
    SemanticType result = currentType;
    ExpressionInfo info = classifyExpression(expr, result);
    semanticModel.record(expr, info);
    Token token = expressionToken(expr);
    SemanticBindingKind bindingKind = SemanticBindingKind::None;
    bool mutableBinding = false;
    bool staticMember = false;
    SymbolId symbolId = 0;
    OccurrenceRole roles = requestedRoles;
    if (const auto *variable = dynamic_cast<const Variable *>(&expr)) {
      if (const Symbol *symbol = resolve(variable->name())) {
        bindingKind = symbol->bindingKind;
        mutableBinding = symbol->assignable;
        staticMember = symbol->staticMember || symbol->internalLinkage;
        symbolId = toolingSymbolFor(*symbol);
      } else if (const std::optional<CompileTimeValue> value =
                     resolveValueParameter(variable->name());
                 value && value->kind == CompileTimeValue::Parameter) {
        const GenericParameterInfo *parameter =
            genericParameterInfo(value->parameterId);
        if (parameter != nullptr) {
          symbolId = symbolForDeclaration(parameter->name);
        }
      }
    } else if (const auto *pack = dynamic_cast<const PackExpansion *>(&expr)) {
      token = pack->name();
      if (const Symbol *symbol = resolve(pack->name())) {
        bindingKind = symbol->bindingKind;
        mutableBinding = symbol->assignable;
        staticMember = symbol->staticMember || symbol->internalLinkage;
        symbolId = toolingSymbolFor(*symbol);
      }
    } else if (const auto *qualified =
                   dynamic_cast<const QualifiedName *>(&expr)) {
      if (const Symbol *symbol = resolveQualified(qualified->name())) {
        bindingKind = symbol->bindingKind;
        mutableBinding = symbol->assignable;
        staticMember = symbol->staticMember || symbol->internalLinkage;
        symbolId = toolingSymbolFor(*symbol);
      }
      if (const ResolvedEnumeratorInfo *enumerator =
              semanticModel.findEnumerator(*qualified);
          enumerator != nullptr && enumerator->declaration != nullptr) {
        symbolId = symbolForDeclaration(enumerator->declaration->name);
        if (symbolId == 0) {
          const EnumTypeInfo *owner =
              semanticModel.findEnumType(enumerator->owner);
          const SemanticType enumType =
              SemanticType::enumType(enumerator->owner);
          symbolId = recordToolingSymbol(
              enumerator->declaration->name, SymbolKind::Enumerator,
              (owner == nullptr ? std::string{} : owner->qualifiedName + "::") +
                  enumerator->declaration->name.lexeme,
              enumType);
        }
      }
    } else if (const auto *member = dynamic_cast<const Get *>(&expr)) {
      if (const MemberInfo *resolved =
              findMember(memberAccessObjectType(*member), member->name())) {
        bindingKind = resolved->symbol.bindingKind;
        mutableBinding = resolved->symbol.assignable;
        staticMember =
            resolved->symbol.staticMember || resolved->symbol.internalLinkage;
        symbolId = toolingSymbolFor(resolved->symbol);
      }
    } else if (const auto *assignment = dynamic_cast<const Assign *>(&expr)) {
      token = assignment->name();
      roles = OccurrenceRole::Reference | OccurrenceRole::Write;
      if (assignment->oper().kind != TokenKind::EQUAL) {
        roles |= OccurrenceRole::Read;
      }
      const Symbol *symbol = assignment->path().segments.size() > 1
                                 ? resolveQualified(assignment->path())
                                 : resolve(assignment->name());
      if (symbol != nullptr) {
        bindingKind = symbol->bindingKind;
        mutableBinding = symbol->assignable;
        staticMember = symbol->staticMember || symbol->internalLinkage;
        symbolId = toolingSymbolFor(*symbol);
      }
    } else if (const auto *set = dynamic_cast<const Set *>(&expr)) {
      roles = OccurrenceRole::Reference | OccurrenceRole::Write;
      if (set->oper().kind != TokenKind::EQUAL) {
        roles |= OccurrenceRole::Read;
      }
      if (const MemberInfo *resolved =
              findMember(memberAccessObjectType(*set), set->name())) {
        bindingKind = resolved->symbol.bindingKind;
        mutableBinding = resolved->symbol.assignable;
        staticMember =
            resolved->symbol.staticMember || resolved->symbol.internalLinkage;
        symbolId = toolingSymbolFor(resolved->symbol);
      }
    }
    semanticModel.recordResolvedSymbol(expr, symbolId);
    if (!token.generated) {
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(token),
           .kind = SemanticOccurrenceKind::Expression,
           .symbol = symbolId,
           .roles = roles,
           .name = token.lexeme,
           .type = info.type,
           .traits = info.traits,
           .access = info.access,
           .mutableBinding = mutableBinding,
           .bindingKind = bindingKind,
           .staticMember = staticMember});
    }
    currentType = result;
    return result;
  }

  SemanticType analyze(const ExprPtr &expr) {
    return expr ? analyze(*expr) : SemanticType::Unknown;
  }

  SemanticType analyze(const ExprPtr &expr, OccurrenceRole roles) {
    return expr ? analyze(*expr, roles) : SemanticType::Unknown;
  }

  SemanticType analyzeInitializer(const ExprPtr &expr,
                                  const SemanticType &expectedType) {
    const std::optional<SemanticType> enclosingType = contextualInitializerType;
    contextualInitializerType = expectedType;
    const SemanticType result = analyze(expr);
    contextualInitializerType = enclosingType;
    return result;
  }

  [[nodiscard]] bool isDefaultLibraryUnit(SourceUnitId sourceUnit) const {
    if (sourceGraph == nullptr) {
      return false;
    }
    const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
    return unit != nullptr &&
           (unit->prelude || unit->standardLibraryName.has_value());
  }

  [[nodiscard]] AccessModifier memberAccess(ClassId ownerId,
                                            const Token &declaration) const {
    if (ownerId == 0) {
      return AccessModifier::Public;
    }
    const ClassInfo &owner = classInfo(ownerId);
    const auto member = owner.members.find(declaration.lexeme);
    return member == owner.members.end() ? AccessModifier::Public
                                         : member->second.access;
  }

  [[nodiscard]] AccessModifier memberAccess(const Token &declaration) const {
    return memberAccess(currentClass.value_or(0), declaration);
  }

  [[nodiscard]] AccessModifier
  functionAccess(const FunctionInfo *function) const {
    if (function == nullptr || function->ownerClass == 0 ||
        function->declaration == nullptr) {
      return AccessModifier::Public;
    }
    const ClassInfo &owner = classInfo(function->ownerClass);
    const auto member =
        owner.members.find(function->declaration->name().lexeme);
    if (member == owner.members.end()) {
      return AccessModifier::Public;
    }
    const auto overload =
        std::find_if(member->second.symbol.overloads.begin(),
                     member->second.symbol.overloads.end(),
                     [function](const FunctionCandidate &candidate) {
                       return candidate.declaration == function->declaration;
                     });
    return overload == member->second.symbol.overloads.end()
               ? member->second.access
               : overload->access;
  }

  [[nodiscard]] static AccessModifier
  constructorAccess(const ClassInfo &owner,
                    const ConstructorDecl *declaration) {
    const auto found =
        std::find_if(owner.constructors.begin(), owner.constructors.end(),
                     [declaration](const ConstructorInfo &candidate) {
                       return candidate.declaration == declaration;
                     });
    if (found != owner.constructors.end()) {
      return found->access;
    }
    if (owner.copyConstructor &&
        owner.copyConstructor->declaration == declaration) {
      return owner.copyConstructor->access;
    }
    if (owner.moveConstructor &&
        owner.moveConstructor->declaration == declaration) {
      return owner.moveConstructor->access;
    }
    return AccessModifier::Public;
  }

  SymbolId recordToolingSymbol(const Token &name, SymbolKind kind,
                               std::string qualified, const SemanticType &type,
                               bool mutableBinding = false,
                               bool definition = true,
                               AccessModifier access = AccessModifier::Public,
                               bool staticMember = false,
                               bool internalLinkage = false) {
    const SourceUnitId sourceUnit = sourceUnitFor(name);
    const SourceSpan span = tokenSpan(name);
    return semanticModel.recordSymbol(
        {.kind = kind,
         .name = name.lexeme,
         .qualifiedName = std::move(qualified),
         .sourceUnit = sourceUnit,
         .nameSpan = span,
         .declarationSpan = span,
         .definitionSpan =
             definition ? std::optional<SourceSpan>(span) : std::nullopt,
         .type = type,
         .traits = typeTraits(type),
         .access = access,
         .mutableBinding = mutableBinding,
         .defaultLibrary = isDefaultLibraryUnit(sourceUnit),
         .staticMember = staticMember,
         .internalLinkage = internalLinkage});
  }

  [[nodiscard]] SymbolId symbolForDeclaration(const Token &name) const {
    return semanticModel.symbolForDeclaration(sourceUnitFor(name),
                                              tokenSpan(name));
  }

  [[nodiscard]] static SymbolKind
  bindingSymbolKind(SemanticBindingKind bindingKind) {
    switch (bindingKind) {
    case SemanticBindingKind::GlobalVariable:
      return SymbolKind::GlobalVariable;
    case SemanticBindingKind::LocalVariable:
      return SymbolKind::LocalVariable;
    case SemanticBindingKind::Parameter:
      return SymbolKind::Parameter;
    case SemanticBindingKind::Field:
    case SemanticBindingKind::StaticField:
      return SymbolKind::Field;
    case SemanticBindingKind::LambdaCapture:
      return SymbolKind::LambdaCapture;
    case SemanticBindingKind::None:
      return SymbolKind::LocalVariable;
    }
    return SymbolKind::LocalVariable;
  }

  SymbolId recordBindingSymbol(const Token &name, const SemanticType &type,
                               bool mutableBinding,
                               SemanticBindingKind bindingKind,
                               ClassId ownerClass = 0,
                               std::string_view resolvedQualifiedName = {},
                               bool staticMember = false,
                               bool internalLinkage = false) {
    std::string qualified = name.lexeme;
    if (bindingKind == SemanticBindingKind::GlobalVariable) {
      qualified = resolvedQualifiedName.empty()
                      ? qualifiedName(currentNamespace, name.lexeme)
                      : std::string(resolvedQualifiedName);
    } else if (bindingKind == SemanticBindingKind::Field ||
               bindingKind == SemanticBindingKind::StaticField) {
      ownerClass = ownerClass == 0 ? currentClass.value_or(0) : ownerClass;
    }
    if ((bindingKind == SemanticBindingKind::Field ||
         bindingKind == SemanticBindingKind::StaticField) &&
        ownerClass != 0) {
      const ClassInfo &owner = classInfo(ownerClass);
      qualified = qualifiedName(owner.namespaceScope,
                                owner.name.lexeme + "::" + name.lexeme);
    }
    return recordToolingSymbol(name, bindingSymbolKind(bindingKind),
                               std::move(qualified), type, mutableBinding, true,
                               (bindingKind == SemanticBindingKind::Field ||
                                bindingKind == SemanticBindingKind::StaticField)
                                   ? memberAccess(ownerClass, name)
                                   : AccessModifier::Public,
                               staticMember, internalLinkage);
  }

  SymbolId recordFunctionSymbol(const FunctionDecl &declaration) {
    const FunctionInfo *info = semanticModel.findFunction(declaration);
    const Token &name = declaration.operatorName()
                            ? declaration.operatorName()->symbol
                            : declaration.name();
    const bool method = info != nullptr && info->ownerClass != 0;
    const SymbolKind kind =
        declaration.operatorName()
            ? SymbolKind::Operator
            : (method ? SymbolKind::Method : SymbolKind::Function);
    return recordToolingSymbol(
        name, kind, info == nullptr ? name.lexeme : info->qualifiedName,
        info == nullptr ? SemanticType::Unknown : info->returnType, false,
        declaration.body() != nullptr, functionAccess(info),
        info != nullptr && info->staticMember,
        info != nullptr && info->internalLinkage);
  }

  SymbolId toolingSymbolFor(const Symbol &symbol) {
    SymbolId id = symbolForDeclaration(symbol.declaration);
    if (id != 0) {
      return id;
    }
    if (symbol.bindingKind != SemanticBindingKind::None) {
      return recordBindingSymbol(symbol.declaration, symbol.type,
                                 symbol.assignable, symbol.bindingKind,
                                 symbol.ownerClass, symbol.qualifiedName,
                                 symbol.staticMember, symbol.internalLinkage);
    }
    if (symbol.overloads.size() == 1 &&
        symbol.overloads.front().declaration != nullptr) {
      return recordFunctionSymbol(*symbol.overloads.front().declaration);
    }
    return 0;
  }

  [[nodiscard]] const GenericParameterInfo *
  genericParameterInfo(GenericParameterId id) const {
    for (const ClassInfo &owner : classes) {
      const auto found = std::find_if(
          owner.genericParameters.begin(), owner.genericParameters.end(),
          [id](const GenericParameterInfo &parameter) {
            return parameter.id == id;
          });
      if (found != owner.genericParameters.end()) {
        return &*found;
      }
    }
    for (const auto &[_, parameters] : functionGenericParameters) {
      const auto found =
          std::find_if(parameters.begin(), parameters.end(),
                       [id](const GenericParameterInfo &parameter) {
                         return parameter.id == id;
                       });
      if (found != parameters.end()) {
        return &*found;
      }
    }
    return nullptr;
  }

  void recordGenericParameterUse(const Token &use,
                                 const CompileTimeValue &value,
                                 OccurrenceRole role) {
    if (value.kind != CompileTimeValue::Parameter) {
      return;
    }
    const GenericParameterInfo *parameter =
        genericParameterInfo(value.parameterId);
    if (parameter == nullptr) {
      return;
    }
    const SymbolId symbol = symbolForDeclaration(parameter->name);
    if (symbol == 0) {
      return;
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(use),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Reference | role,
         .name = use.lexeme,
         .type = SemanticType::UInt64,
         .traits = typeTraits(SemanticType::UInt64)});
  }

  void recordTypeUse(const TypeRef &type) {
    if (type.name.last().kind != TokenKind::IDENTIFIER) {
      return;
    }

    recordQualifiedPathUses(type.name);

    SymbolId symbol = 0;
    SemanticType resolvedType = typeOf(type);
    if (const std::optional<SemanticType> parameter =
            resolveTypeParameter(type.name)) {
      const GenericParameterInfo *info =
          genericParameterInfo(parameter->genericParameterId);
      if (info != nullptr) {
        symbol = symbolForDeclaration(info->name);
      }
    } else if (const std::optional<TypeAliasId> aliasId =
                   resolveTypeAliasPath(type.name, currentNamespace)) {
      if (*aliasId != 0 && *aliasId <= typeAliases.size()) {
        const RegisteredTypeAlias &alias = typeAliases[*aliasId - 1];
        symbol = symbolForDeclaration(alias.declaration->name());
        if (symbol == 0) {
          symbol = recordToolingSymbol(alias.declaration->name(),
                                       SymbolKind::TypeAlias,
                                       alias.qualifiedName, alias.type);
        }
      }
    } else if (const std::optional<EnumId> enumId =
                   resolveEnumPath(type.name, currentNamespace)) {
      if (*enumId != 0 && *enumId <= enums.size()) {
        const EnumInfo &owner = enums[*enumId - 1];
        symbol = symbolForDeclaration(owner.name);
        if (symbol == 0) {
          symbol = recordToolingSymbol(
              owner.name, SymbolKind::Enum,
              qualifiedName(owner.namespaceScope, owner.name.lexeme),
              SemanticType::enumType(owner.id));
        }
      }
    } else if (const std::optional<ClassId> classId =
                   resolveClassPath(type.name, currentNamespace)) {
      const ClassInfo &owner = classInfo(*classId);
      symbol = symbolForDeclaration(owner.name);
      if (symbol == 0) {
        symbol = recordToolingSymbol(
            owner.name,
            owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                            : SymbolKind::Class,
            qualifiedName(owner.namespaceScope, owner.name.lexeme),
            SemanticType::classType(owner.id));
      }
    }
    if (symbol == 0) {
      return;
    }

    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(type.name.last()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
         .name = type.name.last().lexeme,
         .type = resolvedType,
         .traits = typeTraits(resolvedType)});
  }

  void recordQualifiedPathUses(const NamePath &path,
                               bool finalSegmentIsNamespace = false) {
    if (path.segments.empty()) {
      return;
    }
    const std::size_t namespaceSegments = finalSegmentIsNamespace
                                              ? path.segments.size()
                                              : path.segments.size() - 1;
    const NamePath namespacePath(std::vector<Token>(
        path.segments.begin(), path.segments.begin() + namespaceSegments));
    const std::vector<ResolvedNamespaceSegment> resolvedSegments =
        resolveNamespaceSegments(namespacePath, currentNamespace);
    for (std::size_t index = 0; index < resolvedSegments.size(); ++index) {
      const auto symbol =
          namespaceToolingSymbols.find(resolvedSegments[index].declaration);
      if (symbol == namespaceToolingSymbols.end() || symbol->second == 0) {
        continue;
      }
      const Token &token = path.segments[index];
      semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                      .span = tokenSpan(token),
                                      .kind = SemanticOccurrenceKind::Symbol,
                                      .symbol = symbol->second,
                                      .roles = OccurrenceRole::Reference,
                                      .name = token.lexeme});
    }

    if (finalSegmentIsNamespace || path.segments.size() < 2) {
      return;
    }
    const NamePath ownerPath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    if (const std::optional<ClassId> classId =
            resolveClassPath(ownerPath, currentNamespace)) {
      const ClassInfo &owner = classInfo(*classId);
      SymbolId symbol = symbolForDeclaration(owner.name);
      if (symbol == 0) {
        symbol = recordToolingSymbol(
            owner.name,
            owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                            : SymbolKind::Class,
            qualifiedName(owner.namespaceScope, owner.name.lexeme),
            SemanticType::classType(owner.id));
      }
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(ownerPath.last()),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbol,
           .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
           .name = ownerPath.last().lexeme,
           .type = SemanticType::classType(owner.id),
           .traits = typeTraits(SemanticType::classType(owner.id))});
      return;
    }
    const std::optional<EnumId> enumId =
        resolveEnumPath(ownerPath, currentNamespace);
    if (!enumId || *enumId == 0 || *enumId > enums.size()) {
      return;
    }
    const EnumInfo &owner = enums[*enumId - 1];
    SymbolId symbol = symbolForDeclaration(owner.name);
    if (symbol == 0) {
      symbol = recordToolingSymbol(
          owner.name, SymbolKind::Enum,
          qualifiedName(owner.namespaceScope, owner.name.lexeme),
          SemanticType::enumType(owner.id));
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(ownerPath.last()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
         .name = ownerPath.last().lexeme,
         .type = SemanticType::enumType(owner.id),
         .traits = typeTraits(SemanticType::enumType(owner.id))});
  }

  SymbolId recordBindingOccurrence(const Token &name, const SemanticType &type,
                                   bool mutableBinding,
                                   SemanticBindingKind bindingKind,
                                   bool staticMember = false,
                                   bool internalLinkage = false) {
    const AccessMode access =
        mutableBinding ? AccessMode::Mutable : AccessMode::ReadOnly;
    const SymbolId symbol =
        recordBindingSymbol(name, type, mutableBinding, bindingKind, 0, {},
                            staticMember, internalLinkage);
    OccurrenceRole roles = OccurrenceRole::Declaration;
    if (bindingKind != SemanticBindingKind::Parameter) {
      roles |= OccurrenceRole::Definition;
    }
    if (!name.generated) {
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(name),
           .kind = SemanticOccurrenceKind::Binding,
           .symbol = symbol,
           .roles = roles,
           .name = name.lexeme,
           .type = type,
           .traits = typeTraits(type),
           .access = access,
           .mutableBinding = mutableBinding,
           .bindingKind = bindingKind,
           .staticMember = staticMember || internalLinkage});
    }
    return symbol;
  }

  [[nodiscard]] static std::optional<std::size_t>
  genericParameterIndex(const FunctionCandidate &function,
                        GenericParameterId id) {
    for (std::size_t index = 0; index < function.genericParameters.size();
         ++index) {
      if (function.genericParameters[index].id == id) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static bool
  sameSignatureType(const SemanticType &left, const FunctionCandidate &leftFn,
                    const SemanticType &right,
                    const FunctionCandidate &rightFn) {
    const std::optional<std::size_t> leftParameter =
        left.kind == SemanticType::TypeParameter
            ? genericParameterIndex(leftFn, left.genericParameterId)
            : std::nullopt;
    const std::optional<std::size_t> rightParameter =
        right.kind == SemanticType::TypeParameter
            ? genericParameterIndex(rightFn, right.genericParameterId)
            : std::nullopt;
    if (leftParameter || rightParameter) {
      return leftParameter && rightParameter &&
             *leftParameter == *rightParameter;
    }
    if (left.kind != right.kind || left.classId != right.classId ||
        left.genericParameterId != right.genericParameterId ||
        left.arrayLength != right.arrayLength ||
        left.arrayLengthParameterId != right.arrayLengthParameterId ||
        left.valueArguments != right.valueArguments ||
        left.referenceAccess != right.referenceAccess ||
        left.arguments.size() != right.arguments.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.arguments.size(); ++index) {
      if (!sameSignatureType(left.arguments[index], leftFn,
                             right.arguments[index], rightFn)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static bool
  sameFunctionSignature(const FunctionCandidate &left,
                        const FunctionCandidate &right) {
    const bool receiverQualified =
        left.ownerClass != 0 && right.ownerClass != 0;
    if ((receiverQualified &&
         left.receiverMutability != right.receiverMutability) ||
        left.parameterPack != right.parameterPack ||
        left.genericParameters.size() != right.genericParameters.size() ||
        left.parameterTypes.size() != right.parameterTypes.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.parameterTypes.size(); ++index) {
      if (!sameSignatureType(left.parameterTypes[index], left,
                             right.parameterTypes[index], right)) {
        return false;
      }
    }
    return true;
  }

  bool appendFunctionOverload(Symbol &existing, Symbol incoming,
                              const Token &name) {
    if (existing.type != SemanticType::Function ||
        incoming.type != SemanticType::Function ||
        incoming.overloads.size() != 1) {
      return false;
    }

    if (existing.ownerClass != 0 && incoming.ownerClass != 0 &&
        existing.staticMember != incoming.staticMember) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2039", DiagnosticPhase::Semantics, name,
          "Static and instance methods cannot share an overload set in the "
          "current class model.");
      diagnostic.related.push_back({tokenSpan(existing.declaration),
                                    "Previous method declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }

    FunctionCandidate candidate = std::move(incoming.overloads.front());
    const FunctionInfo *candidateInfo =
        candidate.declaration == nullptr
            ? nullptr
            : semanticModel.findFunction(*candidate.declaration);
    if (candidateInfo != nullptr && candidateInfo->entryPoint) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                         "The main entry point cannot be overloaded.");
      diagnostic.related.push_back(
          {tokenSpan(existing.overloads.front().declaration->name()),
           "Previous main declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }

    for (const FunctionCandidate &previous : existing.overloads) {
      if ((previous.declaration != nullptr &&
           previous.declaration->runtimeBinding()) ||
          (candidate.declaration != nullptr &&
           candidate.declaration->runtimeBinding())) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                           "Runtime-bound functions cannot be overloaded.");
        diagnostic.related.push_back(
            {tokenSpan(previous.declaration->name()),
             "Previous function declaration is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return true;
      }
      if (sameFunctionSignature(previous, candidate)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2011", DiagnosticPhase::Semantics, name,
            "Duplicate overload signature for '" + name.lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(previous.declaration->name()),
             "Previous overload with this parameter signature is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return true;
      }
    }

    existing.overloads.emplace_back(std::move(candidate));
    return true;
  }

  bool declare(const Token &name, SemanticType type, bool assignable,
               SemanticBindingKind bindingKind,
               const VariableDecl *variableDeclaration = nullptr,
               const Parameter *parameterDeclaration = nullptr) {
    return declare(name, Symbol{.type = type,
                                .assignable = assignable,
                                .declaration = name,
                                .variableDeclaration = variableDeclaration,
                                .parameterDeclaration = parameterDeclaration,
                                .bindingKind = bindingKind});
  }

  bool declare(const Token &name, Symbol symbol) {
    const auto existing = scopes.back().find(name.lexeme);
    if (existing != scopes.back().end()) {
      if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
        return true;
      }
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                         "Duplicate declaration of '" + name.lexeme + "'.");
      diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                    "Previous declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    scopes.back().emplace(name.lexeme, std::move(symbol));
    return true;
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, SemanticType type,
                              bool assignable, bool internalLinkage = false,
                              SymbolId toolingSymbol = 0) {
    return declareNamespaceSymbol(
        scope, name,
        Symbol{.type = type,
               .sourceUnit = currentSourceUnit,
               .assignable = assignable,
               .declaration = name,
               .bindingKind = type.kind == SemanticType::TypeName
                                  ? SemanticBindingKind::None
                                  : SemanticBindingKind::GlobalVariable,
               .internalLinkage = internalLinkage,
               .toolingSymbol = toolingSymbol});
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, Symbol symbol) {
    const std::string qualified = qualifiedName(scope, name.lexeme);
    symbol.qualifiedName = qualified;
    const bool sourceLocal = symbol.internalLinkage && sourceGraph != nullptr &&
                             currentSourceUnit != 0;
    bool categoryConflict = namespaces.contains(qualified) ||
                            namespaceAliases.contains(qualified) ||
                            typeAliasIds.contains(qualified) ||
                            enumIds.contains(qualified);
    if (sourceLocal) {
      categoryConflict = namespaceIsVisible(qualified) ||
                         findNamespaceAlias(qualified) != nullptr ||
                         currentTypeAliasIds().contains(qualified) ||
                         currentEnumIds().contains(qualified);
    }
    if (categoryConflict) {
      report(name, "Duplicate declaration of '" + name.lexeme + "'.");
      return false;
    }

    if (sourceLocal) {
      auto &unitSymbols = internalNamespaceSymbols[currentSourceUnit];
      const auto existing = unitSymbols.find(qualified);
      if (existing != unitSymbols.end()) {
        const Symbol published = symbol;
        if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
          publishNamespaceSymbol(qualified, published);
          return true;
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, name,
            "Duplicate static declaration of '" + name.lexeme + "'.");
        diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                      "Previous static declaration is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return false;
      }
      if (const auto external = namespaceSymbols.find(qualified);
          external != namespaceSymbols.end() &&
          sourceVisible(declarationSourceUnit(external->second))) {
        report(name,
               "Static declaration of '" + name.lexeme +
                   "' conflicts with a visible namespace declaration.",
               "GTI-S2006");
        return false;
      }
      publishNamespaceSymbol(qualified, symbol);
      unitSymbols.emplace(qualified, std::move(symbol));
      return true;
    }

    if (const auto unit = internalNamespaceSymbols.find(currentSourceUnit);
        unit != internalNamespaceSymbols.end() &&
        unit->second.contains(qualified)) {
      report(name,
             "Declaration of '" + name.lexeme +
                 "' conflicts with a static declaration in this source "
                 "file.",
             "GTI-S2006");
      return false;
    }

    const auto existing = namespaceSymbols.find(qualified);
    if (existing != namespaceSymbols.end()) {
      const Symbol published = symbol;
      if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
        publishNamespaceSymbol(qualified, published);
        return true;
      }
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                         "Duplicate declaration of '" + name.lexeme + "'.");
      diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                    "Previous declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    publishNamespaceSymbol(qualified, symbol);
    namespaceSymbols.emplace(qualified, std::move(symbol));
    return true;
  }

  [[nodiscard]] static bool directNamespaceChild(std::string_view qualified,
                                                 std::string_view parent,
                                                 std::string &name) {
    std::string_view remainder = qualified;
    if (!parent.empty()) {
      if (!qualified.starts_with(parent) ||
          qualified.size() <= parent.size() + 2 ||
          qualified.substr(parent.size(), 2) != "::") {
        return false;
      }
      remainder.remove_prefix(parent.size() + 2);
    }
    if (remainder.empty() || remainder.find("::") != std::string_view::npos) {
      return false;
    }
    name = std::string(remainder);
    return true;
  }

  [[nodiscard]] SemanticCompletionCandidateKind
  completionKind(const Symbol &symbol) const {
    if (symbol.type == SemanticType::Function) {
      return symbol.ownerClass == 0 ? SemanticCompletionCandidateKind::Function
                                    : SemanticCompletionCandidateKind::Method;
    }
    if (symbol.type.kind == SemanticType::TypeName) {
      const ClassInfo *owner =
          classInfo(SemanticType::classType(symbol.type.classId));
      return owner != nullptr && owner->kind == ClassKind::Struct
                 ? SemanticCompletionCandidateKind::Struct
                 : SemanticCompletionCandidateKind::Class;
    }
    switch (symbol.bindingKind) {
    case SemanticBindingKind::GlobalVariable:
      return SemanticCompletionCandidateKind::GlobalVariable;
    case SemanticBindingKind::Parameter:
      return SemanticCompletionCandidateKind::Parameter;
    case SemanticBindingKind::Field:
    case SemanticBindingKind::StaticField:
      return SemanticCompletionCandidateKind::Field;
    case SemanticBindingKind::LocalVariable:
    case SemanticBindingKind::LambdaCapture:
      return SemanticCompletionCandidateKind::LocalVariable;
    case SemanticBindingKind::None:
      return symbol.ownerClass == 0
                 ? SemanticCompletionCandidateKind::LocalVariable
                 : SemanticCompletionCandidateKind::Field;
    }
    return SemanticCompletionCandidateKind::LocalVariable;
  }

  void appendSymbolCompletions(
      std::vector<SemanticCompletionCandidateRecord> &result,
      const std::string &name, const std::string &qualifiedName,
      const Symbol &symbol, std::size_t scopeDistance,
      bool substitutedCallable = false, bool mutableReceiver = true) const {
    if (name.rfind("__gti_", 0) == 0 ||
        symbol.valueState != ValueState::Available) {
      return;
    }
    if (currentStaticMemberFunction && symbol.ownerClass != 0 &&
        !symbol.staticMember) {
      return;
    }
    if (symbol.type == SemanticType::Function) {
      for (const FunctionCandidate &overload : symbol.overloads) {
        if (overload.access == AccessModifier::Private &&
            currentClass != overload.ownerClass) {
          continue;
        }
        if (!mutableReceiver &&
            overload.receiverMutability == ReceiverMutability::Mutable) {
          continue;
        }
        result.push_back(
            {.kind = overload.ownerClass == 0
                         ? SemanticCompletionCandidateKind::Function
                         : SemanticCompletionCandidateKind::Method,
             .name = name,
             .qualifiedName = qualifiedName,
             .type = overload.returnType,
             .scopeDistance = scopeDistance,
             .function = overload.id,
             .parameterTypes = overload.parameterTypes,
             .substitutedCallable = substitutedCallable,
             .staticMember =
                 overload.staticMember || overload.internalLinkage});
      }
      return;
    }

    SemanticCompletionCandidateRecord candidate{
        .kind = completionKind(symbol),
        .name = name,
        .qualifiedName = qualifiedName,
        .type = symbol.type,
        .mutableBinding = symbol.assignable,
        .scopeDistance = scopeDistance,
        .staticMember = symbol.staticMember || symbol.internalLinkage};
    if (symbol.type.kind == SemanticType::TypeName) {
      candidate.classType = symbol.type.classId;
    }
    result.push_back(std::move(candidate));
  }

  void appendNamespaceChildren(
      std::vector<SemanticCompletionCandidateRecord> &result,
      const std::string &parent, std::size_t scopeDistance,
      std::unordered_set<std::string> *seen = nullptr) const {
    const auto appendName = [&](const std::string &name) {
      return seen == nullptr || seen->insert(name).second;
    };
    for (const auto &[qualified, symbol] : currentNamespaceSymbols()) {
      std::string name;
      if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
        appendSymbolCompletions(result, name, qualified, symbol, scopeDistance);
      }
    }
    for (const auto &[qualified, id] : currentEnumIds()) {
      std::string name;
      if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
        result.push_back({.kind = SemanticCompletionCandidateKind::Enum,
                          .name = name,
                          .qualifiedName = qualified,
                          .type = SemanticType::enumType(id),
                          .scopeDistance = scopeDistance,
                          .enumType = id});
      }
    }
    for (const auto &[qualified, id] : currentTypeAliasIds()) {
      std::string name;
      if (!directNamespaceChild(qualified, parent, name) || !appendName(name) ||
          id == 0 || id > typeAliases.size()) {
        continue;
      }
      const RegisteredTypeAlias &alias = typeAliases[id - 1];
      result.push_back({.kind = SemanticCompletionCandidateKind::TypeAlias,
                        .name = name,
                        .qualifiedName = qualified,
                        .type = alias.type,
                        .scopeDistance = scopeDistance,
                        .typeAlias = alias.declaration});
    }

    const auto &visibleNamespaceSet = [&]() -> const auto & {
      if (sourceGraph == nullptr || currentSourceUnit == 0) {
        return namespaces;
      }
      const auto found = visibleNamespaces.find(currentSourceUnit);
      if (found != visibleNamespaces.end()) {
        return found->second;
      }
      static const std::unordered_set<std::string> empty;
      return empty;
    }();
    for (const std::string &qualified : visibleNamespaceSet) {
      std::string name;
      if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
        result.push_back({.kind = SemanticCompletionCandidateKind::Namespace,
                          .name = name,
                          .qualifiedName = qualified,
                          .scopeDistance = scopeDistance});
      }
    }

    const auto &visibleAliases = [&]() -> const auto & {
      if (sourceGraph == nullptr || currentSourceUnit == 0) {
        return namespaceAliases;
      }
      const auto found = visibleNamespaceAliases.find(currentSourceUnit);
      if (found != visibleNamespaceAliases.end()) {
        return found->second;
      }
      static const std::unordered_map<std::string, NamespaceAliasInfo> empty;
      return empty;
    }();
    for (const auto &[qualified, _] : visibleAliases) {
      std::string name;
      if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
        result.push_back({.kind = SemanticCompletionCandidateKind::Namespace,
                          .name = name,
                          .qualifiedName = qualified,
                          .scopeDistance = scopeDistance});
      }
    }
  }

  void captureUnqualifiedCompletion(const Token &completion) {
    SemanticCompletionContext context{.kind =
                                          SemanticCompletionKind::Unqualified,
                                      .sourceUnit = currentSourceUnit,
                                      .replacementRange = tokenSpan(completion),
                                      .prefix = completion.lexeme};
    std::unordered_set<std::string> seen;
    std::size_t distance = 0;
    for (auto scope = scopes.rbegin(); scope != scopes.rend();
         ++scope, ++distance) {
      for (const auto &[name, symbol] : *scope) {
        if (seen.insert(name).second) {
          appendSymbolCompletions(context.candidates, name, name, symbol,
                                  distance);
        }
      }
    }
    for (auto types = typeParameterScopes.rbegin();
         types != typeParameterScopes.rend(); ++types, ++distance) {
      for (const auto &[name, type] : *types) {
        if (seen.insert(name).second) {
          context.candidates.push_back(
              {.kind = SemanticCompletionCandidateKind::TypeParameter,
               .name = name,
               .qualifiedName = name,
               .type = type,
               .scopeDistance = distance});
        }
      }
    }
    for (auto values = valueParameterScopes.rbegin();
         values != valueParameterScopes.rend(); ++values, ++distance) {
      for (const auto &[name, _] : *values) {
        if (seen.insert(name).second) {
          context.candidates.push_back(
              {.kind = SemanticCompletionCandidateKind::ValueParameter,
               .name = name,
               .qualifiedName = name,
               .type = SemanticType::UInt64,
               .scopeDistance = distance});
        }
      }
    }
    for (std::size_t depth = currentNamespace.size() + 1; depth > 0;
         --depth, ++distance) {
      const std::vector<std::string> scope(
          currentNamespace.begin(), currentNamespace.begin() + depth - 1);
      std::string parent = qualifiedName(scope, "");
      if (!parent.empty()) {
        parent.resize(parent.size() - 2);
      }
      appendNamespaceChildren(context.candidates, parent, distance, &seen);
    }
    semanticModel.recordCompletion(std::move(context));
  }

  void captureQualifiedCompletion(const NamePath &path) {
    const Token &completion = path.last();
    SemanticCompletionContext context{.kind = SemanticCompletionKind::Namespace,
                                      .sourceUnit = currentSourceUnit,
                                      .replacementRange = tokenSpan(completion),
                                      .prefix = completion.lexeme};
    if (path.segments.size() < 2) {
      semanticModel.recordCompletion(std::move(context));
      return;
    }
    const NamePath ownerPath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    if (const std::optional<EnumId> owner =
            resolveEnumPath(ownerPath, currentNamespace)) {
      context.kind = SemanticCompletionKind::Enum;
      const EnumInfo &enumeration = enums.at(*owner - 1);
      for (const auto &[name, enumerator] : enumeration.enumerators) {
        context.candidates.push_back(
            {.kind = SemanticCompletionCandidateKind::Enumerator,
             .name = name,
             .qualifiedName = qualifiedName(enumeration.namespaceScope,
                                            enumeration.name.lexeme) +
                              "::" + name,
             .type = enumerator.symbol.type,
             .scopeDistance = 0,
             .enumType = *owner});
      }
    } else if (const std::optional<ClassId> owner =
                   resolveClassPath(ownerPath, currentNamespace)) {
      context.kind = SemanticCompletionKind::Member;
      const ClassInfo &classType = classInfo(*owner);
      for (const auto &[name, member] : classType.members) {
        if (!member.symbol.staticMember ||
            (member.access == AccessModifier::Private &&
             currentClass != member.symbol.ownerClass)) {
          continue;
        }
        appendSymbolCompletions(
            context.candidates, name,
            qualifiedName(classType.namespaceScope,
                          classType.name.lexeme + "::" + name),
            member.symbol, 0);
      }
    } else if (const std::optional<std::string> owner =
                   resolveNamespacePath(ownerPath, currentNamespace)) {
      appendNamespaceChildren(context.candidates, *owner, 0);
    }
    semanticModel.recordCompletion(std::move(context));
  }

  void captureMemberCompletion(const Token &completion,
                               const SemanticType &objectType,
                               bool mutableReceiver) {
    SemanticCompletionContext context{.kind = SemanticCompletionKind::Member,
                                      .sourceUnit = currentSourceUnit,
                                      .replacementRange = tokenSpan(completion),
                                      .prefix = completion.lexeme};
    if (objectType.kind == SemanticType::Class) {
      if (const ClassInfo *owner = classInfo(objectType)) {
        for (const auto &[name, member] : owner->members) {
          if (member.symbol.staticMember) {
            continue;
          }
          if (member.access == AccessModifier::Private &&
              currentClass != member.symbol.ownerClass) {
            continue;
          }
          const Symbol substituted =
              substituteSymbol(member.symbol, objectType);
          appendSymbolCompletions(context.candidates, name, name, substituted,
                                  0, true, mutableReceiver);
        }
      }
    } else if (objectType.kind == SemanticType::Array) {
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::Method,
           .name = "size",
           .qualifiedName = "size",
           .detail = "uint64_t size()",
           .scopeDistance = 0});
    } else if (objectType.kind == SemanticType::StringView) {
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::Method,
           .name = "size",
           .qualifiedName = "size",
           .detail = "uint64_t size()",
           .scopeDistance = 0});
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::Method,
           .name = "empty",
           .qualifiedName = "empty",
           .detail = "bool empty()",
           .scopeDistance = 0});
    }
    semanticModel.recordCompletion(std::move(context));
  }

  [[nodiscard]] const Symbol *resolve(const Token &name) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (const auto found = scope->find(name.lexeme); found != scope->end()) {
        return &found->second;
      }
    }

    const auto &symbols = currentNamespaceSymbols();
    for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
      std::vector<std::string> scope(currentNamespace.begin(),
                                     currentNamespace.begin() + depth - 1);
      const auto found = symbols.find(qualifiedName(scope, name.lexeme));
      if (found != symbols.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] Symbol *resolveMutable(const Token &name) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (const auto found = scope->find(name.lexeme); found != scope->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<ResolvedNamespaceSegment>
  resolveNamespaceSegments(const NamePath &path,
                           const std::vector<std::string> &fromScope) const {
    std::vector<ResolvedNamespaceSegment> result;
    if (path.segments.empty()) {
      return result;
    }

    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      std::vector<std::string> scope(fromScope.begin(),
                                     fromScope.begin() + depth - 1);
      const std::string candidate = qualifiedName(scope, path.first().lexeme);
      if (const NamespaceAliasInfo *alias = findNamespaceAlias(candidate)) {
        result.push_back({.declaration = candidate, .target = alias->target});
        break;
      }
      if (namespaceIsVisible(candidate)) {
        result.push_back({.declaration = candidate, .target = candidate});
        break;
      }
    }
    if (result.empty()) {
      return result;
    }

    for (std::size_t index = 1; index < path.segments.size(); ++index) {
      const std::string candidate =
          result.back().target + "::" + path.segments[index].lexeme;
      if (const NamespaceAliasInfo *alias = findNamespaceAlias(candidate)) {
        result.push_back({.declaration = candidate, .target = alias->target});
      } else if (namespaceIsVisible(candidate)) {
        result.push_back({.declaration = candidate, .target = candidate});
      } else {
        break;
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path,
                       const std::vector<std::string> &fromScope) const {
    if (path.segments.empty()) {
      return std::nullopt;
    }
    const std::vector<ResolvedNamespaceSegment> segments =
        resolveNamespaceSegments(path, fromScope);
    return segments.size() == path.segments.size()
               ? std::optional<std::string>(segments.back().target)
               : std::nullopt;
  }

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path) const {
    return resolveNamespacePath(path, currentNamespace);
  }

  [[nodiscard]] const Symbol *resolveQualified(const NamePath &path) const {
    if (path.segments.size() < 2) {
      return resolve(path.last());
    }

    NamePath namespacePath(std::vector<Token>(path.segments.begin(),
                                              path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath);
    if (!resolvedNamespace) {
      if (const std::optional<ClassId> classId =
              resolveClassPath(namespacePath, currentNamespace)) {
        const ClassInfo &owner = classInfo(*classId);
        const auto member = owner.members.find(path.last().lexeme);
        return member != owner.members.end() &&
                       member->second.symbol.staticMember
                   ? &member->second.symbol
                   : nullptr;
      }
      const EnumeratorRecord *enumerator = resolveEnumerator(path);
      return enumerator == nullptr ? nullptr : &enumerator->symbol;
    }
    const auto &symbols = currentNamespaceSymbols();
    const auto symbol =
        symbols.find(*resolvedNamespace + "::" + path.last().lexeme);
    if (symbol != symbols.end()) {
      return &symbol->second;
    }
    const EnumeratorRecord *enumerator = resolveEnumerator(path);
    return enumerator == nullptr ? nullptr : &enumerator->symbol;
  }

  [[nodiscard]] std::optional<std::string> resolveInitialNamespaceGlobally(
      const Token &name, const std::vector<std::string> &fromScope) const {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const std::vector<std::string> scope(fromScope.begin(),
                                           fromScope.begin() + depth - 1);
      const std::string candidate = qualifiedName(scope, name.lexeme);
      if (const auto alias = namespaceAliases.find(candidate);
          alias != namespaceAliases.end()) {
        return alias->second.target;
      }
      if (namespaces.contains(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string> resolveNamespacePathGlobally(
      const NamePath &path, const std::vector<std::string> &fromScope) const {
    std::optional<std::string> current =
        resolveInitialNamespaceGlobally(path.first(), fromScope);
    if (!current) {
      return std::nullopt;
    }
    for (std::size_t index = 1; index < path.segments.size(); ++index) {
      const std::string candidate =
          *current + "::" + path.segments[index].lexeme;
      if (const auto alias = namespaceAliases.find(candidate);
          alias != namespaceAliases.end()) {
        current = alias->second.target;
      } else if (namespaces.contains(candidate)) {
        current = candidate;
      } else {
        return std::nullopt;
      }
    }
    return current;
  }

  [[nodiscard]] const Symbol *resolveGlobally(const Token &name) const {
    for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
      const std::vector<std::string> scope(
          currentNamespace.begin(), currentNamespace.begin() + depth - 1);
      const auto found =
          namespaceSymbols.find(qualifiedName(scope, name.lexeme));
      if (found != namespaceSymbols.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const Symbol *
  resolveQualifiedGlobally(const NamePath &path) const {
    if (path.segments.size() < 2) {
      return resolveGlobally(path.last());
    }
    const NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePathGlobally(namespacePath, currentNamespace);
    if (!resolvedNamespace) {
      if (const std::optional<ClassId> classId =
              resolveClassPathGlobally(namespacePath, currentNamespace)) {
        const ClassInfo &owner = classInfo(*classId);
        const auto member = owner.members.find(path.last().lexeme);
        return member != owner.members.end() &&
                       member->second.symbol.staticMember
                   ? &member->second.symbol
                   : nullptr;
      }
      const EnumeratorRecord *enumerator = resolveEnumerator(path, true);
      return enumerator == nullptr ? nullptr : &enumerator->symbol;
    }
    const auto symbol = namespaceSymbols.find(
        *resolvedNamespace + "::" + path.last().lexeme);
    if (symbol != namespaceSymbols.end()) {
      return &symbol->second;
    }
    const EnumeratorRecord *enumerator = resolveEnumerator(path, true);
    return enumerator == nullptr ? nullptr : &enumerator->symbol;
  }

  [[nodiscard]] SourceUnitId declarationSourceUnit(const Symbol &symbol) const {
    if (!symbol.overloads.empty()) {
      return symbol.overloads.front().sourceUnit;
    }
    return symbol.sourceUnit;
  }

  bool reportInvisibleDeclaration(const Token &use, std::string name,
                                  const Token &declaration,
                                  SourceUnitId declarationUnit) {
    if (sourceGraph == nullptr || currentSourceUnit == 0 ||
        declarationUnit == 0 || sourceVisible(declarationUnit)) {
      return false;
    }

    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2024", DiagnosticPhase::Semantics, use,
        "Declaration '" + name +
            "' is not visible from this source file; include its file "
            "directly.");
    diagnostic.related.push_back(
        {tokenSpan(declaration), "Declaration is in this source unit."});
    const SourceUnit *requester = sourceGraph->findUnit(currentSourceUnit);
    const SourceUnit *target = sourceGraph->findUnit(declarationUnit);
    if (requester != nullptr && target != nullptr) {
      if (target->standardLibraryName) {
        diagnostic.hints.emplace_back("Add 'include <" +
                                      *target->standardLibraryName +
                                      ">' to this source file.");
      } else {
        std::filesystem::path relative =
            target->path.lexically_relative(requester->path.parent_path());
        if (relative.empty()) {
          relative = target->path.filename();
        }
        diagnostic.hints.emplace_back("Add 'include \"" +
                                      relative.generic_string() +
                                      "\"' to this source file.");
      }
    }
    diagnostics.push_back(std::move(diagnostic));
    return true;
  }

  bool reportInvisibleSymbol(const Token &use, std::string name,
                             const Symbol *symbol) {
    return symbol != nullptr &&
           reportInvisibleDeclaration(use, std::move(name), symbol->declaration,
                                      declarationSourceUnit(*symbol));
  }

  [[nodiscard]] std::optional<TypeAliasId>
  resolveTypeAliasPath(const NamePath &path,
                       const std::vector<std::string> &fromScope) const {
    const auto &visibleAliases = currentTypeAliasIds();
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            visibleAliases.find(qualifiedName(scope, path.last().lexeme));
        if (found != visibleAliases.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }

    NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        visibleAliases.find(*resolvedNamespace + "::" + path.last().lexeme);
    return found == visibleAliases.end()
               ? std::nullopt
               : std::optional<TypeAliasId>(found->second);
  }

  [[nodiscard]] std::optional<TypeAliasId> resolveTypeAliasPathGlobally(
      const NamePath &path, const std::vector<std::string> &fromScope) const {
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            typeAliasIds.find(qualifiedName(scope, path.last().lexeme));
        if (found != typeAliasIds.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }

    NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePathGlobally(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        typeAliasIds.find(*resolvedNamespace + "::" + path.last().lexeme);
    return found == typeAliasIds.end()
               ? std::nullopt
               : std::optional<TypeAliasId>(found->second);
  }

  [[nodiscard]] std::optional<ClassId>
  resolveClassPath(const NamePath &path,
                   const std::vector<std::string> &fromScope) const {
    const auto &visibleClasses = currentClassIds();
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            visibleClasses.find(qualifiedName(scope, path.last().lexeme));
        if (found != visibleClasses.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }

    NamePath namespacePath(std::vector<Token>(path.segments.begin(),
                                              path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        visibleClasses.find(*resolvedNamespace + "::" + path.last().lexeme);
    return found == visibleClasses.end()
               ? std::nullopt
               : std::optional<ClassId>(found->second);
  }

  [[nodiscard]] std::optional<ClassId>
  resolveClassPathGlobally(const NamePath &path,
                           const std::vector<std::string> &fromScope) const {
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            classIds.find(qualifiedName(scope, path.last().lexeme));
        if (found != classIds.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }
    const NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePathGlobally(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        classIds.find(*resolvedNamespace + "::" + path.last().lexeme);
    return found == classIds.end() ? std::nullopt
                                   : std::optional<ClassId>(found->second);
  }

  [[nodiscard]] std::optional<EnumId>
  resolveEnumPath(const NamePath &path,
                  const std::vector<std::string> &fromScope) const {
    const auto &visibleEnums = currentEnumIds();
    const auto &visibleAliases = currentTypeAliasIds();
    const auto enumFromAlias = [&](const std::string &name)
        -> std::optional<EnumId> {
      const auto alias = visibleAliases.find(name);
      if (alias == visibleAliases.end() || alias->second == 0 ||
          alias->second > typeAliases.size()) {
        return std::nullopt;
      }
      const SemanticType &type = typeAliases[alias->second - 1].type;
      return type.kind == SemanticType::Enum
                 ? std::optional<EnumId>(type.enumId)
                 : std::nullopt;
    };
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            visibleEnums.find(qualifiedName(scope, path.last().lexeme));
        if (found != visibleEnums.end()) {
          return found->second;
        }
        if (const std::optional<EnumId> alias =
                enumFromAlias(qualifiedName(scope, path.last().lexeme))) {
          return alias;
        }
      }
      return std::nullopt;
    }

    const NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        visibleEnums.find(*resolvedNamespace + "::" + path.last().lexeme);
    if (found != visibleEnums.end()) {
      return found->second;
    }
    return enumFromAlias(*resolvedNamespace + "::" + path.last().lexeme);
  }

  [[nodiscard]] std::optional<EnumId>
  resolveEnumPathGlobally(const NamePath &path,
                          const std::vector<std::string> &fromScope) const {
    const auto enumFromAlias = [&](const std::string &name)
        -> std::optional<EnumId> {
      const auto alias = typeAliasIds.find(name);
      if (alias == typeAliasIds.end() || alias->second == 0 ||
          alias->second > typeAliases.size()) {
        return std::nullopt;
      }
      const SemanticType &type = typeAliases[alias->second - 1].type;
      return type.kind == SemanticType::Enum
                 ? std::optional<EnumId>(type.enumId)
                 : std::nullopt;
    };
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            enumIds.find(qualifiedName(scope, path.last().lexeme));
        if (found != enumIds.end()) {
          return found->second;
        }
        if (const std::optional<EnumId> alias =
                enumFromAlias(qualifiedName(scope, path.last().lexeme))) {
          return alias;
        }
      }
      return std::nullopt;
    }
    const NamePath namespacePath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePathGlobally(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        enumIds.find(*resolvedNamespace + "::" + path.last().lexeme);
    if (found != enumIds.end()) {
      return found->second;
    }
    return enumFromAlias(*resolvedNamespace + "::" + path.last().lexeme);
  }

  [[nodiscard]] const EnumeratorRecord *
  resolveEnumerator(const NamePath &path, bool global = false) const {
    if (path.segments.size() < 2) {
      return nullptr;
    }
    const NamePath enumPath(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const std::optional<EnumId> enumId =
        global ? resolveEnumPathGlobally(enumPath, currentNamespace)
               : resolveEnumPath(enumPath, currentNamespace);
    if (!enumId || *enumId == 0 || *enumId > enums.size()) {
      return nullptr;
    }
    const EnumInfo &owner = enums[*enumId - 1];
    const auto found = owner.enumerators.find(path.last().lexeme);
    return found == owner.enumerators.end() ? nullptr : &found->second;
  }

  [[nodiscard]] std::optional<Symbol>
  resolveExpressionSymbol(const ExprPtr &expression) const {
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(expression.get())) {
      const Symbol *symbol = resolveQualified(qualified->name());
      return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get())) {
      const SemanticType memberObjectType = memberAccessObjectType(*get);
      if (memberObjectType != SemanticType::Unknown) {
        if (const MemberInfo *member =
                findMember(memberObjectType, get->name())) {
          return substituteSymbol(member->symbol, memberObjectType);
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isMutableTarget(const ExprPtr &expression) const {
    if (expression) {
      if (const ExpressionInfo *info =
              semanticModel.findExpression(*expression)) {
        return info->category == ValueCategory::Place &&
               info->access == AccessMode::Mutable;
      }
    }
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol == nullptr) {
        return true;
      }
      return symbol->assignable &&
             (symbol->ownerClass == 0 || symbol->staticMember ||
              currentReceiverMutability == ReceiverMutability::Mutable);
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get())) {
      const SemanticType memberObjectType = memberAccessObjectType(*get);
      if (memberObjectType == SemanticType::Unknown) {
        return true;
      }
      const MemberInfo *member = findMember(memberObjectType, get->name());
      return member == nullptr ||
             (member->symbol.assignable && memberReceiverIsMutable(*get));
    }
    return false;
  }

  [[nodiscard]] bool isMutableObject(const ExprPtr &expression) const {
    if (expression) {
      if (const ExpressionInfo *info =
              semanticModel.findExpression(*expression)) {
        return info->category == ValueCategory::Place &&
               info->access == AccessMode::Mutable;
      }
    }
    return isMutableTarget(expression);
  }

  [[nodiscard]] static bool isThisObject(const ExprPtr &expression) {
    if (dynamic_cast<const This *>(expression.get()) != nullptr) {
      return true;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return isThisObject(grouping->expression());
    }
    return false;
  }

  [[nodiscard]] std::optional<ResolvedFieldUse>
  receiverRestrictedField(const ExprPtr &expression) const {
    if (!currentClass ||
        currentReceiverMutability == ReceiverMutability::Mutable ||
        currentStaticMemberFunction || !expression) {
      return std::nullopt;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return receiverRestrictedField(grouping->expression());
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return receiverRestrictedField(index->object());
    }
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol != nullptr && symbol->ownerClass == *currentClass &&
          !symbol->staticMember && symbol->assignable) {
        return ResolvedFieldUse{.use = &variable->name(), .symbol = symbol};
      }
      return std::nullopt;
    }
    const auto *member = dynamic_cast<const Get *>(expression.get());
    if (member == nullptr || member->access().kind != TokenKind::DOT ||
        !isThisObject(member->object())) {
      return std::nullopt;
    }
    const SemanticType objectType = memberAccessObjectType(*member);
    const MemberInfo *resolved = findMember(objectType, member->name());
    if (resolved == nullptr || resolved->symbol.ownerClass != *currentClass ||
        resolved->symbol.staticMember || !resolved->symbol.assignable) {
      return std::nullopt;
    }
    return ResolvedFieldUse{.use = &member->name(),
                            .symbol = &resolved->symbol};
  }

  bool reportReceiverRestrictedArrayField(const ExprPtr &expression) {
    const std::optional<ResolvedFieldUse> field =
        receiverRestrictedField(expression);
    if (!field || field->use == nullptr || field->symbol == nullptr) {
      return false;
    }
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2002", DiagnosticPhase::Semantics, *field->use,
                       "Cannot modify field '" + field->use->lexeme +
                           "' through a read-only receiver.");
    diagnostic.related.push_back({tokenSpan(field->symbol->declaration),
                                  "Field '" +
                                      field->symbol->declaration.lexeme +
                                      "' is declared mutable here."});
    diagnostic.hints.emplace_back(
        "Methods are read-only by default; add trailing 'mut' to this method "
        "when it must modify mutable fields.");
    diagnostics.emplace_back(std::move(diagnostic));
    return true;
  }

  [[nodiscard]] SemanticType memberAccessObjectType(const Get &member) const {
    const SemanticType *objectType = semanticModel.findType(*member.object());
    if (objectType == nullptr) {
      return SemanticType::Unknown;
    }
    if (member.access().kind != TokenKind::ARROW) {
      return *objectType;
    }
    const ResolvedOperatorInfo *resolved = semanticModel.findOperator(member);
    if (resolved != nullptr &&
        resolved->returnType.kind == SemanticType::Reference &&
        resolved->returnType.arguments.size() == 1) {
      return resolved->returnType.arguments[0];
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] SemanticType memberAccessObjectType(const Set &member) const {
    const SemanticType *objectType = semanticModel.findType(*member.object());
    if (objectType == nullptr) {
      return SemanticType::Unknown;
    }
    if (member.access().kind != TokenKind::ARROW) {
      return *objectType;
    }
    const ResolvedOperatorInfo *resolved = semanticModel.findOperator(member);
    if (resolved != nullptr &&
        resolved->returnType.kind == SemanticType::Reference &&
        resolved->returnType.arguments.size() == 1) {
      return resolved->returnType.arguments[0];
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] bool memberReceiverIsMutable(const Get &member) const {
    if (member.access().kind != TokenKind::ARROW) {
      return isMutableObject(member.object());
    }
    const ResolvedOperatorInfo *resolved = semanticModel.findOperator(member);
    if (resolved != nullptr) {
      return resolved->returnType.kind == SemanticType::Reference &&
             resolved->returnType.referenceAccess == AccessMode::Mutable;
    }
    return isMutableObject(member.object());
  }

  [[nodiscard]] bool callReceiverIsMutable(const ExprPtr &callee) const {
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      return memberReceiverIsMutable(*member);
    }
    return currentReceiverMutability == ReceiverMutability::Mutable;
  }

  [[nodiscard]] const ClassInfo *classInfo(const SemanticType &type) const {
    if (type.kind != SemanticType::Class || type.classId == 0 ||
        type.classId > classes.size()) {
      return nullptr;
    }
    return &classes.at(type.classId - 1);
  }

  [[nodiscard]] ClassInfo &classInfo(ClassId id) {
    return classes.at(id - 1);
  }

  [[nodiscard]] const ClassInfo &classInfo(ClassId id) const {
    return classes.at(id - 1);
  }

  [[nodiscard]] const MemberInfo *findMember(const SemanticType &objectType,
                                             const Token &name) const {
    const ClassInfo *owner = classInfo(objectType);
    if (owner == nullptr) {
      return nullptr;
    }

    const auto found = owner->members.find(name.lexeme);
    return found == owner->members.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const MemberInfo *resolveMember(const SemanticType &objectType,
                                                const Token &name) {
    const ClassInfo *owner = classInfo(objectType);
    if (owner == nullptr) {
      if (objectType != SemanticType::Unknown) {
        report(name, "Member access requires a class or struct value.");
      }
      return nullptr;
    }

    const MemberInfo *member = findMember(objectType, name);
    if (member == nullptr) {
      report(name, "Unknown member '" + name.lexeme + "' on '" +
                       owner->name.lexeme + "'.");
      return nullptr;
    }
    if (member->symbol.type != SemanticType::Function &&
        member->access == AccessModifier::Private &&
        currentClass != member->symbol.ownerClass) {
      const ClassInfo &declaringOwner = classInfo(member->symbol.ownerClass);
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, name,
                         "Member '" + name.lexeme + "' of '" +
                             declaringOwner.name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(member->symbol.declaration), "Member declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    return member;
  }

  [[nodiscard]] std::string typeSpelling(const SemanticType &type) const {
    return SemanticTypePrinter(semanticModel).print(type);
  }

  [[nodiscard]] bool tracksValueState(const Symbol &symbol) const {
    const bool localValue =
        symbol.bindingKind == SemanticBindingKind::LocalVariable ||
        symbol.bindingKind == SemanticBindingKind::Parameter;
    return localValue && symbol.type.kind != SemanticType::Reference &&
           (typeTraits(symbol.type).movable || packRequiresMove(symbol.type));
  }

  [[nodiscard]] bool packRequiresMove(const SemanticType &type) const {
    if (type.kind != SemanticType::TypePack || !type.concretePack) {
      return false;
    }
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       [this](const SemanticType &element) {
                         return element.kind == SemanticType::TypePack
                                    ? packRequiresMove(element)
                                    : isMoveOnlyOwnerType(element);
                       });
  }

  void reportUnavailableValue(const Token &use, const Symbol &symbol) {
    report(use,
           symbol.valueState == ValueState::Moved
               ? "Value '" + use.lexeme + "' has already been moved."
               : "Value '" + use.lexeme +
                     "' may have been moved on another control-flow path.",
           "GTI-S2018");
  }

  [[nodiscard]] static ValueState mergedValueState(ValueState left,
                                                   ValueState right) {
    return left == right ? left : ValueState::MaybeMoved;
  }

  void mergeValueStates(const ScopeStack &base, const ScopeStack &left,
                        const ScopeStack &right) {
    const std::size_t depth =
        std::min({scopes.size(), base.size(), left.size(), right.size()});
    for (std::size_t scopeIndex = 0; scopeIndex < depth; ++scopeIndex) {
      for (const auto &[name, baseSymbol] : base[scopeIndex]) {
        if (!tracksValueState(baseSymbol)) {
          continue;
        }
        const auto leftSymbol = left[scopeIndex].find(name);
        const auto rightSymbol = right[scopeIndex].find(name);
        const auto target = scopes[scopeIndex].find(name);
        if (leftSymbol != left[scopeIndex].end() &&
            rightSymbol != right[scopeIndex].end() &&
            target != scopes[scopeIndex].end()) {
          target->second.valueState = mergedValueState(
              leftSymbol->second.valueState, rightSymbol->second.valueState);
          target->second.borrowedStorage = leftSymbol->second.borrowedStorage ||
                                           rightSymbol->second.borrowedStorage;
        }
      }
    }
  }

  void beginScope() { scopes.emplace_back(); }
  void endScope() { scopes.pop_back(); }

  void report(const Token &token, std::string message,
              std::string code = "GTI-S2000") {
    diagnostics.push_back(makeDiagnostic(std::move(code),
                                         DiagnosticPhase::Semantics, token,
                                         std::move(message)));
  }

  void requireBool(const ExprPtr &expression, SemanticType type,
                   const Token &token, std::string_view message) {
    if (type.kind == SemanticType::Class && expression) {
      const std::optional<FunctionCandidate> selected =
          resolveOperator(*expression, OverloadedOperator::ContextualBool,
                          expression, type, token, {}, {}, true);
      (void)selected;
      return;
    }
    if (type != SemanticType::Unknown && !isContextuallyBool(type)) {
      report(token,
             std::string(message) + " Found '" + typeSpelling(type) + "'.",
             "GTI-S2003");
    }
  }

  void requireNumeric(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isNumeric(left)) ||
        (right != SemanticType::Unknown && !isNumeric(right))) {
      report(token,
             "Operator '" + token.lexeme +
                 "' requires numeric operands but found '" +
                 typeSpelling(left) + "' and '" + typeSpelling(right) + "'.",
             "GTI-S2004");
    }
  }

  void requireOrdered(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isOrdered(left)) ||
        (right != SemanticType::Unknown && !isOrdered(right))) {
      report(token,
             "Operator '" + token.lexeme +
                 "' requires ordered operands but found '" +
                 typeSpelling(left) + "' and '" + typeSpelling(right) + "'.",
             "GTI-S2004");
    }
  }

  void requireInteger(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isIntegral(left)) ||
        (right != SemanticType::Unknown && !isIntegral(right))) {
      report(token,
             "Operator '" + token.lexeme +
                 "' requires integer operands but found '" +
                 typeSpelling(left) + "' and '" + typeSpelling(right) + "'.",
             "GTI-S2004");
    }
  }

  void validateShiftCount(SemanticType result, const Expr *right,
                          const Token &token) {
    const std::optional<IntegerConstant> count = integerConstant(right);
    if (!count) {
      return;
    }
    if (count->negative) {
      report(token, "Shift count cannot be negative.");
      return;
    }
    const int width = integerRank(result);
    if (width != 0 && count->magnitude >= static_cast<std::uint64_t>(width)) {
      report(token,
             "Shift count must be less than " + std::to_string(width) + ".");
    }
  }

  [[nodiscard]] static bool isInteger(SemanticType type) {
    return type == SemanticType::Int8 || type == SemanticType::Int16 ||
           type == SemanticType::Int32 || type == SemanticType::Int64 ||
           type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
           type == SemanticType::UInt32 || type == SemanticType::UInt64;
  }

  [[nodiscard]] static bool constraintImplies(GenericConstraintKind actual,
                                              GenericConstraintKind required) {
    if (required == GenericConstraintKind::None ||
        actual == GenericConstraintKind::Invalid) {
      return true;
    }
    if (actual == required) {
      return true;
    }
    switch (required) {
    case GenericConstraintKind::Ordered:
      return actual == GenericConstraintKind::Numeric ||
             actual == GenericConstraintKind::SignedNumeric ||
             actual == GenericConstraintKind::Integral ||
             actual == GenericConstraintKind::SignedIntegral ||
             actual == GenericConstraintKind::UnsignedIntegral ||
             actual == GenericConstraintKind::FloatingPoint;
    case GenericConstraintKind::Numeric:
      return actual == GenericConstraintKind::SignedNumeric ||
             actual == GenericConstraintKind::Integral ||
             actual == GenericConstraintKind::SignedIntegral ||
             actual == GenericConstraintKind::UnsignedIntegral ||
             actual == GenericConstraintKind::FloatingPoint;
    case GenericConstraintKind::SignedNumeric:
      return actual == GenericConstraintKind::SignedIntegral ||
             actual == GenericConstraintKind::FloatingPoint;
    case GenericConstraintKind::Integral:
      return actual == GenericConstraintKind::SignedIntegral ||
             actual == GenericConstraintKind::UnsignedIntegral;
    case GenericConstraintKind::None:
    case GenericConstraintKind::Invalid:
    case GenericConstraintKind::SignedIntegral:
    case GenericConstraintKind::UnsignedIntegral:
    case GenericConstraintKind::FloatingPoint:
      return false;
    }
    return false;
  }

  [[nodiscard]] GenericConstraintKind
  constraintOf(const SemanticType &type) const {
    if (type.kind != SemanticType::TypeParameter &&
        type.kind != SemanticType::TypePack) {
      return GenericConstraintKind::None;
    }
    const auto found = genericConstraints.find(type.genericParameterId);
    return found == genericConstraints.end() ? GenericConstraintKind::None
                                             : found->second;
  }

  [[nodiscard]] bool isNumeric(SemanticType type) const {
    return isInteger(type) || type == SemanticType::Float ||
           (type.kind == SemanticType::TypeParameter &&
            constraintImplies(constraintOf(type),
                              GenericConstraintKind::Numeric));
  }

  [[nodiscard]] bool isOrdered(SemanticType type) const {
    return isInteger(type) || type == SemanticType::Float ||
           (type.kind == SemanticType::TypeParameter &&
            constraintImplies(constraintOf(type),
                              GenericConstraintKind::Ordered));
  }

  [[nodiscard]] bool isIntegral(SemanticType type) const {
    return isInteger(type) ||
           (type.kind == SemanticType::TypeParameter &&
            constraintImplies(constraintOf(type),
                              GenericConstraintKind::Integral));
  }

  [[nodiscard]] bool isSignedNumeric(SemanticType type) const {
    return isSignedInteger(type) || type == SemanticType::Float ||
           (type.kind == SemanticType::TypeParameter &&
            constraintImplies(constraintOf(type),
                              GenericConstraintKind::SignedNumeric));
  }

  [[nodiscard]] static bool isSignedInteger(SemanticType type) {
    return type == SemanticType::Int8 || type == SemanticType::Int16 ||
           type == SemanticType::Int32 || type == SemanticType::Int64;
  }

  [[nodiscard]] static bool isUnsignedInteger(SemanticType type) {
    return type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
           type == SemanticType::UInt32 || type == SemanticType::UInt64;
  }

  [[nodiscard]] static int integerRank(SemanticType type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::UInt8:
      return 8;
    case SemanticType::Int16:
    case SemanticType::UInt16:
      return 16;
    case SemanticType::Int32:
    case SemanticType::UInt32:
      return 32;
    case SemanticType::Int64:
    case SemanticType::UInt64:
      return 64;
    default:
      return 0;
    }
  }

  struct IntegerConstant {
    bool negative = false;
    std::uint64_t magnitude = 0;
  };

  [[nodiscard]] static bool integerFits(SemanticType type,
                                        IntegerConstant value) {
    const int rank = integerRank(type);
    if (rank == 0) {
      return false;
    }
    if (isUnsignedInteger(type)) {
      const std::uint64_t maximum =
          rank == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << rank) - 1;
      return !value.negative && value.magnitude <= maximum;
    }
    const std::uint64_t limit = std::uint64_t{1} << (rank - 1);
    return value.negative ? value.magnitude <= limit
                          : value.magnitude < limit;
  }

  [[nodiscard]] static std::optional<IntegerConstant>
  integerConstant(const Expr *expression) {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(expression)) {
      const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
      if (magnitude == nullptr) {
        return std::nullopt;
      }
      return IntegerConstant{.negative = false, .magnitude = *magnitude};
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(expression)) {
      return integerConstant(grouping->expression().get());
    }
    const auto *unary = dynamic_cast<const Unary *>(expression);
    if (unary == nullptr || (unary->oper().kind != TokenKind::MINUS &&
                             unary->oper().kind != TokenKind::PLUS)) {
      return std::nullopt;
    }
    std::optional<IntegerConstant> value =
        integerConstant(unary->right().get());
    if (!value || unary->oper().kind == TokenKind::PLUS) {
      return value;
    }
    value->negative = value->magnitude != 0 && !value->negative;
    return value;
  }

  [[nodiscard]] std::optional<SwitchCaseValue>
  switchCaseConstant(const Expr *expression,
                     const SemanticType &subjectType) const {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(expression)) {
      return switchCaseConstant(grouping->expression().get(), subjectType);
    }
    if (subjectType.kind == SemanticType::Enum) {
      const auto *qualified = dynamic_cast<const QualifiedName *>(expression);
      if (qualified == nullptr) {
        return std::nullopt;
      }
      const ResolvedEnumeratorInfo *enumerator =
          semanticModel.findEnumerator(*qualified);
      if (enumerator == nullptr || enumerator->owner != subjectType.enumId) {
        return std::nullopt;
      }
      return SwitchCaseValue{.kind = SwitchCaseKind::Enumerator,
                             .type = subjectType,
                             .value = enumerator->value,
                             .enumOwner = enumerator->owner};
    }
    if (subjectType == SemanticType::Char) {
      const auto *literal = dynamic_cast<const LiteralExpr *>(expression);
      if (literal == nullptr) {
        return std::nullopt;
      }
      const auto *character = std::get_if<CharacterLiteral>(&literal->value());
      if (character == nullptr) {
        return std::nullopt;
      }
      return SwitchCaseValue{.kind = SwitchCaseKind::Character,
                             .type = subjectType,
                             .value =
                                 EnumConstant{.magnitude = character->value}};
    }
    if (!isInteger(subjectType)) {
      return std::nullopt;
    }

    const Expr *constantExpression = expression;
    if (const auto *conversion =
            dynamic_cast<const Conversion *>(constantExpression)) {
      constantExpression = conversion->value().get();
    } else if (const auto *call =
                   dynamic_cast<const Call *>(constantExpression)) {
      const ResolvedCallInfo *resolution = semanticModel.findCall(*call);
      if (resolution == nullptr ||
          resolution->intrinsic != IntrinsicKind::NumericAliasConversion ||
          call->arguments().size() != 1) {
        return std::nullopt;
      }
      constantExpression = call->arguments().front().get();
    }
    const std::optional<IntegerConstant> integer =
        integerConstant(constantExpression);
    if (!integer) {
      return std::nullopt;
    }
    return SwitchCaseValue{.kind = SwitchCaseKind::Integer,
                           .type = subjectType,
                           .value =
                               EnumConstant{.negative = integer->negative,
                                            .magnitude = integer->magnitude}};
  }

  [[nodiscard]] static bool integerRangeFits(SemanticType target,
                                             SemanticType value) {
    if (isSignedInteger(target) == isSignedInteger(value)) {
      return integerRank(value) <= integerRank(target);
    }
    return isSignedInteger(target) && isUnsignedInteger(value) &&
           integerRank(value) < integerRank(target);
  }

  [[nodiscard]] static SemanticType promotedInteger(SemanticType type) {
    if (type == SemanticType::Int8 || type == SemanticType::Int16 ||
        type == SemanticType::UInt8 || type == SemanticType::UInt16) {
      return SemanticType::Int32;
    }
    return type;
  }

  [[nodiscard]] static SemanticType widerInteger(SemanticType left,
                                                 SemanticType right) {
    return integerRank(left) >= integerRank(right) ? left : right;
  }

  [[nodiscard]] static bool canConvertToUnsigned(
      SemanticType originalType, const Expr *expression,
      SemanticType unsignedTarget) {
    if (isUnsignedInteger(originalType) &&
        integerRank(originalType) <= integerRank(unsignedTarget)) {
      return true;
    }
    if (const std::optional<IntegerConstant> constant =
            integerConstant(expression)) {
      return integerFits(unsignedTarget, *constant);
    }
    return false;
  }

  [[nodiscard]] static bool isContextuallyBool(const SemanticType &type) {
    return type == SemanticType::Bool || type.kind == SemanticType::Expected;
  }

  [[nodiscard]] static bool isExpectedVoid(const SemanticType &type) {
    return type.kind == SemanticType::Expected && type.arguments.size() == 2 &&
           type.arguments[0] == SemanticType::Void;
  }

  [[nodiscard]] static SemanticType numericResult(
      SemanticType left, SemanticType right, const Expr *leftExpression,
      const Expr *rightExpression) {
    if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
      return SemanticType::Unknown;
    }
    if (left == SemanticType::Float || right == SemanticType::Float) {
      return SemanticType::Float;
    }
    if (!isInteger(left) || !isInteger(right)) {
      return SemanticType::Unknown;
    }

    if (isSignedInteger(left) != isSignedInteger(right)) {
      const SemanticType signedOriginal =
          isSignedInteger(left) ? left : right;
      const SemanticType unsignedOriginal =
          isUnsignedInteger(left) ? left : right;
      if (integerRangeFits(signedOriginal, unsignedOriginal)) {
        return promotedInteger(signedOriginal);
      }
      if (integerRank(left) < 32 && integerRank(right) < 32) {
        return SemanticType::Int32;
      }
    }

    const SemanticType promotedLeft = promotedInteger(left);
    const SemanticType promotedRight = promotedInteger(right);
    if (isSignedInteger(promotedLeft) == isSignedInteger(promotedRight)) {
      return widerInteger(promotedLeft, promotedRight);
    }

    const bool leftIsSigned = isSignedInteger(promotedLeft);
    const SemanticType signedType =
        leftIsSigned ? promotedLeft : promotedRight;
    const SemanticType unsignedType =
        leftIsSigned ? promotedRight : promotedLeft;
    if (integerRank(signedType) > integerRank(unsignedType)) {
      return signedType;
    }

    const SemanticType originalSignedType = leftIsSigned ? left : right;
    const Expr *signedExpression =
        leftIsSigned ? leftExpression : rightExpression;
    if (canConvertToUnsigned(originalSignedType, signedExpression,
                             unsignedType)) {
      return unsignedType;
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] bool isOwnershipAssignable(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const {
    if (target.kind == SemanticType::UniqueOwner &&
        value == SemanticType::NullPtr) {
      return true;
    }
    if (!isAssignable(target, value, expression.get())) {
      return false;
    }
    if (target != value || !expression ||
        target.kind == SemanticType::TypePack) {
      return true;
    }
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    if (info == nullptr) {
      return true;
    }
    const SemanticTypeTraits traits = typeTraits(target);
    return info->category == ValueCategory::Value ? traits.movable
                                                  : traits.copyable;
  }

  [[nodiscard]] bool isOwnershipAssignment(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const {
    const ExpressionInfo *info =
        expression ? semanticModel.findExpression(*expression) : nullptr;
    const bool moving =
        info != nullptr && info->category == ValueCategory::Value;
    const SemanticTypeTraits traits = typeTraits(target);
    if ((moving && !traits.moveAssignable) ||
        (!moving && !traits.copyAssignable)) {
      return target == SemanticType::Unknown || value == SemanticType::Unknown;
    }
    return isOwnershipAssignable(target, value, expression);
  }

  [[nodiscard]] static bool isAssignable(SemanticType target,
                                         SemanticType value,
                                         const Expr *expression = nullptr) {
    if (target == SemanticType::Unknown || value == SemanticType::Unknown) {
      return true;
    }
    if (target == SemanticType::Float && isInteger(value)) {
      return true;
    }
    if (isInteger(target) && isInteger(value)) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(expression)) {
        return integerFits(target, *constant);
      }
      return integerRangeFits(target, value);
    }
    if (target == value) {
      return true;
    }
    if (target.kind != SemanticType::Expected || target.arguments.size() != 2) {
      return false;
    }
    if (value.kind == SemanticType::Unexpected &&
        value.arguments.size() == 1) {
      const Expr *errorExpression = expression;
      if (const auto *unexpected =
              dynamic_cast<const Unexpected *>(expression)) {
        errorExpression = unexpected->error().get();
      }
      return isAssignable(target.arguments[1], value.arguments[0],
                          errorExpression);
    }
    return target.arguments[0] != SemanticType::Void &&
           isAssignable(target.arguments[0], value, expression);
  }

  [[nodiscard]] bool isComparable(SemanticType left, SemanticType right,
                                  const Expr *leftExpression,
                                  const Expr *rightExpression) const {
    if (left.kind == SemanticType::TypeParameter ||
        right.kind == SemanticType::TypeParameter) {
      return left == right && isOrdered(left);
    }
    if (isInteger(left) && isInteger(right)) {
      return numericResult(left, right, leftExpression, rightExpression) !=
             SemanticType::Unknown;
    }
    return isAssignable(left, right, rightExpression) ||
           isAssignable(right, left, leftExpression);
  }

  [[nodiscard]] SemanticType
  baseTypeOf(const TypeRef &type,
             const std::vector<std::string> &fromScope) const {
    if (isGtiInternalTextView(type)) {
      return type.arguments.empty() ? SemanticType::StringView
                                    : SemanticType::Unknown;
    }
    if (isGtiInternalUniqueOwner(type)) {
      return type.arguments.size() == 1 ? SemanticType::uniqueOwnerOf(typeOf(
                                              type.arguments[0], fromScope))
                                        : SemanticType::Unknown;
    }
    if (isGtiInternalStorage(type)) {
      return type.arguments.size() == 1
                 ? SemanticType::storageOf(typeOf(type.arguments[0], fromScope))
                 : SemanticType::Unknown;
    }
    switch (type.name.last().kind) {
    case TokenKind::AUTO:
      return SemanticType::Unknown;
    case TokenKind::VOID:
      return SemanticType::Void;
    case TokenKind::INT:
    case TokenKind::INT32:
      return SemanticType::Int32;
    case TokenKind::INT8:
      return SemanticType::Int8;
    case TokenKind::INT16:
      return SemanticType::Int16;
    case TokenKind::INT64:
      return SemanticType::Int64;
    case TokenKind::UINT:
    case TokenKind::UINT32:
      return SemanticType::UInt32;
    case TokenKind::UINT8:
      return SemanticType::UInt8;
    case TokenKind::UINT16:
      return SemanticType::UInt16;
    case TokenKind::UINT64:
      return SemanticType::UInt64;
    case TokenKind::FLOAT:
      return SemanticType::Float;
    case TokenKind::BOOL:
      return SemanticType::Bool;
    case TokenKind::CHAR:
      return SemanticType::Char;
    case TokenKind::NULLPTR_TYPE:
      return SemanticType::NullPtr;
    case TokenKind::EXPECTED: {
      std::vector<SemanticType> arguments;
      arguments.reserve(type.arguments.size());
      for (const TypeRef &argument : type.arguments) {
        arguments.emplace_back(typeOf(argument, fromScope));
      }
      return SemanticType(SemanticType::Expected, std::move(arguments));
    }
    default:
      if (const std::optional<SemanticType> parameter =
              resolveTypeParameter(type.name)) {
        return type.arguments.empty() ? *parameter : SemanticType::Unknown;
      }
      if (const std::optional<TypeAliasId> alias =
              resolveTypeAliasPath(type.name, fromScope)) {
        if (!type.arguments.empty() || *alias == 0 ||
            *alias > typeAliases.size()) {
          return SemanticType::Unknown;
        }
        const RegisteredTypeAlias &declaration = typeAliases[*alias - 1];
        return declaration.resolution == TypeAliasResolution::Resolved
                   ? declaration.type
                   : SemanticType::Unknown;
      }
      if (const std::optional<EnumId> id =
              resolveEnumPath(type.name, fromScope)) {
        return type.arguments.empty() ? SemanticType::enumType(*id)
                                      : SemanticType::Unknown;
      }
      if (const std::optional<ClassId> id =
              resolveClassPath(type.name, fromScope)) {
        std::vector<SemanticType> arguments;
        std::vector<CompileTimeValue> valueArguments;
        const ClassInfo &owner = classInfo(*id);
        arguments.reserve(genericTypeParameterCount(owner.genericParameters));
        valueArguments.reserve(
            genericValueParameterCount(owner.genericParameters));
        const std::size_t count =
            std::min(type.arguments.size(), owner.genericParameters.size());
        for (std::size_t index = 0; index < count; ++index) {
          const GenericParameterInfo &parameter =
              owner.genericParameters[index];
          const TypeRef &argument = type.arguments[index];
          if (parameter.value) {
            valueArguments.emplace_back(
                genericValueArgument(argument).value_or(CompileTimeValue{}));
          } else {
            arguments.emplace_back(typeOf(argument, fromScope));
          }
        }
        return SemanticType::classType(*id, std::move(arguments),
                                       std::move(valueArguments));
      }
      return SemanticType::Unknown;
    }
  }

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, const std::vector<std::string> &fromScope) const {
    SemanticType result = baseTypeOf(type, fromScope);
    for (auto extent = type.arrayExtents.rbegin();
         extent != type.arrayExtents.rend(); ++extent) {
      const std::optional<CompileTimeValue> length =
          resolvedArrayExtent(*extent);
      if (!length || length->kind == CompileTimeValue::Unknown) {
        return SemanticType::Unknown;
      }
      result = SemanticType::arrayOf(std::move(result), *length);
    }
    if (type.reference) {
      result = SemanticType::referenceTo(std::move(result));
    }
    return result;
  }

  [[nodiscard]] SemanticType typeOf(const TypeRef &type) const {
    return typeOf(type, currentNamespace);
  }

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, Mutability mutability,
         const std::vector<std::string> &fromScope) const {
    SemanticType result = typeOf(type, fromScope);
    if (result.kind == SemanticType::Reference) {
      result.referenceAccess = mutability == Mutability::Mutable
                                   ? AccessMode::Mutable
                                   : AccessMode::ReadOnly;
    }
    return result;
  }

  [[nodiscard]] SemanticType typeOf(const TypeRef &type,
                                    Mutability mutability) const {
    return typeOf(type, mutability, currentNamespace);
  }

  [[nodiscard]] SemanticType
  typeOf(const Parameter &parameter,
         const std::vector<std::string> &fromScope) const {
    return typeOf(parameter.type, parameter.mutability, fromScope);
  }

  [[nodiscard]] SemanticType typeOf(const Parameter &parameter) const {
    return typeOf(parameter, currentNamespace);
  }

  [[nodiscard]] static SemanticType literalType(const Literal &literal) {
    if (const auto *value = std::get_if<std::uint64_t>(&literal)) {
      if (*value <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        return SemanticType::Int32;
      }
      if (*value <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return SemanticType::Int64;
      }
      return SemanticType::UInt64;
    }
    if (std::holds_alternative<double>(literal)) {
      return SemanticType::Float;
    }
    if (std::holds_alternative<CharacterLiteral>(literal)) {
      return SemanticType::Char;
    }
    if (std::holds_alternative<std::string>(literal)) {
      return SemanticType::StringView;
    }
    if (std::holds_alternative<bool>(literal)) {
      return SemanticType::Bool;
    }
    if (std::holds_alternative<std::nullptr_t>(literal)) {
      return SemanticType::NullPtr;
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] static Token expressionToken(const Expr &expression) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
      return literal->token();
    }
    if (const auto *variable = dynamic_cast<const Variable *>(&expression)) {
      return variable->name();
    }
    if (const auto *receiver = dynamic_cast<const This *>(&expression)) {
      return receiver->keyword();
    }
    if (const auto *binary = dynamic_cast<const Binary *>(&expression)) {
      return binary->oper();
    }
    if (const auto *logical = dynamic_cast<const Logical *>(&expression)) {
      return logical->oper();
    }
    if (const auto *pack = dynamic_cast<const PackExpansion *>(&expression)) {
      return pack->ellipsis();
    }
    if (const auto *unary = dynamic_cast<const Unary *>(&expression)) {
      return unary->oper();
    }
    if (const auto *unexpected =
            dynamic_cast<const Unexpected *>(&expression)) {
      return unexpected->keyword();
    }
    if (const auto *postfix = dynamic_cast<const Postfix *>(&expression)) {
      return postfix->oper();
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(&expression)) {
      return qualified->name().last();
    }
    if (const auto *assign = dynamic_cast<const Assign *>(&expression)) {
      return assign->oper();
    }
    if (const auto *initializer =
            dynamic_cast<const ArrayInitializer *>(&expression)) {
      return initializer->brace();
    }
    if (const auto *call = dynamic_cast<const Call *>(&expression)) {
      return call->paren();
    }
    if (const auto *conversion =
            dynamic_cast<const Conversion *>(&expression)) {
      return conversion->targetType().name.last();
    }
    if (const auto *initializer =
            dynamic_cast<const DirectInitializer *>(&expression)) {
      return initializer->brace();
    }
    if (const auto *dereference =
            dynamic_cast<const DereferenceSet *>(&expression)) {
      return dereference->dereference();
    }
    if (const auto *get = dynamic_cast<const Get *>(&expression)) {
      return get->name();
    }
    if (const auto *index = dynamic_cast<const Index *>(&expression)) {
      return index->bracket();
    }
    if (const auto *indexSet = dynamic_cast<const IndexSet *>(&expression)) {
      return indexSet->bracket();
    }
    if (const auto *lambda = dynamic_cast<const Lambda *>(&expression)) {
      return lambda->bracket();
    }
    if (const auto *set = dynamic_cast<const Set *>(&expression)) {
      return set->name();
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(&expression)) {
      return expressionToken(grouping->expression());
    }
    return Token{};
  }

  [[nodiscard]] static Token expressionToken(const ExprPtr &expr) {
    return expr ? expressionToken(*expr) : Token{};
  }

  [[nodiscard]] static Token callableToken(const ExprPtr &callee) {
    if (const auto *variable = dynamic_cast<const Variable *>(callee.get())) {
      return variable->name();
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(callee.get())) {
      return qualified->name().last();
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      return member->name();
    }
    return expressionToken(callee);
  }

  std::vector<SemanticDiagnostic> diagnostics;
  ScopeStack scopes;
  std::unordered_set<std::string> namespaces;
  std::unordered_map<std::string, SymbolId> namespaceToolingSymbols;
  std::unordered_map<std::string, NamespaceAliasInfo> namespaceAliases;
  std::unordered_map<std::string, TypeAliasId> typeAliasIds;
  std::vector<RegisteredTypeAlias> typeAliases;
  std::unordered_map<std::string, Symbol> namespaceSymbols;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
      internalNamespaceSymbols;
  std::unordered_map<std::string, ClassId> classIds;
  std::unordered_map<std::string, EnumId> enumIds;
  std::unordered_map<SourceUnitId, std::unordered_set<std::string>>
      visibleNamespaces;
  std::unordered_map<SourceUnitId,
                     std::unordered_map<std::string, NamespaceAliasInfo>>
      visibleNamespaceAliases;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, TypeAliasId>>
      visibleTypeAliasIds;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
      visibleNamespaceSymbols;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, ClassId>>
      visibleClassIds;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, EnumId>>
      visibleEnumIds;
  std::unordered_map<const ClassDecl *, ClassId> classDeclIds;
  std::unordered_map<const FunctionDecl *, std::vector<GenericParameterInfo>>
      functionGenericParameters;
  std::unordered_map<GenericParameterId, GenericConstraintKind>
      genericConstraints;
  std::vector<ClassInfo> classes;
  std::vector<EnumInfo> enums;
  std::vector<std::unordered_map<std::string, SemanticType>>
      typeParameterScopes;
  std::vector<std::unordered_map<std::string, CompileTimeValue>>
      valueParameterScopes;
  std::vector<std::unordered_set<GenericParameterId>> typePackScopes;
  TypeSubstitution instanceTypeSubstitution;
  ValueSubstitution instanceValueSubstitution;
  SemanticModel semanticModel;
  std::vector<std::string> currentNamespace;
  SourceUnitId currentSourceUnit = 0;
  std::unordered_set<const VariableDecl *> predeclaredVariables;
  std::vector<std::unordered_map<std::string, Token>> lambdaUncapturedLocals;
  TargetInfo target;
  const SourceGraph *sourceGraph = nullptr;
  SemanticType currentType = SemanticType::Unknown;
  SemanticType currentReturnType = SemanticType::Unknown;
  std::optional<ClassId> currentClass;
  bool analyzingFieldInitializer = false;
  bool analyzingConstructorInitializer = false;
  bool analyzingCallCallee = false;
  bool currentStaticMemberFunction = false;
  bool allowPackTypeReference = false;
  bool receiverStorageBorrowed = false;
  bool instanceClassContextActive = false;
  std::optional<SemanticType> contextualInitializerType;
  ReceiverMutability currentReceiverMutability = ReceiverMutability::ReadOnly;
  std::size_t constructorDepth = 0;
  std::size_t destructorDepth = 0;
  std::size_t functionDepth = 0;
  std::size_t loopDepth = 0;
  std::size_t switchDepth = 0;
  std::size_t lambdaDepth = 0;
  GenericParameterId nextGenericParameterId = 1;
  ConstructorId nextConstructorId = 1;
  FunctionId nextFunctionId = 1;
  LambdaId nextLambdaId = 1;
};

} // namespace lang
