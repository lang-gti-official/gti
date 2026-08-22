#pragma once

#include "gti/ast.h"
#include "gti/concurrency.h"
#include "gti/diagnostic.h"
#include "gti/hir_instance_index.h"
#include "gti/semantic_analyzer.h"
#include "gti/target.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

using HirValueId = std::size_t;
using HirBindingId = std::size_t;
using HirStatementId = std::size_t;
using HirClassInstanceId = std::size_t;
using HirFunctionInstanceId = std::size_t;
using HirConstructorInstanceId = std::size_t;
using HirDestructorInstanceId = std::size_t;
using HirLambdaId = std::size_t;
using HirFullExpressionId = std::size_t;
using HirDropObligationId = std::size_t;

enum class HirDropObligationKind {
  Binding,
  Value,
};

struct HirDropType {
  SemanticType type = SemanticType::Unknown;
  std::optional<HirClassInstanceId> classInstance;
  std::optional<HirLambdaId> lambdaInstance;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveCleanup = false;
};

struct HirDropObligation {
  HirDropObligationId id = 0;
  std::size_t constructionOrder = 0;
  HirDropObligationKind kind = HirDropObligationKind::Value;
  HirBindingId binding = 0;
  HirValueId value = 0;
  HirFullExpressionId fullExpression = 0;
  HirDropType dropType;
  bool initiallyActive = false;
};

struct HirFullExpression {
  HirFullExpressionId id = 0;
  HirStatementId statement = 0;
  std::size_t constructorInitializer = 0;
  std::vector<HirValueId> roots;
};

enum class HirValueKind {
  Assignment,
  ArrayInitializer,
  Binary,
  Call,
  Conditional,
  Move,
  Conversion,
  DirectInitializer,
  DereferenceSet,
  MemberAccess,
  Grouping,
  Index,
  IndexSet,
  Lambda,
  LayoutQuery,
  Literal,
  Logical,
  PackFold,
  PackExpansion,
  PayloadConstruction,
  PayloadExtraction,
  Postfix,
  QualifiedName,
  This,
  MemberSet,
  Unary,
  Unexpected,
  Variable,
};

enum class HirStatementKind {
  Block,
  CompileTimeBranch,
  DoWhile,
  Empty,
  Expression,
  For,
  RangeFor,
  If,
  Break,
  Continue,
  Return,
  Switch,
  StructuredBinding,
  Variable,
  While,
};

struct HirBinding {
  HirBindingId id = 0;
  const VariableDecl *variable = nullptr;
  const Parameter *parameter = nullptr;
  const Token *payloadPattern = nullptr;
  const StructuredBindingDecl *structuredSource = nullptr;
  BindingInfo info;
  std::optional<HirDropObligationId> dropObligation;
};

struct HirLoan {
  SemanticLoanId semanticLoan = 0;
  SemanticLoanId parent = 0;
  SemanticLoanPlace place;
  AccessMode access = AccessMode::ReadOnly;
  std::vector<HirBindingId> carriers;
  bool entry = false;
};

enum class HirCallInputKind {
  Value,
  CopyValue,
  MoveValue,
  ReadBorrow,
  MutableBorrow,
};

struct HirCallReceiver {
  HirValueId value = 0;
  HirCallInputKind kind = HirCallInputKind::Value;
  SemanticType type = SemanticType::Unknown;
};

struct HirCallArgument {
  std::size_t parameterIndex = 0;
  HirValueId value = 0;
  SemanticType parameterType = SemanticType::Unknown;
  HirCallInputKind kind = HirCallInputKind::Value;
};

struct HirCallPlan {
  std::optional<HirCallReceiver> receiver;
  std::vector<HirCallArgument> arguments;
};

struct HirPackFoldElement {
  SemanticType elementType = SemanticType::Unknown;
  HirFunctionInstanceId functionTarget = 0;
  std::vector<SemanticType> parameterTypes;
};

struct HirPackExpansionElement {
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  HirCallInputKind kind = HirCallInputKind::Value;
};

struct HirValue {
  HirValueId id = 0;
  HirValueKind kind = HirValueKind::Literal;
  const Expr *source = nullptr;
  ExpressionInfo info;
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  SymbolId symbol = 0;
  std::vector<HirValueId> operands;
  std::vector<SemanticType> parameterTypes;
  std::optional<TokenKind> operation;
  std::optional<Literal> literal;
  std::optional<ConstantValue> constant;
  bool programConstantSubstitution = false;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  SynchronizationOperation synchronization;
  DefinedFailureOperation definedFailure;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> borrowPlace;
  bool storedReferenceAccess = false;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirValueId> receiver;
  std::optional<HirCallPlan> callPlan;
  SymbolId packFoldSymbol = 0;
  GenericParameterId packFoldParameter = 0;
  FunctionId packFoldFunction = 0;
  std::size_t packFoldArgument = 0;
  std::vector<HirPackFoldElement> packFoldElements;
  std::vector<HirPackExpansionElement> packExpansionElements;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirFunctionInstanceId> contextualBoolTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<CallableArgumentBoundary> callableArguments;
  std::optional<CallableBoundary> callableBoundary;
  std::optional<CallableInvocationCapability> callableInvocation;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  std::optional<std::size_t> enumVariant;
  std::optional<std::size_t> payloadIndex;
  std::optional<PlaceKey> place;
  std::optional<OwnershipEvent> ownership;
  HirFullExpressionId fullExpression = 0;
  std::optional<HirDropObligationId> dropObligation;
};

struct HirSwitchLabel {
  const SwitchLabel *source = nullptr;
  bool isDefault = false;
  std::optional<HirValueId> value;
  std::optional<SwitchCaseValue> constant;
};

struct HirSwitchArm {
  std::vector<HirSwitchLabel> labels;
  struct PayloadBinding {
    HirBindingId binding = 0;
    HirValueId value = 0;
  };
  std::vector<PayloadBinding> payloadBindings;
  std::vector<HirStatementId> statements;
  std::vector<SemanticLoanId> entryEndedLoans;
};

enum class HirStructuredBindingProjectionKind {
  Field,
  ArrayElement,
};

struct HirStructuredBindingElement {
  HirBindingId binding = 0;
  HirStructuredBindingProjectionKind projection =
      HirStructuredBindingProjectionKind::Field;
  SymbolId field = 0;
  std::optional<HirValueId> index;
};

struct HirStatement {
  HirStatementId id = 0;
  HirStatementKind kind = HirStatementKind::Empty;
  const Stmt *source = nullptr;
  bool unsafeBlock = false;
  std::optional<HirBindingId> binding;
  std::optional<HirValueId> value;
  std::optional<HirValueId> condition;
  std::optional<HirValueId> increment;
  std::optional<HirStatementId> initializer;
  std::optional<HirStatementId> body;
  std::optional<HirStatementId> elseBranch;
  std::vector<HirStatementId> statements;
  std::vector<HirSwitchArm> switchArms;
  bool exhaustiveSwitch = false;
  std::vector<HirStructuredBindingElement> structuredBindings;
  std::vector<SemanticLoanId> endedLoans;
  std::vector<SemanticLoanId> thenEntryEndedLoans;
  std::vector<SemanticLoanId> elseEntryEndedLoans;
};

struct HirBody {
  PlaceDomain placeDomain;
  std::vector<HirBinding> bindings;
  std::vector<HirLoan> loans;
  std::vector<HirFullExpression> fullExpressions;
  std::vector<HirDropObligation> dropObligations;
  std::vector<HirValue> values;
  std::vector<HirStatement> statements;
  std::vector<HirStatementId> roots;

  [[nodiscard]] const HirValue *findValue(HirValueId id) const;

  [[nodiscard]] const HirStatement *findStatement(HirStatementId id) const;

  [[nodiscard]] const HirLoan *findLoan(SemanticLoanId id) const;

  [[nodiscard]] const HirDropObligation *
  findDropObligation(HirDropObligationId id) const {
    return id == 0 || id > dropObligations.size() ? nullptr
                                                  : &dropObligations[id - 1];
  }
};

struct HirLambda {
  HirLambdaId id = 0;
  LambdaId declaration = 0;
  const Lambda *source = nullptr;
  SemanticType type = SemanticType::Unknown;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  std::vector<LambdaCaptureInfo> captures;
  std::vector<bool> captureRequiresActiveCleanup;
  SemanticTypeTraits traits{};
  HirBody body;
};

struct HirEnumerator {
  const EnumeratorDecl *source = nullptr;
  EnumConstant value;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
};

struct HirEnum {
  EnumId declaration = 0;
  SourceUnitId sourceUnit = 0;
  const EnumDecl *source = nullptr;
  std::string qualifiedName;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<HirEnumerator> enumerators;
  bool payload = false;
};

struct HirClassField {
  const VariableDecl *declaration = nullptr;
  HirBindingId binding = 0;
  std::optional<HirValueId> initializer;
  BindingInfo info;
  bool requiresActiveCleanup = false;
};

struct HirBaseInstance {
  HirClassInstanceId instance = 0;
  SemanticType type = SemanticType::Unknown;
  bool interface = false;

  friend bool operator==(const HirBaseInstance &,
                         const HirBaseInstance &) = default;
};

struct HirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  const ClassDecl *source = nullptr;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  std::optional<CAbiRecordLayout> cAbiLayout;
  std::optional<UnionLayout> unionLayout;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructorStatus = SpecialMemberStatus::Generated;
  std::vector<HirClassField> fields;
  HirBody fieldInitializers;
  std::vector<HirClassField> staticFields;
  HirBody staticFieldInitializers;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveDropState = false;
  bool requiresActiveCleanup = false;
};

struct HirCallableSignature {
  const Call *source = nullptr;
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability requiredCapability =
      CallableInvocationCapability::Read;
  std::optional<CallableInvocationCapability> selectedCapability;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;
};

struct HirCallableForwarding {
  const Call *source = nullptr;
  std::size_t parameterIndex = 0;
  std::optional<HirFunctionInstanceId> functionTarget;
};

struct HirCallableParameter {
  std::size_t parameterIndex = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  CallableBoundary boundary = CallableBoundary::Confined;
  std::optional<CallableOwnedTransport> ownedTransport;
  std::vector<HirCallableSignature> signatures;
  std::vector<HirCallableForwarding> forwardings;
};

struct HirFunctionInstance {
  HirFunctionInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  FunctionId declaration = 0;
  const FunctionDecl *source = nullptr;
  std::optional<HirClassInstanceId> owner;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  std::optional<HirFunctionInstanceId> entryArgumentAppendTarget;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
  bool staticMember = false;
  bool internalLinkage = false;
  bool constexprFunction = false;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
  std::vector<HirCallableParameter> callableParameters;
};

struct HirConstructorInitializer {
  const ConstructorInitializer *source = nullptr;
  ConstructorInitializerTargetKind kind =
      ConstructorInitializerTargetKind::Field;
  SemanticType targetType = SemanticType::Unknown;
  SymbolId field = 0;
  std::optional<HirClassInstanceId> base;
  std::optional<HirConstructorInstanceId> constructorTarget;
  std::vector<HirValueId> arguments;
  bool storesReference = false;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
  std::optional<std::size_t> ownedParameter;
};

struct HirConstructorInstance {
  HirConstructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ConstructorId declaration = 0;
  const ConstructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::vector<HirConstructorInitializer> initializers;
  std::vector<HirValueId> initializerValues;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
};

struct HirDestructorInstance {
  HirDestructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  const DestructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  HirBody body;
};

struct HirProgramInitializationStep {
  ProgramInitializationStepId id = 0;
  SourceUnitId sourceUnit = 0;
  ProgramStorageKind kind = ProgramStorageKind::NamespaceGlobal;
  ProgramInitializationStepRole role =
      ProgramInitializationStepRole::Initializer;
  const VariableDecl *source = nullptr;
  SymbolId symbol = 0;
  HirClassInstanceId ownerClass = 0;
  bool requiresActiveCleanup = false;
  HirBindingId binding = 0;
  std::optional<HirValueId> initializer;
  HirStatementId statement = 0;
};

struct HirProgramInitializationPlan {
  std::vector<SourceUnitId> unitOrder;
  std::vector<HirProgramInitializationStep> steps;

  [[nodiscard]] const HirProgramInitializationStep *
  findStepForSymbol(SymbolId symbol) const {
    const auto found =
        std::find_if(steps.begin(), steps.end(), [symbol](const auto &step) {
          return step.symbol == symbol;
        });
    return found == steps.end() ? nullptr : &*found;
  }
};

struct HirHostedProgramEntryPlan {
  FunctionId semanticEntry = 0;
  FunctionId semanticAppendFunction = 0;
  ConstructorId semanticVectorConstructor = 0;
  ConstructorId semanticStringConstructor = 0;
  HirFunctionInstanceId entry = 0;
  HirFunctionInstanceId appendFunction = 0;
  HirConstructorInstanceId vectorConstructor = 0;
  HirConstructorInstanceId stringConstructor = 0;
  ProgramEntryKind kind = ProgramEntryKind::None;
  SourceUnitId sourceUnit = 0;
  SourceSpan mainAnchor;
  DefinedFailureOperation validateCount;
  DefinedFailureOperation convertCount;
};

class HirProgram {
public:
  [[nodiscard]] bool valid() const { return valid_; }

  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }

  [[nodiscard]] const SemanticAnalysisSeal &analysisSeal() const {
    return semanticAnalysisSeal;
  }

  [[nodiscard]] const std::vector<HirClassInstance> &classInstances() const {
    return classes;
  }

  [[nodiscard]] const std::vector<HirEnum> &enumDeclarations() const {
    return enums;
  }

  [[nodiscard]] const std::vector<HirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<HirConstructorInstance> &
  constructorInstances() const {
    return constructors;
  }

  [[nodiscard]] const std::vector<HirDestructorInstance> &
  destructorInstances() const {
    return destructors;
  }

  [[nodiscard]] const std::vector<HirLambda> &lambdaInstances() const {
    return lambdas;
  }

  [[nodiscard]] const HirBody &module() const { return moduleBody; }

  [[nodiscard]] const HirProgramInitializationPlan &
  programInitializationPlan() const {
    return programInitialization;
  }

  [[nodiscard]] const std::optional<HirHostedProgramEntryPlan> &
  hostedProgramEntryPlan() const {
    return hostedProgramEntry;
  }

  [[nodiscard]] std::size_t valueCount() const;

  [[nodiscard]] std::size_t statementCount() const;

  [[nodiscard]] const HirFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const {
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  }

  [[nodiscard]] const HirConstructorInstance *
  findConstructorInstance(HirConstructorInstanceId id) const {
    return id == 0 || id > constructors.size() ? nullptr
                                               : &constructors[id - 1];
  }

  [[nodiscard]] const HirDestructorInstance *
  findDestructorInstance(HirDestructorInstanceId id) const {
    return id == 0 || id > destructors.size() ? nullptr : &destructors[id - 1];
  }

  [[nodiscard]] const HirLambda *findLambda(HirLambdaId id) const {
    return id == 0 || id > lambdas.size() ? nullptr : &lambdas[id - 1];
  }

  [[nodiscard]] const std::vector<HirValueId> &
  valueIdsForSource(const Expr &source) const;

private:
  friend class HirLowerer;

  bool valid_ = true;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  SemanticAnalysisSeal semanticAnalysisSeal;
  std::vector<HirEnum> enums;
  std::vector<HirClassInstance> classes;
  std::vector<HirFunctionInstance> functions;
  std::vector<HirConstructorInstance> constructors;
  std::vector<HirDestructorInstance> destructors;
  std::vector<HirLambda> lambdas;
  HirProgramInitializationPlan programInitialization;
  std::optional<HirHostedProgramEntryPlan> hostedProgramEntry;
  HirBody moduleBody;
  std::unordered_map<const Expr *, std::vector<HirValueId>> sourceValueIds;
};

struct HirProgramPlanVerificationResult {
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

[[nodiscard]] HirProgramPlanVerificationResult
verifyHirProgramPlans(const SemanticModel &semantics,
                      const HirProgram &program);

struct HirLoweringResult {
  HirProgram program;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool valid() const {
    return diagnostics.empty() && program.valid();
  }
};

class HirLowerer {
public:
  explicit HirLowerer(TargetInfo target = TargetInfo::host());
  ~HirLowerer();

  HirLowerer(const HirLowerer &) = delete;
  HirLowerer &operator=(const HirLowerer &) = delete;
  HirLowerer(HirLowerer &&) noexcept;
  HirLowerer &operator=(HirLowerer &&) noexcept;

  // Non-const: concrete instance reanalysis mutates the analyzer through its
  // detach/restore bracket.
  [[nodiscard]] HirLoweringResult lower(const Program &source,
                                        SemanticVisitor &semantics);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
