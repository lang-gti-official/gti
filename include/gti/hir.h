#pragma once

#include "gti/ast.h"
#include "gti/concurrency.h"
#include "gti/diagnostic.h"
#include "gti/hir_instance_index.h"
#include "gti/semantic_model.h"
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

class SemanticInstanceProvider;

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
using HirNativeCallbackAdapterId = std::size_t;
using HirThreadTaskAdapterId = std::size_t;

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
  // Concrete pack bindings retain one lexical obligation per owning element.
  std::optional<std::size_t> packElement;
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
  NativeCallback,
  FunctionItem,
  Conversion,
  DirectInitializer,
  DereferenceSet,
  PlaceSet,
  ExpectedPropagation,
  Propagation,
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
  // The hidden owner of a consumed payload-enum switch subject. It holds the
  // enum from the end of the subject's full expression to the selected arm's
  // entry, where the arm's by-value bindings take its fields or it is dropped.
  const SwitchStmt *switchSubject = nullptr;
  BindingInfo info;
  std::optional<HirDropObligationId> dropObligation;
};

struct HirReferenceSelection {
  HirValueId value = 0;
  HirValueId carrier = 0;
  SemanticLoanId transferredFrom = 0;
  std::vector<SymbolId> fields;
  std::vector<BorrowDependencyCondition> conditions;
  SemanticType reference = SemanticType::Unknown;
};

struct HirLoan {
  SemanticLoanId semanticLoan = 0;
  SemanticLoanKind kind = SemanticLoanKind::Reference;
  SemanticLoanId parent = 0;
  SemanticLoanId storedDependency = 0;
  SemanticLoanPlace place;
  AccessMode access = AccessMode::ReadOnly;
  std::vector<SymbolId> designatedFields;
  std::vector<BorrowDependencyCondition> conditions;
  std::vector<SemanticLoanDependency> additionalDependencies;
  std::optional<HirReferenceSelection> referenceSelection;
  bool lifecycleBound = false;
  std::vector<HirBindingId> carriers;
  bool entry = false;
};

enum class HirCallInputKind {
  Value,
  CopyValue,
  MoveValue,
  ReadBorrow,
  MutableBorrow,
  ReadTemporaryBorrow,
  MutableTemporaryBorrow,
  // An exact fresh borrowed-state carrier moved into one ordinary call or
  // constructor parameter. This is not an escaping transfer: MIR verifies
  // the producer, retained owner loan, lifetime slot, and unique consumer.
  MoveBorrowedTemporary,
  // Once access to an owned field of consuming this; cleanup stays in this.
  RetainedReceiver,
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
  bool defaultArgument = false;
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

struct HirExpectedPropagationPlan {
  HirValueId operand = 0;
  HirValueId successExtraction = 0;
  HirValueId successValue = 0;
  HirValueId errorExtraction = 0;
  HirValueId errorValue = 0;
  HirValueId unexpected = 0;
  bool moves = false;
};

// Prefix `?` through `std::tryable`. Every step is an ordinary call of the
// selected conformance block's function, in the operand's full expression.
// `test` borrows the operand; `output` and `residual` each consume it on
// their own path. A temporary is consumed in place, an explicitly moved place
// through a move of that place (`movedPlace`), and a named place by copy.
// `residualReturn` constructs the enclosing return value from the residual;
// zero means the residual is that value itself.
struct HirPropagationPlan {
  HirValueId operand = 0;
  HirValueId movedPlace = 0;
  HirValueId test = 0;
  HirValueId output = 0;
  HirValueId residual = 0;
  HirValueId residualReturn = 0;
  bool consumesOperand = false;
};

struct HirValue {
  HirValueId id = 0;
  HirValueKind kind = HirValueKind::Literal;
  const Expr *source = nullptr;
  ExpressionInfo info;
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  SymbolId symbol = 0;
  std::vector<HirValueId> operands;
  // Caller-expanded declaration defaults, in exact operand-suffix order.
  // Retained even when a concrete pack prevents a fixed-arity call plan.
  std::vector<HirValueId> defaultArguments;
  std::vector<SemanticType> parameterTypes;
  std::optional<TokenKind> operation;
  std::optional<Literal> literal;
  std::optional<ConstantValue> constant;
  bool programConstantSubstitution = false;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  // A runtime representation query names a type, never an object or loan.
  std::optional<SemanticType> objectLayoutType;
  // Checked source control-flow contracts, retained independently of the
  // backend's private status-returning failure convention.
  bool neverReturns = false;
  std::optional<SemanticNoNormalBorrowResultProof> noNormalBorrowResultProof;
  bool tracksCaller = false;
  std::optional<CallerLocationOrigin> callerLocation;
  std::optional<SemanticCallerLocationProof> callerLocationProof;
  std::optional<SourceLocationLiteral> sourceLocation;
  SynchronizationOperation synchronization;
  DefinedFailureOperation definedFailure;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  // Exact argument origins retained for composed owner-dependent values.
  std::vector<std::size_t> borrowArguments;
  std::vector<BorrowedFieldOrigin> borrowFields;
  bool restoresReferenceResult = false;
  std::vector<RestoredBorrowField> restoredBorrowFields;
  std::optional<BorrowOriginPlace> borrowPlace;
  // Presence is part of the proof: an empty complete set differs from missing
  // semantic evidence. Origin places are body-qualified; structural paths are
  // declaration identities and must not be retagged.
  std::optional<SemanticBorrowDependencySet> borrowDependencies;
  // Exact closure-construction inputs, consumed by its existing checked loan
  // producer; not a complete result dependency set.
  std::vector<SemanticLoanDependency> captureBorrowDependencies;
  bool storedReferenceAccess = false;
  bool retainedReceiverProjection = false;
  std::optional<HirValueId> retainedReceiverOwner;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirValueId> receiver;
  std::optional<HirCallPlan> callPlan;
  std::optional<HirExpectedPropagationPlan> expectedPropagation;
  std::optional<HirPropagationPlan> propagation;
  // Eager call inputs evaluated in source order before a synthesized branch.
  // Unlike branch operands these retain their full-expression cleanup on both
  // paths, including when the conditional mapper is never invoked.
  std::vector<HirValueId> conditionalInputs;
  // Evaluations that must complete in order before this value begins, such
  // as a comma's left operand. Their results are discarded; they share the
  // enclosing full-expression lifetime, not a nested scope.
  std::vector<HirValueId> sequencedInputs;
  SymbolId packFoldSymbol = 0;
  GenericParameterId packFoldParameter = 0;
  FunctionId packFoldFunction = 0;
  std::size_t packFoldArgument = 0;
  std::vector<HirPackFoldElement> packFoldElements;
  std::vector<HirPackExpansionElement> packExpansionElements;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirFunctionInstanceId> functionPointerFunctionTarget;
  std::optional<HirLambdaId> functionPointerLambdaTarget;
  std::optional<SemanticType> nativeCalleeType;
  std::optional<HirNativeCallbackAdapterId> nativeCallbackAdapter;
  std::optional<HirThreadTaskAdapterId> threadTaskAdapter;
  std::vector<HirThreadTaskAdapterId> scopedThreadTasks;
  std::optional<HirConstructorInstanceId> constructorTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  // Semantics resolved a full-field construction; stage all inputs before
  // publishing fields, with ordinary constructor argument failure cleanup.
  bool designatedConstruction = false;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<CallableArgumentBoundary> callableArguments;
  std::optional<CallableBoundary> callableBoundary;
  std::optional<CallableInvocationCapability> callableInvocation;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  std::optional<std::size_t> enumVariant;
  std::optional<std::size_t> payloadIndex;
  std::optional<PayloadExtractionKind> payloadExtractionKind;
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

struct HirRangeIteration {
  RangeIterationStrategy strategy = RangeIterationStrategy::Structural;
  SemanticType rangeType = SemanticType::Unknown;
  SemanticType iteratorType = SemanticType::Unknown;
  SemanticType sentinelType = SemanticType::Unknown;
  SemanticType elementType = SemanticType::Unknown;
  AccessMode elementAccess = AccessMode::ReadOnly;
  HirBindingId owner = 0;
  HirBindingId rangeCarrier = 0;
  HirBindingId iterator = 0;
  HirBindingId sentinel = 0;
  HirBindingId element = 0;
  HirBindingId elementOwner = 0;
  HirStatementId elementOwnerStatement = 0;
  HirValueId source = 0;
  HirValueId begin = 0;
  HirValueId end = 0;
  HirValueId comparison = 0;
  HirValueId elementAccessValue = 0;
  HirValueId increment = 0;
  HirStatementId coreLoop = 0;
  SemanticLoanId rangeLoan = 0;
  SemanticLoanId elementLoan = 0;
  SemanticLoanId elementOwnerLoan = 0;
  FunctionId beginFunction = 0;
  FunctionId endFunction = 0;
  FunctionId compareFunction = 0;
  FunctionId dereferenceFunction = 0;
  FunctionId incrementFunction = 0;
  std::uint64_t arrayLength = 0;
  bool ownsRangeTemporary = false;
  bool expectedPayloadProjection = false;
  bool nonBorrowingEndpoint = false;
  bool materializesElementValue = false;

  friend bool operator==(const HirRangeIteration &,
                         const HirRangeIteration &) = default;
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
  std::optional<HirRangeIteration> rangeIteration;
  std::vector<SemanticLoanId> endedLoans;
  std::vector<SemanticLoanId> thenEntryEndedLoans;
  std::vector<SemanticLoanId> elseEntryEndedLoans;
};

// Non-executing source wrappers share their producer's value and lifetime.
// Keep their exact source/type witness rather than inventing an Identity value
// with a second object/drop obligation.
struct HirDirectInitialization {
  const Expr *source = nullptr;
  const Expr *producer = nullptr;
  HirValueId value = 0;
  SemanticType type = SemanticType::Unknown;
  // Source-bound selected construction from the concrete semantic model.
  // Grouping needs no construction proof; ordinary invocation cannot get one.
  std::optional<SemanticDirectInitializationProof> constructionProof;
};

// A declaration initializer whose own type differs from its declared class
// type, consumed by the constructor semantics selected for it. `value` is that
// source-less construction; its first operand is the initializer's own value.
struct HirContextualConstruction {
  const Expr *source = nullptr;
  HirValueId value = 0;
  std::optional<SemanticContextualConstructionProof> constructionProof;
};

struct HirEmptyReturnConstruction {
  const ReturnStmt *source = nullptr;
  HirValueId value = 0;
  std::optional<SemanticEmptyReturnConstructionProof> constructionProof;
};

struct HirBody {
  PlaceDomain placeDomain;
  std::vector<HirBinding> bindings;
  std::vector<HirLoan> loans;
  std::vector<HirFullExpression> fullExpressions;
  std::vector<HirDropObligation> dropObligations;
  std::vector<HirValue> values;
  std::vector<HirDirectInitialization> directInitializations;
  std::vector<HirContextualConstruction> contextualConstructions;
  std::vector<HirEmptyReturnConstruction> emptyReturnConstructions;
  // Source expressions that semantics aliased to another expression (a
  // reflected field access and the projection it denotes): the source shares
  // the aliased expression's value without a value of its own.
  struct SourceAlias {
    const Expr *source = nullptr;
    HirValueId value = 0;
  };
  std::vector<SourceAlias> sourceAliases;
  std::vector<HirStatement> statements;
  std::vector<HirStatementId> roots;

  [[nodiscard]] const HirValue *findValue(HirValueId id) const;

  [[nodiscard]] const HirValue *valueForSource(const Expr &source) const;

  // The value a declaration initializer supplies to its binding or field:
  // its contextual construction when one consumes it, else its own value.
  [[nodiscard]] const HirValue *
  initializerValueForSource(const Expr &source) const;

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
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  std::vector<bool> captureRequiresActiveCleanup;
  SemanticTypeTraits traits{};
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  HirBody body;
};

struct HirEnumerator {
  const EnumeratorDecl *source = nullptr;
  EnumConstant value;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
  std::vector<SymbolId> payloadSymbols;
  std::vector<bool> payloadRequiresActiveCleanup;
};

struct HirEnum {
  EnumId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SourceUnitId sourceUnit = 0;
  const EnumDecl *source = nullptr;
  std::string qualifiedName;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<HirEnumerator> enumerators;
  SemanticTypeTraits traits{};
  bool requiresActiveCleanup = false;
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

struct HirClassTypeAlias {
  TypeAliasId declaration = 0;
  const TypeAliasDecl *source = nullptr;
  SemanticType type = SemanticType::Unknown;
};

struct HirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  const ClassDecl *source = nullptr;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::optional<SourceSpan> instantiationSite;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  CompilerCapabilityTypeKind compilerCapability =
      CompilerCapabilityTypeKind::None;
  bool lifecycleBoundBorrowCarrier = false;
  bool mutexGuardBorrowCarrier = false;
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  std::optional<SemanticType> transferCondition;
  std::optional<SemanticType> shareCondition;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  bool cOpaqueHandle = false;
  std::optional<CAbiRecordLayout> cAbiLayout;
  std::optional<UnionLayout> unionLayout;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  bool generatedDefaultRequiresSchedule = false;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructorStatus = SpecialMemberStatus::Generated;
  std::vector<HirClassField> fields;
  std::vector<HirClassTypeAlias> typeAliases;
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
  std::optional<SemanticType> nativeCalleeType;
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
  std::vector<std::size_t> ownerlessTextParameters;
  std::optional<SemanticOwnerlessTextContractProof> ownerlessTextContractProof;
  std::vector<HirBindingId> parameterBindings;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  std::optional<HirFunctionInstanceId> entryArgumentAppendTarget;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  // Preserve the source constraint separately from inferred return facts.
  bool explicitReturnBorrowOrigin = false;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::vector<std::size_t> returnBorrowParameters;
  std::vector<BorrowedFieldOrigin> returnBorrowFields;
  std::optional<FunctionBorrowDependencySummary> completeReturnBorrow;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  bool restoresReferenceResult = false;
  std::vector<RestoredBorrowField> restoredBorrowFields;
  std::optional<OwnedPointeeProjection> ownedPointeeProjection;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
  bool staticMember = false;
  bool internalLinkage = false;
  bool constexprFunction = false;
  bool requiresUnsafe = false;
  bool neverReturns = false;
  bool tracksCaller = false;
  CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
  bool threadConfinedCallback = false;
  ProcessMemoryRuntimeOperation processMemoryOperation =
      ProcessMemoryRuntimeOperation::None;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
  std::vector<HirCallableParameter> callableParameters;
};

struct HirNativeCallbackAdapter {
  HirNativeCallbackAdapterId id = 0;
  HirFunctionInstanceId target = 0;
  SemanticType type = SemanticType::Unknown;
  CThreadCallbackKind cThreadCallback = CThreadCallbackKind::None;
};

struct HirThreadTaskAdapter {
  HirThreadTaskAdapterId id = 0;
  SemanticType taskType = SemanticType::Unknown;
  HirValueId spawnValue = 0;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;
  CallableInvocationCapability targetInvocation =
      CallableInvocationCapability::Read;
  CallableInvocationCapability invocation = CallableInvocationCapability::Once;
  // A scoped adapter is rooted at one operand of a synchronous join-all call.
  // Ordinary spawn adapters must not carry these borrowed-place dependencies.
  bool scoped = false;
  std::size_t scopeArgument = 0;
  std::vector<SemanticLoanDependency> scopeDependencies;

  friend bool operator==(const HirThreadTaskAdapter &,
                         const HirThreadTaskAdapter &) = default;
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
  std::size_t explicitArgumentCount = 0;
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
  // Structural default construction with an explicit, selected base or
  // declaration-field initializer schedule.
  // Such instances have no source constructor declaration.
  bool generatedDefault = false;
  bool constexprConstructor = false;
  bool requiresUnsafe = false;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::vector<std::size_t> borrowParameters;
  std::optional<FunctionBorrowDependencySummary> completeBorrow;
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

// Executable-root authority for a library snapshot. The semantic declaration
// and its one concrete HIR function instance are both retained so MIR and
// lowered-program verification can prove that wrapper planning never reopens
// name lookup.
struct HirEmbeddingExport {
  FunctionId semanticFunction = 0;
  HirFunctionInstanceId function = 0;
  std::string qualifiedName;
  std::string symbol;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  SourceSpan requestSource;

  friend bool operator==(const HirEmbeddingExport &,
                         const HirEmbeddingExport &) = default;
};

struct HirEmbeddingPlan {
  std::uint32_t abiVersion = embeddingAbiVersionV1;
  std::vector<HirEmbeddingExport> exports;

  friend bool operator==(const HirEmbeddingPlan &,
                         const HirEmbeddingPlan &) = default;
};

class HirProgram {
public:
  [[nodiscard]] const std::vector<ForeignStorageInfo> &
  foreignStorageDeclarations() const {
    return foreignStorage_;
  }
  [[nodiscard]] const ForeignStorageInfo *
  findForeignStorage(SymbolId symbol) const {
    const auto found = std::find_if(
        foreignStorage_.begin(), foreignStorage_.end(),
        [symbol](const auto &storage) { return storage.symbol == symbol; });
    return found == foreignStorage_.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool valid() const { return valid_; }

  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }

  [[nodiscard]] ProgramKind programKind() const { return programKind_; }

  [[nodiscard]] const SemanticAnalysisSeal &analysisSeal() const {
    return semanticAnalysisSeal;
  }

  [[nodiscard]] const std::vector<HirClassInstance> &classInstances() const {
    return classes;
  }

  [[nodiscard]] const std::vector<HirEnum> &enumDeclarations() const {
    return enums;
  }

  // Semantic declarations created only by concrete body analysis (for example
  // locals in a selected dependent constexpr arm), with request-wide IDs.
  [[nodiscard]] const std::vector<SymbolRecord> &instanceSymbols() const {
    return concreteSymbols;
  }

  [[nodiscard]] const std::vector<HirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<HirNativeCallbackAdapter> &
  nativeCallbackAdapters() const {
    return nativeCallbacks;
  }

  [[nodiscard]] const std::vector<HirThreadTaskAdapter> &
  threadTaskAdapters() const {
    return threadTasks;
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

  [[nodiscard]] const std::optional<HirEmbeddingPlan> &embeddingPlan() const {
    return embeddingPlan_;
  }

  [[nodiscard]] std::size_t valueCount() const;

  [[nodiscard]] std::size_t statementCount() const;

  [[nodiscard]] const HirFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const {
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  }

  [[nodiscard]] const HirNativeCallbackAdapter *
  findNativeCallbackAdapter(HirNativeCallbackAdapterId id) const {
    return id == 0 || id > nativeCallbacks.size() ? nullptr
                                                  : &nativeCallbacks[id - 1];
  }

  [[nodiscard]] const HirThreadTaskAdapter *
  findThreadTaskAdapter(HirThreadTaskAdapterId id) const {
    return id == 0 || id > threadTasks.size() ? nullptr : &threadTasks[id - 1];
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
  std::vector<ForeignStorageInfo> foreignStorage_;
  friend class HirLowerer;

  bool valid_ = true;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  ProgramKind programKind_ = ProgramKind::Executable;
  SemanticAnalysisSeal semanticAnalysisSeal;
  std::vector<HirEnum> enums;
  std::vector<SymbolRecord> concreteSymbols;
  std::vector<HirClassInstance> classes;
  std::vector<HirFunctionInstance> functions;
  std::vector<HirNativeCallbackAdapter> nativeCallbacks;
  std::vector<HirThreadTaskAdapter> threadTasks;
  std::vector<HirConstructorInstance> constructors;
  std::vector<HirDestructorInstance> destructors;
  std::vector<HirLambda> lambdas;
  HirProgramInitializationPlan programInitialization;
  std::optional<HirHostedProgramEntryPlan> hostedProgramEntry;
  std::optional<HirEmbeddingPlan> embeddingPlan_;
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
  // The prefix of `program` reachable from its program roots when that prefix
  // is smaller and verifies. Executable lowering uses it; `program` keeps every
  // concrete instance, whose analysis reported the program's diagnostics.
  std::optional<HirProgram> reachable;
  std::vector<Diagnostic> diagnostics;

  // Discovery stopped at these unchecked primary bodies. This is not usable
  // HIR: the frontend must obtain their semantic facts and lower again.
  std::vector<const FunctionDecl *> requiredSemanticBodies;
  // Function, constructor and destructor instances whose bodies were omitted
  // under `HirLoweringScope::omitLibraryOnlyBodies`.
  std::size_t omittedInstanceBodies = 0;

  [[nodiscard]] bool valid() const {
    return requiredSemanticBodies.empty() &&
           !hasErrorDiagnostics(diagnostics) && program.valid();
  }
};

// Which declarations seed whole-program lowering. By default every
// declaration is seeded and the result keeps every concrete instance. With
// `reachablePrefix`, discovery starts from the program roots and the result
// also carries the prefix reachable from them for executable lowering. With
// `entryDeclarations`, a half-open index range into the program's top-level
// declarations (one source unit's own declarations), only the program roots
// and that range are seeded and nothing else is lowered: the result holds what
// the unit's code reaches, and its diagnostics cover only those instances.
// With `reachablePrefix` and `completeDeclarations`, half-open ranges of
// top-level declarations, only those ranges are lowered completely after the
// prefix closes. Any other declaration is lowered only if the prefix reaches
// it, so its concrete-instance diagnostics are reported only then.
// With `omitLibraryOnlyBodies`, a function, constructor or destructor
// instance is library-only when its declaration belongs to one of
// `trustedUnits` and every class and function argument names only classes
// and enums of those units (no lambda, function item, open parameter or
// symbolic value). Such an instance is still analysed, so the diagnostics its
// own analysis reports are kept, but its body is not lowered: instances that
// only its body would request are not discovered and a primary body only it
// needs is not demanded. The LSP sets it because it never reads HIR bodies.
struct HirLoweringScope {
  bool reachablePrefix = false;
  std::optional<std::pair<std::size_t, std::size_t>> entryDeclarations;
  std::optional<std::vector<std::pair<std::size_t, std::size_t>>>
      completeDeclarations;
  bool omitLibraryOnlyBodies = false;
  std::vector<SourceUnitId> trustedUnits;
  // Diagnostic paths of compiler-trusted units. An error inside a generic
  // instance located in one of them is reported at the first request from
  // other code, with the library location kept as related information.
  std::vector<std::string> trustedSources;
};

class HirLowerer {
public:
  explicit HirLowerer(TargetInfo target = TargetInfo::host(),
                      HirLoweringScope scope = {});
  HirLowerer(TargetInfo target, bool reachablePrefix)
      : HirLowerer(std::move(target),
                   HirLoweringScope{.reachablePrefix = reachablePrefix}) {}
  ~HirLowerer();

  HirLowerer(const HirLowerer &) = delete;
  HirLowerer &operator=(const HirLowerer &) = delete;
  HirLowerer(HirLowerer &&) noexcept;
  HirLowerer &operator=(HirLowerer &&) noexcept;

  // The base semantic snapshot is immutable. Concrete discovery receives only
  // the narrow reanalysis capability, never the full analyzer facade.
  [[nodiscard]] HirLoweringResult
  lower(const Program &source, const SemanticModel &semantics,
        SemanticInstanceProvider &instanceProvider);

  // Staged execution only: lower a completed function/method/constructor root
  // and its closure without publishing whole-program initialization or entry.
  // The result deliberately has no whole-program analysis seal and cannot
  // cross the normal LoweredProgram/backend boundary.
  [[nodiscard]] HirLoweringResult lowerConstantFunction(
      const SemanticModel &semantics,
      SemanticInstanceProvider &instanceProvider, FunctionId root,
      const std::vector<FunctionId> &completedFunctions,
      const GenericSubstitution &substitution = {},
      const std::vector<ConstructorId> &completedConstructors = {});

  [[nodiscard]] HirLoweringResult lowerConstantConstructor(
      const SemanticModel &semantics,
      SemanticInstanceProvider &instanceProvider,
      const ResolvedConstructionInfo &root,
      const std::vector<FunctionId> &completedFunctions,
      const std::vector<ConstructorId> &completedConstructors);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
