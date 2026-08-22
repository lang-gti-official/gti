#pragma once

#include "gti/diagnostic.h"
#include "gti/mir.h"
#include "gti/target.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lang {

class HirProgram;
class Program;
class SemanticModel;
struct LoweredProgramTestAccess;

using LoweredDeclarationId = std::size_t;

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
  ConcreteInstanceAdapter,
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

struct LoweredBody {
  LoweredBodyIdentity identity;
  LoweredBodyRole role = LoweredBodyRole::SourceExecutable;
  std::vector<LoweredGeneratedItemIdentity> requiredGeneratedItems;

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

  friend bool operator==(const LoweredGenericParameter &,
                         const LoweredGenericParameter &) = default;
};

struct LoweredConceptRequirement {
  ConceptId conceptId = 0;
  std::vector<SemanticType> arguments;

  friend bool operator==(const LoweredConceptRequirement &,
                         const LoweredConceptRequirement &) = default;
};

struct LoweredParameter {
  SymbolId symbol = 0;
  std::string name;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  Mutability mutability = Mutability::Immutable;
  bool pack = false;
  bool hasDefault = false;
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
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<LoweredClassBase> bases;
  SemanticTypeTraits traits;
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
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

  friend bool operator==(const LoweredClassDeclaration &,
                         const LoweredClassDeclaration &) = default;
};

struct LoweredEnumerator {
  std::string name;
  EnumConstant value;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
  SourceSpan source;

  friend bool operator==(const LoweredEnumerator &,
                         const LoweredEnumerator &) = default;
};

struct LoweredEnumDeclaration {
  EnumId id = 0;
  SourceUnitId sourceUnit = 0;
  std::string qualifiedName;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<LoweredEnumerator> enumerators;
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
  std::vector<LoweredParameter> parameters;
  std::size_t requiredParameterCount = 0;
  std::optional<std::size_t> borrowParameter;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::optional<SpecialMemberSpecifierKind> specifier;
  bool compilerPrivate = false;

  friend bool operator==(const LoweredConstructorDeclaration &,
                         const LoweredConstructorDeclaration &) = default;
};

struct LoweredDestructorDeclaration {
  ClassId owner = 0;
  AccessModifier access = AccessModifier::Public;

  friend bool operator==(const LoweredDestructorDeclaration &,
                         const LoweredDestructorDeclaration &) = default;
};

struct LoweredStorageDeclaration {
  SymbolId symbol = 0;
  ClassId ownerClass = 0;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits;
  std::optional<ConstantValue> constant;
  Mutability mutability = Mutability::Immutable;
  bool staticStorage = false;
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

  friend bool operator==(const LoweredSymbol &,
                         const LoweredSymbol &) = default;
};

struct LoweredClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  SourceSpan source;

  friend bool operator==(const LoweredClassInstance &,
                         const LoweredClassInstance &) = default;
};

struct LoweredFunctionInstance {
  HirFunctionInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  FunctionId declaration = 0;
  std::optional<HirClassInstanceId> owner;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::optional<SourceSpan> instantiationSource;
  SourceSpan source;

  friend bool operator==(const LoweredFunctionInstance &,
                         const LoweredFunctionInstance &) = default;
};

struct LoweredConstructorInstance {
  HirConstructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ConstructorId declaration = 0;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::optional<SourceSpan> instantiationSource;
  SourceSpan source;

  friend bool operator==(const LoweredConstructorInstance &,
                         const LoweredConstructorInstance &) = default;
};

struct LoweredDestructorInstance {
  HirDestructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  HirClassInstanceId owner = 0;
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
  HirLambdaId id = 0;
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

struct LoweredNativeCallbackItem {
  MirNativeCallbackAdapter adapter;

  friend bool operator==(const LoweredNativeCallbackItem &,
                         const LoweredNativeCallbackItem &) = default;
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
  Count,
};

// One target-independent containment adapter around one concrete destructor
// MIR body. A backend chooses names and ABI spelling; these facts select the
// exact class representation and whether the helper needs failure dispatch.
struct LoweredLifecycleCleanupItem {
  ClassId ownerClass = 0;
  HirClassInstanceId classInstance = 0;
  HirDestructorInstanceId destructorInstance = 0;
  LoweredLifecycleCleanupForm form = LoweredLifecycleCleanupForm::OrdinaryClass;
  bool mayRaiseDefinedFailure = true;

  friend bool operator==(const LoweredLifecycleCleanupItem &,
                         const LoweredLifecycleCleanupItem &) = default;
};

using LoweredGeneratedItemPayload =
    std::variant<std::monostate, LoweredStructuralOperatorAdapterItem,
                 LoweredCallableAdapterItem, LoweredLifecycleCleanupItem,
                 LoweredNativeCallbackItem>;

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

class LoweredProgram final {
public:
  LoweredProgram(const LoweredProgram &) = default;
  LoweredProgram(LoweredProgram &&) noexcept = default;
  LoweredProgram &operator=(const LoweredProgram &) = default;
  LoweredProgram &operator=(LoweredProgram &&) noexcept = default;

  [[nodiscard]] const TargetInfo &target() const { return target_; }
  [[nodiscard]] const MirProgram &mir() const { return mir_; }
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
  [[nodiscard]] const LoweredEnumDeclaration *
  findEnumDeclaration(EnumId id) const;
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
  findClassInstance(HirClassInstanceId id) const;
  [[nodiscard]] const LoweredFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const;
  [[nodiscard]] const LoweredConstructorInstance *
  findConstructorInstance(HirConstructorInstanceId id) const;
  [[nodiscard]] const LoweredDestructorInstance *
  findDestructorInstance(HirDestructorInstanceId id) const;
  [[nodiscard]] const LoweredLambdaInstance *
  findLambdaInstance(HirLambdaId id) const;
  [[nodiscard]] const LoweredGeneratedItem *
  findGeneratedItem(const LoweredGeneratedItemIdentity &identity) const;

private:
  LoweredProgram() = default;

  TargetInfo target_;
  MirProgram mir_;
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

  friend class LoweredProgramBuilder;
  friend struct LoweredProgramTestAccess;
  friend std::vector<LoweredProgramIssue>
  verifyLoweredProgram(const LoweredProgram &program);
};

struct LoweredProgramBuild {
  std::optional<LoweredProgram> program;
  std::vector<LoweredProgramIssue> issues;

  [[nodiscard]] bool valid() const {
    return program.has_value() && issues.empty();
  }
};

class LoweredProgramBuilder final {
public:
  [[nodiscard]] LoweredProgramBuild
  build(const Program &program, const SemanticModel &semantics,
        const HirProgram &hir, const MirProgram &sourceMir,
        const MirProgram &optimizedMir, const TargetInfo &target) const;
};

[[nodiscard]] std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program);

} // namespace lang
