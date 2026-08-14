#pragma once

#include "gti/hir.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

using MirBlockId = std::size_t;
using MirPlaceId = std::size_t;
using MirLoanId = std::size_t;
using MirInstructionId = std::size_t;
using MirValueId = std::size_t;
using MirTemporaryId = std::size_t;
using MirDropObligationId = std::size_t;

struct MirFullExpression {
  HirFullExpressionId id = 0;
  HirFullExpressionId hirExpression = 0;
  HirStatementId statement = 0;
  std::size_t constructorInitializer = 0;
  std::vector<HirValueId> roots;
};

struct MirCleanupBoundary {
  std::size_t id = 0;
  std::vector<MirDropObligationId> obligations;
};

enum class MirBodyKind {
  Module,
  FieldInitializers,
  StaticFieldInitializers,
  Function,
  Constructor,
  Destructor,
  Lambda,
};

enum class MirPlaceRootKind {
  Binding,
  Symbol,
  This,
  Temporary,
  Value,
  Loan,
};

enum class MirProjectionKind {
  Field,
  Index,
  Dereference,
  RawIndex,
  RawDereference,
};

struct MirPlaceProjection {
  MirProjectionKind kind = MirProjectionKind::Field;
  SymbolId field = 0;
  MirValueId index = 0;
  std::optional<std::uint64_t> constantIndex;
  std::size_t selection = 0;
};

struct MirPlace {
  MirPlaceId id = 0;
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  std::size_t capture = 0;
  MirTemporaryId temporary = 0;
  MirValueId value = 0;
  MirLoanId loan = 0;
  std::vector<MirPlaceProjection> projections;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
  HirValueId sourceValue = 0;
  std::optional<PlaceKey> key;
  bool initiallyAvailable = false;
};

enum class MirOperandKind {
  Value,
  Constant,
  Address,
  Copy,
  Move,
  BorrowRead,
  BorrowWrite,
  Loan,
};

struct MirOperand {
  MirOperandKind kind = MirOperandKind::Value;
  MirValueId value = 0;
  MirPlaceId place = 0;
  MirLoanId loan = 0;
  std::optional<Literal> literal;
  SemanticType type = SemanticType::Unknown;
};

enum class MirLoanKind {
  Local,
  CallResult,
  Stored,
  Return,
  Parameter,
};

struct MirLoan {
  MirLoanId id = 0;
  SemanticLoanId semanticLoan = 0;
  MirLoanId parent = 0;
  MirLoanKind kind = MirLoanKind::Local;
  MirPlaceId source = 0;
  AccessMode access = AccessMode::ReadOnly;
  HirValueId producedBy = 0;
  std::vector<HirBindingId> carriers;
  SymbolId storedField = 0;
  bool entry = false;
  bool escapes = false;
};

enum class MirDropObligationKind {
  Binding,
  Value,
};

struct MirDropType {
  SemanticType type = SemanticType::Unknown;
  std::optional<HirClassInstanceId> classInstance;
  std::optional<HirLambdaId> lambdaInstance;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveCleanup = false;
};

struct MirDropObligation {
  MirDropObligationId id = 0;
  HirDropObligationId hirObligation = 0;
  std::size_t constructionOrder = 0;
  MirDropObligationKind kind = MirDropObligationKind::Value;
  MirPlaceId place = 0;
  HirBindingId binding = 0;
  HirValueId value = 0;
  HirFullExpressionId hirFullExpression = 0;
  HirFullExpressionId fullExpression = 0;
  MirDropType dropType;
  bool initiallyActive = false;
};

enum class MirLifecycleEventKind {
  Initialize,
  Move,
  Reparent,
  Replace,
  TransferOut,
  Drop,
};

struct MirLifecycleEvent {
  MirLifecycleEventKind kind = MirLifecycleEventKind::Initialize;
  MirDropObligationId source = 0;
  MirDropObligationId target = 0;
  bool conditional = false;
};

enum class MirInstructionKind {
  Compute,
  Load,
  Initialize,
  Assign,
  Modify,
  Move,
  Borrow,
  CallInput,
  Call,
  Construct,
  Drop,
  EndBorrow,
  Lifecycle,
  Count,
};

enum class MirCallInputRole {
  Receiver,
  Argument,
};

enum class MirOperation {
  None,
  Literal,
  EnumConstant,
  Aggregate,
  Index,
  Identity,
  Convert,
  ExpectedHasValue,
  Closure,
  PackExpansion,
  PayloadConstruct,
  PayloadExtract,
  Unexpected,
  AddressOf,
  PointerAdd,
  PointerSubtract,
  PointerDifference,
  Comma,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Positive,
  Negate,
  LogicalNot,
  BitwiseNot,
  Assign,
  AddAssign,
  SubtractAssign,
  MultiplyAssign,
  DivideAssign,
  RemainderAssign,
  BitwiseAndAssign,
  BitwiseOrAssign,
  BitwiseXorAssign,
  ShiftLeftAssign,
  ShiftRightAssign,
  PreIncrement,
  PreDecrement,
  PostIncrement,
  PostDecrement,
  Count,
};

struct MirInstruction {
  MirInstructionId id = 0;
  MirInstructionKind kind = MirInstructionKind::Compute;
  HirValueId hirValue = 0;
  HirStatementId hirStatement = 0;
  HirValueId callSite = 0;
  std::optional<MirCallInputRole> callInputRole;
  std::size_t callInputIndex = 0;
  HirCallInputKind callInputKind = HirCallInputKind::Value;
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  bool rawMemoryAccess = false;
  std::optional<MirValueId> result;
  std::optional<MirPlaceId> destination;
  std::optional<MirOperand> receiver;
  std::vector<MirOperand> operands;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> closureCaptureTypes;
  std::vector<LambdaCaptureMode> closureCaptureModes;
  std::optional<MirLoanId> loan;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  MirOperation operation = MirOperation::None;
  std::optional<Literal> literal;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  std::optional<std::size_t> enumVariant;
  std::optional<std::size_t> payloadIndex;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<CallableArgumentBoundary> callableArguments;
  std::optional<CallableBoundary> callableBoundary;
  std::optional<CallableInvocationCapability> callableInvocation;
  ExpressionInfo info;
  std::optional<OwnershipEvent> ownership;
  std::vector<MirLifecycleEvent> lifecycle;
  HirFullExpressionId fullExpressionEnd = 0;
  std::size_t cleanupBoundaryEnd = 0;
};

enum class MirTerminatorKind {
  None,
  Goto,
  Branch,
  Switch,
  Return,
  Unreachable,
  Exit,
};

struct MirSwitchTarget {
  std::optional<SwitchCaseValue> value;
  MirBlockId target = 0;
};

struct MirTerminator {
  MirTerminatorKind kind = MirTerminatorKind::None;
  HirValueId hirValue = 0;
  HirStatementId hirStatement = 0;
  std::optional<MirOperand> value;
  std::optional<MirLoanId> returnLoan;
  MirBlockId target = 0;
  MirBlockId elseTarget = 0;
  std::vector<MirSwitchTarget> switchTargets;
};

struct MirBlock {
  MirBlockId id = 0;
  std::vector<MirInstruction> instructions;
  MirTerminator terminator;
  bool reachable = false;
};

struct MirValue {
  MirValueId id = 0;
  HirValueId sourceValue = 0;
  ExpressionInfo info;
  MirBlockId definitionBlock = 0;
  MirInstructionId definition = 0;
};

enum class MirValueUseKind {
  InstructionOperand,
  InstructionReceiver,
  Terminator,
  PlaceRoot,
  PlaceIndex,
};

struct MirValueUse {
  MirValueId value = 0;
  MirValueUseKind kind = MirValueUseKind::InstructionOperand;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  std::size_t operandIndex = 0;
};

struct MirBody {
  MirBodyKind kind = MirBodyKind::Function;
  PlaceDomain placeDomain;
  MirBlockId entry = 0;
  SemanticType returnType = SemanticType::Void;
  std::vector<MirBlock> blocks;
  std::vector<MirPlace> places;
  std::vector<MirLoan> loans;
  std::vector<MirFullExpression> fullExpressions;
  std::vector<MirCleanupBoundary> cleanupBoundaries;
  std::vector<MirDropObligation> dropObligations;
  std::vector<MirValue> values;
  std::vector<std::vector<MirValueUse>> valueUses;

  [[nodiscard]] const MirBlock *findBlock(MirBlockId id) const {
    return id == 0 || id > blocks.size() ? nullptr : &blocks[id - 1];
  }

  [[nodiscard]] const MirPlace *findPlace(MirPlaceId id) const {
    return id == 0 || id > places.size() ? nullptr : &places[id - 1];
  }

  [[nodiscard]] const MirLoan *findLoan(MirLoanId id) const {
    return id == 0 || id > loans.size() ? nullptr : &loans[id - 1];
  }

  [[nodiscard]] const MirDropObligation *
  findDropObligation(MirDropObligationId id) const {
    return id == 0 || id > dropObligations.size() ? nullptr
                                                  : &dropObligations[id - 1];
  }

  [[nodiscard]] const MirValue *findValue(MirValueId id) const {
    return id == 0 || id > values.size() ? nullptr : &values[id - 1];
  }

  [[nodiscard]] const std::vector<MirValueUse> &usesOf(MirValueId id) const;

  [[nodiscard]] std::size_t instructionCount() const;
};

struct MirVerificationError {
  MirBodyKind bodyKind = MirBodyKind::Function;
  std::size_t owner = 0;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::string message;
};

struct MirVerificationResult {
  std::vector<MirVerificationError> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

void rebuildMirReachability(MirBody &body);
[[nodiscard]] bool rebuildMirValueUses(MirBody &body);
[[nodiscard]] MirVerificationResult verifyMirBody(const MirBody &body,
                                                  std::size_t owner = 0);

struct MirFieldDrop {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  bool requiresActiveCleanup = false;
};

struct MirClassFieldLifecycle {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  DropKind dropKind = DropKind::Trivial;
  bool requiresActiveCleanup = false;
};

struct MirClassFieldInfo {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  DropKind dropKind = DropKind::Trivial;
  bool requiresActiveCleanup = false;
};

struct MirClassInstance {
  HirClassInstanceId id = 0;
  ClassId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  std::vector<HirBaseInstance> structuralBases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  std::optional<CAbiRecordLayout> cAbiLayout;
  std::optional<UnionLayout> unionLayout;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveDropState = false;
  bool requiresActiveCleanup = false;
  std::vector<MirClassFieldInfo> declaredFields;
  std::vector<MirClassFieldLifecycle> fields;
  MirBody fieldInitializers;
  MirBody staticFieldInitializers;
  std::vector<MirFieldDrop> fieldDropOrder;
};

struct MirCallableSignature {
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability requiredCapability =
      CallableInvocationCapability::Read;
  std::optional<CallableInvocationCapability> selectedCapability;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;
};

struct MirCallableForwarding {
  std::size_t parameterIndex = 0;
  std::optional<HirFunctionInstanceId> functionTarget;
};

struct MirCallableParameter {
  std::size_t parameterIndex = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  CallableBoundary boundary = CallableBoundary::Confined;
  std::optional<CallableOwnedTransport> ownedTransport;
  std::vector<MirCallableSignature> signatures;
  std::vector<MirCallableForwarding> forwardings;
};

struct MirFunctionInstance {
  HirFunctionInstanceId id = 0;
  std::optional<HirClassInstanceId> owner;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  std::optional<HirFunctionInstanceId> entryArgumentAppendTarget;
  bool staticMember = false;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  std::optional<OverloadedOperator> overloadedOperator;
  bool constexprFunction = false;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
  std::vector<MirCallableParameter> callableParameters;
  MirBody body;
};

struct MirConstructorInitializer {
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

struct MirConstructorInstance {
  HirConstructorInstanceId id = 0;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::vector<MirConstructorInitializer> initializers;
  MirBody body;
};

struct MirDestructorInstance {
  HirDestructorInstanceId id = 0;
  HirClassInstanceId owner = 0;
  MirBody body;
};

struct MirLambdaInstance {
  HirLambdaId id = 0;
  LambdaId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> captureTypes;
  std::vector<LambdaCaptureMode> captureModes;
  std::vector<SymbolId> captureSymbols;
  std::vector<bool> captureRequiresActiveCleanup;
  MirBody body;
};

class MirProgram {
public:
  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }
  [[nodiscard]] const MirBody &module() const { return moduleBody; }

  [[nodiscard]] const std::vector<MirClassInstance> &classInstances() const {
    return classes;
  }

  [[nodiscard]] const std::vector<MirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<MirConstructorInstance> &
  constructorInstances() const {
    return constructors;
  }

  [[nodiscard]] const std::vector<MirDestructorInstance> &
  destructorInstances() const {
    return destructors;
  }

  [[nodiscard]] const std::vector<MirLambdaInstance> &lambdaInstances() const {
    return lambdas;
  }

  [[nodiscard]] const MirFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const {
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  }

  [[nodiscard]] const MirClassInstance *
  findClassInstance(HirClassInstanceId id) const {
    return id == 0 || id > classes.size() ? nullptr : &classes[id - 1];
  }

  [[nodiscard]] const MirConstructorInstance *
  findConstructorInstance(HirConstructorInstanceId id) const {
    return id == 0 || id > constructors.size() ? nullptr
                                               : &constructors[id - 1];
  }

  [[nodiscard]] const MirDestructorInstance *
  findDestructorInstance(HirDestructorInstanceId id) const {
    return id == 0 || id > destructors.size() ? nullptr : &destructors[id - 1];
  }

  [[nodiscard]] const MirLambdaInstance *findLambda(HirLambdaId id) const {
    return id == 0 || id > lambdas.size() ? nullptr : &lambdas[id - 1];
  }

  [[nodiscard]] std::size_t blockCount() const;

private:
  friend class MirLowerer;
  friend class MirProgramEditor;

  bool valid_ = true;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  MirBody moduleBody;
  std::vector<MirClassInstance> classes;
  std::vector<MirFunctionInstance> functions;
  std::vector<MirConstructorInstance> constructors;
  std::vector<MirDestructorInstance> destructors;
  std::vector<MirLambdaInstance> lambdas;
};

[[nodiscard]] MirVerificationResult verifyMirProgram(const MirProgram &program);

struct MirLoweringResult {
  MirProgram program;

  [[nodiscard]] bool valid() const { return program.valid(); }
};

class MirLowerer {
public:
  [[nodiscard]] MirLoweringResult lower(const HirProgram &source) const;

private:
  [[nodiscard]] static MirBody lowerBody(
      const HirProgram &program, const HirBody &body, MirBodyKind kind,
      SemanticType returnType, const std::vector<HirValueId> &prologueValues,
      bool &valid, bool implicitZeroReturn = false,
      const std::vector<HirConstructorInitializer> *initializers = nullptr,
      const HirFunctionInstance *function = nullptr,
      const HirLambda *lambda = nullptr);
};

} // namespace lang
