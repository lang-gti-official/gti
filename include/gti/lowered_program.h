#pragma once

#include "gti/diagnostic.h"
#include "gti/mir.h"
#include "gti/target.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

class LoweredProgramBuilder;
struct LoweredProgramTestAccess;

using LoweredDeclarationId = std::size_t;
using LoweredClassInstanceId = std::size_t;
using LoweredFunctionInstanceId = std::size_t;
using LoweredConstructorInstanceId = std::size_t;
using LoweredDestructorInstanceId = std::size_t;
using LoweredLambdaInstanceId = std::size_t;

enum class LoweredBodyDefinitionKind {
  ImplicitSource,
  Source,
  CompilerGenerated,
  RuntimeBinding,
  Declaration,
  Count,
};

enum class LoweredBodyRole {
  SourceExecutable,
  AbiDeclaration,
  DataOnly,
  Count,
};

struct LoweredBodyIdentity {
  MirBodyAddress address;
  PlaceDomain placeDomain;
  LoweredBodyDefinitionKind definition =
      LoweredBodyDefinitionKind::ImplicitSource;
  std::size_t declaration = 0;
  std::size_t concreteOwner = 0;
  SourceSpan source;

  friend bool operator==(const LoweredBodyIdentity &,
                         const LoweredBodyIdentity &) = default;
};

enum class LoweredGeneratedItemKind {
  ProgramInitialization,
  HostedEntry,
  StructuralOperatorAdapter,
  CallableAdapter,
  LifecycleCleanup,
  NativeInteropAdapter,
  ThreadTaskAdapter,
  ConcreteInstanceAdapter,
  ThreadLocalStorage,
  Count,
};

struct LoweredGeneratedItemIdentity {
  LoweredGeneratedItemKind kind =
      LoweredGeneratedItemKind::ProgramInitialization;
  std::size_t owner = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const LoweredGeneratedItemIdentity &,
                         const LoweredGeneratedItemIdentity &) = default;
};

// Optional statement-level provenance for an exact optimized MIR stage.
// Instruction zero denotes the block terminator; generated glue has no row.
// This is debug metadata only, never executable or ownership authority.
struct LoweredSourceLocation {
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  SourceSpan source;

  friend bool operator==(const LoweredSourceLocation &,
                         const LoweredSourceLocation &) = default;
};

struct LoweredBody {
  LoweredBodyIdentity identity;
  LoweredBodyRole role = LoweredBodyRole::SourceExecutable;
  std::vector<LoweredGeneratedItemIdentity> requiredGeneratedItems;
  std::vector<LoweredSourceLocation> sourceLocations;

  friend bool operator==(const LoweredBody &, const LoweredBody &) = default;
};

enum class LoweredDeclarationKind {
  Namespace,
  NamespaceAlias,
  TypeAlias,
  Class,
  Enum,
  Function,
  Constructor,
  Destructor,
  Storage,
  Access,
  LanguageLinkage,
  Concept,
  Empty,
  Other,
  Count,
};

struct LoweredGenericParameter {
  GenericParameterId id = 0;
  std::string name;
  bool pack = false;
  bool value = false;
  GenericConstraintSet constraints = 0;
  std::vector<std::string> constraintName;
  ConceptId constraintConcept = 0;
  std::vector<std::size_t> typeArgumentPath;

  friend bool operator==(const LoweredGenericParameter &,
                         const LoweredGenericParameter &) = default;
};

struct LoweredConceptRequirement {
  ConceptId conceptId = 0;
  std::vector<SemanticType> arguments;
  // For an unnamed structural leaf, arguments are receiver/result followed by
  // exact parameter types. Semantics establishes receiver access and the
  // caller's unsafe obligation; neither is inferred from a member spelling.
  std::string memberName;
  bool mutableReceiver = false;
  bool requiresUnsafe = false;

  friend bool operator==(const LoweredConceptRequirement &,
                         const LoweredConceptRequirement &) = default;
};

struct LoweredConstraintExpressionNode {
  ConstraintExpressionKind kind = ConstraintExpressionKind::Application;
  std::size_t application = ConstraintExpressionNode::invalidIndex;
  std::size_t left = ConstraintExpressionNode::invalidIndex;
  std::size_t right = ConstraintExpressionNode::invalidIndex;

  friend bool operator==(const LoweredConstraintExpressionNode &,
                         const LoweredConstraintExpressionNode &) = default;
};

struct LoweredParameter {
  SymbolId symbol = 0;
  std::string name;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  Mutability mutability = Mutability::Immutable;
  bool pack = false;
  bool hasDefault = false;
  bool explicitlyMoved = false;
  SourceSpan source;

  friend bool operator==(const LoweredParameter &,
                         const LoweredParameter &) = default;
};

struct LoweredCallableSignature {
  // Unknown is the explicit declaration-level "constraint deferred" state
  // for a result or argument; concrete MIR callable signatures replace it
  // with an exact type.
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;

  friend bool operator==(const LoweredCallableSignature &,
                         const LoweredCallableSignature &) = default;
};

struct LoweredCallableForwarding {
  FunctionId function = 0;
  std::size_t parameterIndex = 0;

  friend bool operator==(const LoweredCallableForwarding &,
                         const LoweredCallableForwarding &) = default;
};

struct LoweredCallableParameter {
  std::size_t parameterIndex = 0;
  GenericParameterId genericParameter = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  CallableBoundary boundary = CallableBoundary::Confined;
  std::optional<CallableOwnedTransport> ownedTransport;
  std::vector<LoweredCallableSignature> signatures;
  std::vector<LoweredCallableForwarding> forwardings;

  friend bool operator==(const LoweredCallableParameter &,
                         const LoweredCallableParameter &) = default;
};

struct LoweredClassBase {
  SemanticType type = SemanticType::Unknown;
  AccessModifier access = AccessModifier::Public;
  bool interface = false;

  friend bool operator==(const LoweredClassBase &,
                         const LoweredClassBase &) = default;
};

struct LoweredClassDeclaration {
  ClassId id = 0;
  SourceUnitId sourceUnit = 0;
  std::string qualifiedName;
  ClassKind kind = ClassKind::Class;
  ClassId definitionFamilyPrimary = 0;
  ClassId exactSpecializationPrimary = 0;
  std::vector<SemanticType> exactTypeArguments;
  std::vector<CompileTimeValue> exactValueArguments;
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<SemanticType> genericArgumentPatterns;
  std::vector<LoweredClassBase> bases;
  SemanticTypeTraits traits;
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  std::optional<SemanticType> transferCondition;
  std::optional<SemanticType> shareCondition;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  bool generatedDefaultRequiresSchedule = false;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructor = SpecialMemberStatus::Generated;
  std::optional<MirCAbiRecordLayout> cAbiLayout;
  std::optional<MirUnionLayout> unionLayout;
  CompilerCapabilityTypeKind compilerCapability =
      CompilerCapabilityTypeKind::None;
  bool forwardDeclaration = false;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  bool cOpaqueHandle = false;
  bool requiresActiveDropState = false;
  bool compilerPrivate = false;
  // Every member is emitted public: GTI access control is enforced in
  // semantics, and reflected access may read private fields.
  bool reflectPrivate = false;
  ComputedTypeProjectionKind computedTypeProjection =
      ComputedTypeProjectionKind::None;

  friend bool operator==(const LoweredClassDeclaration &,
                         const LoweredClassDeclaration &) = default;
};

struct LoweredEnumerator {
  std::string name;
  EnumConstant value;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
  std::vector<bool> payloadRequiresActiveCleanup;
  SourceSpan source;

  friend bool operator==(const LoweredEnumerator &,
                         const LoweredEnumerator &) = default;
};

struct LoweredEnumDeclaration {
  EnumId id = 0;
  SemanticType type = SemanticType::Unknown;
  SourceUnitId sourceUnit = 0;
  std::string qualifiedName;
  // Declaration-bound selector shape; executable payloads below are concrete.
  std::vector<LoweredGenericParameter> genericParameters;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<LoweredEnumerator> enumerators;
  SemanticTypeTraits traits{};
  bool requiresActiveCleanup = false;
  bool payload = false;
  bool compilerPrivate = false;

  friend bool operator==(const LoweredEnumDeclaration &,
                         const LoweredEnumDeclaration &) = default;
};

struct LoweredFunctionDeclaration {
  FunctionId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId ownerClass = 0;
  std::string qualifiedName;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<LoweredParameter> parameters;
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<LoweredConceptRequirement> requirements;
  std::vector<LoweredConstraintExpressionNode> requirementExpression;
  std::size_t requirementRoot = ConstraintExpressionNode::invalidIndex;
  std::vector<LoweredCallableParameter> callableParameters;
  std::size_t requiredParameterCount = 0;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  FunctionId entryArgumentAppendFunction = 0;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  Mutability returnMutability = Mutability::Immutable;
  std::optional<OverloadedOperator> overloadedOperator;
  MirDefinitionKind definitionKind = MirDefinitionKind::Declaration;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  std::vector<FunctionId> virtualRoots;
  bool parameterPack = false;
  bool staticMember = false;
  bool internalLinkage = false;
  bool constexprFunction = false;
  bool requiresUnsafe = false;
  bool neverReturns = false;
  bool tracksCaller = false;
  bool threadConfinedCallback = false;
  CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
  ProcessMemoryRuntimeOperation processMemoryOperation =
      ProcessMemoryRuntimeOperation::None;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  bool hasRequiresClause = false;
  bool compilerPrivate = false;

  friend bool operator==(const LoweredFunctionDeclaration &,
                         const LoweredFunctionDeclaration &) = default;
};

struct LoweredConstructorDeclaration {
  ConstructorId id = 0;
  ClassId owner = 0;
  ConstructorKind kind = ConstructorKind::Ordinary;
  AccessModifier access = AccessModifier::Public;
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<LoweredConceptRequirement> requirements;
  std::vector<LoweredConstraintExpressionNode> requirementExpression;
  std::size_t requirementRoot = ConstraintExpressionNode::invalidIndex;
  std::vector<LoweredParameter> parameters;
  std::size_t requiredParameterCount = 0;
  std::optional<std::size_t> borrowParameter;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::optional<SpecialMemberSpecifierKind> specifier;
  bool hasRequiresClause = false;
  bool constexprConstructor = false;
  bool requiresUnsafe = false;
  bool compilerPrivate = false;

  friend bool operator==(const LoweredConstructorDeclaration &,
                         const LoweredConstructorDeclaration &) = default;
};

struct LoweredDestructorDeclaration {
  ClassId owner = 0;
  AccessModifier access = AccessModifier::Public;
  std::optional<SpecialMemberSpecifierKind> specifier;

  friend bool operator==(const LoweredDestructorDeclaration &,
                         const LoweredDestructorDeclaration &) = default;
};

struct LoweredStorageDeclaration {
  SymbolId symbol = 0;
  std::string externalSymbol;
  ClassId ownerClass = 0;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits;
  std::optional<ConstantValue> constant;
  Mutability mutability = Mutability::Immutable;
  bool staticStorage = false;
  bool threadLocalStorage = false;
  bool internalLinkage = false;
  bool constexprStorage = false;
  bool hasInitializer = false;

  friend bool operator==(const LoweredStorageDeclaration &,
                         const LoweredStorageDeclaration &) = default;
};

struct LoweredNamespaceAliasDeclaration {
  std::vector<std::string> target;

  friend bool operator==(const LoweredNamespaceAliasDeclaration &,
                         const LoweredNamespaceAliasDeclaration &) = default;
};

struct LoweredTypeAliasDeclaration {
  SourceUnitId sourceUnit = 0;
  std::string qualifiedName;
  SemanticType type = SemanticType::Unknown;
  bool compilerPrivate = false;

  friend bool operator==(const LoweredTypeAliasDeclaration &,
                         const LoweredTypeAliasDeclaration &) = default;
};

struct LoweredAccessDeclaration {
  AccessModifier access = AccessModifier::Public;

  friend bool operator==(const LoweredAccessDeclaration &,
                         const LoweredAccessDeclaration &) = default;
};

struct LoweredLanguageLinkageDeclaration {
  LanguageLinkage linkage = LanguageLinkage::Gti;

  friend bool operator==(const LoweredLanguageLinkageDeclaration &,
                         const LoweredLanguageLinkageDeclaration &) = default;
};

using LoweredDeclarationPayload =
    std::variant<std::monostate, LoweredNamespaceAliasDeclaration,
                 LoweredTypeAliasDeclaration, LoweredClassDeclaration,
                 LoweredEnumDeclaration, LoweredFunctionDeclaration,
                 LoweredConstructorDeclaration, LoweredDestructorDeclaration,
                 LoweredStorageDeclaration, LoweredAccessDeclaration,
                 LoweredLanguageLinkageDeclaration>;

// A flat, resolved declaration IR. Parent identities preserve active source
// order and nesting without retaining AST nodes.
struct LoweredDeclaration {
  LoweredDeclarationId id = 0;
  LoweredDeclarationKind kind = LoweredDeclarationKind::Other;
  LoweredDeclarationId parent = 0;
  std::size_t semanticIdentity = 0;
  ClassId ownerClass = 0;
  std::size_t ordinal = 0;
  std::string name;
  std::vector<std::string> namespaceScope;
  SourceSpan source;
  bool generic = false;
  std::vector<LoweredGeneratedItemIdentity> requiredGeneratedItems;
  LoweredDeclarationPayload payload;

  friend bool operator==(const LoweredDeclaration &,
                         const LoweredDeclaration &) = default;
};

struct LoweredSymbol {
  SymbolId id = 0;
  SymbolKind kind = SymbolKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  SourceUnitId sourceUnit = 0;
  SourceSpan nameSource;
  SourceSpan declarationSource;
  std::optional<SourceSpan> definitionSource;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  AccessModifier access = AccessModifier::Public;
  bool mutableBinding = false;
  bool defaultLibrary = false;
  bool staticMember = false;
  bool internalLinkage = false;
  bool generated = false;
  bool compilerPrivate = false;
  bool instanceDependentType = false;

  friend bool operator==(const LoweredSymbol &,
                         const LoweredSymbol &) = default;
};

struct LoweredClassInstance {
  LoweredClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  SourceSpan source;

  friend bool operator==(const LoweredClassInstance &,
                         const LoweredClassInstance &) = default;
};

struct LoweredFunctionInstance {
  LoweredFunctionInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  FunctionId declaration = 0;
  std::optional<LoweredClassInstanceId> owner;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::optional<SourceSpan> instantiationSource;
  SourceSpan source;

  friend bool operator==(const LoweredFunctionInstance &,
                         const LoweredFunctionInstance &) = default;
};

struct LoweredConstructorInstance {
  LoweredConstructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ConstructorId declaration = 0;
  LoweredClassInstanceId owner = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<SemanticType> parameterTypes;
  std::optional<SourceSpan> instantiationSource;
  SourceSpan source;

  friend bool operator==(const LoweredConstructorInstance &,
                         const LoweredConstructorInstance &) = default;
};

struct LoweredDestructorInstance {
  LoweredDestructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  LoweredClassInstanceId owner = 0;
  SourceSpan source;

  friend bool operator==(const LoweredDestructorInstance &,
                         const LoweredDestructorInstance &) = default;
};

struct LoweredLambdaCapture {
  SymbolId symbol = 0;
  std::string name;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  LambdaCaptureMode mode = LambdaCaptureMode::Copy;
  bool requiresActiveCleanup = false;

  friend bool operator==(const LoweredLambdaCapture &,
                         const LoweredLambdaCapture &) = default;
};

struct LoweredLambdaInstance {
  LoweredLambdaInstanceId id = 0;
  LambdaId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<LoweredParameter> parameters;
  std::vector<LoweredLambdaCapture> captures;
  SemanticTypeTraits traits;
  SourceSpan source;

  friend bool operator==(const LoweredLambdaInstance &,
                         const LoweredLambdaInstance &) = default;
};

// Immutable ABI-planning roots consumed by future wrapper and header
// backends. This is a copy of verified MIR authority, not a place for either
// backend to repeat semantic name or type eligibility checks.
struct LoweredEmbeddingExport {
  FunctionId declaration = 0;
  LoweredFunctionInstanceId function = 0;
  std::string qualifiedName;
  std::string symbol;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  SourceSpan requestSource;

  friend bool operator==(const LoweredEmbeddingExport &,
                         const LoweredEmbeddingExport &) = default;
};

struct LoweredEmbeddingPlan {
  std::uint32_t abiVersion = embeddingAbiVersionV1;
  std::vector<LoweredEmbeddingExport> exports;

  friend bool operator==(const LoweredEmbeddingPlan &,
                         const LoweredEmbeddingPlan &) = default;
};

struct LoweredNativeCallbackItem {
  MirNativeCallbackAdapter adapter;

  friend bool operator==(const LoweredNativeCallbackItem &,
                         const LoweredNativeCallbackItem &) = default;
};

struct LoweredThreadTaskAdapterItem {
  MirThreadTaskAdapter adapter;

  friend bool operator==(const LoweredThreadTaskAdapterItem &,
                         const LoweredThreadTaskAdapterItem &) = default;
};

struct LoweredStructuralOperatorAdapterItem {
  FunctionId function = 0;
  OverloadedOperator operation = OverloadedOperator::Dereference;

  friend bool
  operator==(const LoweredStructuralOperatorAdapterItem &,
             const LoweredStructuralOperatorAdapterItem &) = default;
};

struct LoweredCallableAdapterItem {
  FunctionId function = 0;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;

  friend bool operator==(const LoweredCallableAdapterItem &,
                         const LoweredCallableAdapterItem &) = default;
};

enum class LoweredLifecycleCleanupForm {
  OrdinaryClass,
  ConcreteSpecialization,
  RetainedPrimary,
  Count,
};

// One target-independent containment adapter around one concrete destructor
// MIR body. A backend chooses names and ABI spelling; these facts select the
// exact class representation and whether the helper needs failure dispatch.
struct LoweredLifecycleCleanupItem {
  ClassId ownerClass = 0;
  LoweredClassInstanceId classInstance = 0;
  LoweredDestructorInstanceId destructorInstance = 0;
  LoweredLifecycleCleanupForm form = LoweredLifecycleCleanupForm::OrdinaryClass;
  bool mayRaiseDefinedFailure = true;

  friend bool operator==(const LoweredLifecycleCleanupItem &,
                         const LoweredLifecycleCleanupItem &) = default;
};

enum class LoweredConcreteInstanceAdapterKind {
  Function,
  Constructor,
  Count,
};

// A concrete executable instance whose source declaration is generic either
// itself or through its owning class. Backends choose monomorphization or
// specialization mechanics; the lowered program owns the exact body and
// declaration relationship that must be represented.
struct LoweredConcreteInstanceAdapterItem {
  LoweredConcreteInstanceAdapterKind kind =
      LoweredConcreteInstanceAdapterKind::Function;
  MirBodyAddress body;
  std::size_t declaration = 0;
  LoweredClassInstanceId ownerClassInstance = 0;
  bool mayRaiseDefinedFailure = true;

  friend bool operator==(const LoweredConcreteInstanceAdapterItem &,
                         const LoweredConcreteInstanceAdapterItem &) = default;
};

using LoweredGeneratedItemPayload =
    std::variant<std::monostate, LoweredStructuralOperatorAdapterItem,
                 LoweredCallableAdapterItem, LoweredLifecycleCleanupItem,
                 LoweredConcreteInstanceAdapterItem, LoweredNativeCallbackItem,
                 LoweredThreadTaskAdapterItem>;

enum class LoweredGeneratedItemSourceKind {
  Body,
  Declaration,
  Count,
};

struct LoweredGeneratedItem {
  LoweredGeneratedItemIdentity identity;
  LoweredGeneratedItemSourceKind sourceKind =
      LoweredGeneratedItemSourceKind::Body;
  MirBodyAddress sourceBody;
  LoweredDeclarationId sourceDeclaration = 0;
  std::vector<LoweredGeneratedItemIdentity> dependencies;
  LoweredGeneratedItemPayload payload;

  friend bool operator==(const LoweredGeneratedItem &,
                         const LoweredGeneratedItem &) = default;
};

enum class LoweredProgramIssueKind {
  UnsupportedTarget,
  FrontendMismatch,
  InvalidSourceMir,
  InvalidOptimizedMir,
  InvalidOptimization,
  InvalidFailureMetadata,
  InvalidBodyInventory,
  InvalidDeclarationInventory,
  InvalidSymbolInventory,
  InvalidInstanceInventory,
  InvalidGeneratedItemInventory,
  InvalidEmbeddingPlan,
  MissingGeneratedItem,
  DuplicateGeneratedItem,
  OrphanGeneratedItem,
  CyclicGeneratedItemDependency,
  InvalidConstructionSeal,
};

struct LoweredProgramIssue {
  LoweredProgramIssueKind kind = LoweredProgramIssueKind::FrontendMismatch;
  std::string detail;
  std::optional<MirBodyAddress> body;
  std::optional<LoweredDeclarationId> declaration;
  std::optional<LoweredGeneratedItemIdentity> generatedItem;
};

class LoweredProgram;
class VerifiedLoweredProgram;

namespace lowered_detail {
// Builder-only: every verifier check except the construction seal, which the
// builder establishes from the same value once these checks pass.
[[nodiscard]] std::vector<LoweredProgramIssue>
verifyLoweredProgramContents(const LoweredProgram &program);
} // namespace lowered_detail

class LoweredProgram final {
public:
  LoweredProgram(const LoweredProgram &) = default;
  LoweredProgram(LoweredProgram &&) noexcept = default;
  LoweredProgram &operator=(const LoweredProgram &) = default;
  LoweredProgram &operator=(LoweredProgram &&) noexcept = default;

  [[nodiscard]] const TargetInfo &target() const { return target_; }
  [[nodiscard]] const MirProgram &mir() const { return mir_; }
  [[nodiscard]] ProgramKind programKind() const { return programKind_; }
  [[nodiscard]] const std::optional<LoweredEmbeddingPlan> &
  embeddingPlan() const {
    return embeddingPlan_;
  }
  [[nodiscard]] const std::vector<LoweredBody> &bodies() const {
    return bodies_;
  }
  [[nodiscard]] const std::vector<LoweredDeclaration> &declarations() const {
    return declarations_;
  }
  [[nodiscard]] const std::vector<LoweredSymbol> &symbols() const {
    return symbols_;
  }
  [[nodiscard]] const std::vector<LoweredClassInstance> &
  classInstances() const {
    return classInstances_;
  }
  [[nodiscard]] const std::vector<LoweredFunctionInstance> &
  functionInstances() const {
    return functionInstances_;
  }
  [[nodiscard]] const std::vector<LoweredConstructorInstance> &
  constructorInstances() const {
    return constructorInstances_;
  }
  [[nodiscard]] const std::vector<LoweredDestructorInstance> &
  destructorInstances() const {
    return destructorInstances_;
  }
  [[nodiscard]] const std::vector<LoweredLambdaInstance> &
  lambdaInstances() const {
    return lambdaInstances_;
  }
  [[nodiscard]] const std::vector<LoweredGeneratedItem> &
  generatedItems() const {
    return generatedItems_;
  }

  [[nodiscard]] const LoweredBody *findBody(MirBodyAddress address) const;
  [[nodiscard]] const LoweredDeclaration *
  findDeclaration(LoweredDeclarationId id) const;
  [[nodiscard]] const LoweredClassDeclaration *
  findClassDeclaration(ClassId id) const;
  // The declaration row carrying a class or enum declaration payload.
  [[nodiscard]] const LoweredDeclaration *
  findClassDeclarationRow(ClassId id) const;
  [[nodiscard]] const LoweredDeclaration *
  findEnumDeclarationRow(const SemanticType &type) const;
  [[nodiscard]] const LoweredEnumDeclaration *
  findEnumDeclaration(EnumId id) const;
  [[nodiscard]] const LoweredEnumDeclaration *
  findEnumDeclaration(const SemanticType &type) const;
  [[nodiscard]] const LoweredFunctionDeclaration *
  findFunctionDeclaration(FunctionId id) const;
  [[nodiscard]] const LoweredConstructorDeclaration *
  findConstructorDeclaration(ConstructorId id) const;
  [[nodiscard]] const LoweredStorageDeclaration *
  findStorageDeclaration(SymbolId id) const;
  [[nodiscard]] const LoweredGenericParameter *
  findGenericParameter(GenericParameterId id) const;
  [[nodiscard]] const LoweredSymbol *findSymbol(SymbolId id) const;
  [[nodiscard]] const LoweredClassInstance *
  findClassInstance(LoweredClassInstanceId id) const;
  [[nodiscard]] const LoweredFunctionInstance *
  findFunctionInstance(LoweredFunctionInstanceId id) const;
  [[nodiscard]] const LoweredConstructorInstance *
  findConstructorInstance(LoweredConstructorInstanceId id) const;
  [[nodiscard]] const LoweredDestructorInstance *
  findDestructorInstance(LoweredDestructorInstanceId id) const;
  [[nodiscard]] const LoweredLambdaInstance *
  findLambdaInstance(LoweredLambdaInstanceId id) const;
  [[nodiscard]] const LoweredGeneratedItem *
  findGeneratedItem(const LoweredGeneratedItemIdentity &identity) const;

private:
  LoweredProgram() = default;

  // Rebuilds the lookup hints from the completed declaration and symbol rows.
  void indexDeclarations();

  TargetInfo target_;
  MirProgram mir_;
  ProgramKind programKind_ = ProgramKind::Executable;
  std::optional<LoweredEmbeddingPlan> embeddingPlan_;
  std::vector<LoweredBody> bodies_;
  std::vector<LoweredDeclaration> declarations_;
  std::vector<LoweredSymbol> symbols_;
  std::vector<LoweredClassInstance> classInstances_;
  std::vector<LoweredFunctionInstance> functionInstances_;
  std::vector<LoweredConstructorInstance> constructorInstances_;
  std::vector<LoweredDestructorInstance> destructorInstances_;
  std::vector<LoweredLambdaInstance> lambdaInstances_;
  std::vector<LoweredGeneratedItem> generatedItems_;
  std::optional<std::uint64_t> constructionSeal_;
  // Derived row hints keyed by semantic identity, never part of the printed
  // contract. A lookup uses a hint only after confirming that its row still
  // carries the requested identity, and otherwise scans, so emission does
  // not repeatedly scan every declaration of an imported library.
  std::unordered_map<std::size_t, std::size_t> classRows_;
  std::unordered_map<std::size_t, std::size_t> functionRows_;
  std::unordered_map<std::size_t, std::size_t> constructorRows_;
  std::unordered_map<std::size_t, std::size_t> storageRows_;
  std::unordered_map<std::size_t, std::size_t> symbolRows_;
  std::unordered_map<std::size_t, std::size_t> enumRows_;
  std::unordered_map<std::size_t, std::size_t> genericParameterRows_;

  friend class LoweredProgramBuilder;
  friend struct LoweredProgramTestAccess;
  friend std::vector<LoweredProgramIssue>
  verifyLoweredProgram(const LoweredProgram &program);
  friend std::vector<LoweredProgramIssue>
  lowered_detail::verifyLoweredProgramContents(const LoweredProgram &program);
};

[[nodiscard]] std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program);

// Takes an independent value for a consumer and returns owning evidence only
// when that exact snapshot passes verification. Lvalues copy; rvalues move.
// The construction seal supplements the inventory checks; the inspection
// print is not an exhaustive serialization of every verified invariant.
[[nodiscard]] std::optional<VerifiedLoweredProgram>
verifiedLoweredProgram(LoweredProgram program,
                       std::vector<LoweredProgramIssue> &issues);

// Evidence that verifyLoweredProgram accepted this exact, owned const value.
// A caller may replace or destroy its original LoweredProgram without
// making this proof stale. Consumer stages share the snapshot rather than
// repeating the inventory and seal check or copying another complete program.
class VerifiedLoweredProgram final {
public:
  // Deliberately copy shared ownership even for an rvalue: there is no empty
  // or moved-from evidence object with a dangling/absent program.
  VerifiedLoweredProgram(const VerifiedLoweredProgram &) = default;
  VerifiedLoweredProgram &operator=(const VerifiedLoweredProgram &) = default;

  [[nodiscard]] const LoweredProgram &program() const noexcept {
    return *program_;
  }

private:
  explicit VerifiedLoweredProgram(
      std::shared_ptr<const LoweredProgram> program) noexcept
      : program_(std::move(program)) {}

  std::shared_ptr<const LoweredProgram> program_;

  friend std::optional<VerifiedLoweredProgram>
  verifiedLoweredProgram(LoweredProgram program,
                         std::vector<LoweredProgramIssue> &issues);
};

} // namespace lang
