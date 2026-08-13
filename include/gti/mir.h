#pragma once

#include "gti/hir.h"

#include <algorithm>
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
  Call,
  Construct,
  Drop,
  EndBorrow,
  Lifecycle,
  Count,
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
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  bool rawMemoryAccess = false;
  std::optional<MirValueId> result;
  std::optional<MirPlaceId> destination;
  std::optional<MirOperand> receiver;
  std::vector<MirOperand> operands;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> closureCaptureTypes;
  std::optional<MirLoanId> loan;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  MirOperation operation = MirOperation::None;
  std::optional<Literal> literal;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<std::size_t> nonEscapingArguments;
  bool nonEscapingCallable = false;
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

  [[nodiscard]] const std::vector<MirValueUse> &usesOf(MirValueId id) const {
    static const std::vector<MirValueUse> empty;
    return id == 0 || id > valueUses.size() ? empty : valueUses[id - 1];
  }

  [[nodiscard]] std::size_t instructionCount() const {
    std::size_t result = 0;
    for (const MirBlock &block : blocks) {
      result += block.instructions.size();
    }
    return result;
  }
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
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveDropState = false;
  bool requiresActiveCleanup = false;
  std::vector<MirClassFieldLifecycle> fields;
  MirBody fieldInitializers;
  MirBody staticFieldInitializers;
  std::vector<MirFieldDrop> fieldDropOrder;
};

struct MirCallableSignature {
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
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
  bool nonEscaping = true;
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
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> captureTypes;
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

  [[nodiscard]] std::size_t blockCount() const {
    std::size_t result = moduleBody.blocks.size();
    for (const MirClassInstance &instance : classes) {
      result += instance.fieldInitializers.blocks.size();
      result += instance.staticFieldInitializers.blocks.size();
    }
    for (const MirFunctionInstance &instance : functions) {
      result += instance.body.blocks.size();
    }
    for (const MirConstructorInstance &instance : constructors) {
      result += instance.body.blocks.size();
    }
    for (const MirDestructorInstance &instance : destructors) {
      result += instance.body.blocks.size();
    }
    for (const MirLambdaInstance &instance : lambdas) {
      result += instance.body.blocks.size();
    }
    return result;
  }

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

class MirBodyLowerer {
public:
  MirBodyLowerer(const HirProgram &program, const HirBody &source,
                 MirBodyKind kind, SemanticType returnType,
                 bool implicitZeroReturn = false,
                 const HirFunctionInstance *function = nullptr)
      : program(program), source(source),
        implicitZeroReturn(implicitZeroReturn), function(function) {
    output.kind = kind;
    output.placeDomain = source.placeDomain;
    output.returnType = std::move(returnType);
    for (const HirValue &value : source.values) {
      values.emplace(value.id, &value);
    }
    for (const HirStatement &statement : source.statements) {
      statements.emplace(statement.id, &statement);
    }
    for (const HirBinding &binding : source.bindings) {
      bindings.emplace(binding.id, &binding);
      if (binding.info.symbol != 0) {
        localSymbols.insert_or_assign(binding.info.symbol, binding.id);
      }
    }
  }

  [[nodiscard]] MirBody
  lower(const std::vector<HirValueId> &prologueValues = {},
        const std::vector<HirConstructorInitializer> *initializers = nullptr) {
    output.entry = appendBlock();
    current = output.entry;
    scopes.push_back({});
    seedParameterDrops();
    seedEntryLoans();
    seedReturnBorrow();

    (void)prologueValues;
    if (initializers != nullptr) {
      for (std::size_t initializerIndex = 0;
           initializerIndex < initializers->size(); ++initializerIndex) {
        const HirConstructorInitializer &initializer =
            (*initializers)[initializerIndex];
        const std::vector<Scope> incomingScopes = scopes;
        const std::vector<TemporaryDrop> incomingTemporaryDrops =
            temporaryDrops;
        for (const HirValueId argument : initializer.arguments) {
          (void)emitValue(argument);
        }
        if (initializer.storesReference && initializer.arguments.size() == 1) {
          markStoredBorrow(initializer.arguments.front(), initializer.field,
                           initializer.borrowAccess);
        }
        const HirConstructorInstance *target =
            initializer.constructorTarget ? program.findConstructorInstance(
                                                *initializer.constructorTarget)
                                          : nullptr;
        for (std::size_t index = 0; index < initializer.arguments.size();
             ++index) {
          const bool referenceParameter =
              target != nullptr && index < target->parameterTypes.size() &&
              target->parameterTypes[index].kind == SemanticType::Reference;
          if (!initializer.storesReference && !referenceParameter) {
            emitTemporaryTransfer(initializer.arguments[index]);
          }
        }
        endFullExpressionLoans(incomingScopes);
        emitTemporaryDrops(incomingTemporaryDrops, 0, 0, initializerIndex + 1);
      }
    }
    lowerStatements(source.roots);
    if (!terminated()) {
      emitScopeExit(0);
      if (output.kind == MirBodyKind::Module ||
          output.kind == MirBodyKind::FieldInitializers ||
          output.kind == MirBodyKind::StaticFieldInitializers) {
        terminate({.kind = MirTerminatorKind::Exit});
      } else if (implicitZeroReturn) {
        terminate({.kind = MirTerminatorKind::Return,
                   .value = MirOperand{.kind = MirOperandKind::Constant,
                                       .literal = Literal{std::uint64_t{0}},
                                       .type = SemanticType::Int32}});
      } else if (output.returnType.kind == SemanticType::Void) {
        terminate({.kind = MirTerminatorKind::Return});
      } else {
        terminate({.kind = MirTerminatorKind::Unreachable});
      }
    }
    rebuildMirReachability(output);
    valid = rebuildMirValueUses(output) && valid;
    valid = validateSourceProvenance() && valid;
    valid = verifyMirBody(output).valid() && valid;
    return std::move(output);
  }

  [[nodiscard]] bool isValid() const { return valid; }

private:
  struct Scope {
    std::vector<MirPlaceId> drops;
    std::vector<MirLoanId> loans;
  };

  struct TemporaryDrop {
    MirDropObligationId obligation = 0;
    bool conditional = false;

    friend bool operator==(const TemporaryDrop &,
                           const TemporaryDrop &) = default;
  };

  [[nodiscard]] static bool sameScopeState(const std::vector<Scope> &left,
                                           const std::vector<Scope> &right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const Scope &lhs, const Scope &rhs) {
                        return lhs.drops == rhs.drops && lhs.loans == rhs.loans;
                      });
  }

  [[nodiscard]] static std::unordered_map<HirBindingId, MirLoanId>
  projectOuterBindingLoans(
      const std::unordered_map<HirBindingId, MirLoanId> &incoming,
      const std::unordered_map<HirBindingId, MirLoanId> &outgoing) {
    std::unordered_map<HirBindingId, MirLoanId> projected;
    projected.reserve(incoming.size());
    for (const auto &[binding, _] : incoming) {
      if (const auto found = outgoing.find(binding); found != outgoing.end()) {
        projected.emplace(binding, found->second);
      }
    }
    return projected;
  }

  struct BreakContext {
    MirBlockId target = 0;
    std::size_t keepScopes = 0;
    std::vector<SemanticLoanId> exitLoans;
  };

  struct ContinueContext {
    MirBlockId target = 0;
    std::size_t keepScopes = 0;
  };

  [[nodiscard]] const HirValue *findValue(HirValueId id) const {
    const auto found = values.find(id);
    return found == values.end() ? nullptr : found->second;
  }

  [[nodiscard]] const HirStatement *findStatement(HirStatementId id) const {
    const auto found = statements.find(id);
    return found == statements.end() ? nullptr : found->second;
  }

  [[nodiscard]] const HirBinding *findBinding(HirBindingId id) const {
    const auto found = bindings.find(id);
    return found == bindings.end() ? nullptr : found->second;
  }

  [[nodiscard]] static MirDropType lowerDropType(const HirDropType &type) {
    return {.type = type.type,
            .classInstance = type.classInstance,
            .lambdaInstance = type.lambdaInstance,
            .destructor = type.destructor,
            .requiresActiveCleanup = type.requiresActiveCleanup};
  }

  [[nodiscard]] MirDropObligationId
  ensureDropObligation(const HirDropObligation &obligation,
                       MirPlaceId place = 0) {
    if (const auto found = dropObligations.find(obligation.id);
        found != dropObligations.end()) {
      MirDropObligation &existing = output.dropObligations[found->second - 1];
      if (place != 0) {
        if (existing.place != 0 && existing.place != place) {
          valid = false;
        } else {
          existing.place = place;
        }
      }
      return found->second;
    }

    if (place == 0) {
      if (obligation.kind == HirDropObligationKind::Binding) {
        place = placeForBinding(obligation.binding);
      } else {
        const HirValue *value = findValue(obligation.value);
        if (value != nullptr) {
          place = appendPlace({.root = MirPlaceRootKind::Value,
                               .value = mirValueFor(*value),
                               .type = value->info.type,
                               .access = AccessMode::Mutable,
                               .traits = value->info.traits,
                               .sourceValue = value->id});
        }
      }
    }
    if (place == 0) {
      valid = false;
      return 0;
    }
    const MirDropObligationId id = output.dropObligations.size() + 1;
    output.dropObligations.push_back(
        {.id = id,
         .hirObligation = obligation.id,
         .constructionOrder = obligation.constructionOrder,
         .kind = obligation.kind == HirDropObligationKind::Binding
                     ? MirDropObligationKind::Binding
                     : MirDropObligationKind::Value,
         .place = place,
         .binding = obligation.binding,
         .value = obligation.value,
         .hirFullExpression = obligation.fullExpression,
         .fullExpression = fullExpressionIds.contains(obligation.fullExpression)
                               ? fullExpressionIds.at(obligation.fullExpression)
                               : 0,
         .dropType = lowerDropType(obligation.dropType),
         .initiallyActive = obligation.initiallyActive});
    dropObligations.emplace(obligation.id, id);
    return id;
  }

  [[nodiscard]] MirDropObligationId
  dropObligationForBinding(HirBindingId bindingId) {
    const HirBinding *binding = findBinding(bindingId);
    if (binding == nullptr || !binding->dropObligation) {
      return 0;
    }
    const HirDropObligation *obligation =
        source.findDropObligation(*binding->dropObligation);
    return obligation == nullptr ? 0 : ensureDropObligation(*obligation);
  }

  [[nodiscard]] MirDropObligationId
  dropObligationForValue(HirValueId valueId, MirPlaceId place = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr || !value->dropObligation) {
      return 0;
    }
    const HirDropObligation *obligation =
        source.findDropObligation(*value->dropObligation);
    return obligation == nullptr ? 0 : ensureDropObligation(*obligation, place);
  }

  [[nodiscard]] MirDropObligationId
  exactBindingDropObligation(HirValueId valueId) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return 0;
    }
    if (value->dropObligation) {
      return dropObligationForValue(valueId);
    }
    const MirPlaceId place = placeForValue(valueId);
    const MirPlace *resolved = output.findPlace(place);
    if (resolved == nullptr || resolved->root != MirPlaceRootKind::Binding ||
        !resolved->projections.empty()) {
      return 0;
    }
    return dropObligationForBinding(resolved->binding);
  }

  [[nodiscard]] auto temporaryDrop(MirDropObligationId obligation) {
    return std::find_if(temporaryDrops.begin(), temporaryDrops.end(),
                        [&](const TemporaryDrop &candidate) {
                          return candidate.obligation == obligation;
                        });
  }

  [[nodiscard]] auto temporaryDrop(MirDropObligationId obligation) const {
    return std::find_if(temporaryDrops.begin(), temporaryDrops.end(),
                        [&](const TemporaryDrop &candidate) {
                          return candidate.obligation == obligation;
                        });
  }

  [[nodiscard]] bool temporaryIsActive(MirDropObligationId obligation) const {
    return obligation != 0 && temporaryDrop(obligation) != temporaryDrops.end();
  }

  void registerTemporary(MirDropObligationId obligation,
                         bool conditional = false) {
    if (obligation == 0) {
      return;
    }
    if (auto found = temporaryDrop(obligation); found != temporaryDrops.end()) {
      found->conditional = found->conditional || conditional;
      return;
    }
    temporaryDrops.push_back(
        {.obligation = obligation, .conditional = conditional});
  }

  [[nodiscard]] bool removeTemporary(MirDropObligationId obligation) {
    const auto found = temporaryDrop(obligation);
    if (found == temporaryDrops.end()) {
      return false;
    }
    temporaryDrops.erase(found);
    return true;
  }

  [[nodiscard]] std::vector<TemporaryDrop>
  mergeTemporaryDrops(const std::vector<TemporaryDrop> &left,
                      const std::vector<TemporaryDrop> &right) {
    std::vector<TemporaryDrop> result = left;
    for (TemporaryDrop &candidate : result) {
      const auto found = std::find_if(
          right.begin(), right.end(), [&](const TemporaryDrop &other) {
            return other.obligation == candidate.obligation;
          });
      candidate.conditional =
          candidate.conditional || found == right.end() || found->conditional;
    }
    for (const TemporaryDrop &candidate : right) {
      if (std::none_of(result.begin(), result.end(),
                       [&](const TemporaryDrop &other) {
                         return other.obligation == candidate.obligation;
                       })) {
        result.push_back(
            {.obligation = candidate.obligation, .conditional = true});
      }
    }
    for (const TemporaryDrop &candidate : result) {
      if (!candidate.conditional) {
        continue;
      }
      MirDropObligation *obligation =
          candidate.obligation == 0 ||
                  candidate.obligation > output.dropObligations.size()
              ? nullptr
              : &output.dropObligations[candidate.obligation - 1];
      MirPlace *place = obligation == nullptr || obligation->place == 0 ||
                                obligation->place > output.places.size()
                            ? nullptr
                            : &output.places[obligation->place - 1];
      if (place == nullptr) {
        valid = false;
        continue;
      }
      if (place->root == MirPlaceRootKind::Value) {
        const MirValueId producedValue = place->value;
        const MirValue *value = output.findValue(producedValue);
        MirBlock *definitionBlock =
            value == nullptr || value->definitionBlock == 0 ||
                    value->definitionBlock > output.blocks.size()
                ? nullptr
                : &output.blocks[value->definitionBlock - 1];
        const auto definition =
            definitionBlock == nullptr
                ? std::vector<MirInstruction>::iterator{}
                : std::find_if(definitionBlock->instructions.begin(),
                               definitionBlock->instructions.end(),
                               [&](const MirInstruction &instruction) {
                                 return instruction.id == value->definition;
                               });
        const bool materializingKind =
            definitionBlock != nullptr &&
            definition != definitionBlock->instructions.end() &&
            (definition->kind == MirInstructionKind::Compute ||
             definition->kind == MirInstructionKind::Move ||
             definition->kind == MirInstructionKind::Call ||
             definition->kind == MirInstructionKind::Construct);
        const bool initializesObligation =
            materializingKind && definition->result == producedValue &&
            std::any_of(
                definition->lifecycle.begin(), definition->lifecycle.end(),
                [&](const MirLifecycleEvent &event) {
                  return event.target == candidate.obligation &&
                         (event.kind == MirLifecycleEventKind::Initialize ||
                          event.kind == MirLifecycleEventKind::Move ||
                          event.kind == MirLifecycleEventKind::Reparent);
                });
        if (!initializesObligation ||
            (definition->destination &&
             *definition->destination != obligation->place)) {
          valid = false;
          continue;
        }
        // A branch-only owning value needs executable storage that remains
        // nameable after the CFG merge. Retarget its existing producer to the
        // hidden result slot; this is direct materialization, not an extra
        // observable move.
        definition->destination = obligation->place;
        place->root = MirPlaceRootKind::Temporary;
        place->temporary = nextTemporary++;
        place->value = 0;
      }
    }
    std::stable_sort(
        result.begin(), result.end(),
        [&](const TemporaryDrop &leftDrop, const TemporaryDrop &rightDrop) {
          const MirDropObligation *left =
              output.findDropObligation(leftDrop.obligation);
          const MirDropObligation *right =
              output.findDropObligation(rightDrop.obligation);
          return left != nullptr && right != nullptr &&
                 left->constructionOrder < right->constructionOrder;
        });
    return result;
  }

  void appendLifecycle(MirInstruction &instruction, MirLifecycleEvent event) {
    instruction.lifecycle.push_back(event);
  }

  void appendReparentOrTypedTransfer(MirInstruction &instruction,
                                     MirDropObligationId source,
                                     MirDropObligationId target) {
    const MirDropObligation *sourceDrop = output.findDropObligation(source);
    const MirDropObligation *targetDrop = output.findDropObligation(target);
    if (sourceDrop == nullptr || targetDrop == nullptr) {
      valid = false;
      return;
    }
    if (sourceDrop->dropType.type == targetDrop->dropType.type) {
      appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Reparent,
                                    .source = source,
                                    .target = target});
      return;
    }
    // A conversion into a distinct owning wrapper transfers the source object
    // and begins a different typed lifetime. It is not a same-type reparenting
    // of one object identity.
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::TransferOut,
                                  .source = source});
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Initialize,
                                  .target = target});
  }

  [[nodiscard]] MirBlockId appendBlock() {
    const MirBlockId id = output.blocks.size() + 1;
    output.blocks.push_back({.id = id});
    return id;
  }

  [[nodiscard]] MirBlock *currentBlock() {
    return current == 0 || current > output.blocks.size()
               ? nullptr
               : &output.blocks[current - 1];
  }

  [[nodiscard]] bool terminated() const {
    return current == 0 || current > output.blocks.size() ||
           output.blocks[current - 1].terminator.kind !=
               MirTerminatorKind::None;
  }

  void terminate(MirTerminator terminator) {
    if (MirBlock *block = currentBlock();
        block != nullptr && block->terminator.kind == MirTerminatorKind::None) {
      block->terminator = std::move(terminator);
    }
  }

  MirInstructionId appendInstruction(MirInstruction instruction) {
    MirBlock *block = currentBlock();
    if (block == nullptr || terminated()) {
      return 0;
    }
    if (instruction.hirValue != 0) {
      if (const HirValue *source = findValue(instruction.hirValue)) {
        if (source->unsafeOperation != UnsafeOperationKind::None) {
          instruction.unsafeOperation = source->unsafeOperation;
        }
        if (source->ownership) {
          instruction.ownership = source->ownership;
        }
      }
    }
    const auto rawPlace = [&](MirPlaceId id) {
      const MirPlace *place = output.findPlace(id);
      return place != nullptr &&
             std::any_of(place->projections.begin(), place->projections.end(),
                         [](const MirPlaceProjection &projection) {
                           return projection.kind ==
                                      MirProjectionKind::RawDereference ||
                                  projection.kind ==
                                      MirProjectionKind::RawIndex;
                         });
    };
    instruction.rawMemoryAccess =
        (instruction.destination && rawPlace(*instruction.destination)) ||
        (instruction.receiver && instruction.receiver->place != 0 &&
         rawPlace(instruction.receiver->place)) ||
        std::any_of(instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      return operand.kind != MirOperandKind::Address &&
                             operand.place != 0 && rawPlace(operand.place);
                    });
    instruction.id = nextInstruction++;
    const MirInstructionId id = instruction.id;
    if (instruction.result) {
      if (*instruction.result == 0 ||
          *instruction.result > output.values.size()) {
        valid = false;
      } else {
        MirValue &result = output.values[*instruction.result - 1];
        if (result.definition != 0) {
          valid = false;
        } else {
          result.definitionBlock = block->id;
          result.definition = id;
        }
      }
    }
    block->instructions.push_back(std::move(instruction));
    return id;
  }

  [[nodiscard]] bool
  temporaryConditional(MirDropObligationId obligation) const {
    const auto found = temporaryDrop(obligation);
    return found != temporaryDrops.end() && found->conditional;
  }

  void transferTemporaryOut(MirInstruction &instruction,
                            HirValueId sourceValue) {
    const MirDropObligationId sourceObligation =
        dropObligationForValue(sourceValue);
    if (!temporaryIsActive(sourceObligation)) {
      return;
    }
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::TransferOut,
                                  .source = sourceObligation});
    (void)removeTemporary(sourceObligation);
  }

  [[nodiscard]] MirDropObligationId
  initializeValueLifecycle(MirInstruction &instruction, const HirValue &value,
                           MirPlaceId place = 0) {
    const MirDropObligationId target = dropObligationForValue(value.id, place);
    if (target == 0) {
      return 0;
    }

    MirDropObligationId sourceObligation = 0;
    MirLifecycleEventKind transition = MirLifecycleEventKind::Initialize;
    bool sourceRemainsTemporary = false;
    if (value.kind == HirValueKind::Move && !value.operands.empty()) {
      sourceObligation = exactBindingDropObligation(value.operands.front());
      transition = MirLifecycleEventKind::Move;
      sourceRemainsTemporary = temporaryIsActive(sourceObligation);
    } else if ((value.kind == HirValueKind::Grouping ||
                value.kind == HirValueKind::Conversion ||
                (value.kind == HirValueKind::Binary &&
                 value.operation == TokenKind::COMMA)) &&
               !value.operands.empty()) {
      sourceObligation = dropObligationForValue(value.operands.back());
      if (temporaryIsActive(sourceObligation)) {
        transition = MirLifecycleEventKind::Reparent;
      } else {
        sourceObligation = 0;
      }
    }

    bool conditional = false;
    if (sourceObligation != 0) {
      conditional = temporaryConditional(sourceObligation);
      if (transition == MirLifecycleEventKind::Reparent) {
        appendReparentOrTypedTransfer(instruction, sourceObligation, target);
      } else {
        appendLifecycle(
            instruction,
            {.kind = transition, .source = sourceObligation, .target = target});
      }
      if (!sourceRemainsTemporary) {
        (void)removeTemporary(sourceObligation);
      }
    } else {
      appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Initialize,
                                    .target = target});
    }
    registerTemporary(target, conditional);
    return target;
  }

  [[nodiscard]] HirFullExpressionId
  publishFullExpression(const HirFullExpression &expression) {
    if (expression.id == 0 || expression.roots.empty()) {
      valid = false;
      return 0;
    }
    if (const auto found = fullExpressionIds.find(expression.id);
        found != fullExpressionIds.end()) {
      return found->second;
    }
    const HirFullExpressionId id = output.fullExpressions.size() + 1;
    output.fullExpressions.push_back(
        {.id = id,
         .hirExpression = expression.id,
         .statement = expression.statement,
         .constructorInitializer = expression.constructorInitializer,
         .roots = expression.roots});
    fullExpressionIds.emplace(expression.id, id);
    for (MirDropObligation &obligation : output.dropObligations) {
      if (obligation.hirFullExpression == expression.id) {
        obligation.fullExpression = id;
      }
    }
    return id;
  }

  void emitTemporaryDrops(const std::vector<TemporaryDrop> &baseline,
                          HirValueId hirValue = 0,
                          HirStatementId hirStatement = 0,
                          std::size_t constructorInitializer = 0) {
    for (auto candidate = temporaryDrops.rbegin();
         candidate != temporaryDrops.rend(); ++candidate) {
      if (std::any_of(baseline.begin(), baseline.end(),
                      [&](const TemporaryDrop &existing) {
                        return existing.obligation == candidate->obligation;
                      })) {
        continue;
      }
      const MirDropObligation *obligation =
          output.findDropObligation(candidate->obligation);
      if (obligation == nullptr || obligation->place == 0) {
        valid = false;
        continue;
      }
      const MirPlace *place = output.findPlace(obligation->place);
      if (place == nullptr) {
        valid = false;
        continue;
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::Drop,
           .hirValue = hirValue,
           .hirStatement = hirStatement,
           .destination = obligation->place,
           .info = ExpressionInfo{.type = obligation->dropType.type,
                                  .category = ValueCategory::Place,
                                  .access = AccessMode::Mutable,
                                  .traits = place->traits},
           .lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                          .source = candidate->obligation,
                          .conditional = candidate->conditional}}});
    }
    temporaryDrops = baseline;
    const auto boundary = std::find_if(
        source.fullExpressions.begin(), source.fullExpressions.end(),
        [&](const HirFullExpression &expression) {
          if (constructorInitializer != 0) {
            return expression.constructorInitializer == constructorInitializer;
          }
          return hirStatement != 0 && expression.statement == hirStatement &&
                 hirValue != 0 &&
                 std::find(expression.roots.begin(), expression.roots.end(),
                           hirValue) != expression.roots.end();
        });
    if (boundary == source.fullExpressions.end()) {
      return;
    }
    const bool ambiguous = std::any_of(
        std::next(boundary), source.fullExpressions.end(),
        [&](const HirFullExpression &expression) {
          if (constructorInitializer != 0) {
            return expression.constructorInitializer == constructorInitializer;
          }
          return expression.statement == hirStatement &&
                 std::find(expression.roots.begin(), expression.roots.end(),
                           hirValue) != expression.roots.end();
        });
    if (ambiguous) {
      valid = false;
      return;
    }
    const HirFullExpressionId boundaryId = publishFullExpression(*boundary);
    if (boundaryId == 0) {
      return;
    }
    (void)appendInstruction({.kind = MirInstructionKind::Lifecycle,
                             .hirValue = hirValue,
                             .hirStatement = hirStatement,
                             .fullExpressionEnd = boundaryId});
  }

  void emitTemporaryTransfer(HirValueId value, HirStatementId statement = 0) {
    MirInstruction transfer{.kind = MirInstructionKind::Lifecycle,
                            .hirValue = value,
                            .hirStatement = statement};
    transferTemporaryOut(transfer, value);
    if (!transfer.lifecycle.empty()) {
      (void)appendInstruction(std::move(transfer));
    }
  }

  MirValueId appendValue(HirValueId sourceValue, ExpressionInfo info) {
    const MirValueId id = output.values.size() + 1;
    output.values.push_back(
        {.id = id, .sourceValue = sourceValue, .info = std::move(info)});
    return id;
  }

  [[nodiscard]] MirValueId mirValueFor(const HirValue &value) {
    if (const auto found = loweredValues.find(value.id);
        found != loweredValues.end()) {
      return found->second;
    }
    const MirValueId id = appendValue(value.id, value.info);
    loweredValues.emplace(value.id, id);
    return id;
  }

  [[nodiscard]] MirValueId mirValueFor(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return 0;
    }
    return mirValueFor(*value);
  }

  [[nodiscard]] std::optional<MirValueId> resultFor(const HirValue &value) {
    return value.info.type.kind == SemanticType::Void
               ? std::nullopt
               : std::optional<MirValueId>{mirValueFor(value)};
  }

  MirPlaceId appendPlace(MirPlace place) {
    place.id = output.places.size() + 1;
    const MirPlaceId id = place.id;
    output.places.push_back(std::move(place));
    return id;
  }

  void attachPlaceIdentity(MirPlaceId id, const HirValue &value) {
    MirPlace *place =
        id == 0 || id > output.places.size() ? nullptr : &output.places[id - 1];
    if (place == nullptr || !value.place) {
      return;
    }
    place->key = value.place;
    const auto semanticIndex = std::find_if(
        value.place->projections.rbegin(), value.place->projections.rend(),
        [](const PlaceProjection &projection) {
          return projection.kind == PlaceProjectionKind::ConstantIndex ||
                 projection.kind == PlaceProjectionKind::DynamicIndex;
        });
    const auto mirIndex =
        std::find_if(place->projections.rbegin(), place->projections.rend(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::Index;
                     });
    if (semanticIndex == value.place->projections.rend() ||
        mirIndex == place->projections.rend()) {
      return;
    }
    if (semanticIndex->kind == PlaceProjectionKind::ConstantIndex) {
      mirIndex->constantIndex = semanticIndex->index;
      mirIndex->selection = 0;
    } else {
      mirIndex->constantIndex.reset();
      mirIndex->selection = semanticIndex->selection;
    }
  }

  [[nodiscard]] MirPlaceId placeForBinding(HirBindingId id) {
    if (const auto found = bindingPlaces.find(id);
        found != bindingPlaces.end()) {
      return found->second;
    }
    const HirBinding *binding = findBinding(id);
    if (binding == nullptr) {
      return 0;
    }
    std::optional<PlaceKey> key;
    if (binding->info.symbol != 0) {
      key =
          PlaceKey{.domain = output.placeDomain, .root = binding->info.symbol};
    }
    const MirPlaceId place =
        appendPlace({.root = MirPlaceRootKind::Binding,
                     .binding = id,
                     .symbol = binding->info.symbol,
                     .type = binding->info.type,
                     .access = binding->info.access,
                     .traits = binding->info.traits,
                     .key = std::move(key),
                     .initiallyAvailable = binding->parameter != nullptr});
    bindingPlaces.emplace(id, place);
    return place;
  }

  [[nodiscard]] MirPlaceId placeForLoan(const HirLoan &loan) {
    if (const auto found = semanticLoanPlaces.find(loan.semanticLoan);
        found != semanticLoanPlaces.end()) {
      return found->second;
    }

    MirPlace lowered;
    if (loan.place.receiver) {
      lowered.root = MirPlaceRootKind::This;
      lowered.access = loan.access;
    } else if (const auto local = localSymbols.find(loan.place.root);
               local != localSymbols.end()) {
      const MirPlaceId base = placeForBinding(local->second);
      const MirPlace *place = output.findPlace(base);
      if (place == nullptr) {
        valid = false;
        return 0;
      }
      if (loan.place.projections.empty()) {
        semanticLoanPlaces.emplace(loan.semanticLoan, base);
        return base;
      }
      lowered = *place;
      lowered.id = 0;
      lowered.access = loan.access;
    } else if (loan.place.root != 0) {
      lowered.root = MirPlaceRootKind::Symbol;
      lowered.symbol = loan.place.root;
      lowered.access = loan.access;
    } else {
      valid = false;
      return 0;
    }
    lowered.key = loan.place;

    for (const SemanticLoanPlaceProjection &projection :
         loan.place.projections) {
      switch (projection.kind) {
      case SemanticLoanPlaceProjectionKind::Field:
        if (projection.field == 0) {
          valid = false;
          return 0;
        }
        lowered.projections.push_back(
            {.kind = MirProjectionKind::Field, .field = projection.field});
        break;
      case SemanticLoanPlaceProjectionKind::Dereference:
        lowered.projections.push_back({.kind = MirProjectionKind::Dereference});
        break;
      case SemanticLoanPlaceProjectionKind::ConstantIndex:
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .constantIndex = projection.index});
        break;
      case SemanticLoanPlaceProjectionKind::DynamicIndex:
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .selection = projection.selection});
        break;
      }
    }

    const MirPlaceId place = appendPlace(std::move(lowered));
    semanticLoanPlaces.emplace(loan.semanticLoan, place);
    return place;
  }

  [[nodiscard]] const HirLoan *loanForBinding(HirBindingId binding) const {
    const HirBinding *resolved = findBinding(binding);
    return resolved == nullptr || resolved->info.retainedLoan == 0
               ? nullptr
               : source.findLoan(resolved->info.retainedLoan);
  }

  void recordLoanIdentity(MirLoanId id, const HirLoan &loan,
                          HirBindingId binding = 0) {
    if (id == 0 || id > output.loans.size()) {
      valid = false;
      return;
    }
    if (const auto found = semanticLoans.find(loan.semanticLoan);
        found != semanticLoans.end() && found->second != id) {
      valid = false;
      return;
    }
    MirLoan &lowered = output.loans[id - 1];
    if (lowered.semanticLoan != 0 &&
        lowered.semanticLoan != loan.semanticLoan) {
      valid = false;
      return;
    }
    lowered.semanticLoan = loan.semanticLoan;
    if (lowered.access == AccessMode::ReadOnly &&
        loan.access == AccessMode::Mutable) {
      valid = false;
      return;
    }
    for (const HirBindingId carrier : loan.carriers) {
      if (std::find(lowered.carriers.begin(), lowered.carriers.end(),
                    carrier) == lowered.carriers.end()) {
        lowered.carriers.push_back(carrier);
      }
    }
    if (binding != 0 &&
        std::find(lowered.carriers.begin(), lowered.carriers.end(), binding) ==
            lowered.carriers.end()) {
      lowered.carriers.push_back(binding);
    }
    semanticLoans.insert_or_assign(loan.semanticLoan, id);
  }

  [[nodiscard]] MirPlaceId clonePlace(MirPlaceId base, const HirValue &value) {
    const MirPlace *sourcePlace = output.findPlace(base);
    if (sourcePlace == nullptr) {
      return 0;
    }
    MirPlace result = *sourcePlace;
    result.id = 0;
    result.type = value.info.type;
    result.access = value.info.access;
    result.traits = value.info.traits;
    result.sourceValue = value.id;
    return appendPlace(std::move(result));
  }

  [[nodiscard]] MirPlaceId valueRootPlace(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Value,
                        .value = mirValueFor(value),
                        .type = value.info.type,
                        .access = value.info.access,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  [[nodiscard]] MirPlaceId rawPointerPlace(HirValueId pointer,
                                           const HirValue &value,
                                           MirProjectionKind projection,
                                           MirValueId index = 0) {
    (void)valueOperand(pointer);
    MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                    .value = mirValueFor(pointer),
                                    .type = value.info.type,
                                    .access = value.info.access,
                                    .traits = value.info.traits,
                                    .sourceValue = value.id});
    output.places[place - 1].projections.push_back(
        {.kind = projection, .index = index});
    return place;
  }

  [[nodiscard]] MirPlaceId placeForValue(HirValueId id) {
    if (const auto found = valuePlaces.find(id); found != valuePlaces.end()) {
      return found->second;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      return 0;
    }

    MirPlaceId place = 0;
    switch (value->kind) {
    case HirValueKind::Variable:
    case HirValueKind::QualifiedName: {
      const auto local = localSymbols.find(value->symbol);
      if (local != localSymbols.end()) {
        place = placeForBinding(local->second);
        const HirBinding *binding = findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          place = clonePlace(place, *value);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
      } else {
        place = appendPlace({.root = MirPlaceRootKind::Symbol,
                             .symbol = value->symbol,
                             .type = value->info.type,
                             .access = value->info.access,
                             .traits = value->info.traits,
                             .sourceValue = value->id});
      }
      break;
    }
    case HirValueKind::This:
      place = appendPlace({.root = MirPlaceRootKind::This,
                           .type = value->info.type,
                           .access = value->info.access,
                           .traits = value->info.traits,
                           .sourceValue = value->id});
      break;
    case HirValueKind::Grouping:
      if (!value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.front()), *value);
      }
      break;
    case HirValueKind::Binary:
      if (value->operation == TokenKind::COMMA && !value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.back()), *value);
      }
      break;
    case HirValueKind::MemberAccess:
      if (value->functionTarget) {
        place = loanOrValuePlace(*value);
        if (place != 0 && value->info.category == ValueCategory::Place &&
            value->symbol != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value->symbol});
        }
      } else if (!value->operands.empty() &&
                 value->unsafeOperation == UnsafeOperationKind::RawMember) {
        place = rawPointerPlace(value->operands.front(), *value,
                                MirProjectionKind::RawDereference);
        output.places[place - 1].projections.push_back(
            {.kind = MirProjectionKind::Field, .field = value->symbol});
      } else if (!value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.front()), *value);
        if (place != 0) {
          if (value->operation == TokenKind::ARROW) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value->symbol});
          if (value->storedReferenceAccess) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
        }
      }
      break;
    case HirValueKind::Index:
      if (value->functionTarget) {
        place = loanOrValuePlace(*value);
      } else if (value->operands.size() >= 2 &&
                 value->unsafeOperation == UnsafeOperationKind::RawIndex) {
        (void)valueOperand(value->operands[0]);
        (void)valueOperand(value->operands[1]);
        place = rawPointerPlace(value->operands[0], *value,
                                MirProjectionKind::RawIndex,
                                mirValueFor(value->operands[1]));
      } else if (value->operands.size() >= 2) {
        place = clonePlace(placeForValue(value->operands[0]), *value);
        if (place != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Index,
               .index = mirValueFor(value->operands[1])});
        }
      }
      break;
    case HirValueKind::Call:
      place = loanOrValuePlace(*value);
      break;
    case HirValueKind::Unary:
      if (value->operation == TokenKind::STAR && !value->operands.empty() &&
          value->unsafeOperation == UnsafeOperationKind::RawDereference) {
        place = rawPointerPlace(value->operands.front(), *value,
                                MirProjectionKind::RawDereference);
      } else {
        place = loanOrValuePlace(*value);
        if (place == 0 && value->operation == TokenKind::STAR &&
            !value->operands.empty()) {
          place = valueRootPlace(*value);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
      }
      break;
    default:
      if (value->info.category == ValueCategory::Place) {
        place = valueRootPlace(*value);
      }
      break;
    }
    if (place == 0 && value->info.category == ValueCategory::Place) {
      place = valueRootPlace(*value);
    }
    if (place != 0) {
      attachPlaceIdentity(place, *value);
      valuePlaces.insert_or_assign(id, place);
    }
    return place;
  }

  [[nodiscard]] MirPlaceId loanOrValuePlace(const HirValue &value) {
    const auto found = valueLoans.find(value.id);
    if (found != valueLoans.end()) {
      return appendPlace({.root = MirPlaceRootKind::Loan,
                          .loan = found->second,
                          .type = value.info.type,
                          .access = value.info.access,
                          .traits = value.info.traits,
                          .sourceValue = value.id});
    }
    return value.info.category == ValueCategory::Place ? valueRootPlace(value)
                                                       : 0;
  }

  [[nodiscard]] MirPlaceId destinationFor(const HirValue &value) {
    switch (value.kind) {
    case HirValueKind::Assignment: {
      const auto local = localSymbols.find(value.symbol);
      if (local != localSymbols.end()) {
        const MirPlaceId bindingPlace = placeForBinding(local->second);
        const HirBinding *binding = findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          const MirPlaceId referent = clonePlace(bindingPlace, value);
          if (referent != 0) {
            output.places[referent - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          return referent;
        }
        return bindingPlace;
      }
      return appendPlace({.root = MirPlaceRootKind::Symbol,
                          .symbol = value.symbol,
                          .type = value.info.type,
                          .access = value.info.access,
                          .traits = value.info.traits,
                          .sourceValue = value.id});
    }
    case HirValueKind::MemberSet:
      if (!value.operands.empty()) {
        MirPlaceId place =
            value.unsafeOperation == UnsafeOperationKind::RawMember
                ? rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawDereference)
                : clonePlace(placeForValue(value.operands[0]), value);
        if (place != 0) {
          if (value.operation == TokenKind::ARROW &&
              value.unsafeOperation != UnsafeOperationKind::RawMember) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value.symbol});
        }
        return place;
      }
      break;
    case HirValueKind::IndexSet:
      if (value.operands.size() >= 2) {
        MirPlaceId place = 0;
        if (value.unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value.operands[0]);
          (void)valueOperand(value.operands[1]);
          place = rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawIndex,
                                  mirValueFor(value.operands[1]));
        } else {
          place = clonePlace(placeForValue(value.operands[0]), value);
        }
        if (place != 0) {
          if (value.unsafeOperation != UnsafeOperationKind::RawIndex) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Index,
                 .index = mirValueFor(value.operands[1])});
          }
        }
        return place;
      }
      break;
    case HirValueKind::DereferenceSet:
      if (!value.operands.empty()) {
        MirPlaceId place =
            value.unsafeOperation == UnsafeOperationKind::RawDereference
                ? rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawDereference)
                : valueRootPlace(value);
        if (value.unsafeOperation != UnsafeOperationKind::RawDereference) {
          output.places[place - 1].value = mirValueFor(value.operands[0]);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
        return place;
      }
      break;
    default:
      break;
    }
    return 0;
  }

  [[nodiscard]] MirOperand valueOperand(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    if (emittedValues.contains(id)) {
      const MirLoanId loan = loanForValue(id);
      const MirLoan *borrow = output.findLoan(loan);
      if (value->info.category == ValueCategory::Place && borrow != nullptr &&
          borrow->kind == MirLoanKind::CallResult) {
        if (const auto found = materializedValues.find(id);
            found != materializedValues.end()) {
          return {.kind = MirOperandKind::Value,
                  .value = found->second,
                  .type = value->info.type};
        }
        ExpressionInfo loadedInfo = value->info;
        loadedInfo.category = ValueCategory::Value;
        loadedInfo.access = AccessMode::ReadOnly;
        const MirValueId loaded = appendValue(id, loadedInfo);
        const MirPlaceId place = placeForValue(id);
        (void)appendInstruction({.kind = MirInstructionKind::Load,
                                 .hirValue = id,
                                 .result = loaded,
                                 .operands = {{.kind = MirOperandKind::Copy,
                                               .place = place,
                                               .type = value->info.type}},
                                 .info = loadedInfo});
        materializedValues.emplace(id, loaded);
        endConsumedCallResultLoans({id}, *value);
        return {.kind = MirOperandKind::Value,
                .value = loaded,
                .type = value->info.type};
      }
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    if (value->kind == HirValueKind::Move && !value->operands.empty()) {
      emitPlaceDependencies(value->operands.front());
      MirPlaceId sourcePlace = placeForValue(value->operands.front());
      const HirValue *source = findValue(value->operands.front());
      if (source != nullptr && (source->kind == HirValueKind::Variable ||
                                source->kind == HirValueKind::QualifiedName)) {
        const auto local = localSymbols.find(source->symbol);
        const HirBinding *binding =
            local == localSymbols.end() ? nullptr : findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          sourcePlace = placeForBinding(binding->id);
        }
      }
      MirInstruction move{.kind = MirInstructionKind::Move,
                          .hirValue = value->id,
                          .result = resultFor(*value),
                          .operands = {{.kind = MirOperandKind::Move,
                                        .place = sourcePlace,
                                        .type = value->info.type}},
                          .intrinsic = value->intrinsic,
                          .info = value->info};
      (void)initializeValueLifecycle(move, *value);
      (void)appendInstruction(std::move(move));
      if (const MirLoanId loan = loanForValue(value->operands.front());
          loan != 0) {
        valueLoans.insert_or_assign(value->id, loan);
      }
      emittedValues.insert(value->id);
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind)) {
      emitPlaceDependencies(id);
      if (emittedValues.contains(id)) {
        return valueOperand(id);
      }
      const MirPlaceId place = placeForValue(id);
      (void)appendInstruction({.kind = MirInstructionKind::Load,
                               .hirValue = value->id,
                               .result = resultFor(*value),
                               .operands = {{.kind = MirOperandKind::Copy,
                                             .place = place,
                                             .type = value->info.type}},
                               .info = value->info});
      emittedValues.insert(id);
      endConsumedCallResultLoans(value->operands, *value);
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    emitValue(id);
    return {.kind = MirOperandKind::Value,
            .value = mirValueFor(*value),
            .type = value->info.type};
  }

  [[nodiscard]] MirOperand addressOperand(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    emitPlaceDependencies(id);
    const MirPlaceId place = placeForValue(id);
    if (place == 0) {
      valid = false;
    }
    return {.kind = MirOperandKind::Address,
            .place = place,
            .type = value->info.type};
  }

  [[nodiscard]] static bool isPlaceExpression(HirValueKind kind) {
    switch (kind) {
    case HirValueKind::Call:
    case HirValueKind::Grouping:
    case HirValueKind::Index:
    case HirValueKind::MemberAccess:
    case HirValueKind::QualifiedName:
    case HirValueKind::This:
    case HirValueKind::Unary:
    case HirValueKind::Variable:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool isCallLike(const HirValue &value) {
    return value.kind == HirValueKind::Call || value.functionTarget;
  }

  void emitPlaceDependencies(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }
    if (isCallLike(*value)) {
      emitValue(id);
      return;
    }
    switch (value->kind) {
    case HirValueKind::Binary:
      if (value->operation == TokenKind::COMMA && !value->operands.empty()) {
        for (auto operand = value->operands.begin();
             operand != std::prev(value->operands.end()); ++operand) {
          (void)valueOperand(*operand);
        }
        emitPlaceDependencies(value->operands.back());
      }
      break;
    case HirValueKind::Grouping:
      if (!value->operands.empty()) {
        emitPlaceDependencies(value->operands.front());
      }
      break;
    case HirValueKind::MemberAccess:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      }
      break;
    case HirValueKind::Index:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      }
      if (value->operands.size() >= 2) {
        (void)valueOperand(value->operands[1]);
      }
      break;
    case HirValueKind::Unary:
      if (value->operation == TokenKind::STAR && !value->operands.empty()) {
        (void)valueOperand(value->operands.front());
      }
      break;
    default:
      break;
    }
  }

  [[nodiscard]] MirLoanId loanForValue(HirValueId id) const {
    if (const auto direct = valueLoans.find(id); direct != valueLoans.end()) {
      return direct->second;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      return 0;
    }
    if (value->kind == HirValueKind::Variable ||
        value->kind == HirValueKind::QualifiedName) {
      const auto local = localSymbols.find(value->symbol);
      if (local != localSymbols.end()) {
        const auto loan = bindingLoans.find(local->second);
        return loan == bindingLoans.end() ? 0 : loan->second;
      }
      return 0;
    }
    if ((value->kind == HirValueKind::Grouping ||
         value->kind == HirValueKind::Move) &&
        !value->operands.empty()) {
      return loanForValue(value->operands.front());
    }
    if (value->kind == HirValueKind::Binary &&
        value->operation == TokenKind::COMMA && !value->operands.empty()) {
      return loanForValue(value->operands.back());
    }
    if (value->kind == HirValueKind::MemberAccess &&
        value->storedReferenceAccess && !value->operands.empty()) {
      return loanForValue(value->operands.front());
    }
    return 0;
  }

  [[nodiscard]] MirPlaceId borrowedSourcePlace(HirValueId id) const {
    const MirLoanId loan = loanForValue(id);
    const MirLoan *resolved = output.findLoan(loan);
    return resolved == nullptr ? 0 : resolved->source;
  }

  [[nodiscard]] MirOperand argumentOperand(HirValueId id,
                                           const SemanticType &parameter) {
    if (parameter.kind != SemanticType::Reference) {
      return valueOperand(id);
    }
    emitPlaceDependencies(id);
    if (const MirLoanId existing = loanForValue(id); existing != 0) {
      return {
          .kind = MirOperandKind::Loan, .loan = existing, .type = parameter};
    }
    const MirPlaceId place = placeForValue(id);
    return {.kind = parameter.referenceAccess == AccessMode::Mutable
                        ? MirOperandKind::BorrowWrite
                        : MirOperandKind::BorrowRead,
            .place = place,
            .type = parameter};
  }

  [[nodiscard]] std::vector<HirValueId>
  callArgumentValues(const HirValue &value) const {
    std::size_t argumentCount = value.parameterTypes.size();
    if (value.kind == HirValueKind::Call) {
      if (const auto *call = dynamic_cast<const Call *>(value.source)) {
        argumentCount = call->arguments().size();
      }
    }
    if (argumentCount > value.operands.size()) {
      return value.operands;
    }
    return std::vector<HirValueId>(
        value.operands.end() - static_cast<std::ptrdiff_t>(argumentCount),
        value.operands.end());
  }

  [[nodiscard]] std::optional<HirValueId>
  receiverValue(const HirValue &value) const {
    if (value.kind == HirValueKind::Call && !value.operands.empty()) {
      const HirValue *callee = findValue(value.operands.front());
      if (callee != nullptr && callee->kind == HirValueKind::MemberAccess &&
          !callee->operands.empty()) {
        return callee->unsafeOperation == UnsafeOperationKind::RawMember
                   ? std::optional<HirValueId>{callee->id}
                   : std::optional<HirValueId>{callee->operands.front()};
      }
      if (value.receiver) {
        return value.receiver;
      }
    }
    if (value.functionTarget && value.kind != HirValueKind::Call &&
        !value.operands.empty()) {
      return value.operands.front();
    }
    return std::nullopt;
  }

  [[nodiscard]] AccessMode
  receiverAccess(HirFunctionInstanceId targetId) const {
    if (targetId == 0) {
      return AccessMode::ReadOnly;
    }
    const HirFunctionInstance *target = program.findFunctionInstance(targetId);
    return target != nullptr && target->source != nullptr &&
                   target->source->receiverMutability() ==
                       ReceiverMutability::Mutable
               ? AccessMode::Mutable
               : AccessMode::ReadOnly;
  }

  [[nodiscard]] AccessMode receiverAccess(const HirValue &value) const {
    return value.functionTarget ? receiverAccess(*value.functionTarget)
                                : AccessMode::ReadOnly;
  }

  [[nodiscard]] MirOperand receiverOperand(HirValueId id, AccessMode access) {
    const HirValue *receiver = findValue(id);
    if (receiver == nullptr) {
      valid = false;
      return {};
    }
    if (receiver->kind == HirValueKind::MemberAccess &&
        receiver->unsafeOperation == UnsafeOperationKind::RawMember &&
        !receiver->operands.empty()) {
      const HirValueId pointerId = receiver->operands.front();
      const HirValue *pointer = findValue(pointerId);
      if (pointer == nullptr ||
          pointer->info.type.kind != SemanticType::RawPointer ||
          pointer->info.type.arguments.size() != 1) {
        valid = false;
        return {};
      }
      (void)valueOperand(pointerId);
      const SemanticType pointee = pointer->info.type.arguments.front();
      const AccessMode pointeeAccess = pointer->info.type.pointerAccess;
      const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                            .value = mirValueFor(pointerId),
                                            .type = pointee,
                                            .access = pointeeAccess,
                                            .traits = semanticTraits(pointee),
                                            .sourceValue = receiver->id});
      output.places[place - 1].projections.push_back(
          {.kind = MirProjectionKind::RawDereference});
      return {.kind = access == AccessMode::Mutable
                          ? MirOperandKind::BorrowWrite
                          : MirOperandKind::BorrowRead,
              .place = place,
              .type = pointee};
    }
    if (receiver->info.category == ValueCategory::Place) {
      emitPlaceDependencies(id);
      const MirPlaceId place = placeForValue(id);
      if (place != 0) {
        return {.kind = access == AccessMode::Mutable
                            ? MirOperandKind::BorrowWrite
                            : MirOperandKind::BorrowRead,
                .place = place,
                .type = receiver->info.type};
      }
    }
    return valueOperand(id);
  }

  [[nodiscard]] static MirOperation binaryOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::COMMA:
      return MirOperation::Comma;
    case TokenKind::PLUS:
      return MirOperation::Add;
    case TokenKind::MINUS:
      return MirOperation::Subtract;
    case TokenKind::STAR:
      return MirOperation::Multiply;
    case TokenKind::SLASH:
      return MirOperation::Divide;
    case TokenKind::PERCENT:
      return MirOperation::Remainder;
    case TokenKind::AMPERSAND:
      return MirOperation::BitwiseAnd;
    case TokenKind::PIPE:
      return MirOperation::BitwiseOr;
    case TokenKind::CARET:
      return MirOperation::BitwiseXor;
    case TokenKind::SHIFT_LEFT:
      return MirOperation::ShiftLeft;
    case TokenKind::SHIFT_RIGHT:
      return MirOperation::ShiftRight;
    case TokenKind::EQUAL_EQUAL:
      return MirOperation::Equal;
    case TokenKind::BANG_EQUAL:
      return MirOperation::NotEqual;
    case TokenKind::LESS:
      return MirOperation::Less;
    case TokenKind::LESS_EQUAL:
      return MirOperation::LessEqual;
    case TokenKind::GREATER:
      return MirOperation::Greater;
    case TokenKind::GREATER_EQUAL:
      return MirOperation::GreaterEqual;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation unaryOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::PLUS:
      return MirOperation::Positive;
    case TokenKind::MINUS:
      return MirOperation::Negate;
    case TokenKind::BANG:
      return MirOperation::LogicalNot;
    case TokenKind::TILDE:
      return MirOperation::BitwiseNot;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation assignmentOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::EQUAL:
      return MirOperation::Assign;
    case TokenKind::PLUS_EQUAL:
      return MirOperation::AddAssign;
    case TokenKind::MINUS_EQUAL:
      return MirOperation::SubtractAssign;
    case TokenKind::STAR_EQUAL:
      return MirOperation::MultiplyAssign;
    case TokenKind::SLASH_EQUAL:
      return MirOperation::DivideAssign;
    case TokenKind::PERCENT_EQUAL:
      return MirOperation::RemainderAssign;
    case TokenKind::AMPERSAND_EQUAL:
      return MirOperation::BitwiseAndAssign;
    case TokenKind::PIPE_EQUAL:
      return MirOperation::BitwiseOrAssign;
    case TokenKind::CARET_EQUAL:
      return MirOperation::BitwiseXorAssign;
    case TokenKind::SHIFT_LEFT_EQUAL:
      return MirOperation::ShiftLeftAssign;
    case TokenKind::SHIFT_RIGHT_EQUAL:
      return MirOperation::ShiftRightAssign;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation modifierOperation(const HirValue &value) {
    if (!value.operation) {
      return MirOperation::None;
    }
    switch (*value.operation) {
    case TokenKind::PLUS_PLUS:
      return value.kind == HirValueKind::Postfix ? MirOperation::PostIncrement
                                                 : MirOperation::PreIncrement;
    case TokenKind::MINUS_MINUS:
      return value.kind == HirValueKind::Postfix ? MirOperation::PostDecrement
                                                 : MirOperation::PreDecrement;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation computeOperation(const HirValue &value) {
    if (value.unsafeOperation == UnsafeOperationKind::AddressOf) {
      return MirOperation::AddressOf;
    }
    if (value.unsafeOperation == UnsafeOperationKind::PointerArithmetic &&
        value.kind == HirValueKind::Binary) {
      if (value.info.type.kind != SemanticType::RawPointer) {
        return MirOperation::PointerDifference;
      }
      return value.operation == TokenKind::MINUS ? MirOperation::PointerSubtract
                                                 : MirOperation::PointerAdd;
    }
    switch (value.kind) {
    case HirValueKind::ArrayInitializer:
      return MirOperation::Aggregate;
    case HirValueKind::Binary:
      return value.operation ? binaryOperation(*value.operation)
                             : MirOperation::None;
    case HirValueKind::Conversion:
      return MirOperation::Convert;
    case HirValueKind::Grouping:
      return MirOperation::Identity;
    case HirValueKind::Index:
      return MirOperation::Index;
    case HirValueKind::Lambda:
      return MirOperation::Closure;
    case HirValueKind::LayoutQuery:
      return MirOperation::Literal;
    case HirValueKind::Literal:
      return MirOperation::Literal;
    case HirValueKind::PackExpansion:
      return MirOperation::PackExpansion;
    case HirValueKind::QualifiedName:
      return value.enumOwner && value.enumValue ? MirOperation::EnumConstant
                                                : MirOperation::None;
    case HirValueKind::Unary:
      return value.operation ? unaryOperation(*value.operation)
                             : MirOperation::None;
    case HirValueKind::Unexpected:
      return MirOperation::Unexpected;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] MirPlaceId canonicalBorrowSource(MirPlaceId placeId) {
    const auto resolve = [&](const auto &self, MirPlaceId current,
                             std::size_t depth) -> MirPlaceId {
      const MirPlace *place = output.findPlace(current);
      if (place == nullptr || depth > output.loans.size() + 1) {
        return 0;
      }

      const MirLoan *loan = nullptr;
      std::size_t projectionOffset = 0;
      if (place->root == MirPlaceRootKind::Loan) {
        loan = output.findLoan(place->loan);
      } else if (place->root == MirPlaceRootKind::Binding &&
                 !place->projections.empty() &&
                 place->projections.front().kind ==
                     MirProjectionKind::Dereference) {
        const HirBinding *binding = findBinding(place->binding);
        const auto semantic =
            binding == nullptr || binding->info.retainedLoan == 0
                ? semanticLoans.end()
                : semanticLoans.find(binding->info.retainedLoan);
        const auto found = bindingLoans.find(place->binding);
        loan = semantic != semanticLoans.end()
                   ? output.findLoan(semantic->second)
                   : (found == bindingLoans.end()
                          ? nullptr
                          : output.findLoan(found->second));
        projectionOffset = loan == nullptr ? 0 : 1;
      } else if (place->root == MirPlaceRootKind::Value) {
        const MirValue *value = output.findValue(place->value);
        const MirLoanId loanId =
            value == nullptr ? 0 : loanForValue(value->sourceValue);
        loan = output.findLoan(loanId);
      }
      if (loan == nullptr || loan->source == current) {
        return current;
      }

      const MirPlaceId baseId = self(self, loan->source, depth + 1);
      const MirPlace *base = output.findPlace(baseId);
      place = output.findPlace(current);
      if (base == nullptr || place == nullptr) {
        return 0;
      }
      if (projectionOffset == place->projections.size()) {
        return baseId;
      }
      MirPlace composed = *base;
      composed.id = 0;
      composed.type = place->type;
      composed.access = place->access;
      composed.traits = place->traits;
      composed.sourceValue = place->sourceValue;
      composed.projections.insert(
          composed.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
          place->projections.end());
      return appendPlace(std::move(composed));
    };
    return resolve(resolve, placeId, 0);
  }

  [[nodiscard]] bool sameBorrowSource(MirPlaceId left, MirPlaceId right) {
    left = canonicalBorrowSource(left);
    right = canonicalBorrowSource(right);
    const MirPlace *lhs = output.findPlace(left);
    const MirPlace *rhs = output.findPlace(right);
    if (lhs == nullptr || rhs == nullptr || lhs->root != rhs->root ||
        lhs->binding != rhs->binding || lhs->symbol != rhs->symbol ||
        lhs->temporary != rhs->temporary || lhs->value != rhs->value ||
        lhs->loan != rhs->loan ||
        lhs->projections.size() != rhs->projections.size()) {
      return false;
    }
    return std::equal(lhs->projections.begin(), lhs->projections.end(),
                      rhs->projections.begin(),
                      [](const MirPlaceProjection &leftProjection,
                         const MirPlaceProjection &rightProjection) {
                        return leftProjection.kind == rightProjection.kind &&
                               leftProjection.field == rightProjection.field &&
                               leftProjection.index == rightProjection.index &&
                               leftProjection.constantIndex ==
                                   rightProjection.constantIndex &&
                               leftProjection.selection ==
                                   rightProjection.selection;
                      });
  }

  [[nodiscard]] MirPlaceId borrowOriginPlace(const HirValue &value,
                                             const MirInstruction &call) {
    if (value.borrowOrigin == BorrowOriginKind::Receiver && call.receiver) {
      if (const std::optional<HirValueId> receiver = receiverValue(value)) {
        if (const MirPlaceId source = borrowedSourcePlace(*receiver);
            source != 0) {
          return canonicalBorrowSource(source);
        }
        if (call.receiver->place != 0) {
          return canonicalBorrowSource(call.receiver->place);
        }
        if (call.receiver->kind == MirOperandKind::Loan) {
          const MirLoan *loan = output.findLoan(call.receiver->loan);
          return loan == nullptr ? 0 : canonicalBorrowSource(loan->source);
        }
        if (const HirValue *receiverValue = findValue(*receiver);
            receiverValue != nullptr) {
          const MirPlaceId source = valueRootPlace(*receiverValue);
          valuePlaces.insert_or_assign(*receiver, source);
          return source;
        }
      }
      return canonicalBorrowSource(call.receiver->place);
    }
    if (value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < call.operands.size()) {
      const std::vector<HirValueId> arguments = callArgumentValues(value);
      if (value.borrowArgument < arguments.size()) {
        if (const MirPlaceId source =
                borrowedSourcePlace(arguments[value.borrowArgument]);
            source != 0) {
          return canonicalBorrowSource(source);
        }
      }
      const MirOperand &argument = call.operands[value.borrowArgument];
      if (argument.kind == MirOperandKind::Loan) {
        const MirLoan *loan = output.findLoan(argument.loan);
        return loan == nullptr ? 0 : canonicalBorrowSource(loan->source);
      }
      if (argument.place != 0) {
        return canonicalBorrowSource(argument.place);
      }
      if (value.borrowArgument < arguments.size()) {
        return canonicalBorrowSource(
            placeForValue(arguments[value.borrowArgument]));
      }
    }
    return 0;
  }

  [[nodiscard]] MirLoanId
  createLoan(MirLoanKind kind, MirPlaceId sourcePlace, AccessMode access,
             HirValueId producedBy = 0, HirBindingId binding = 0,
             SymbolId storedField = 0, MirLoanId parent = 0) {
    const MirLoanId id = output.loans.size() + 1;
    output.loans.push_back(
        {.id = id,
         .parent = parent,
         .kind = kind,
         .source = sourcePlace,
         .access = access,
         .producedBy = producedBy,
         .carriers = binding == 0 ? std::vector<HirBindingId>{}
                                  : std::vector<HirBindingId>{binding},
         .storedField = storedField});
    return id;
  }

  void emitCall(const HirValue &value) {
    MirInstruction call{.kind = MirInstructionKind::Call,
                        .hirValue = value.id,
                        .result = resultFor(value),
                        .intrinsic = value.intrinsic,
                        .dispatch = value.dispatch,
                        .dispatchOwner = value.dispatchOwner,
                        .functionTarget = value.functionTarget,
                        .constructorTarget = value.constructorTarget,
                        .constructorKind = value.constructorKind,
                        .lambdaTarget = value.lambdaTarget,
                        .nonEscapingArguments = value.nonEscapingArguments,
                        .nonEscapingCallable = value.nonEscapingCallable,
                        .info = value.info};

    if (const std::optional<HirValueId> receiver = receiverValue(value)) {
      const AccessMode access = receiverAccess(value);
      call.receiver = receiverOperand(*receiver, access);
      if (const HirValue *source = findValue(*receiver);
          source != nullptr &&
          source->unsafeOperation == UnsafeOperationKind::RawMember) {
        call.unsafeOperation = UnsafeOperationKind::RawMember;
      }
    }

    if (value.lambdaTarget && !value.operands.empty() &&
        !value.functionTarget) {
      const std::size_t argumentCount = callArgumentValues(value).size();
      if (value.operands.size() > argumentCount) {
        call.receiver = valueOperand(value.operands.front());
      }
    }

    const std::vector<HirValueId> arguments = callArgumentValues(value);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const SemanticType parameter =
          index < value.parameterTypes.size()
              ? value.parameterTypes[index]
              : (findValue(arguments[index]) == nullptr
                     ? SemanticType::Unknown
                     : findValue(arguments[index])->info.type);
      call.parameterTypes.push_back(parameter);
      call.operands.push_back(argumentOperand(arguments[index], parameter));
    }

    const auto operandLoan = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::Loan) {
        return operand.loan;
      }
      const MirPlace *place = output.findPlace(operand.place);
      if (place == nullptr) {
        return MirLoanId{0};
      }
      if (place->root == MirPlaceRootKind::Loan) {
        return place->loan;
      }
      if (place->root == MirPlaceRootKind::Binding) {
        const auto found = bindingLoans.find(place->binding);
        if (found != bindingLoans.end()) {
          return found->second;
        }
      }
      if (place->root == MirPlaceRootKind::Value) {
        const MirValue *resolved = output.findValue(place->value);
        return resolved == nullptr ? MirLoanId{0}
                                   : loanForValue(resolved->sourceValue);
      }
      return MirLoanId{0};
    };
    MirLoanId originLoan = 0;
    if (value.borrowOrigin == BorrowOriginKind::Receiver) {
      if (call.receiver) {
        originLoan = operandLoan(*call.receiver);
      }
    } else if (value.borrowOrigin == BorrowOriginKind::Argument &&
               value.borrowArgument < arguments.size() &&
               value.borrowArgument < call.operands.size()) {
      originLoan = operandLoan(call.operands[value.borrowArgument]);
    }
    const MirPlaceId origin = borrowOriginPlace(value, call);
    call.borrowOrigin = value.borrowOrigin;
    call.borrowArgument = value.borrowArgument;
    call.borrowAccess = value.borrowAccess;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanKind kind = value.info.traits.containsBorrowedState
                                   ? MirLoanKind::Stored
                                   : MirLoanKind::CallResult;
      MirLoanId parent = 0;
      if (kind == MirLoanKind::CallResult && originLoan != 0 &&
          originLoan <= output.loans.size()) {
        const AccessMode parentAccess = output.loans[originLoan - 1].access;
        if (parentAccess == AccessMode::Mutable ||
            value.borrowAccess == AccessMode::ReadOnly) {
          parent = originLoan;
        }
      }
      const MirLoanId loan =
          createLoan(kind, origin, value.borrowAccess, value.id, 0, 0, parent);
      call.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const SemanticType parameter =
          index < value.parameterTypes.size()
              ? value.parameterTypes[index]
              : (findValue(arguments[index]) == nullptr
                     ? SemanticType::Unknown
                     : findValue(arguments[index])->info.type);
      if (parameter.kind != SemanticType::Reference) {
        transferTemporaryOut(call, arguments[index]);
      }
    }
    (void)initializeValueLifecycle(call, value);
    (void)appendInstruction(std::move(call));
  }

  void emitConstruct(const HirValue &value) {
    MirInstruction construct{.kind = MirInstructionKind::Construct,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .constructorTarget = value.constructorTarget,
                             .constructorKind = value.constructorKind,
                             .info = value.info};
    const std::vector<HirValueId> arguments = callArgumentValues(value);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const SemanticType parameter =
          index < value.parameterTypes.size()
              ? value.parameterTypes[index]
              : (findValue(arguments[index]) == nullptr
                     ? SemanticType::Unknown
                     : findValue(arguments[index])->info.type);
      construct.parameterTypes.push_back(parameter);
      construct.operands.push_back(
          value.constructorKind == ConstructorKind::Ordinary
              ? argumentOperand(arguments[index], parameter)
              : valueOperand(arguments[index]));
    }
    const MirPlaceId origin = borrowOriginPlace(value, construct);
    construct.borrowOrigin = value.borrowOrigin;
    construct.borrowArgument = value.borrowArgument;
    construct.borrowAccess = value.borrowAccess;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanId loan =
          createLoan(MirLoanKind::Stored, origin, value.borrowAccess, value.id);
      construct.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const SemanticType parameter =
          index < value.parameterTypes.size()
              ? value.parameterTypes[index]
              : (findValue(arguments[index]) == nullptr
                     ? SemanticType::Unknown
                     : findValue(arguments[index])->info.type);
      if (parameter.kind != SemanticType::Reference) {
        transferTemporaryOut(construct, arguments[index]);
      }
    }
    (void)initializeValueLifecycle(construct, value);
    (void)appendInstruction(std::move(construct));
  }

  [[nodiscard]] MirOperand conditionOperand(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    if (value->info.type == SemanticType::Bool) {
      return valueOperand(id);
    }
    if (const auto found = contextualValues.find(id);
        found != contextualValues.end()) {
      return {.kind = MirOperandKind::Value,
              .value = found->second,
              .type = SemanticType::Bool};
    }

    const ExpressionInfo info{.type = SemanticType::Bool,
                              .category = ValueCategory::Value,
                              .access = AccessMode::ReadOnly,
                              .traits = semanticTraits(SemanticType::Bool)};
    const MirValueId result = appendValue(id, info);
    if (value->info.type.kind == SemanticType::Expected) {
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = id,
                               .result = result,
                               .operands = {valueOperand(id)},
                               .operation = MirOperation::ExpectedHasValue,
                               .info = info});
    } else if (value->contextualBoolTarget) {
      const AccessMode access = receiverAccess(*value->contextualBoolTarget);
      (void)appendInstruction({.kind = MirInstructionKind::Call,
                               .hirValue = id,
                               .result = result,
                               .receiver = receiverOperand(id, access),
                               .dispatch = value->dispatch,
                               .dispatchOwner = value->dispatchOwner,
                               .functionTarget = value->contextualBoolTarget,
                               .info = info});
    } else {
      valid = false;
    }
    contextualValues.emplace(id, result);
    return {.kind = MirOperandKind::Value,
            .value = result,
            .type = SemanticType::Bool};
  }

  [[nodiscard]] MirPlaceId logicalTemporary(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Temporary,
                        .temporary = nextTemporary++,
                        .type = SemanticType::Bool,
                        .access = AccessMode::Mutable,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  [[nodiscard]] MirPlaceId conditionalTemporary(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Temporary,
                        .temporary = nextTemporary++,
                        .type = value.info.type,
                        .access = AccessMode::Mutable,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  void endAddedLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      const std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      if (after.size() < before.size() ||
          !std::equal(before.begin(), before.end(), after.begin())) {
        valid = false;
        continue;
      }
      for (std::size_t index = after.size(); index > before.size(); --index) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = after[index - 1]});
      }
    }
  }

  void endFullExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      if (after.size() < before.size() ||
          !std::equal(before.begin(), before.end(), after.begin())) {
        valid = false;
        continue;
      }
      for (std::size_t index = after.size(); index > before.size(); --index) {
        const MirLoan &loan = output.loans[after[index - 1] - 1];
        if (loan.carriers.empty() && !loan.escapes) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan.id});
        }
      }
      after.erase(std::remove_if(after.begin() +
                                     static_cast<std::ptrdiff_t>(before.size()),
                                 after.end(),
                                 [&](MirLoanId loan) {
                                   const MirLoan &candidate =
                                       output.loans[loan - 1];
                                   return candidate.carriers.empty() &&
                                          !candidate.escapes;
                                 }),
                  after.end());
    }
  }

  void endReturnExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      for (auto candidate = after.begin(); candidate != after.end();) {
        const bool added =
            std::find(before.begin(), before.end(), *candidate) == before.end();
        const MirLoan *loan = output.findLoan(*candidate);
        if (added && loan != nullptr && loan->carriers.empty() &&
            !loan->escapes) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan->id});
          candidate = after.erase(candidate);
        } else {
          ++candidate;
        }
      }
    }
  }

  [[nodiscard]] MirLoanId mirLoanForSemanticLoan(SemanticLoanId semanticLoan) {
    const auto found = semanticLoans.find(semanticLoan);
    if (found == semanticLoans.end() || found->second == 0 ||
        found->second > output.loans.size() ||
        output.loans[found->second - 1].escapes) {
      valid = false;
      return 0;
    }
    return found->second;
  }

  [[nodiscard]] static bool loanIsActive(const std::vector<Scope> &scopeState,
                                         MirLoanId loan) {
    return std::any_of(
        scopeState.begin(), scopeState.end(), [&](const Scope &scope) {
          return std::find(scope.loans.begin(), scope.loans.end(), loan) !=
                 scope.loans.end();
        });
  }

  static void removeLoanFromState(
      MirLoanId loan, std::vector<Scope> &scopeState,
      std::unordered_map<HirBindingId, MirLoanId> &bindingLoanState) {
    for (Scope &scope : scopeState) {
      std::erase(scope.loans, loan);
    }
    std::erase_if(bindingLoanState,
                  [&](const auto &entry) { return entry.second == loan; });
  }

  void endConsumedCallResultLoans(const std::vector<HirValueId> &operands,
                                  const HirValue &consumer) {
    if (consumer.info.type.kind == SemanticType::Reference ||
        consumer.info.traits.containsBorrowedState) {
      return;
    }
    std::unordered_set<MirLoanId> visited;
    for (const HirValueId operand : operands) {
      MirLoanId loan = loanForValue(operand);
      while (loan != 0 && loan <= output.loans.size() &&
             visited.insert(loan).second && loanIsActive(scopes, loan)) {
        const MirLoan &candidate = output.loans[loan - 1];
        if (candidate.kind != MirLoanKind::CallResult ||
            !candidate.carriers.empty() || candidate.escapes) {
          break;
        }
        const MirLoanId parent = candidate.parent;
        (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                 .hirValue = consumer.id,
                                 .loan = loan});
        removeLoanFromState(loan, scopes, bindingLoans);
        loan = parent;
      }
    }
  }

  [[nodiscard]] std::vector<MirLoanId>
  childFirstLoans(const std::vector<SemanticLoanId> &loans) {
    std::vector<MirLoanId> result;
    result.reserve(loans.size());
    for (const SemanticLoanId semanticLoan : loans) {
      const MirLoanId loan = mirLoanForSemanticLoan(semanticLoan);
      if (loan != 0 &&
          std::find(result.begin(), result.end(), loan) == result.end()) {
        result.push_back(loan);
      }
    }
    const auto depth = [&](MirLoanId loan) {
      std::size_t result = 0;
      std::unordered_set<MirLoanId> visited;
      while (loan != 0 && loan <= output.loans.size() &&
             visited.insert(loan).second) {
        ++result;
        loan = output.loans[loan - 1].parent;
      }
      return result;
    };
    std::stable_sort(result.begin(), result.end(),
                     [&](MirLoanId left, MirLoanId right) {
                       return depth(left) > depth(right);
                     });
    return result;
  }

  [[nodiscard]] bool mustReachBorrowedReturn(MirLoanId loan) const {
    if (loan == 0 || loan > output.loans.size() || function == nullptr ||
        function->returnBorrowOrigin != BorrowOriginKind::Argument ||
        function->returnBorrowParameter >= function->parameterBindings.size()) {
      return false;
    }
    const MirLoan &candidate = output.loans[loan - 1];
    const HirBindingId parameter =
        function->parameterBindings[function->returnBorrowParameter];
    return candidate.entry &&
           std::find(candidate.carriers.begin(), candidate.carriers.end(),
                     parameter) != candidate.carriers.end();
  }

  void endSemanticLoans(const std::vector<SemanticLoanId> &loans,
                        HirStatementId statement) {
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan)) {
        continue;
      }
      if (!loanIsActive(scopes, loan)) {
        valid = false;
        continue;
      }
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement,
                               .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void endSemanticLoansIfActive(const std::vector<SemanticLoanId> &loans,
                                HirStatementId statement) {
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan)) {
        continue;
      }
      if (!loanIsActive(scopes, loan)) {
        continue;
      }
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement,
                               .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void normalizeSemanticLoanState(
      const std::vector<SemanticLoanId> &loans, std::vector<Scope> &scopeState,
      std::unordered_map<HirBindingId, MirLoanId> &bindingLoanState) {
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan)) {
        continue;
      }
      if (!loanIsActive(scopeState, loan)) {
        valid = false;
        continue;
      }
      removeLoanFromState(loan, scopeState, bindingLoanState);
    }
  }

  void endSemanticLoans(const HirStatement &statement) {
    endSemanticLoans(statement.endedLoans, statement.id);
  }

  void emitLogical(const HirValue &value) {
    if (value.operands.size() != 2 || !value.operation ||
        (*value.operation != TokenKind::AND &&
         *value.operation != TokenKind::OR)) {
      valid = false;
      return;
    }

    const MirOperand left = conditionOperand(value.operands[0]);
    const std::vector<TemporaryDrop> leftTemporaryDrops = temporaryDrops;
    const MirPlaceId temporary = logicalTemporary(value);
    const MirBlockId rightBlock = appendBlock();
    const MirBlockId shortCircuitBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    const bool conjunction = *value.operation == TokenKind::AND;
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = left,
               .target = conjunction ? rightBlock : shortCircuitBlock,
               .elseTarget = conjunction ? shortCircuitBlock : rightBlock});

    const std::vector<Scope> incomingScopes = scopes;
    current = shortCircuitBlock;
    scopes = incomingScopes;
    temporaryDrops = leftTemporaryDrops;
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirValue = value.id,
         .destination = temporary,
         .operands = {{.kind = MirOperandKind::Constant,
                       .literal = Literal{!conjunction},
                       .type = SemanticType::Bool}},
         .info = ExpressionInfo{.type = SemanticType::Bool,
                                .category = ValueCategory::Place,
                                .access = AccessMode::Mutable,
                                .traits = value.info.traits}});
    terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    const std::vector<TemporaryDrop> shortCircuitTemporaryDrops =
        temporaryDrops;

    current = rightBlock;
    scopes = incomingScopes;
    temporaryDrops = leftTemporaryDrops;
    const MirOperand right = conditionOperand(value.operands[1]);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirValue = value.id,
         .destination = temporary,
         .operands = {right},
         .info = ExpressionInfo{.type = SemanticType::Bool,
                                .category = ValueCategory::Place,
                                .access = AccessMode::Mutable,
                                .traits = value.info.traits}});
    endAddedLoans(incomingScopes);
    terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    const std::vector<TemporaryDrop> rightTemporaryDrops = temporaryDrops;

    current = mergeBlock;
    scopes = incomingScopes;
    temporaryDrops =
        mergeTemporaryDrops(shortCircuitTemporaryDrops, rightTemporaryDrops);
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = temporary,
                                           .type = SemanticType::Bool}},
                             .info = value.info});
    emittedValues.insert(value.id);
  }

  void emitConditional(const HirValue &value) {
    if (value.operands.size() != 3) {
      valid = false;
      return;
    }

    const MirOperand condition = conditionOperand(value.operands[0]);
    const MirBlockId thenBlock = appendBlock();
    const MirBlockId elseBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = condition,
               .target = thenBlock,
               .elseTarget = elseBlock});

    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const bool hasResult = value.info.type.kind != SemanticType::Void;
    const MirPlaceId temporary =
        hasResult ? conditionalTemporary(value) : MirPlaceId{0};
    const MirDropObligationId resultObligation =
        hasResult ? dropObligationForValue(value.id, temporary) : 0;
    std::vector<TemporaryDrop> thenTemporaryDrops;
    std::vector<TemporaryDrop> elseTemporaryDrops;
    const auto emitArm = [&](MirBlockId block, HirValueId arm) {
      current = block;
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      if (hasResult) {
        MirInstruction initialize{
            .kind = MirInstructionKind::Initialize,
            .hirValue = value.id,
            .destination = temporary,
            .operands = {valueOperand(arm)},
            .info = ExpressionInfo{.type = value.info.type,
                                   .category = ValueCategory::Place,
                                   .access = AccessMode::Mutable,
                                   .traits = value.info.traits}};
        const MirDropObligationId sourceObligation =
            dropObligationForValue(arm);
        if (resultObligation != 0) {
          if (temporaryIsActive(sourceObligation)) {
            appendReparentOrTypedTransfer(initialize, sourceObligation,
                                          resultObligation);
            (void)removeTemporary(sourceObligation);
          } else {
            appendLifecycle(initialize,
                            {.kind = MirLifecycleEventKind::Initialize,
                             .target = resultObligation});
          }
          registerTemporary(resultObligation);
        }
        (void)appendInstruction(std::move(initialize));
      } else {
        emitValue(arm);
      }
      endAddedLoans(incomingScopes);
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
      return temporaryDrops;
    };

    thenTemporaryDrops = emitArm(thenBlock, value.operands[1]);
    elseTemporaryDrops = emitArm(elseBlock, value.operands[2]);

    current = mergeBlock;
    scopes = incomingScopes;
    temporaryDrops =
        mergeTemporaryDrops(thenTemporaryDrops, elseTemporaryDrops);
    if (hasResult) {
      const bool moveResult = value.info.traits.drop != DropKind::Trivial ||
                              !value.info.traits.copyable;
      (void)appendInstruction(
          {.kind =
               moveResult ? MirInstructionKind::Move : MirInstructionKind::Load,
           .hirValue = value.id,
           .result = resultFor(value),
           .operands = {{.kind = moveResult ? MirOperandKind::Move
                                            : MirOperandKind::Copy,
                         .place = temporary,
                         .type = value.info.type}},
           .info = value.info});
    }
    emittedValues.insert(value.id);
  }

  void emitModify(const HirValue &value) {
    if (value.operands.size() != 1) {
      valid = false;
      return;
    }
    emitPlaceDependencies(value.operands.front());
    const MirOperation operation = modifierOperation(value);
    const MirPlaceId destination = placeForValue(value.operands.front());
    if (operation == MirOperation::None || destination == 0) {
      valid = false;
      return;
    }
    (void)appendInstruction({.kind = MirInstructionKind::Modify,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .destination = destination,
                             .operation = operation,
                             .info = value.info});
    emittedValues.insert(value.id);
  }

  void emitValue(HirValueId id) {
    if (id == 0 || emittedValues.contains(id)) {
      return;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }

    if (value->kind == HirValueKind::Move) {
      (void)valueOperand(id);
      return;
    }
    if (value->kind == HirValueKind::Assignment ||
        value->kind == HirValueKind::MemberSet ||
        value->kind == HirValueKind::IndexSet ||
        value->kind == HirValueKind::DereferenceSet) {
      if (value->kind == HirValueKind::MemberSet && !value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      } else if (value->kind == HirValueKind::IndexSet &&
                 value->operands.size() >= 2) {
        if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands[0]);
        } else {
          emitPlaceDependencies(value->operands[0]);
        }
        (void)valueOperand(value->operands[1]);
      } else if (value->kind == HirValueKind::DereferenceSet &&
                 !value->operands.empty()) {
        (void)valueOperand(value->operands.front());
      }
      const MirPlaceId destination = destinationFor(*value);
      attachPlaceIdentity(destination, *value);
      std::vector<MirOperand> operands;
      MirDropObligationId sourceObligation = 0;
      if (!value->operands.empty()) {
        const HirValueId sourceValue = value->operands.back();
        operands.push_back(valueOperand(sourceValue));
        sourceObligation = dropObligationForValue(sourceValue);
      }
      MirInstruction assign{
          .kind = MirInstructionKind::Assign,
          .hirValue = value->id,
          .result = resultFor(*value),
          .destination = destination,
          .operands = std::move(operands),
          .operation = value->operation ? assignmentOperation(*value->operation)
                                        : MirOperation::None,
          .info = value->info};
      if (temporaryIsActive(sourceObligation)) {
        const MirPlace *destinationPlace = output.findPlace(destination);
        const MirDropObligationId target =
            destinationPlace != nullptr &&
                    destinationPlace->root == MirPlaceRootKind::Binding &&
                    destinationPlace->projections.empty()
                ? dropObligationForBinding(destinationPlace->binding)
                : 0;
        appendLifecycle(assign, {.kind = MirLifecycleEventKind::Replace,
                                 .source = sourceObligation,
                                 .target = target});
      }
      (void)appendInstruction(std::move(assign));
      emittedValues.insert(id);
      return;
    }
    if (value->kind == HirValueKind::Call &&
        value->intrinsic != IntrinsicKind::None) {
      emitCall(*value);
      emittedValues.insert(id);
      return;
    }
    if (value->constructorTarget ||
        value->constructorKind != ConstructorKind::Ordinary ||
        value->kind == HirValueKind::DirectInitializer) {
      emitConstruct(*value);
      emittedValues.insert(id);
      return;
    }
    if (value->kind == HirValueKind::Call || value->functionTarget) {
      emitCall(*value);
      emittedValues.insert(id);
      return;
    }
    if ((value->kind == HirValueKind::Unary ||
         value->kind == HirValueKind::Postfix) &&
        modifierOperation(*value) != MirOperation::None) {
      emitModify(*value);
      return;
    }
    if (value->kind == HirValueKind::Logical) {
      emitLogical(*value);
      return;
    }
    if (value->kind == HirValueKind::Conditional) {
      emitConditional(*value);
      return;
    }
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind)) {
      (void)valueOperand(id);
      return;
    }

    MirInstruction instruction{.kind = MirInstructionKind::Compute,
                               .hirValue = value->id,
                               .result = resultFor(*value),
                               .operation = computeOperation(*value),
                               .literal = value->literal,
                               .enumOwner = value->enumOwner,
                               .enumValue = value->enumValue,
                               .intrinsic = value->intrinsic,
                               .lambdaTarget = value->lambdaTarget,
                               .info = value->info};
    if (instruction.operation == MirOperation::Closure &&
        instruction.lambdaTarget) {
      const HirLambda *lambda = program.findLambda(*instruction.lambdaTarget);
      if (lambda == nullptr) {
        valid = false;
      } else {
        instruction.closureCaptureTypes.reserve(lambda->captures.size());
        for (const LambdaCaptureInfo &capture : lambda->captures) {
          instruction.closureCaptureTypes.push_back(capture.type);
        }
      }
    }
    if (instruction.operation == MirOperation::None) {
      valid = false;
    }
    for (const HirValueId operand : value->operands) {
      instruction.operands.push_back(
          instruction.operation == MirOperation::LogicalNot
              ? conditionOperand(operand)
          : instruction.operation == MirOperation::AddressOf
              ? addressOperand(operand)
              : valueOperand(operand));
    }
    if (value->kind == HirValueKind::ArrayInitializer ||
        value->kind == HirValueKind::Unexpected) {
      for (const HirValueId operand : value->operands) {
        transferTemporaryOut(instruction, operand);
      }
    }
    (void)initializeValueLifecycle(instruction, *value);
    (void)appendInstruction(std::move(instruction));
    endConsumedCallResultLoans(value->operands, *value);
    emittedValues.insert(id);
  }

  [[nodiscard]] MirOperand referenceOperand(HirValueId valueId,
                                            HirBindingId binding = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return {};
    }
    emitPlaceDependencies(valueId);
    const MirLoanId existing = loanForValue(valueId);
    const HirLoan *desired = binding == 0 ? nullptr : loanForBinding(binding);
    if (desired != nullptr) {
      if (const auto found = semanticLoans.find(desired->semanticLoan);
          found != semanticLoans.end()) {
        if (existing != 0 && existing != found->second) {
          const MirLoan *transient = output.findLoan(existing);
          const MirLoan *retained = output.findLoan(found->second);
          if (transient == nullptr || retained == nullptr ||
              desired->access != AccessMode::ReadOnly ||
              !sameBorrowSource(transient->source, retained->source)) {
            valid = false;
          }
        }
        recordLoanIdentity(found->second, *desired, binding);
        return {.kind = MirOperandKind::Loan,
                .loan = found->second,
                .type = value->info.type};
      }

      MirLoanId parent = 0;
      if (desired->parent != 0) {
        const MirPlaceId desiredSource = placeForLoan(*desired);
        if (desiredSource == 0) {
          return {};
        }
        const auto found = semanticLoans.find(desired->parent);
        if (found == semanticLoans.end()) {
          valid = false;
          return {};
        }
        parent = found->second;
        if (existing != 0 && existing != parent) {
          const MirLoan *parentLoan = output.findLoan(parent);
          std::vector<MirLoanId> transientChain;
          std::unordered_set<MirLoanId> visited;
          MirLoanId transientId = existing;
          bool compatible = parentLoan != nullptr;
          while (compatible && transientId != parent) {
            const MirLoan *transient = output.findLoan(transientId);
            compatible =
                transient != nullptr && visited.insert(transientId).second &&
                transient->kind == MirLoanKind::CallResult &&
                (desired->access == AccessMode::ReadOnly ||
                 transient->access == AccessMode::Mutable) &&
                transient->carriers.empty() && !transient->escapes &&
                loanIsActive(scopes, transientId) &&
                sameBorrowSource(transient->source, desiredSource) &&
                (transientChain.empty() ? transient->producedBy == valueId
                                        : transient->producedBy != 0);
            if (!compatible) {
              break;
            }
            transientChain.push_back(transientId);
            transientId = transient->parent;
          }
          compatible =
              compatible && transientId == parent && !transientChain.empty();
          if (!compatible) {
            valid = false;
          } else {
            for (const MirLoanId transient : transientChain) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = valueId,
                                       .loan = transient});
              removeLoanFromState(transient, scopes, bindingLoans);
            }
          }
        }
      } else if (existing != 0) {
        const MirLoan &sourceLoan = output.loans[existing - 1];
        if (sourceLoan.kind == MirLoanKind::CallResult) {
          std::vector<MirLoanId> transientChain;
          std::unordered_set<MirLoanId> visited;
          MirLoanId transient = existing;
          bool compatible = true;
          while (transient != 0 && compatible &&
                 visited.insert(transient).second) {
            const MirLoan &candidate = output.loans[transient - 1];
            compatible = candidate.kind == MirLoanKind::CallResult &&
                         candidate.carriers.empty() && !candidate.escapes &&
                         loanIsActive(scopes, transient);
            if (compatible) {
              transientChain.push_back(transient);
              transient = candidate.parent;
            }
          }
          if (!compatible || transient != 0 || transientChain.empty()) {
            valid = false;
          } else {
            for (const MirLoanId ended : transientChain) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = valueId,
                                       .loan = ended});
              removeLoanFromState(ended, scopes, bindingLoans);
            }
          }
        } else if (!sourceLoan.entry ||
                   sourceLoan.semanticLoan == desired->semanticLoan) {
          recordLoanIdentity(existing, *desired, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
      }

      const MirPlaceId sourcePlace = placeForLoan(*desired);
      if (sourcePlace == 0) {
        return {};
      }
      const MirLoanId loan =
          createLoan(MirLoanKind::Local, sourcePlace, desired->access, valueId,
                     binding, 0, parent);
      recordLoanIdentity(loan, *desired, binding);
      MirOperand sourceOperand;
      if (parent != 0) {
        sourceOperand = {.kind = MirOperandKind::Loan,
                         .loan = parent,
                         .type = value->info.type};
      } else {
        sourceOperand = {.kind = desired->access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = sourcePlace,
                         .type = value->info.type};
      }
      (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                               .hirValue = valueId,
                               .operands = {sourceOperand},
                               .loan = loan,
                               .info = value->info});
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
      valueLoans.insert_or_assign(valueId, loan);
      return {
          .kind = MirOperandKind::Loan, .loan = loan, .type = value->info.type};
    }

    if (existing != 0) {
      std::vector<HirBindingId> &carriers = output.loans[existing - 1].carriers;
      if (binding != 0 && std::find(carriers.begin(), carriers.end(),
                                    binding) == carriers.end()) {
        carriers.push_back(binding);
      }
      return {.kind = MirOperandKind::Loan,
              .loan = existing,
              .type = value->info.type};
    }
    const MirPlaceId sourcePlace = placeForValue(valueId);
    const MirLoanId loan = createLoan(MirLoanKind::Local, sourcePlace,
                                      value->info.access, valueId, binding);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Borrow,
         .hirValue = valueId,
         .operands = {{.kind = value->info.access == AccessMode::Mutable
                                   ? MirOperandKind::BorrowWrite
                                   : MirOperandKind::BorrowRead,
                       .place = sourcePlace,
                       .type = value->info.type}},
         .loan = loan,
         .info = value->info});
    if (!scopes.empty()) {
      scopes.back().loans.push_back(loan);
    }
    return {
        .kind = MirOperandKind::Loan, .loan = loan, .type = value->info.type};
  }

  void markStoredBorrow(HirValueId valueId, SymbolId field, AccessMode access) {
    if (const MirLoanId sourceLoan = loanForValue(valueId); sourceLoan != 0) {
      const MirPlaceId sourcePlace = output.loans[sourceLoan - 1].source;
      const MirLoanId stored = createLoan(MirLoanKind::Stored, sourcePlace,
                                          access, valueId, 0, field);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Borrow,
           .hirValue = valueId,
           .operands = {{.kind = access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = sourcePlace,
                         .type = findValue(valueId) == nullptr
                                     ? SemanticType::Unknown
                                     : findValue(valueId)->info.type}},
           .loan = stored,
           .info = findValue(valueId) == nullptr ? ExpressionInfo{}
                                                 : findValue(valueId)->info});
      output.loans[stored - 1].escapes = true;
      return;
    }
    MirOperand borrow = referenceOperand(valueId);
    if (borrow.loan == 0 || borrow.loan > output.loans.size()) {
      valid = false;
      return;
    }
    MirLoan &loan = output.loans[borrow.loan - 1];
    loan.kind = MirLoanKind::Stored;
    loan.access = access;
    loan.storedField = field;
    loan.escapes = true;
    for (Scope &scope : scopes) {
      std::erase(scope.loans, borrow.loan);
    }
  }

  void registerDrop(HirBindingId bindingId, MirPlaceId place) {
    if (scopes.empty() || !tracksLocalDrops()) {
      return;
    }
    const HirBinding *binding = findBinding(bindingId);
    if (binding != nullptr &&
        binding->info.type.kind != SemanticType::Reference &&
        binding->info.traits.drop == DropKind::Lexical &&
        !binding->info.staticStorage) {
      if (dropObligationForBinding(bindingId) == 0) {
        valid = false;
        return;
      }
      scopes.back().drops.push_back(place);
    }
  }

  [[nodiscard]] bool tracksLocalDrops() const {
    return output.kind == MirBodyKind::Function ||
           output.kind == MirBodyKind::Constructor ||
           output.kind == MirBodyKind::Destructor ||
           output.kind == MirBodyKind::Lambda;
  }

  void seedParameterDrops() {
    if (!tracksLocalDrops()) {
      return;
    }
    for (const HirBinding &binding : source.bindings) {
      if (binding.parameter == nullptr) {
        continue;
      }
      registerDrop(binding.id, placeForBinding(binding.id));
    }
  }

  void seedEntryLoans() {
    if (scopes.empty()) {
      return;
    }
    for (const HirLoan &loan : source.loans) {
      if (!loan.entry) {
        continue;
      }
      if (loan.semanticLoan == 0 || loan.parent != 0 || loan.carriers.empty()) {
        valid = false;
        continue;
      }
      const MirPlaceId sourcePlace = placeForLoan(loan);
      if (sourcePlace == 0) {
        continue;
      }
      const MirLoanId lowered =
          createLoan(MirLoanKind::Parameter, sourcePlace, loan.access);
      output.loans[lowered - 1].entry = true;
      recordLoanIdentity(lowered, loan);
      for (const HirBindingId carrier : loan.carriers) {
        bindingLoans.insert_or_assign(carrier, lowered);
      }
      scopes.front().loans.push_back(lowered);
    }
  }

  void seedReturnBorrow() {
    if (function == nullptr ||
        function->returnBorrowOrigin != BorrowOriginKind::Argument ||
        function->source == nullptr || function->source->body() == nullptr) {
      return;
    }
    if (function->returnBorrowParameter >= function->parameterBindings.size()) {
      return;
    }
    const HirBindingId binding =
        function->parameterBindings[function->returnBorrowParameter];
    if (bindingLoans.contains(binding)) {
      return;
    }
    const MirPlaceId sourcePlace = placeForBinding(binding);
    if (sourcePlace == 0 || scopes.empty()) {
      return;
    }
    const MirLoanId loan = createLoan(MirLoanKind::Parameter, sourcePlace,
                                      function->returnBorrowAccess, 0, binding);
    output.loans[loan - 1].entry = true;
    bindingLoans.insert_or_assign(binding, loan);
    scopes.front().loans.push_back(loan);
  }

  void emitScope(const Scope &scope) {
    for (auto loan = scope.loans.rbegin(); loan != scope.loans.rend(); ++loan) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
    }
    std::vector<MirDropObligationId> cleanup;
    cleanup.reserve(scope.drops.size());
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation =
          place != nullptr && place->root == MirPlaceRootKind::Binding
              ? dropObligationForBinding(place->binding)
              : 0;
      (void)appendInstruction(
          {.kind = MirInstructionKind::Drop,
           .destination = *drop,
           .info = place == nullptr
                       ? ExpressionInfo{}
                       : ExpressionInfo{.type = place->type,
                                        .category = ValueCategory::Place,
                                        .access = place->access,
                                        .traits = place->traits},
           .lifecycle = obligation == 0
                            ? std::vector<MirLifecycleEvent>{}
                            : std::vector<MirLifecycleEvent>{
                                  {.kind = MirLifecycleEventKind::Drop,
                                   .source = obligation}}});
      if (obligation != 0) {
        cleanup.push_back(obligation);
      }
    }
    if (!cleanup.empty()) {
      const std::size_t id = output.cleanupBoundaries.size() + 1;
      output.cleanupBoundaries.push_back(
          {.id = id, .obligations = std::move(cleanup)});
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle, .cleanupBoundaryEnd = id});
    }
  }

  void emitScopeExit(std::size_t keepScopes) {
    for (std::size_t depth = scopes.size(); depth > keepScopes; --depth) {
      emitScope(scopes[depth - 1]);
    }
  }

  void lowerStatements(const std::vector<HirStatementId> &ids) {
    for (const HirStatementId id : ids) {
      if (terminated()) {
        return;
      }
      lowerStatement(id);
    }
  }

  void lowerNestedStatement(const std::optional<HirStatementId> &id) {
    if (id) {
      lowerStatement(*id);
    }
  }

  void lowerScopedStatement(const std::optional<HirStatementId> &id) {
    scopes.push_back({});
    lowerNestedStatement(id);
    if (!terminated()) {
      emitScope(scopes.back());
    }
    scopes.pop_back();
  }

  void lowerStatement(HirStatementId id) {
    const HirStatement *statement = findStatement(id);
    if (statement == nullptr) {
      valid = false;
      return;
    }
    switch (statement->kind) {
    case HirStatementKind::Block:
      scopes.push_back({});
      lowerStatements(statement->statements);
      if (!terminated()) {
        emitScope(scopes.back());
      }
      scopes.pop_back();
      return;
    case HirStatementKind::CompileTimeBranch:
      lowerStatements(statement->statements);
      return;
    case HirStatementKind::Expression:
      if (statement->value) {
        const std::vector<Scope> incomingScopes = scopes;
        const std::vector<TemporaryDrop> incomingTemporaryDrops =
            temporaryDrops;
        emitValue(*statement->value);
        endFullExpressionLoans(incomingScopes);
        emitTemporaryDrops(incomingTemporaryDrops, *statement->value,
                           statement->id);
      }
      endSemanticLoans(*statement);
      return;
    case HirStatementKind::DoWhile:
      lowerDoWhile(*statement);
      return;
    case HirStatementKind::Variable:
      lowerVariable(*statement);
      return;
    case HirStatementKind::StructuredBinding:
      lowerStructuredBinding(*statement);
      return;
    case HirStatementKind::If:
      lowerIf(*statement);
      return;
    case HirStatementKind::While:
      lowerWhile(*statement);
      return;
    case HirStatementKind::For:
      lowerFor(*statement);
      return;
    case HirStatementKind::RangeFor:
      lowerNestedStatement(statement->body);
      return;
    case HirStatementKind::Switch:
      lowerSwitch(*statement);
      return;
    case HirStatementKind::Break:
      if (breakContexts.empty()) {
        valid = false;
        return;
      }
      endSemanticLoansIfActive(breakContexts.back().exitLoans, id);
      emitScopeExit(breakContexts.back().keepScopes);
      terminate({.kind = MirTerminatorKind::Goto,
                 .hirStatement = id,
                 .target = breakContexts.back().target});
      return;
    case HirStatementKind::Continue:
      if (continueContexts.empty()) {
        valid = false;
        return;
      }
      emitScopeExit(continueContexts.back().keepScopes);
      terminate({.kind = MirTerminatorKind::Goto,
                 .hirStatement = id,
                 .target = continueContexts.back().target});
      return;
    case HirStatementKind::Return:
      lowerReturn(*statement);
      return;
    case HirStatementKind::Empty:
      return;
    }
  }

  void lowerVariable(const HirStatement &statement) {
    if (!statement.binding) {
      return;
    }
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const HirBinding *binding = findBinding(*statement.binding);
    const MirPlaceId destination = placeForBinding(*statement.binding);
    std::vector<MirOperand> operands;
    MirLoanId retainedLoan = 0;
    if (statement.value) {
      if (binding != nullptr &&
          binding->info.type.kind == SemanticType::Reference) {
        MirOperand reference =
            referenceOperand(*statement.value, *statement.binding);
        retainedLoan = reference.loan;
        operands.push_back(std::move(reference));
      } else {
        operands.push_back(valueOperand(*statement.value));
        if (binding != nullptr && binding->info.traits.containsBorrowedState) {
          retainedLoan = loanForValue(*statement.value);
        }
      }
    }
    if (retainedLoan != 0) {
      if (binding != nullptr && binding->info.retainedLoan != 0) {
        const SemanticLoanId semanticLoan = binding->info.retainedLoan;
        if (const auto found = semanticLoans.find(semanticLoan);
            found != semanticLoans.end() && found->second != retainedLoan) {
          const MirLoan *existing = output.findLoan(found->second);
          MirLoan *candidate =
              retainedLoan == 0 || retainedLoan > output.loans.size()
                  ? nullptr
                  : &output.loans[retainedLoan - 1];
          if (existing != nullptr && candidate != nullptr &&
              loanIsActive(scopes, existing->id) &&
              (existing->access == candidate->access ||
               (existing->access == AccessMode::Mutable &&
                candidate->access == AccessMode::ReadOnly))) {
            candidate->source = existing->source;
            retainedLoan = existing->id;
          } else {
            valid = false;
          }
        } else if (found == semanticLoans.end()) {
          semanticLoans.emplace(semanticLoan, retainedLoan);
        }
        MirLoan &retained = output.loans[retainedLoan - 1];
        if (retained.semanticLoan != 0 &&
            retained.semanticLoan != semanticLoan) {
          valid = false;
        } else {
          retained.semanticLoan = semanticLoan;
        }
      }
      bindingLoans.insert_or_assign(*statement.binding, retainedLoan);
      std::vector<HirBindingId> &carriers =
          output.loans[retainedLoan - 1].carriers;
      if (std::find(carriers.begin(), carriers.end(), *statement.binding) ==
          carriers.end()) {
        carriers.push_back(*statement.binding);
      }
    } else if (binding != nullptr &&
               binding->info.traits.containsBorrowedState && statement.value) {
      valid = false;
    }
    MirInstruction initialize{
        .kind = MirInstructionKind::Initialize,
        .hirStatement = statement.id,
        .destination = destination,
        .operands = std::move(operands),
        .info = binding == nullptr
                    ? ExpressionInfo{}
                    : ExpressionInfo{.type = binding->info.type,
                                     .category = ValueCategory::Place,
                                     .access = binding->info.access,
                                     .traits = binding->info.traits}};
    const MirDropObligationId target =
        dropObligationForBinding(*statement.binding);
    const MirDropObligationId sourceObligation =
        statement.value ? dropObligationForValue(*statement.value) : 0;
    if (target != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation, target);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = target});
      }
    } else if (temporaryIsActive(sourceObligation) &&
               (output.kind == MirBodyKind::Module ||
                output.kind == MirBodyKind::FieldInitializers ||
                output.kind == MirBodyKind::StaticFieldInitializers)) {
      // Persistent storage is owned outside this body, so it has no local drop
      // obligation to reparent into. The Initialize still consumes the exact
      // source temporary and transfers that lifetime into the external owner.
      appendLifecycle(initialize, {.kind = MirLifecycleEventKind::TransferOut,
                                   .source = sourceObligation});
      (void)removeTemporary(sourceObligation);
    }
    (void)appendInstruction(std::move(initialize));
    registerDrop(*statement.binding, destination);
    endFullExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    endSemanticLoans(statement);
  }

  void lowerStructuredBinding(const HirStatement &statement) {
    if (!statement.binding) {
      valid = false;
      return;
    }
    const HirBinding *sourceBinding = findBinding(*statement.binding);
    const MirPlaceId sourcePlaceId = placeForBinding(*statement.binding);
    const MirPlace *sourcePlace = output.findPlace(sourcePlaceId);
    if (sourceBinding == nullptr || sourcePlace == nullptr ||
        !statement.value) {
      valid = false;
      return;
    }
    const MirPlace sourceSnapshot = *sourcePlace;
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;

    MirInstruction initialize{.kind = MirInstructionKind::Initialize,
                              .hirStatement = statement.id,
                              .destination = sourcePlaceId,
                              .operands = {valueOperand(*statement.value)},
                              .info = {.type = sourceBinding->info.type,
                                       .category = ValueCategory::Place,
                                       .access = sourceBinding->info.access,
                                       .traits = sourceBinding->info.traits}};
    const MirDropObligationId target =
        dropObligationForBinding(*statement.binding);
    const MirDropObligationId sourceObligation =
        dropObligationForValue(*statement.value);
    if (target != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation, target);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = target});
      }
    }
    (void)appendInstruction(std::move(initialize));
    registerDrop(*statement.binding, sourcePlaceId);

    for (const HirStructuredBindingElement &element :
         statement.structuredBindings) {
      const HirBinding *binding = findBinding(element.binding);
      if (binding == nullptr) {
        valid = false;
        continue;
      }
      MirPlace projected = sourceSnapshot;
      projected.id = 0;
      projected.symbol = binding->info.symbol;
      projected.type = binding->info.type;
      projected.access = binding->info.access;
      projected.traits = binding->info.traits;
      projected.sourceValue = statement.value.value_or(0);
      if (element.projection == HirStructuredBindingProjectionKind::Field) {
        if (element.field == 0) {
          valid = false;
          continue;
        }
        projected.projections.push_back(
            {.kind = MirProjectionKind::Field, .field = element.field});
      } else {
        if (!element.index) {
          valid = false;
          continue;
        }
        emitValue(*element.index);
        projected.projections.push_back({.kind = MirProjectionKind::Index,
                                         .index = mirValueFor(*element.index)});
      }
      bindingPlaces.insert_or_assign(element.binding,
                                     appendPlace(std::move(projected)));
    }
    endFullExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, *statement.value, statement.id);
  }

  void lowerIf(const HirStatement &statement) {
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    const MirBlockId thenBlock = appendBlock();
    const MirBlockId elseBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = thenBlock,
               .elseTarget = elseBlock});

    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> incomingBindingLoans =
        bindingLoans;
    current = thenBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    endSemanticLoans(statement.thenEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.body);
    const bool thenFallsThrough = !terminated();
    const std::vector<Scope> thenScopes = scopes;
    const std::vector<TemporaryDrop> thenTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> thenBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);
    if (thenFallsThrough) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

    current = elseBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    endSemanticLoans(statement.elseEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.elseBranch);
    const bool elseFallsThrough = !terminated();
    const std::vector<Scope> elseScopes = scopes;
    const std::vector<TemporaryDrop> elseTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> elseBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);
    if (elseFallsThrough) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

    current = mergeBlock;
    if (thenFallsThrough && elseFallsThrough) {
      if (!sameScopeState(thenScopes, elseScopes) ||
          thenBindingLoans != elseBindingLoans ||
          thenTemporaryDrops != elseTemporaryDrops) {
        valid = false;
      }
      scopes = thenScopes;
      temporaryDrops = thenTemporaryDrops;
      bindingLoans = thenBindingLoans;
    } else if (thenFallsThrough) {
      scopes = thenScopes;
      temporaryDrops = thenTemporaryDrops;
      bindingLoans = thenBindingLoans;
    } else if (elseFallsThrough) {
      scopes = elseScopes;
      temporaryDrops = elseTemporaryDrops;
      bindingLoans = elseBindingLoans;
    } else {
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      bindingLoans = incomingBindingLoans;
      terminate({.kind = MirTerminatorKind::Unreachable,
                 .hirStatement = statement.id});
      return;
    }
    endSemanticLoans(statement);
  }

  void lowerWhile(const HirStatement &statement) {
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.endedLoans.empty() ? exitBlock : appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = bodyBlock,
               .elseTarget = naturalExitBlock});

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    std::vector<Scope> exitScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    if (naturalExitBlock != exitBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = conditionBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();
    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
  }

  void lowerDoWhile(const HirStatement &statement) {
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.endedLoans.empty() ? exitBlock : appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = bodyBlock});

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    std::vector<Scope> exitScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = conditionBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();

    current = conditionBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = bodyBlock,
               .elseTarget = naturalExitBlock});

    if (naturalExitBlock != exitBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
  }

  void lowerFor(const HirStatement &statement) {
    scopes.push_back({});
    lowerNestedStatement(statement.initializer);
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId incrementBlock = appendBlock();
    const MirBlockId cleanupBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.condition && !statement.endedLoans.empty() ? appendBlock()
                                                             : cleanupBlock;
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    if (statement.condition) {
      const std::vector<Scope> conditionScopes = scopes;
      const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
      const MirOperand condition = conditionOperand(*statement.condition);
      endFullExpressionLoans(conditionScopes);
      emitTemporaryDrops(conditionTemporaryDrops, *statement.condition,
                         statement.id);
      terminate({.kind = MirTerminatorKind::Branch,
                 .hirStatement = statement.id,
                 .value = condition,
                 .target = bodyBlock,
                 .elseTarget = naturalExitBlock});
    } else {
      terminate({.kind = MirTerminatorKind::Goto, .target = bodyBlock});
    }

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    std::vector<Scope> cleanupScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> cleanupBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, cleanupScopes,
                               cleanupBindingLoans);
    if (naturalExitBlock != cleanupBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = cleanupBlock});
    }

    breakContexts.push_back({.target = cleanupBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = incrementBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = incrementBlock});
    }

    current = incrementBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    if (statement.increment) {
      const std::vector<Scope> incrementScopes = scopes;
      const std::vector<TemporaryDrop> incrementTemporaryDrops = temporaryDrops;
      emitValue(*statement.increment);
      endFullExpressionLoans(incrementScopes);
      emitTemporaryDrops(incrementTemporaryDrops, *statement.increment,
                         statement.id);
    }
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();

    current = cleanupBlock;
    scopes = std::move(cleanupScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(cleanupBindingLoans);
    emitScope(scopes.back());
    terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    scopes.pop_back();
    current = exitBlock;
    temporaryDrops = loopTemporaryDrops;
  }

  void lowerSwitch(const HirStatement &statement) {
    const std::vector<Scope> subjectScopes = scopes;
    const std::vector<TemporaryDrop> subjectTemporaryDrops = temporaryDrops;
    const MirOperand subject =
        statement.value ? valueOperand(*statement.value) : MirOperand{};
    endFullExpressionLoans(subjectScopes);
    emitTemporaryDrops(subjectTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    const MirBlockId exitBlock = appendBlock();
    std::vector<MirBlockId> armBlocks;
    armBlocks.reserve(statement.switchArms.size());
    for (std::size_t index = 0; index < statement.switchArms.size(); ++index) {
      armBlocks.push_back(appendBlock());
    }
    const bool hasDefault =
        std::any_of(statement.switchArms.begin(), statement.switchArms.end(),
                    [](const HirSwitchArm &arm) {
                      return std::any_of(arm.labels.begin(), arm.labels.end(),
                                         [](const HirSwitchLabel &label) {
                                           return label.isDefault;
                                         });
                    });
    const MirBlockId unmatchedBlock =
        !hasDefault && !statement.endedLoans.empty() ? appendBlock()
                                                     : exitBlock;

    MirTerminator terminator{.kind = MirTerminatorKind::Switch,
                             .hirStatement = statement.id,
                             .value = subject,
                             .target = unmatchedBlock};
    for (std::size_t armIndex = 0; armIndex < statement.switchArms.size();
         ++armIndex) {
      for (const HirSwitchLabel &label :
           statement.switchArms[armIndex].labels) {
        if (label.isDefault) {
          terminator.target = armBlocks[armIndex];
        } else {
          terminator.switchTargets.push_back(
              {.value = label.constant, .target = armBlocks[armIndex]});
        }
      }
    }
    terminate(std::move(terminator));

    const std::vector<Scope> switchScopes = scopes;
    const std::vector<TemporaryDrop> switchTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> switchBindingLoans =
        bindingLoans;
    std::vector<Scope> exitScopes = switchScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        switchBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    if (hasDefault && !statement.switchArms.empty()) {
      std::vector<SemanticLoanId> endedOnEveryArm =
          statement.switchArms.front().entryEndedLoans;
      std::erase_if(endedOnEveryArm, [&](SemanticLoanId loan) {
        return std::any_of(
                   statement.switchArms.begin() + 1, statement.switchArms.end(),
                   [&](const HirSwitchArm &arm) {
                     return std::find(arm.entryEndedLoans.begin(),
                                      arm.entryEndedLoans.end(),
                                      loan) == arm.entryEndedLoans.end();
                   }) ||
               std::find(statement.endedLoans.begin(),
                         statement.endedLoans.end(),
                         loan) != statement.endedLoans.end();
      });
      for (const MirLoanId loan : childFirstLoans(endedOnEveryArm)) {
        if (loanIsActive(exitScopes, loan)) {
          removeLoanFromState(loan, exitScopes, exitBindingLoans);
        }
      }
    }
    if (unmatchedBlock != exitBlock) {
      current = unmatchedBlock;
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = switchScopes.size(),
                             .exitLoans = statement.endedLoans});
    for (std::size_t armIndex = 0; armIndex < statement.switchArms.size();
         ++armIndex) {
      current = armBlocks[armIndex];
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
      scopes.push_back({});
      endSemanticLoans(statement.switchArms[armIndex].entryEndedLoans,
                       statement.id);
      lowerStatements(statement.switchArms[armIndex].statements);
      if (!terminated()) {
        if (temporaryDrops != switchTemporaryDrops) {
          valid = false;
        }
        endSemanticLoansIfActive(statement.endedLoans, statement.id);
        emitScope(scopes.back());
        terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
      }
      scopes.pop_back();
    }
    breakContexts.pop_back();
    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = switchTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
  }

  void lowerReturn(const HirStatement &statement) {
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    std::optional<MirOperand> result;
    std::optional<MirLoanId> returnLoan;
    if (statement.value) {
      if (output.returnType.kind == SemanticType::Reference) {
        MirOperand borrow = referenceOperand(*statement.value);
        if (borrow.loan != 0) {
          if (function != nullptr &&
              function->returnBorrowOrigin != BorrowOriginKind::None &&
              borrow.loan <= output.loans.size() &&
              output.loans[borrow.loan - 1].kind == MirLoanKind::CallResult) {
            const MirPlaceId sourcePlace = output.loans[borrow.loan - 1].source;
            std::vector<MirLoanId> transientChain;
            std::unordered_set<MirLoanId> visited;
            MirLoanId transient = borrow.loan;
            while (transient != 0 && transient <= output.loans.size() &&
                   visited.insert(transient).second) {
              const MirLoan &candidate = output.loans[transient - 1];
              if (candidate.kind != MirLoanKind::CallResult ||
                  !candidate.carriers.empty() || candidate.escapes ||
                  !loanIsActive(scopes, transient)) {
                break;
              }
              transientChain.push_back(transient);
              transient = candidate.parent;
            }
            MirLoanId summarizedEntry = 0;
            if (function->returnBorrowOrigin == BorrowOriginKind::Argument &&
                function->returnBorrowParameter <
                    function->parameterBindings.size()) {
              const auto found = bindingLoans.find(
                  function->parameterBindings[function->returnBorrowParameter]);
              summarizedEntry = found == bindingLoans.end() ? 0 : found->second;
            }
            const bool validTail =
                function->returnBorrowOrigin == BorrowOriginKind::Receiver
                    ? transient == 0
                    : summarizedEntry != 0 && transient == summarizedEntry;
            if (!validTail || transientChain.empty()) {
              valid = false;
            } else {
              for (const MirLoanId ended : transientChain) {
                (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                         .hirValue = *statement.value,
                                         .loan = ended});
                removeLoanFromState(ended, scopes, bindingLoans);
              }
              if (summarizedEntry != 0) {
                (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                         .hirValue = *statement.value,
                                         .loan = summarizedEntry});
                removeLoanFromState(summarizedEntry, scopes, bindingLoans);
              }
              const HirValue *value = findValue(*statement.value);
              const MirLoanId normalized =
                  createLoan(MirLoanKind::Return, sourcePlace,
                             function->returnBorrowAccess, *statement.value);
              (void)appendInstruction(
                  {.kind = MirInstructionKind::Borrow,
                   .hirValue = *statement.value,
                   .operands = {{.kind = function->returnBorrowAccess ==
                                                 AccessMode::Mutable
                                             ? MirOperandKind::BorrowWrite
                                             : MirOperandKind::BorrowRead,
                                 .place = sourcePlace,
                                 .type = output.returnType}},
                   .loan = normalized,
                   .info = value == nullptr ? ExpressionInfo{} : value->info});
              borrow.loan = normalized;
            }
          }
          MirLoan &loan = output.loans[borrow.loan - 1];
          loan.kind = MirLoanKind::Return;
          loan.escapes = true;
          for (Scope &scope : scopes) {
            std::erase(scope.loans, borrow.loan);
          }
          returnLoan = borrow.loan;
        }
        result = borrow;
      } else {
        result = valueOperand(*statement.value);
        const HirValue *value = findValue(*statement.value);
        if (value != nullptr && value->info.traits.containsBorrowedState) {
          const MirLoanId retained = loanForValue(*statement.value);
          if (retained == 0 || retained > output.loans.size()) {
            valid = false;
          } else {
            MirLoan &loan = output.loans[retained - 1];
            loan.kind = MirLoanKind::Return;
            loan.escapes = true;
            for (Scope &scope : scopes) {
              std::erase(scope.loans, retained);
            }
            returnLoan = retained;
          }
        }
      }
    }
    if (statement.value && output.returnType.kind != SemanticType::Reference) {
      emitTemporaryTransfer(*statement.value, statement.id);
    }
    endReturnExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    emitScopeExit(0);
    terminate({.kind = MirTerminatorKind::Return,
               .hirStatement = statement.id,
               .value = std::move(result),
               .returnLoan = returnLoan});
  }

  [[nodiscard]] bool validateSourceProvenance() const {
    return std::all_of(output.places.begin(), output.places.end(),
                       [&](const MirPlace &place) {
                         return place.sourceValue == 0 ||
                                findValue(place.sourceValue) != nullptr;
                       }) &&
           std::all_of(output.values.begin(), output.values.end(),
                       [&](const MirValue &value) {
                         return value.sourceValue != 0 &&
                                findValue(value.sourceValue) != nullptr;
                       });
  }

  const HirProgram &program;
  const HirBody &source;
  bool implicitZeroReturn = false;
  const HirFunctionInstance *function = nullptr;
  MirBody output;
  MirBlockId current = 0;
  MirInstructionId nextInstruction = 1;
  MirTemporaryId nextTemporary = 1;
  bool valid = true;
  std::unordered_map<HirValueId, const HirValue *> values;
  std::unordered_map<HirStatementId, const HirStatement *> statements;
  std::unordered_map<HirBindingId, const HirBinding *> bindings;
  std::unordered_map<SymbolId, HirBindingId> localSymbols;
  std::unordered_map<HirBindingId, MirPlaceId> bindingPlaces;
  std::unordered_map<HirDropObligationId, MirDropObligationId> dropObligations;
  std::unordered_map<HirFullExpressionId, HirFullExpressionId>
      fullExpressionIds;
  std::unordered_map<HirValueId, MirValueId> loweredValues;
  std::unordered_map<HirValueId, MirValueId> contextualValues;
  std::unordered_map<HirValueId, MirValueId> materializedValues;
  std::unordered_map<HirValueId, MirPlaceId> valuePlaces;
  std::unordered_map<HirValueId, MirLoanId> valueLoans;
  std::unordered_map<HirBindingId, MirLoanId> bindingLoans;
  std::unordered_map<SemanticLoanId, MirLoanId> semanticLoans;
  std::unordered_map<SemanticLoanId, MirPlaceId> semanticLoanPlaces;
  std::unordered_set<HirValueId> emittedValues;
  std::vector<Scope> scopes;
  std::vector<TemporaryDrop> temporaryDrops;
  std::vector<BreakContext> breakContexts;
  std::vector<ContinueContext> continueContexts;
};

class MirLowerer {
public:
  [[nodiscard]] MirLoweringResult lower(const HirProgram &source) const {
    MirLoweringResult result;
    if (!source.valid()) {
      result.program.valid_ = false;
      return result;
    }

    bool valid = true;
    result.program.executionProfile_ = source.executionProfile();
    result.program.moduleBody =
        lowerBody(source, source.module(), MirBodyKind::Module,
                  SemanticType::Void, {}, valid);

    result.program.classes.reserve(source.classInstances().size());
    for (const HirClassInstance &instance : source.classInstances()) {
      MirClassInstance lowered{
          .id = instance.id,
          .declaration = instance.declaration,
          .type = instance.type,
          .kind = instance.kind,
          .bases = instance.bases,
          .structuralBases = instance.bases,
          .abstract = instance.abstract,
          .polymorphic = instance.polymorphic,
          .cAbiRecord = instance.cAbiRecord,
          .cAbiLayout = instance.cAbiLayout,
          .destructor = instance.destructor,
          .requiresActiveDropState = instance.requiresActiveDropState,
          .requiresActiveCleanup = instance.requiresActiveCleanup};
      lowered.fields.reserve(instance.fields.size());
      for (const HirClassField &field : instance.fields) {
        if (field.info.traits.drop != DropKind::Lexical) {
          continue;
        }
        lowered.fields.push_back(
            {.field = field.binding,
             .symbol = field.info.symbol,
             .type = field.info.type,
             .dropKind = field.info.traits.drop,
             .requiresActiveCleanup = field.requiresActiveCleanup});
      }
      lowered.fieldInitializers = lowerBody(source, instance.fieldInitializers,
                                            MirBodyKind::FieldInitializers,
                                            SemanticType::Void, {}, valid);
      lowered.staticFieldInitializers = lowerBody(
          source, instance.staticFieldInitializers,
          MirBodyKind::StaticFieldInitializers, SemanticType::Void, {}, valid);
      for (auto field = instance.fields.rbegin();
           field != instance.fields.rend(); ++field) {
        if (field->info.traits.drop == DropKind::Lexical) {
          lowered.fieldDropOrder.push_back(
              {.field = field->binding,
               .symbol = field->info.symbol,
               .type = field->info.type,
               .requiresActiveCleanup = field->requiresActiveCleanup});
        }
      }
      result.program.classes.push_back(std::move(lowered));
    }

    result.program.functions.reserve(source.functionInstances().size());
    for (const HirFunctionInstance &instance : source.functionInstances()) {
      const bool implicitZeroReturn = !instance.owner &&
                                      instance.source != nullptr &&
                                      instance.source->name().lexeme == "main";
      std::vector<MirCallableParameter> callableParameters;
      callableParameters.reserve(instance.callableParameters.size());
      for (const HirCallableParameter &parameter :
           instance.callableParameters) {
        MirCallableParameter lowered{.parameterIndex = parameter.parameterIndex,
                                     .callableType = parameter.callableType,
                                     .access = parameter.access,
                                     .nonEscaping = parameter.nonEscaping};
        lowered.signatures.reserve(parameter.signatures.size());
        for (const HirCallableSignature &signature : parameter.signatures) {
          lowered.signatures.push_back(
              {.returnType = signature.returnType,
               .parameterTypes = signature.parameterTypes,
               .functionTarget = signature.functionTarget,
               .lambdaTarget = signature.lambdaTarget});
        }
        lowered.forwardings.reserve(parameter.forwardings.size());
        for (const HirCallableForwarding &forwarding : parameter.forwardings) {
          lowered.forwardings.push_back(
              {.parameterIndex = forwarding.parameterIndex,
               .functionTarget = forwarding.functionTarget});
        }
        callableParameters.emplace_back(std::move(lowered));
      }
      result.program.functions.push_back(
          {.id = instance.id,
           .owner = instance.owner,
           .returnType = instance.returnType,
           .parameterTypes = instance.parameterTypes,
           .parameterBindings = instance.parameterBindings,
           .entryKind = instance.entryKind,
           .entryArgumentAppendTarget = instance.entryArgumentAppendTarget,
           .staticMember = instance.staticMember,
           .constexprFunction = instance.constexprFunction,
           .returnBorrowOrigin = instance.returnBorrowOrigin,
           .returnBorrowParameter = instance.returnBorrowParameter,
           .returnBorrowAccess = instance.returnBorrowAccess,
           .linkage = instance.linkage,
           .externalSymbol = instance.externalSymbol,
           .virtualMethod = instance.virtualMethod,
           .pureVirtual = instance.pureVirtual,
           .overrideMethod = instance.overrideMethod,
           .virtualRoots = instance.virtualRoots,
           .callableParameters = std::move(callableParameters),
           .body = lowerBody(source, instance.body, MirBodyKind::Function,
                             instance.returnType, {}, valid, implicitZeroReturn,
                             nullptr, &instance)});
    }

    result.program.constructors.reserve(source.constructorInstances().size());
    for (const HirConstructorInstance &instance :
         source.constructorInstances()) {
      std::vector<MirConstructorInitializer> initializers;
      initializers.reserve(instance.initializers.size());
      for (const HirConstructorInitializer &initializer :
           instance.initializers) {
        initializers.push_back(
            {.kind = initializer.kind,
             .targetType = initializer.targetType,
             .field = initializer.field,
             .base = initializer.base,
             .constructorTarget = initializer.constructorTarget,
             .arguments = initializer.arguments,
             .storesReference = initializer.storesReference,
             .borrowAccess = initializer.borrowAccess,
             .generatedDefault = initializer.generatedDefault});
      }
      result.program.constructors.push_back(
          {.id = instance.id,
           .owner = instance.owner,
           .parameterTypes = instance.parameterTypes,
           .parameterBindings = instance.parameterBindings,
           .borrowOrigin = instance.borrowOrigin,
           .borrowParameter = instance.borrowParameter,
           .borrowAccess = instance.borrowAccess,
           .initializers = std::move(initializers),
           .body = lowerBody(source, instance.body, MirBodyKind::Constructor,
                             SemanticType::Void, instance.initializerValues,
                             valid, false, &instance.initializers)});
    }

    result.program.destructors.reserve(source.destructorInstances().size());
    for (const HirDestructorInstance &instance : source.destructorInstances()) {
      result.program.destructors.push_back(
          {.id = instance.id,
           .owner = instance.owner,
           .body = lowerBody(source, instance.body, MirBodyKind::Destructor,
                             SemanticType::Void, {}, valid)});
    }

    result.program.lambdas.reserve(source.lambdaInstances().size());
    for (const HirLambda &instance : source.lambdaInstances()) {
      MirLambdaInstance lowered{.id = instance.id,
                                .declaration = instance.declaration,
                                .parameterTypes = instance.parameterTypes};
      lowered.captureTypes.reserve(instance.captures.size());
      lowered.captureRequiresActiveCleanup.reserve(instance.captures.size());
      for (std::size_t capture = 0; capture < instance.captures.size();
           ++capture) {
        lowered.captureTypes.push_back(instance.captures[capture].type);
        lowered.captureRequiresActiveCleanup.push_back(
            capture < instance.captureRequiresActiveCleanup.size()
                ? instance.captureRequiresActiveCleanup[capture]
                : false);
      }
      lowered.body = lowerBody(source, instance.body, MirBodyKind::Lambda,
                               instance.returnType, {}, valid);
      result.program.lambdas.push_back(std::move(lowered));
    }
    result.program.valid_ = valid;
    return result;
  }

private:
  [[nodiscard]] static MirBody lowerBody(
      const HirProgram &program, const HirBody &body, MirBodyKind kind,
      SemanticType returnType, const std::vector<HirValueId> &prologueValues,
      bool &valid, bool implicitZeroReturn = false,
      const std::vector<HirConstructorInitializer> *initializers = nullptr,
      const HirFunctionInstance *function = nullptr) {
    MirBodyLowerer lowerer(program, body, kind, std::move(returnType),
                           implicitZeroReturn, function);
    MirBody result = lowerer.lower(prologueValues, initializers);
    valid = valid && lowerer.isValid();
    return result;
  }
};

} // namespace lang
