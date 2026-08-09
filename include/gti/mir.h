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
};

struct MirPlaceProjection {
  MirProjectionKind kind = MirProjectionKind::Field;
  SymbolId field = 0;
  MirValueId index = 0;
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
};

enum class MirOperandKind {
  Value,
  Constant,
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
};

struct MirLoan {
  MirLoanId id = 0;
  MirLoanKind kind = MirLoanKind::Local;
  MirPlaceId source = 0;
  AccessMode access = AccessMode::ReadOnly;
  HirValueId producedBy = 0;
  HirBindingId binding = 0;
  SymbolId storedField = 0;
  bool escapes = false;
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
  std::optional<MirValueId> result;
  std::optional<MirPlaceId> destination;
  std::optional<MirOperand> receiver;
  std::vector<MirOperand> operands;
  std::optional<MirLoanId> loan;
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
  MirBlockId entry = 0;
  SemanticType returnType = SemanticType::Void;
  std::vector<MirBlock> blocks;
  std::vector<MirPlace> places;
  std::vector<MirLoan> loans;
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
};

struct MirClassInstance {
  HirClassInstanceId id = 0;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  bool abstract = false;
  bool polymorphic = false;
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

struct MirCallableParameter {
  std::size_t parameterIndex = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  bool nonEscaping = true;
  std::vector<MirCallableSignature> signatures;
};

struct MirFunctionInstance {
  HirFunctionInstanceId id = 0;
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
  MirBody body;
};

class MirProgram {
public:
  [[nodiscard]] bool valid() const { return valid_; }
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

  bool valid_ = true;
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
                 bool implicitZeroReturn = false)
      : program(program), source(source),
        implicitZeroReturn(implicitZeroReturn) {
    output.kind = kind;
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

    for (const HirValueId value : prologueValues) {
      (void)emitValue(value);
    }
    if (initializers != nullptr) {
      for (const HirConstructorInitializer &initializer : *initializers) {
        if (initializer.storesReference && initializer.arguments.size() == 1) {
          markStoredBorrow(initializer.arguments.front(), initializer.field,
                           initializer.borrowAccess);
        }
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

  struct BreakContext {
    MirBlockId target = 0;
    std::size_t keepScopes = 0;
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

  [[nodiscard]] std::optional<MirValueId>
  resultFor(const HirValue &value) {
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

  [[nodiscard]] MirPlaceId placeForBinding(HirBindingId id) {
    if (const auto found = bindingPlaces.find(id);
        found != bindingPlaces.end()) {
      return found->second;
    }
    const HirBinding *binding = findBinding(id);
    if (binding == nullptr) {
      return 0;
    }
    const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Binding,
                                          .binding = id,
                                          .symbol = binding->info.symbol,
                                          .type = binding->info.type,
                                          .access = binding->info.access,
                                          .traits = binding->info.traits});
    bindingPlaces.emplace(id, place);
    return place;
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
    case HirValueKind::MemberAccess:
      if (value->functionTarget) {
        place = loanOrValuePlace(*value);
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
    case HirValueKind::Unary:
      place = loanOrValuePlace(*value);
      if (place == 0 && value->operation == TokenKind::STAR &&
          !value->operands.empty()) {
        place = valueRootPlace(*value);
        output.places[place - 1].projections.push_back(
            {.kind = MirProjectionKind::Dereference});
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
        return placeForBinding(local->second);
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
        MirPlaceId place = clonePlace(placeForValue(value.operands[0]), value);
        if (place != 0) {
          if (value.operation == TokenKind::ARROW) {
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
        MirPlaceId place = clonePlace(placeForValue(value.operands[0]), value);
        if (place != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Index,
               .index = mirValueFor(value.operands[1])});
        }
        return place;
      }
      break;
    case HirValueKind::DereferenceSet:
      if (!value.operands.empty()) {
        MirPlaceId place = valueRootPlace(value);
        output.places[place - 1].value = mirValueFor(value.operands[0]);
        output.places[place - 1].projections.push_back(
            {.kind = MirProjectionKind::Dereference});
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
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    if (value->kind == HirValueKind::Move && !value->operands.empty()) {
      emitPlaceDependencies(value->operands.front());
      const MirPlaceId sourcePlace = placeForValue(value->operands.front());
      (void)appendInstruction({.kind = MirInstructionKind::Move,
                               .hirValue = value->id,
                               .result = resultFor(*value),
                               .operands = {{.kind = MirOperandKind::Move,
                                             .place = sourcePlace,
                                             .type = value->info.type}},
                               .intrinsic = value->intrinsic,
                               .info = value->info});
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
        return {.kind = MirOperandKind::Value,
                .value = mirValueFor(*value),
                .type = value->info.type};
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
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    emitValue(id);
    return {.kind = MirOperandKind::Value,
            .value = mirValueFor(*value),
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
    case HirValueKind::Grouping:
    case HirValueKind::MemberAccess:
      if (!value->operands.empty()) {
        emitPlaceDependencies(value->operands.front());
      }
      break;
    case HirValueKind::Index:
      if (!value->operands.empty()) {
        emitPlaceDependencies(value->operands.front());
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
    const std::size_t argumentCount = value.parameterTypes.size();
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
        return callee->operands.front();
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

  [[nodiscard]] MirPlaceId borrowOriginPlace(const HirValue &value,
                                             const MirInstruction &call) {
    if (value.borrowOrigin == BorrowOriginKind::Receiver && call.receiver) {
      if (const std::optional<HirValueId> receiver = receiverValue(value)) {
        if (const MirPlaceId source = borrowedSourcePlace(*receiver);
            source != 0) {
          return source;
        }
      }
      return call.receiver->place;
    }
    if (value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < call.operands.size()) {
      const std::vector<HirValueId> arguments = callArgumentValues(value);
      if (value.borrowArgument < arguments.size()) {
        if (const MirPlaceId source =
                borrowedSourcePlace(arguments[value.borrowArgument]);
            source != 0) {
          return source;
        }
      }
      const MirOperand &argument = call.operands[value.borrowArgument];
      if (argument.place != 0) {
        return argument.place;
      }
      if (value.borrowArgument < arguments.size()) {
        return placeForValue(arguments[value.borrowArgument]);
      }
    }
    return 0;
  }

  [[nodiscard]] MirLoanId createLoan(MirLoanKind kind, MirPlaceId sourcePlace,
                                     AccessMode access,
                                     HirValueId producedBy = 0,
                                     HirBindingId binding = 0,
                                     SymbolId storedField = 0) {
    const MirLoanId id = output.loans.size() + 1;
    output.loans.push_back({.id = id,
                            .kind = kind,
                            .source = sourcePlace,
                            .access = access,
                            .producedBy = producedBy,
                            .binding = binding,
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
                        .lambdaTarget = value.lambdaTarget,
                        .nonEscapingArguments = value.nonEscapingArguments,
                        .nonEscapingCallable = value.nonEscapingCallable,
                        .info = value.info};

    if (const std::optional<HirValueId> receiver = receiverValue(value)) {
      const AccessMode access = receiverAccess(value);
      call.receiver = receiverOperand(*receiver, access);
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
      call.operands.push_back(argumentOperand(arguments[index], parameter));
    }

    const MirPlaceId origin = borrowOriginPlace(value, call);
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanKind kind = value.info.traits.containsBorrowedState
                                   ? MirLoanKind::Stored
                                   : MirLoanKind::CallResult;
      const MirLoanId loan =
          createLoan(kind, origin, value.borrowAccess, value.id);
      call.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
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
      construct.operands.push_back(
          value.constructorKind == ConstructorKind::Ordinary
              ? argumentOperand(arguments[index], parameter)
              : valueOperand(arguments[index]));
    }
    const MirPlaceId origin = borrowOriginPlace(value, construct);
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanId loan =
          createLoan(MirLoanKind::Stored, origin, value.borrowAccess, value.id);
      construct.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
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

  void emitLogical(const HirValue &value) {
    if (value.operands.size() != 2 || !value.operation ||
        (*value.operation != TokenKind::AND &&
         *value.operation != TokenKind::OR)) {
      valid = false;
      return;
    }

    const MirOperand left = conditionOperand(value.operands[0]);
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

    current = rightBlock;
    scopes = incomingScopes;
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

    current = mergeBlock;
    scopes = incomingScopes;
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = temporary,
                                           .type = SemanticType::Bool}},
                             .info = value.info});
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
        emitPlaceDependencies(value->operands.front());
      } else if (value->kind == HirValueKind::IndexSet &&
                 value->operands.size() >= 2) {
        emitPlaceDependencies(value->operands[0]);
        (void)valueOperand(value->operands[1]);
      } else if (value->kind == HirValueKind::DereferenceSet &&
                 !value->operands.empty()) {
        (void)valueOperand(value->operands.front());
      }
      const MirPlaceId destination = destinationFor(*value);
      std::vector<MirOperand> operands;
      if (!value->operands.empty()) {
        const HirValueId sourceValue = value->operands.back();
        operands.push_back(valueOperand(sourceValue));
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::Assign,
           .hirValue = value->id,
           .result = resultFor(*value),
           .destination = destination,
           .operands = std::move(operands),
           .operation = value->operation
                            ? assignmentOperation(*value->operation)
                            : MirOperation::None,
           .info = value->info});
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
    if (instruction.operation == MirOperation::None) {
      valid = false;
    }
    for (const HirValueId operand : value->operands) {
      instruction.operands.push_back(instruction.operation ==
                                             MirOperation::LogicalNot
                                         ? conditionOperand(operand)
                                         : valueOperand(operand));
    }
    (void)appendInstruction(std::move(instruction));
    emittedValues.insert(id);
  }

  [[nodiscard]] MirOperand referenceOperand(HirValueId valueId,
                                            HirBindingId binding = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return {};
    }
    emitPlaceDependencies(valueId);
    if (const auto existing = valueLoans.find(valueId);
        existing != valueLoans.end()) {
      output.loans[existing->second - 1].binding = binding;
      return {.kind = MirOperandKind::Loan,
              .loan = existing->second,
              .type = value->info.type};
    }
    MirPlaceId sourcePlace = borrowedSourcePlace(valueId);
    if (sourcePlace == 0) {
      sourcePlace = placeForValue(valueId);
    }
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

  void emitScope(const Scope &scope) {
    for (auto loan = scope.loans.rbegin(); loan != scope.loans.rend(); ++loan) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
    }
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Drop,
           .destination = *drop,
           .info = place == nullptr
                       ? ExpressionInfo{}
                       : ExpressionInfo{.type = place->type,
                                        .category = ValueCategory::Place,
                                        .access = place->access,
                                        .traits = place->traits}});
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
        emitValue(*statement->value);
      }
      return;
    case HirStatementKind::Variable:
      lowerVariable(*statement);
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
      bindingLoans.insert_or_assign(*statement.binding, retainedLoan);
      output.loans[retainedLoan - 1].binding = *statement.binding;
    } else if (binding != nullptr &&
               binding->info.traits.containsBorrowedState && statement.value) {
      valid = false;
    }
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirStatement = statement.id,
         .destination = destination,
         .operands = std::move(operands),
         .info = binding == nullptr
                     ? ExpressionInfo{}
                     : ExpressionInfo{.type = binding->info.type,
                                      .category = ValueCategory::Place,
                                      .access = binding->info.access,
                                      .traits = binding->info.traits}});
    registerDrop(*statement.binding, destination);
  }

  void lowerIf(const HirStatement &statement) {
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    const MirBlockId thenBlock = appendBlock();
    const MirBlockId elseBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = thenBlock,
               .elseTarget = elseBlock});

    const std::vector<Scope> incomingScopes = scopes;
    current = thenBlock;
    scopes = incomingScopes;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

    current = elseBlock;
    scopes = incomingScopes;
    lowerScopedStatement(statement.elseBranch);
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

    current = mergeBlock;
    scopes = incomingScopes;
  }

  void lowerWhile(const HirStatement &statement) {
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = bodyBlock,
               .elseTarget = exitBlock});

    const std::vector<Scope> loopScopes = scopes;
    breakContexts.push_back(
        {.target = exitBlock, .keepScopes = loopScopes.size()});
    continueContexts.push_back(
        {.target = conditionBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();
    current = exitBlock;
    scopes = loopScopes;
  }

  void lowerFor(const HirStatement &statement) {
    scopes.push_back({});
    lowerNestedStatement(statement.initializer);
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId incrementBlock = appendBlock();
    const MirBlockId cleanupBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    if (statement.condition) {
      terminate({.kind = MirTerminatorKind::Branch,
                 .hirStatement = statement.id,
                 .value = conditionOperand(*statement.condition),
                 .target = bodyBlock,
                 .elseTarget = cleanupBlock});
    } else {
      terminate({.kind = MirTerminatorKind::Goto, .target = bodyBlock});
    }

    const std::vector<Scope> loopScopes = scopes;
    breakContexts.push_back(
        {.target = cleanupBlock, .keepScopes = loopScopes.size()});
    continueContexts.push_back(
        {.target = incrementBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = incrementBlock});
    }

    current = incrementBlock;
    scopes = loopScopes;
    if (statement.increment) {
      emitValue(*statement.increment);
    }
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();

    current = cleanupBlock;
    scopes = loopScopes;
    emitScope(scopes.back());
    terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    scopes.pop_back();
    current = exitBlock;
  }

  void lowerSwitch(const HirStatement &statement) {
    const MirOperand subject =
        statement.value ? valueOperand(*statement.value) : MirOperand{};
    const MirBlockId exitBlock = appendBlock();
    std::vector<MirBlockId> armBlocks;
    armBlocks.reserve(statement.switchArms.size());
    for (std::size_t index = 0; index < statement.switchArms.size(); ++index) {
      armBlocks.push_back(appendBlock());
    }

    MirTerminator terminator{.kind = MirTerminatorKind::Switch,
                             .hirStatement = statement.id,
                             .value = subject,
                             .target = exitBlock};
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
    breakContexts.push_back(
        {.target = exitBlock, .keepScopes = switchScopes.size()});
    for (std::size_t armIndex = 0; armIndex < statement.switchArms.size();
         ++armIndex) {
      current = armBlocks[armIndex];
      scopes = switchScopes;
      scopes.push_back({});
      lowerStatements(statement.switchArms[armIndex].statements);
      if (!terminated()) {
        emitScope(scopes.back());
        terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
      }
      scopes.pop_back();
    }
    breakContexts.pop_back();
    current = exitBlock;
    scopes = switchScopes;
  }

  void lowerReturn(const HirStatement &statement) {
    std::optional<MirOperand> result;
    if (statement.value) {
      if (output.returnType.kind == SemanticType::Reference) {
        MirOperand borrow = referenceOperand(*statement.value);
        if (borrow.loan != 0) {
          MirLoan &loan = output.loans[borrow.loan - 1];
          loan.kind = MirLoanKind::Return;
          loan.escapes = true;
          for (Scope &scope : scopes) {
            std::erase(scope.loans, borrow.loan);
          }
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
          }
        }
      }
    }
    emitScopeExit(0);
    terminate({.kind = MirTerminatorKind::Return,
               .hirStatement = statement.id,
               .value = std::move(result)});
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
  std::unordered_map<HirValueId, MirValueId> loweredValues;
  std::unordered_map<HirValueId, MirValueId> contextualValues;
  std::unordered_map<HirValueId, MirPlaceId> valuePlaces;
  std::unordered_map<HirValueId, MirLoanId> valueLoans;
  std::unordered_map<HirBindingId, MirLoanId> bindingLoans;
  std::unordered_set<HirValueId> emittedValues;
  std::vector<Scope> scopes;
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
    result.program.moduleBody =
        lowerBody(source, source.module(), MirBodyKind::Module,
                  SemanticType::Void, {}, valid);

    result.program.classes.reserve(source.classInstances().size());
    for (const HirClassInstance &instance : source.classInstances()) {
      MirClassInstance lowered{.id = instance.id,
                               .kind = instance.kind,
                               .bases = instance.bases,
                               .abstract = instance.abstract,
                               .polymorphic = instance.polymorphic};
      lowered.fieldInitializers = lowerBody(source, instance.fieldInitializers,
                                            MirBodyKind::FieldInitializers,
                                            SemanticType::Void, {}, valid);
      lowered.staticFieldInitializers = lowerBody(
          source, instance.staticFieldInitializers,
          MirBodyKind::StaticFieldInitializers, SemanticType::Void, {}, valid);
      for (auto field = instance.fields.rbegin();
           field != instance.fields.rend(); ++field) {
        if (field->info.traits.drop == DropKind::Lexical) {
          lowered.fieldDropOrder.push_back({.field = field->binding,
                                            .symbol = field->info.symbol,
                                            .type = field->info.type});
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
        callableParameters.emplace_back(std::move(lowered));
      }
      result.program.functions.push_back(
          {.id = instance.id,
           .virtualMethod = instance.virtualMethod,
           .pureVirtual = instance.pureVirtual,
           .overrideMethod = instance.overrideMethod,
           .virtualRoots = instance.virtualRoots,
           .callableParameters = std::move(callableParameters),
           .body =
               lowerBody(source, instance.body, MirBodyKind::Function,
                         instance.returnType, {}, valid, implicitZeroReturn)});
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
      result.program.lambdas.push_back(
          {.id = instance.id,
           .body = lowerBody(source, instance.body, MirBodyKind::Lambda,
                             instance.returnType, {}, valid)});
    }
    result.program.valid_ = valid;
    return result;
  }

private:
  [[nodiscard]] static MirBody lowerBody(
      const HirProgram &program, const HirBody &body, MirBodyKind kind,
      SemanticType returnType, const std::vector<HirValueId> &prologueValues,
      bool &valid, bool implicitZeroReturn = false,
      const std::vector<HirConstructorInitializer> *initializers = nullptr) {
    MirBodyLowerer lowerer(program, body, kind, std::move(returnType),
                           implicitZeroReturn);
    MirBody result = lowerer.lower(prologueValues, initializers);
    valid = valid && lowerer.isValid();
    return result;
  }
};

} // namespace lang
