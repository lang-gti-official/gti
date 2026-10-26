#include "body.h"
#include "../mir_verification_queries.h"
#include "callables.h"
#include "constructor_field_publication.h"
#include "dispatch.h"
#include "failure.h"
#include "hosted.h"
#include "lifetimes.h"
#include "memory.h"
#include "native_c_abi.h"
#include "native_c_contract.h"
#include "parameter_packs.h"
#include "payloads.h"
#include "placement.h"
#include "places.h"
#include "runtime_abi.h"
#include "scalar.h"
#include "text.h"
#include "thread_locals.h"
#include "thread_lifecycle.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace lang::native_llvm {
namespace {

struct BodyParameterPlan final {
  ConcreteParameterPlan concrete;
  std::span<const HirBindingId> sourceBindings;
};

BodyParameterPlan parameterPlan(const MirProgram &program,
                                MirBodyAddress address, const MirBody &body) {
  const std::vector<HirBindingId> *bindings = nullptr;
  const std::vector<SemanticType> *types = nullptr;
  if (address.kind == MirBodyKind::Function) {
    const auto *owner = program.findFunctionInstance(address.owner);
    if (owner && &owner->body == &body) {
      bindings = &owner->parameterBindings;
      types = &owner->parameterTypes;
    }
  } else if (address.kind == MirBodyKind::Constructor) {
    const auto *owner = program.findConstructorInstance(address.owner);
    if (owner && &owner->body == &body) {
      bindings = &owner->parameterBindings;
      types = &owner->parameterTypes;
    }
  } else if (address.kind == MirBodyKind::Lambda) {
    const auto *owner = program.findLambda(address.owner);
    if (owner && &owner->body == &body) {
      bindings = &owner->parameterBindings;
      types = &owner->parameterTypes;
    }
  } else {
    static const std::vector<HirBindingId> emptyBindings;
    static const std::vector<SemanticType> emptyTypes;
    bindings = &emptyBindings;
    types = &emptyTypes;
  }
  if (!bindings || !types)
    throw std::logic_error("LLVM body lost its exact parameter facts");
  auto result = planConcreteParameters(*types, *bindings, body);
  if (!result)
    throw BackendUnsupportedError(
        "LLVM concrete parameters lost their exact verified ABI mapping");
  return {std::move(*result), *bindings};
}

class Bodies final {
public:
  Bodies(Session &session, const LlvmAdmissionPlan &admission)
      : session(session), failures(session), dispatch(session, admission),
        lifetimes(session, failures, dispatch,
                  [&](MirBodyAddress address) { return function(address); }),
        threadLocals(
            session, failures, lifetimes,
            [&](MirBodyAddress address) { return function(address); },
            [&](MirBodyAddress address) { return signature(address); }),
        callables(
            session, [&](MirBodyAddress address) { return function(address); },
            [&](MirBodyAddress address) { return signature(address); }) {}
  void lower(const LlvmAdmissionPlan &admission);
#if defined(GTI_LLVM_C04_MIR_EVALUATION)
  void lowerEvaluation(std::span<const HirFunctionInstanceId> selected);
#endif

private:
  Session &session;
  Failures failures;
  Dispatch dispatch;
  Lifetimes lifetimes;
  ThreadLocals threadLocals;
  Callables callables;
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  std::map<std::pair<MirBodyKind, std::size_t>, llvm::Function *> functions;
  [[nodiscard]] llvm::Function *function(MirBodyAddress address);
  void body(MirBodyAddress address);
  [[nodiscard]] FunctionAbi signature(MirBodyAddress address);
  [[nodiscard]] const MirClassInstance *owner(MirBodyAddress address);
};

class Body final {
public:
  Body(Session &session, Failures &failures, Dispatch &dispatch,
       Lifetimes &lifetimes, ThreadLocals &threadLocals,
       llvm::Function &function, MirBodyAddress address, const MirBody &body,
       FunctionAbi abi, std::span<const ParameterLane> parameters,
       std::span<const ConcretePackParameter> packs,
       std::span<const HirBindingId> sourceBindings,
       const std::map<SymbolId, llvm::GlobalVariable *> &globals,
       Callables &callables,
       std::function<llvm::Function *(MirBodyAddress)> findFunction,
       std::function<FunctionAbi(MirBodyAddress)> bodyAbi,
       const MirClassInstance *owner, const MirConstructorInstance *constructor,
       const MirLambdaInstance *lambda,
       std::optional<SemanticType> receiverType)
      : session(session), failures(failures), dispatch(dispatch),
        function(function), address(address), input(body), abi(std::move(abi)),
        globals(globals), ir(session.context()), lifetimes(lifetimes),
        threadLocals(threadLocals), bodyAbi(std::move(bodyAbi)), owner(owner),
        constructor(constructor), receiverType(std::move(receiverType)),
        callables(callables),
        scalars(session, ir,
                [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
                  guard(condition, outcome);
                }),
        findFunction(std::move(findFunction)), values(body.values.size() + 1),
        memory(
            session, ir, body, values,
            [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
              guard(condition, outcome);
            },
            lambda, packs),
        objectMemory(
            session, ir,
            [this](const MirDropType &recipe) {
              return this->lifetimes.cleanup(recipe);
            },
            [this](llvm::Value *address, const SemanticType &type) {
              this->lifetimes.moved(ir, address, type);
            },
            [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
              guard(condition, outcome);
            }),
        channels(body.failureRecords.size() + 1),
        blocks(body.blocks.size() + 1), live(body.dropObligations.size() + 1),
        placement(session.program().mir(), body),
        parameterBindings(sourceBindings.begin(), sourceBindings.end()) {
    if (parameterBindings.contains(0))
      throw std::logic_error("LLVM source parameter has no binding identity");
    std::set<HirBindingId> transportedBindings;
    for (const auto &lane : parameters) {
      if (!lane.binding || !parameterBindings.contains(lane.binding))
        throw std::logic_error("LLVM parameter lane lost its source binding");
      transportedBindings.insert(lane.binding);
    }
    for (const auto &pack : packs) {
      if (!pack.binding || !parameterBindings.contains(pack.binding))
        throw std::logic_error("LLVM parameter pack lost its source binding");
      transportedBindings.insert(pack.binding);
    }
    if (transportedBindings != parameterBindings)
      throw std::logic_error("LLVM parameter plan omitted a source formal");
    emit(parameters);
  }

private:
  Session &session;
  Failures &failures;
  Dispatch &dispatch;
  llvm::Function &function;
  MirBodyAddress address;
  const MirBody &input;
  FunctionAbi abi;
  const std::map<SymbolId, llvm::GlobalVariable *> &globals;
  llvm::IRBuilder<> ir;
  Lifetimes &lifetimes;
  ThreadLocals &threadLocals;
  std::function<FunctionAbi(MirBodyAddress)> bodyAbi;
  const MirClassInstance *owner;
  const MirConstructorInstance *constructor;
  std::optional<SemanticType> receiverType;
  Callables &callables;
  Scalars scalars;
  std::function<llvm::Function *(MirBodyAddress)> findFunction;
  std::vector<llvm::Value *> values;
  Places memory;
  Memory objectMemory;
  std::vector<llvm::Value *> channels;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<llvm::Value *> live;
  llvm::Value *thisAddress = nullptr;
  Placement placement;
  std::set<HirBindingId> parameterBindings;
  std::map<MirValueId, llvm::Value *> temporaries;
  llvm::Value *returnDestination = nullptr, *failureDestination = nullptr;
  llvm::Value *callerSite = nullptr, *callerArtifact = nullptr;
  llvm::Value *nativeArgumentCount = nullptr, *nativeArguments = nullptr;
  const MirBlock *current = nullptr;
  const MirInstruction *stage = nullptr;
  llvm::Value *cleanupToken = nullptr;

  [[nodiscard]] StoredFieldPublicationPlan
  fieldPublication(const MirLoan &loan) const {
    if (address.kind != MirBodyKind::Constructor)
      return {};
    auto plan = storedFieldPublication(session.program().mir(), input,
                                       address.owner, loan);
    if (plan.matched() && !plan.exact)
      throw BackendUnsupportedError(
          "LLVM stored field publication lost its exact constructor facts");
    return plan;
  }

  void publishStoredField(const MirLoan &loan, llvm::Value *address) {
    const auto plan = fieldPublication(loan);
    if (!plan.matched() ||
        plan.kind == StoredFieldPublicationKind::ProvenanceOnly)
      return;
    if (plan.kind != StoredFieldPublicationKind::ReferenceAddress ||
        !plan.field || !owner || !thisAddress)
      throw std::logic_error(
          "LLVM stored field publication lost its constructor");
    ir.CreateStore(
        address,
        ir.CreateStructGEP(
            session.layouts().get(owner->type).storage, thisAddress,
            session.layouts().fieldIndex(owner->type, plan.field->symbol)));
  }

  [[nodiscard]] llvm::Value *destination(MirValueId value) {
    const auto *selected = input.findValue(value);
    if (!selected || selected->id != value)
      throw std::logic_error("LLVM destination lost its exact verified value");
    const auto &valueType = selected->info.type;
    const bool hierarchy =
        addressValue(session.program().mir(), valueType) &&
        containsHierarchyStorage(session.program().mir(), valueType);
    if (stage && stage->result && *stage->result == value)
      if (const auto publication = recordPublication(*stage);
          publication.matched()) {
        if (hierarchy)
          throw BackendUnsupportedError(
              "LLVM constructor field publication requires its nested "
              "hierarchy destination support");
        if (publication.transport ==
            UnstagedConstructorFieldPublicationTransport::ExactValue)
          return publicationAddress(*publication.initializer);
        if (publication.transport ==
            UnstagedConstructorFieldPublicationTransport::NullUniqueOwner)
          throw std::logic_error(
              "LLVM null owner publication requested source placement");
      }
    if (const auto *target = placement.target(value)) {
      if (target->place) {
        if (hierarchy) {
          bool exactProducer =
              stage && stage->kind == MirInstructionKind::Construct &&
              stage->constructorKind == ConstructorKind::Ordinary;
          if (stage && isFreshHierarchyTransferProducer(*stage)) {
            const auto transfer = planFreshHierarchyTransfer(
                session.program().mir(), input, *stage);
            exactProducer = transfer.destination == target->place &&
                            transfer.publication &&
                            transfer.publication->destination == target->place;
          }
          const auto *place = input.findPlace(target->place);
          const bool localBody = input.kind == MirBodyKind::Function ||
                                 input.kind == MirBodyKind::Constructor ||
                                 input.kind == MirBodyKind::Destructor;
          const bool publication = std::any_of(
              input.blocks.begin(), input.blocks.end(), [&](const auto &block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const auto &instruction) {
                      return !instruction.tombstone &&
                             instruction.kind ==
                                 MirInstructionKind::Initialize &&
                             instruction.destination == target->place &&
                             instruction.operands.size() == 1 &&
                             instruction.operands[0].kind ==
                                 MirOperandKind::Value &&
                             instruction.operands[0].value == value;
                    });
              });
          if (!place || !localBody || valueType.kind != SemanticType::Class ||
              place->root != MirPlaceRootKind::Binding || !place->binding ||
              parameterBindings.contains(place->binding) ||
              !place->projections.empty() || place->foreignStorage ||
              place->capture || place->type != valueType || !publication ||
              !stage || !exactProducer || stage->result != value)
            throw BackendUnsupportedError("LLVM hierarchy construction lost "
                                          "its exact fresh local publication");
        }
        return memory.address(target->place);
      }
      if (target->returned) {
        if (hierarchy)
          throw BackendUnsupportedError(
              "LLVM hierarchy return storage is not admitted");
        if (!returnDestination)
          throw std::logic_error("LLVM object return lost its ABI destination");
        return returnDestination;
      }
      if (hierarchy)
        throw BackendUnsupportedError(
            "LLVM nested hierarchy destination is not admitted");
      const auto &type = input.findValue(target->aggregate)->info.type;
      if (target->variant)
        return payloadFieldAddress(ir, session.layouts().payload(type),
                                   destination(target->aggregate),
                                   *target->variant, target->element);
      return ir.CreateGEP(session.layouts().get(type).storage,
                          destination(target->aggregate),
                          {ir.getInt64(0), ir.getInt64(target->element)});
    }
    if (hierarchy)
      throw BackendUnsupportedError(
          "LLVM hierarchy construction requires its final local destination");
    auto [entry, inserted] = temporaries.try_emplace(value, nullptr);
    if (inserted)
      entry->second = memory.temporary(input.findValue(value)->info.type);
    return entry->second;
  }

  [[nodiscard]] llvm::Value *load(llvm::Value *address,
                                  const SemanticType &type) {
    return memory.load(address, type);
  }
  void store(llvm::Value *value, llvm::Value *address,
             const SemanticType &type) {
    memory.store(value, address, type);
  }
  [[nodiscard]] UnstagedConstructorFieldPublicationPlan
  recordPublication(const MirInstruction &instruction) const {
    if (address.kind != MirBodyKind::Constructor || !owner)
      return {};
    const auto &program = session.program().mir();
    const auto plan = unstagedConstructorFieldPublication(
        program, input, address.owner, instruction);
    if (!plan.matched())
      return {};
    if (!plan.exact)
      throw BackendUnsupportedError(
          "LLVM constructor record publication has inconsistent sealed "
          "field facts");
    return plan;
  }
  [[nodiscard]] llvm::Value *
  publicationAddress(const MirConstructorInitializer &initializer) {
    if (!thisAddress || !owner)
      throw std::logic_error(
          "LLVM constructor field publication lost its receiver");
    return ir.CreateStructGEP(
        session.layouts().get(owner->type).storage, thisAddress,
        session.layouts().fieldIndex(owner->type, initializer.field));
  }
  [[nodiscard]] llvm::Value *freshTransfer(const MirInstruction &instruction) {
    const auto transfer = Lifetimes::preflightFreshTransfer(session, dispatch,
                                                            input, instruction);
    const auto *target = placement.target(*instruction.result);
    if (!target || target->place != transfer.destination || target->aggregate ||
        target->returned || transfer.source != instruction.operands.front())
      throw BackendUnsupportedError("LLVM hierarchy transfer lost its exact "
                                    "source/publication placement");
    auto *address = destination(*instruction.result);
    // Selecting the sealed source view does not materialize an object snapshot
    // or suppress its cleanup. The typed transfer commits source state last.
    auto *source = memory.selected(transfer.source);
    lifetimes.freshTransfer(ir, address, source, input, instruction);
    return address;
  }
  [[nodiscard]] llvm::Value *operand(const MirOperand &operand,
                                     llvm::Value *target = nullptr) {

    if ((operand.kind == MirOperandKind::Copy ||
         operand.kind == MirOperandKind::Move) &&
        addressValue(session.program().mir(), operand.type) &&
        containsHierarchyStorage(session.program().mir(), operand.type))
      throw BackendUnsupportedError(
          "LLVM hierarchy object copy/move transport is not admitted");
    if (operand.literal)
      return scalars.literal(*operand.literal, operand.type);
    if (operand.kind == MirOperandKind::Address ||
        operand.kind == MirOperandKind::BorrowRead ||
        operand.kind == MirOperandKind::BorrowWrite ||
        operand.kind == MirOperandKind::Loan)
      return memory.selected(operand);
    if (operand.value) {
      auto *value = values.at(operand.value);
      if (!value)
        throw std::logic_error("LLVM operand has no dominating value " +
                               std::to_string(operand.value));
      return value;
    }
    if (operand.place && (operand.kind == MirOperandKind::Copy ||
                          operand.kind == MirOperandKind::Move)) {
      auto *source = memory.address(operand.place);
      auto *value = load(source, operand.type);
      if (addressValue(session.program().mir(), operand.type)) {
        if (!target)
          target = memory.temporary(operand.type);
        store(value, target, operand.type);
        if (operand.kind == MirOperandKind::Move)
          lifetimes.moved(ir, source, operand.type);
        return target;
      }
      if (operand.kind == MirOperandKind::Move &&
          (operand.type.kind == SemanticType::Array ||
           operand.type.kind == SemanticType::UniqueOwner))
        lifetimes.moved(ir, source, operand.type);
      return value;
    }
    throw BackendUnsupportedError(
        "LLVM scalar operand requires a value or a local copy");
  }
  llvm::BasicBlock *edge(MirBlockId target,
                         std::span<const MirOperand> arguments) {
    const auto *destination = input.findBlock(target);
    if (!destination || destination->parameters.size() != arguments.size())
      throw std::logic_error("LLVM edge lost verified MIR parameters");
    if (arguments.empty())
      return blocks.at(target);
    // MIR arguments belong to edges, not predecessors. Distinct branch/switch
    // edges may share both endpoints but pass different values; LLVM PHIs need
    // distinct predecessor blocks in that case. Copy operands execute only on
    // the selected edge, preserving the MIR schedule as well.
    const auto insertion = ir.saveIP();
    auto *transfer =
        llvm::BasicBlock::Create(session.context(), "edge", &function);
    ir.SetInsertPoint(transfer);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      auto *value = operand(arguments[index]);
      llvm::cast<llvm::PHINode>(values.at(destination->parameters[index].value))
          ->addIncoming(value, ir.GetInsertBlock());
    }
    ir.CreateBr(blocks.at(target));
    ir.restoreIP(insertion);
    return transfer;
  }
  void jump(MirBlockId target, std::span<const MirOperand> arguments) {
    ir.CreateBr(edge(target, arguments));
  }
  void failedOperation() {
    const auto &term = current->terminator;
    if (term.kind != MirTerminatorKind::Invoke || !term.failureRecord)
      throw std::logic_error("LLVM failed operation lost its Invoke record");
    failures.checkIncomingCleanup(ir, channels.at(term.failureRecord));
    if (cleanupToken)
      ir.CreateStore(failures.enterCleanup(ir, channels.at(term.failureRecord)),
                     cleanupToken);
    jump(term.elseTarget, term.elseArguments);
  }
  void leaveFailureCleanup() {
    if (cleanupToken)
      failures.leaveCleanup(
          ir, ir.CreateLoad(session.layouts().pointer(), cleanupToken));
  }
  void guard(llvm::Value *condition, DefinedFailureOutcome outcome) {
    bool authorized = false;
    for (std::size_t index = 0;
         index < stage->definedFailure.localOrigins.size(); ++index) {
      const auto &origin = stage->definedFailure.localOrigins[index];
      authorized |= std::find(origin.outcomes.begin(), origin.outcomes.end(),
                              outcome) != origin.outcomes.end() &&
                    !isMirFailureOutcomeDischarged(*stage, index, outcome);
    }
    // An absent outcome is either statically infallible or discharged by
    // verified MIR proof. It does not authorize originating a different record.
    if (!authorized)
      return;
    const auto &term = current->terminator;
    const bool terminalAllocation =
        outcome.code == DefinedFailureCode::AllocationFailure;
    if (!terminalAllocation && (term.kind != MirTerminatorKind::Invoke ||
                                term.invokeInstruction != stage->id))
      throw std::logic_error("LLVM checked operation lost its Invoke");
    auto *failed =
        llvm::BasicBlock::Create(session.context(), "check.failed", &function);
    auto *normal =
        llvm::BasicBlock::Create(session.context(), "check.success", &function);
    ir.CreateCondBr(condition, failed, normal);
    ir.SetInsertPoint(failed);
    if (terminalAllocation) {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      auto *channel = entry.CreateAlloca(session.abi().failureChannel());
      failures.originate(ir, channel, *stage, outcome, callerSite,
                         callerArtifact);
      ir.CreateCall(
          declareRuntimeService(session, RuntimeService::AbortAllocation),
          {failures.record(ir, channel), failures.origin(ir, channel)});
      ir.CreateUnreachable();
    } else {
      failures.originate(ir, channels.at(term.failureRecord), *stage, outcome,
                         callerSite, callerArtifact);
      failedOperation();
    }
    ir.SetInsertPoint(normal);
  }
  [[nodiscard]] llvm::Value *
  formFunctionPointer(const MirInstruction &instruction) {
    if (!instruction.result)
      throw std::logic_error(
          "LLVM function-pointer formation has no result storage");
    auto *wrapper = destination(*instruction.result);
    auto *target = callables.pointerAdapter(instruction);
    ir.CreateStore(
        target,
        ir.CreateStructGEP(session.layouts().get(instruction.info.type).storage,
                           wrapper, 0));
    return wrapper;
  }
  [[nodiscard]] llvm::Value *
  invokeFunctionPointer(const MirInstruction &instruction) {
    if (instruction.operands.empty() || instruction.parameterTypes.empty())
      throw std::logic_error(
          "LLVM function-pointer invocation lost its receiver");
    const auto &receiver = instruction.parameterTypes.front();
    if (receiver.kind != SemanticType::Reference ||
        receiver.arguments.size() != 1)
      throw std::logic_error(
          "LLVM function-pointer invocation lost its capability type");
    const auto &pointerType = receiver.arguments.front();
    const auto callable = callables.pointerSignature(pointerType);
    const auto signature = callables.pointerAbi(pointerType);
    auto *wrapper = memory.selected(instruction.operands.front());
    auto *callee = ir.CreateLoad(
        session.layouts().pointer(),
        ir.CreateStructGEP(session.layouts().get(pointerType).storage, wrapper,
                           0));

    llvm::Value *resultSlot = nullptr;
    if (signature.resultDestination) {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      resultSlot = instruction.result && addressValue(session.program().mir(),
                                                      callable.result)
                       ? destination(*instruction.result)
                       : entry.CreateAlloca(
                             session.layouts().get(callable.result).storage);
    }
    std::vector<llvm::Value *> arguments;
    arguments.reserve(signature.parameters.size());
    for (const auto &parameter : signature.parameters) {
      switch (parameter.channel) {
      case AbiChannel::ResultDestination:
        arguments.push_back(resultSlot);
        break;
      case AbiChannel::FailureDestination:
        arguments.push_back(channels.at(current->terminator.failureRecord));
        break;
      case AbiChannel::Argument: {
        const auto index = parameter.sourceIndex + 1;
        auto *value = operand(instruction.operands.at(index));
        if (parameter.indirect &&
            !addressValue(session.program().mir(),
                          instruction.operands.at(index).type)) {
          llvm::IRBuilder<> entry(&function.getEntryBlock(),
                                  function.getEntryBlock().begin());
          auto *slot = entry.CreateAlloca(value->getType());
          ir.CreateStore(value, slot);
          value = slot;
        }
        arguments.push_back(value);
        break;
      }
      case AbiChannel::Receiver:
      case AbiChannel::CallerSite:
      case AbiChannel::CallerArtifact:
        throw std::logic_error(
            "LLVM function-pointer ABI acquired an invalid channel");
      }
    }
    auto *status = ir.CreateCall(signature.type, callee, arguments);
    signature.applyTo(*status);
    auto *normal =
        llvm::BasicBlock::Create(session.context(), "call.success", &function);
    auto *failed =
        llvm::BasicBlock::Create(session.context(), "call.failed", &function);
    ir.CreateCondBr(status, normal, failed);
    ir.SetInsertPoint(failed);
    failedOperation();
    ir.SetInsertPoint(normal);
    return resultSlot ? load(resultSlot, callable.result) : nullptr;
  }
  [[nodiscard]] llvm::Value *
  constructClosure(const MirInstruction &instruction) {
    if (!instruction.result || !instruction.lambdaTarget)
      throw std::logic_error("LLVM closure construction lost its identity");
    const auto &target = callables.lambda(*instruction.lambdaTarget);
    if (target.type != instruction.info.type ||
        target.captureTypes != instruction.closureCaptureTypes ||
        target.captureModes != instruction.closureCaptureModes ||
        target.captureTypes.size() != instruction.operands.size())
      throw std::logic_error(
          "LLVM closure construction disagrees with sealed capture facts");
    auto *environment = destination(*instruction.result);
    auto *storage = session.layouts().get(target.type).storage;
    if (target.captureTypes.empty()) {
      ir.CreateStore(ir.getInt8(0),
                     ir.CreateStructGEP(storage, environment, 0));
      return environment;
    }
    for (std::size_t index = 0; index < target.captureTypes.size(); ++index)
      store(operand(instruction.operands[index]),
            ir.CreateStructGEP(storage, environment,
                               static_cast<unsigned>(index)),
            target.captureTypes[index]);
    return environment;
  }
  [[nodiscard]] llvm::Value *call(const MirInstruction &instruction,
                                  llvm::Value *constructed = nullptr) {
    if (const auto location = hostedSourceLiteral(instruction))
      return scalars.literal(*location, instruction.info.type);
    if (instruction.intrinsic == IntrinsicKind::Panic ||
        instruction.intrinsic == IntrinsicKind::FailEmptyOwner ||
        instruction.intrinsic == IntrinsicKind::FailInactiveValue ||
        instruction.intrinsic == IntrinsicKind::FailInactiveError) {
      const auto &term = current->terminator;
      if (instruction.definedFailure.localOrigins.size() != 1 ||
          instruction.definedFailure.localOrigins[0].outcomes.size() != 1)
        throw std::logic_error(
            "LLVM terminal intrinsic lost its exact failure outcome");
      failures.originate(ir, channels.at(term.failureRecord), instruction,
                         instruction.definedFailure.localOrigins[0].outcomes[0],
                         callerSite, callerArtifact,
                         instruction.intrinsic == IntrinsicKind::Panic
                             ? operand(instruction.operands.at(0))
                             : nullptr);
      failedOperation();
      return nullptr;
    }
    if (const auto outcome =
            intrinsicBooleanFailureOutcome(instruction.intrinsic)) {
      auto *condition = operand(instruction.operands.at(0));
      if (!condition->getType()->isIntegerTy(1))
        throw std::logic_error(
            "LLVM checked condition lost its sealed Boolean type");
      guard(ir.CreateNot(condition), *outcome);
      return nullptr;
    }
    std::size_t argumentOffset = 0;
    if (instruction.intrinsic == IntrinsicKind::ObjectConstruct) {
      if (!nativeObjectConstructionShape(session.program().mir(), instruction))
        throw BackendUnsupportedError("LLVM object construction requires its "
                                      "exact selected/generated plan");
      constructed = operand(instruction.operands[0]);
      argumentOffset = 1;
      if (!instruction.constructorTarget) {
        const auto &type = instruction.parameterTypes[0].arguments[0];
        if (instruction.operands.size() == 2) {
          auto *value = operand(instruction.operands[1]);
          store(value, constructed, type);
          if (instruction.constructorKind == ConstructorKind::Move &&
              addressValue(session.program().mir(), type))
            lifetimes.moved(ir, value, type);
        } else {
          ir.CreateStore(
              llvm::Constant::getNullValue(session.layouts().get(type).storage),
              constructed);
        }
        lifetimes.active(ir, constructed, type, true);
        return nullptr;
      }
      // Keep the original MIR instruction, stage and ownership events. Only
      // source-argument indexing skips the already evaluated raw destination.
    } else if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
      if (!nativeUniqueConstructionShape(session.program().mir(), instruction))
        throw BackendUnsupportedError("LLVM unique construction requires its "
                                      "exact selected/generated plan");
      const auto &type = instruction.info.type.arguments[0];
      if (type.kind == SemanticType::Class) {
        const auto *allocation =
            input.findDropObligation(instruction.objectAllocation);
        if (!allocation ||
            allocation->kind != MirDropObligationKind::ObjectAllocation ||
            allocation->dropType.type != instruction.info.type)
          throw std::logic_error(
              "LLVM unique construction lost its allocation-only obligation");
        constructed =
            load(memory.address(allocation->place), instruction.info.type);
      } else {
        constructed = objectMemory.allocateObject(instruction);
      }
      if (!instruction.constructorTarget) {
        if (instruction.operands.empty()) {
          ir.CreateStore(
              llvm::Constant::getNullValue(session.layouts().get(type).storage),
              constructed);
        } else {
          auto *value = operand(instruction.operands[0]);
          store(value, constructed, type);
          if (instruction.constructorKind == ConstructorKind::Move &&
              addressValue(session.program().mir(), type))
            lifetimes.moved(ir, value, type);
        }
        lifetimes.active(ir, constructed, type, true);
        return constructed;
      }
    } else if (instruction.intrinsic != IntrinsicKind::None) {
      if (instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem)
        return formFunctionPointer(instruction);
      if (instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke)
        return invokeFunctionPointer(instruction);
      if (Memory::handles(instruction.intrinsic)) {
        std::vector<llvm::Value *> inputs;
        for (const auto &value : instruction.operands)
          inputs.push_back(operand(value));
        const auto &term = current->terminator;
        const bool checked = term.kind == MirTerminatorKind::Invoke &&
                             term.invokeInstruction == instruction.id;
        const auto emitted = objectMemory.emit(
            instruction, inputs,
            checked ? channels.at(term.failureRecord) : nullptr,
            current->activeFailure != 0);
        if (!emitted.handled)
          throw std::logic_error("LLVM memory dispatch lost its operation");
        if (emitted.status && checked) {
          auto *normal = llvm::BasicBlock::Create(session.context(),
                                                  "memory.success", &function);
          auto *failed = llvm::BasicBlock::Create(session.context(),
                                                  "memory.failed", &function);
          ir.CreateCondBr(emitted.status, normal, failed);
          ir.SetInsertPoint(failed);
          failedOperation();
          ir.SetInsertPoint(normal);
        }
        return emitted.value;
      }
      switch (instruction.intrinsic) {
      case IntrinsicKind::StringViewSize:
        return ir.CreateExtractValue(
            load(memory.selected(*instruction.receiver),
                 SemanticType::StringView),
            1);
      case IntrinsicKind::StringViewEmpty:
        return ir.CreateICmpEQ(
            ir.CreateExtractValue(load(memory.selected(*instruction.receiver),
                                       SemanticType::StringView),
                                  1),
            ir.getInt64(0));
      case IntrinsicKind::ArraySize:
        return ir.getInt64(instruction.receiver->type.arrayLength);
      case IntrinsicKind::ArrayRangeBegin:
        return ir.getInt64(0);
      case IntrinsicKind::EraseReference:
      case IntrinsicKind::EraseReferenceMut:
      case IntrinsicKind::RestoreReference:
      case IntrinsicKind::RestoreReferenceMut:
        return memory.selected(instruction.operands.at(0));
      case IntrinsicKind::RawPointerConversion:
      case IntrinsicKind::RawBorrow:
      case IntrinsicKind::RawBorrowMut:
        return operand(instruction.operands.at(0));
      case IntrinsicKind::TextViewFromAddress: {
        llvm::Value *view =
            llvm::Constant::getNullValue(session.abi().textView());
        view =
            ir.CreateInsertValue(view, operand(instruction.operands.at(0)), 0);
        return ir.CreateInsertValue(view, operand(instruction.operands.at(1)),
                                    1);
      }
      case IntrinsicKind::CStringFromAddress:
        return operand(instruction.operands.at(0));
      default:
        break;
      }
      if (instruction.intrinsic ==
              IntrinsicKind::NumericTypeParameterConversion ||
          instruction.intrinsic == IntrinsicKind::NumericAliasConversion)
        return scalars.convert(operand(instruction.operands.at(0)),
                               instruction.operands[0].type,
                               instruction.info.type);
      if (instruction.intrinsic ==
          IntrinsicKind::DefaultTypeParameterConstruction)
        return llvm::Constant::getNullValue(
            session.layouts().get(instruction.info.type).value);
      if (instruction.intrinsic == IntrinsicKind::Move)
        return operand(instruction.operands.at(0));
      if (integerArithmeticIntrinsic(instruction.intrinsic)) {
        std::vector<llvm::Value *> inputs;
        for (const auto &value : instruction.operands)
          inputs.push_back(operand(value));
        return scalars.compute(instruction, inputs);
      }
      throw BackendUnsupportedError("LLVM scalar intrinsic is not implemented");
    }
    const MirFunctionInstance *target =
        instruction.functionTarget
            ? session.program().mir().findFunctionInstance(
                  *instruction.functionTarget)
            : nullptr;
    const MirLambdaInstance *lambdaTarget =
        instruction.lambdaTarget
            ? session.program().mir().findLambda(*instruction.lambdaTarget)
            : nullptr;
    const auto constructor = instruction.constructorTarget;
    if (instruction.nativeCalleeType) {
      if (!nativeCIndirectCallContract(session.program().mir(),
                                             session.program().target(), input,
                                             instruction))
        throw BackendUnsupportedError(
            "LLVM indirect C call requires its exact staged native signature");
      auto *callee = operand(*instruction.receiver);
      std::vector<llvm::Value *> arguments;
      for (const auto &argument : instruction.operands)
        arguments.push_back(operand(argument));
      auto *resultDestination =
          instruction.result &&
                  addressValue(session.program().mir(), instruction.info.type)
              ? destination(*instruction.result)
              : nullptr;
      return emitNativeCIndirectCall(session, ir, input, instruction, callee,
                                     arguments, resultDestination);
    }
    if (!target && !lambdaTarget && !instruction.bodyTarget && !constructor)
      throw BackendUnsupportedError(
          "LLVM scalar call requires an exact direct target");
    if (target && (target->cThreadOperation == CThreadRuntimeOperation::Begin ||
                   target->cThreadOperation == CThreadRuntimeOperation::End)) {
      if (!nativeCThreadLifecycleCall(session.program().mir(), input, instruction))
        throw BackendUnsupportedError(
            "LLVM C-thread lifecycle lost its sealed startup operation");
      ir.CreateCall(declareRuntimeService(
          session, target->cThreadOperation == CThreadRuntimeOperation::Begin
                       ? RuntimeService::BeginCThreadRuntime
                       : RuntimeService::EndCThreadRuntime));
      return nullptr;
    }
    if (target &&
        target->processMemoryOperation != ProcessMemoryRuntimeOperation::None) {
      std::vector<llvm::Value *> inputs;
      for (const auto &value : instruction.operands)
        inputs.push_back(operand(value));
      return objectMemory.processCall(*target, inputs);
    }
    if (target && target->linkage == LanguageLinkage::C) {
      const auto &program = session.program().mir();
      const auto &targetInfo = session.program().target();
      if (!nativeCDirectCallContract(program, targetInfo, input, instruction))
        throw BackendUnsupportedError(
            "LLVM direct C call requires its supported exact import profile");
      std::vector<llvm::Value *> arguments;
      for (const auto &argument : instruction.operands)
        arguments.push_back(operand(argument));
      auto *resultDestination =
          instruction.result && addressValue(program, target->returnType)
              ? destination(*instruction.result)
              : nullptr;
      return emitNativeCDirectCall(session, ir, input, instruction, arguments,
                                   resultDestination);
    }
    FunctionAbi signature;
    llvm::Value *callee = nullptr;
    llvm::Value *receiver = nullptr;
    if (target && instruction.dispatch == CallDispatch::Virtual) {
      const auto plan = dispatch.planCall(instruction);
      const auto selected = dispatch.loadCall(
          ir, instruction, plan, memory.selected(*instruction.receiver));
      signature = plan.abi.call;
      callee = selected.callee;
      receiver = selected.receiver;
    } else if (target) {
      signature =
          target->linkage == LanguageLinkage::C
              ? session.abi().nativeC(target->returnType,
                                      target->parameterTypes)
              : session.abi().gti(target->returnType, target->parameterTypes,
                                  target->mayRaiseDefinedFailure,
                                  target->owner && !target->staticMember,
                                  target->tracksCaller);
      callee = session.abi().declareFunction(target->id);
      if (target->owner && !target->staticMember && instruction.receiver)
        receiver = dispatch.directReceiver(
            ir, instruction, memory.selected(*instruction.receiver));
    } else {
      const auto address =
          constructor ? MirBodyAddress{MirBodyKind::Constructor, *constructor}
          : lambdaTarget ? MirBodyAddress{MirBodyKind::Lambda, lambdaTarget->id}
                         : *instruction.bodyTarget;
      signature = bodyAbi(address);
      callee = findFunction(address);
    }
    const auto &resultType = target         ? target->returnType
                             : lambdaTarget ? lambdaTarget->returnType
                                            : instruction.info.type;
    llvm::Value *resultSlot = nullptr;
    if (signature.resultDestination) {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      resultSlot =
          instruction.result &&
                  addressValue(session.program().mir(), resultType)
              ? destination(*instruction.result)
              : entry.CreateAlloca(session.layouts().get(resultType).storage);
    }
    std::vector<llvm::Value *> arguments;
    for (const auto &parameter : signature.parameters) {
      switch (parameter.channel) {
      case AbiChannel::ResultDestination:
        arguments.push_back(resultSlot);
        break;
      case AbiChannel::FailureDestination:
        arguments.push_back(channels.at(current->terminator.failureRecord));
        break;
      case AbiChannel::CallerSite:
        arguments.push_back(instruction.forwardsCallerLocation
                                ? callerSite
                                : ir.getInt32(instruction.callerLocationSite));
        break;
      case AbiChannel::CallerArtifact:
        arguments.push_back(instruction.forwardsCallerLocation
                                ? callerArtifact
                                : failures.artifact());
        break;
      case AbiChannel::Argument: {
        const auto &source =
            instruction.operands.at(parameter.sourceIndex + argumentOffset);
        auto *value = operand(source);
        if (parameter.indirect &&
            !addressValue(session.program().mir(), source.type)) {
          llvm::IRBuilder<> entry(&function.getEntryBlock(),
                                  function.getEntryBlock().begin());
          auto *slot = entry.CreateAlloca(value->getType());
          ir.CreateStore(value, slot);
          value = slot;
        }
        arguments.push_back(value);
        break;
      }
      case AbiChannel::Receiver:
        arguments.push_back(
            constructed ? constructed
            : instruction.receiver
                ? receiver ? receiver : memory.selected(*instruction.receiver)
                : thisAddress);
        if (!arguments.back())
          throw std::logic_error("LLVM call lost its sealed receiver");
        break;
      }
    }
    auto *result = ir.CreateCall(signature.type, callee, arguments);
    signature.applyTo(*result);
    if (signature.statusResult) {
      auto *normal = llvm::BasicBlock::Create(session.context(), "call.success",
                                              &function);
      auto *failed =
          llvm::BasicBlock::Create(session.context(), "call.failed", &function);
      ir.CreateCondBr(result, normal, failed);
      ir.SetInsertPoint(failed);
      failedOperation();
      ir.SetInsertPoint(normal);
    }
    return constructed                     ? constructed
           : resultSlot                    ? load(resultSlot, resultType)
           : result->getType()->isVoidTy() ? nullptr
                                           : result;
  }
  void lifecycle(std::span<const MirLifecycleEvent> events) {
    for (const auto &event : events) {
      if (event.source)
        ir.CreateStore(ir.getFalse(), live.at(event.source));
      if (event.target)
        ir.CreateStore(ir.getTrue(), live.at(event.target));
    }
  }
  void drop(const MirInstruction &instruction) {
    const MirDropObligation *obligation = nullptr;
    for (const auto &event : instruction.lifecycle)
      if (event.kind == MirLifecycleEventKind::Drop)
        obligation = input.findDropObligation(event.source);
    if (!obligation && instruction.destination)
      for (const auto &candidate : input.dropObligations)
        if (candidate.place == *instruction.destination &&
            candidate.kind == MirDropObligationKind::Binding)
          obligation = &candidate;
    const auto *recipe = instruction.replacementCleanup
                             ? &*instruction.replacementCleanup
                         : obligation ? &obligation->dropType
                                      : nullptr;
    if (!recipe || (!obligation && !instruction.destination))
      throw BackendUnsupportedError(
          "LLVM cleanup requires its exact sealed obligation/recipe");
    auto *done =
        llvm::BasicBlock::Create(session.context(), "drop.done", &function);
    if (obligation) {
      auto *run =
          llvm::BasicBlock::Create(session.context(), "drop", &function);
      auto *active = ir.CreateLoad(ir.getInt1Ty(), live.at(obligation->id));
      ir.CreateStore(ir.getFalse(), live.at(obligation->id));
      ir.CreateCondBr(active, run, done);
      ir.SetInsertPoint(run);
    }
    const auto &term = current->terminator;
    const bool checked = term.kind == MirTerminatorKind::Invoke &&
                         term.invokeInstruction == instruction.id;
    llvm::Value *channel = nullptr;
    if (checked)
      channel = channels.at(term.failureRecord);
    else {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      channel = entry.CreateAlloca(session.abi().failureChannel());
    }
    auto *address = memory.address(
        instruction.destination.value_or(obligation ? obligation->place : 0));
    if (obligation &&
        obligation->kind == MirDropObligationKind::ObjectAllocation) {
      auto *allocation = load(address, recipe->type);
      store(llvm::ConstantPointerNull::get(session.layouts().pointer()),
            address, recipe->type);
      objectMemory.releaseBytes(allocation);
      ir.CreateBr(done);
      ir.SetInsertPoint(done);
      return;
    }
    auto *status = ir.CreateCall(
        lifetimes.cleanup(*recipe, instruction.kind ==
                                       MirInstructionKind::CleanupBeforeAssign),
        {address, channel, ir.getInt1(current->activeFailure != 0)});
    if (checked) {
      auto *bad =
          llvm::BasicBlock::Create(session.context(), "drop.failed", &function);
      ir.CreateCondBr(status, done, bad);
      ir.SetInsertPoint(bad);
      failedOperation();
    } else {
      ir.CreateBr(done);
    }
    ir.SetInsertPoint(done);
  }
  void instruction(const MirInstruction &instruction) {
    stage = &instruction;
    if (instruction.tombstone)
      return;
    const auto published = recordPublication(instruction);
    llvm::Value *publishedAddress =
        published.matched() ? publicationAddress(*published.initializer)
                            : nullptr;
    llvm::Value *result = nullptr;
    const auto *hosted =
        input.kind == MirBodyKind::HostedStartup
            ? hostedArgumentOperation(session.program().mir(), input, *current,
                                      instruction)
            : nullptr;
    switch (instruction.kind) {
    case MirInstructionKind::Compute: {
      if (instruction.operation == MirOperation::NativeCallback) {
        if (!nativeCCallbackStageContract(session.program().mir(),
                                         session.program().target(), input,
                                         instruction))
          throw std::logic_error("LLVM callback lost its exact conversion stage");
        result = nativeCCallbackAddress(session, ir,
                                        *instruction.nativeCallbackAdapter);
        break;
      }
      if (hosted) {
        auto *source = operand(instruction.operands.at(0));
        if (hosted->kind ==
            MirHostedStartupOperationKind::ConvertArgumentCount) {
          guard(hostedCountOutOfRange(ir, source),
                {DefinedFailureCode::NumericConversionOutOfRange,
                 DefinedFailureDetail::HostedArgumentCount});
          result = ir.CreateTrunc(source, ir.getInt32Ty());
        } else if (hosted->kind ==
                   MirHostedStartupOperationKind::ReadArgumentView) {
          result = hostedArgumentView(session, ir, nativeArguments, source);
        } else {
          throw std::logic_error(
              "LLVM hosted compute has the wrong sealed role");
        }
        break;
      }
      if (instruction.operation == MirOperation::FunctionItem) {
        result = ir.getInt8(0);
        break;
      }
      if (instruction.operation == MirOperation::Closure) {
        result = constructClosure(instruction);
        break;
      }
      if (instruction.operation == MirOperation::PayloadConstruct ||
          (instruction.operation == MirOperation::EnumConstant &&
           session.program().mir().findPayloadEnum(instruction.info.type))) {
        const auto &type = instruction.info.type;
        const auto &layout = session.layouts().payload(type);
        const auto variant = instruction.enumVariant.value();
        result = destination(*instruction.result);
        ir.CreateStore(ir.getInt32(PayloadLayout::inactive),
                       payloadTagAddress(ir, layout, result));
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          const auto &source = instruction.operands[index];
          if (source.kind == MirOperandKind::Value)
            if (const auto *target = placement.target(source.value);
                target && target->aggregate == *instruction.result &&
                target->variant == variant && target->element == index)
              continue;
          store(operand(instruction.operands[index]),
                payloadFieldAddress(ir, layout, result, variant, index),
                instruction.operands[index].type);
        }
        ir.CreateStore(ir.getInt32(variant),
                       payloadTagAddress(ir, layout, result));
        break;
      }
      if (instruction.operation == MirOperation::PayloadExtract) {
        const auto type = mirInstructionEnumType(instruction);
        const auto &layout = session.layouts().payload(type);
        const auto &source = instruction.operands.at(0);
        const auto &fieldType = session.program()
                                    .mir()
                                    .findPayloadEnum(type)
                                    ->variants.at(*instruction.enumVariant)
                                    .payloadTypes.at(*instruction.payloadIndex);
        auto *field = payloadFieldAddress(
            ir, layout,
            source.place ? memory.address(source.place) : operand(source),
            instruction.enumVariant.value(), instruction.payloadIndex.value());
        const auto kind = instruction.payloadExtractionKind.value();
        result = kind == PayloadExtractionKind::BorrowValue
                     ? field
                     : load(field, fieldType);
        if (kind == PayloadExtractionKind::MoveValue &&
            addressValue(session.program().mir(), instruction.info.type)) {
          auto *target = destination(*instruction.result);
          store(result, target, instruction.info.type);
          result = target;
          lifetimes.moved(ir, field, instruction.info.type);
        }
        break;
      }
      if (instruction.destination && instruction.operands.size() == 1 &&
          instruction.operation >= MirOperation::AddAssign &&
          instruction.operation <= MirOperation::ShiftRightAssign) {
        const auto &place = *input.findPlace(*instruction.destination);
        auto *address = memory.address(place.id);
        const std::array inputs{load(address, place.type),
                                operand(instruction.operands.front())};
        result = scalars.mutate(instruction, place.type, inputs);
        store(result, address, place.type);
        break;
      }
      if ((instruction.operation == MirOperation::Equal ||
           instruction.operation == MirOperation::NotEqual) &&
          instruction.operands.at(0).type.kind == SemanticType::StringView) {
        result = equalText(session, ir, operand(instruction.operands[0]),
                           operand(instruction.operands[1]));
        if (instruction.operation == MirOperation::NotEqual)
          result = ir.CreateNot(result);
        break;
      }
      if (instruction.operation == MirOperation::AddressOf) {
        result = memory.selected(instruction.operands.at(0));
        break;
      }
      if (instruction.operation == MirOperation::PointerAdd ||
          instruction.operation == MirOperation::PointerSubtract ||
          instruction.operation == MirOperation::PointerDifference) {
        auto *left = operand(instruction.operands.at(0));
        auto *right = operand(instruction.operands.at(1));
        const bool reversed =
            instruction.operation == MirOperation::PointerAdd &&
            instruction.operands[1].type.kind == SemanticType::RawPointer;
        const auto &type = instruction.operands[reversed ? 1 : 0].type;
        if (reversed)
          std::swap(left, right);
        if (instruction.operation == MirOperation::PointerDifference) {
          result = memory.difference(left, right, type.arguments.at(0));
        } else {
          const auto domain = constantIntegerDomain(
              instruction.operands[reversed ? 0 : 1].type);
          result = memory.advance(
              left, type.arguments.at(0), right, domain && domain->signedValue,
              instruction.operation == MirOperation::PointerSubtract);
        }
        break;
      }
      if ((instruction.operation == MirOperation::Equal ||
           instruction.operation == MirOperation::NotEqual) &&
          session.layouts()
              .get(instruction.operands.at(0).type)
              .value->isPointerTy()) {
        auto *left = operand(instruction.operands[0]);
        auto *right = operand(instruction.operands[1]);
        result = instruction.operation == MirOperation::Equal
                     ? ir.CreateICmpEQ(left, right)
                     : ir.CreateICmpNE(left, right);
        break;
      }
      if (instruction.operation == MirOperation::Aggregate) {
        const auto &type = instruction.info.type;
        if (type.kind != SemanticType::Array)
          throw BackendUnsupportedError(
              "LLVM aggregate requires a fixed array");
        if (addressValue(session.program().mir(), type)) {
          result = destination(*instruction.result);
          for (std::size_t index = 0; index < instruction.operands.size();
               ++index) {
            const auto &source = instruction.operands[index];
            // Fresh elements already occupy this slot. Do not even load them
            // into an SSA aggregate: their constructor observed this address.
            if (source.kind == MirOperandKind::Value &&
                placement.target(source.value)) {
              const auto &target = *placement.target(source.value);
              if (target.aggregate == *instruction.result &&
                  target.element == index)
                continue;
            }
            auto *slot =
                ir.CreateGEP(session.layouts().get(type).storage, result,
                             {ir.getInt64(0), ir.getInt64(index)});
            store(operand(source), slot, type.arguments.at(0));
          }
          break;
        }
        result =
            llvm::Constant::getNullValue(session.layouts().get(type).value);
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          auto *element = operand(instruction.operands[index]);
          if (type.arguments.at(0) == SemanticType::Bool)
            element = ir.CreateZExt(element, ir.getInt8Ty());
          if (type.arguments.at(0).kind == SemanticType::Class)
            element = ir.CreateLoad(
                session.layouts().get(type.arguments.at(0)).storage, element);
          result = ir.CreateInsertValue(result, element,
                                        static_cast<unsigned>(index));
        }
        break;
      }
      if (instruction.operation == MirOperation::Index) {
        const auto &source = instruction.operands.at(0);
        auto *container = operand(source);
        const auto &offset = instruction.operands.at(1);
        auto domain = constantIntegerDomain(offset.type);
        auto *index = ir.CreateIntCast(operand(offset), ir.getInt64Ty(),
                                       domain && domain->signedValue);
        auto *address =
            source.type.kind == SemanticType::RawPointer
                ? memory.advance(container, source.type.arguments.at(0), index,
                                 true, false)
                : memory.index(memory.arrayValue(container, source.type),
                               source.type, index, true);
        result = load(address, instruction.info.type);
        break;
      }
      std::vector<llvm::Value *> inputs;
      for (const auto &value : instruction.operands)
        inputs.push_back(operand(value));
      result = scalars.compute(instruction, inputs);
      break;
    }
    case MirInstructionKind::Load:
    case MirInstructionKind::Move:
      if (isFreshHierarchyTransferProducer(instruction) &&
          containsHierarchyStorage(session.program().mir(),
                                   instruction.info.type)) {
        result = freshTransfer(instruction);
        break;
      }
      [[fallthrough]];
    case MirInstructionKind::CallInput:
      if (hosted) {
        if (hosted->kind !=
                MirHostedStartupOperationKind::ValidateArgumentCount ||
            !nativeArgumentCount ||
            !nativeArgumentCount->getType()->isIntegerTy(32))
          throw std::logic_error(
              "LLVM hosted count has no native C Int32 argument");
        result = ir.CreateSExt(nativeArgumentCount, ir.getInt64Ty());
        guard(ir.CreateICmpSLT(result, ir.getInt64(0)),
              {DefinedFailureCode::HostedRuntimeContractFailure,
               DefinedFailureDetail::NegativeArgumentCount});
        break;
      }
      result = operand(instruction.operands.at(0), publishedAddress);
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.info.type == SemanticType::CString &&
          instruction.operands[0].type == SemanticType::StringView) {
        if (!wholeCStringLiteral(input, instruction.operands[0]))
          throw BackendUnsupportedError(
              "LLVM C-string call input requires exact complete source-literal "
              "provenance");
        result = ir.CreateExtractValue(result, 0);
      }
      if (instruction.kind == MirInstructionKind::Move &&
          instruction.destination)
        store(result, memory.address(*instruction.destination),
              input.findPlace(*instruction.destination)->type);
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.destination) {
        const auto &place = *input.findPlace(*instruction.destination);
        auto *destination = memory.address(place.id);
        // Publish before activating the prepared parameter's drop obligation:
        // a later argument can fail before ownership reaches the callee.
        if (result != destination)
          store(result, destination, place.type);
        if (addressValue(session.program().mir(), place.type))
          result = destination;
      }
      break;
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign: {
      const auto &place = *input.findPlace(*instruction.destination);
      if (instruction.operands.empty()) {
        if (owner &&
            isMirDeclarationOnlyFieldInitialization(*owner, input, instruction))
          break; // Storage declaration only; the constructor owns its write.
        if (input.kind != MirBodyKind::Module ||
            !globals.contains(place.symbol))
          throw std::logic_error(
              "LLVM data initialization lost its sealed global");
        break;
      }
      if (instruction.kind == MirInstructionKind::Initialize &&
          place.type.kind == SemanticType::Reference &&
          place.type.arguments.at(0).kind == SemanticType::Class) {
        // The loan still protects its original object. Only this reference
        // slot publishes the selected destination view; later uses load an
        // already-adjusted address and must not repeat this conversion.
        auto presentation = instruction.operands.at(0);
        presentation.type = place.type;
        result = memory.selected(presentation);
      } else {
        result = operand(instruction.operands.at(0));
      }
      if (instruction.kind == MirInstructionKind::Assign &&
          instruction.operation >= MirOperation::AddAssign &&
          instruction.operation <= MirOperation::ShiftRightAssign) {
        const std::array inputs{load(memory.address(place.id), place.type),
                                result};
        result = scalars.mutate(instruction, place.type, inputs);
      }
      if (instruction.kind == MirInstructionKind::Initialize &&
          instruction.operands[0].kind == MirOperandKind::Value &&
          placement.target(instruction.operands[0].value)) {
        const auto &target = *placement.target(instruction.operands[0].value);
        if (target.place == place.id)
          break;
      }
      if (place.type.kind != SemanticType::Reference)
        result = scalars.convert(result, instruction.operands[0].type,
                                 place.type, false);
      store(result, memory.address(place.id), place.type);
      break;
    }
    case MirInstructionKind::AllocateObject:
      store(objectMemory.allocateObject(instruction),
            memory.address(*instruction.destination), instruction.info.type);
      break;
    case MirInstructionKind::InitializeBase: {
      if (!constructor || !instruction.destination)
        throw BackendUnsupportedError(
            "LLVM base initialization requires its exact selected constructor "
            "and destination");
      auto *target = memory.address(*instruction.destination);
      if (instruction.constructorTarget) {
        lifetimes.active(ir, target, instruction.info.type, false);
        static_cast<void>(call(instruction, target));
      } else {
        lifetimes.generatedEmpty(ir, target, input, instruction, constructor);
      }
      // call() leaves IR on the existing normal continuation. A failed base
      // retains its own phase and cannot install any derived dispatch state.
      if (constructor->dispatchState &&
          constructor->dispatchState->afterBaseInitialization == instruction.id)
        dispatch.install(ir, thisAddress,
                         constructor->dispatchState->activeClass);
      break;
    }
    case MirInstructionKind::Construct: {
      if (isFreshHierarchyTransferProducer(instruction) &&
          containsHierarchyStorage(session.program().mir(),
                                   instruction.info.type)) {
        result = freshTransfer(instruction);
        break;
      }
      auto *target = destination(*instruction.result);
      if (instruction.info.type.kind == SemanticType::Class &&
          uninitializedStorageRepresentation(
              session.program().mir(),
              session.layouts().instance(instruction.info.type))) {
        const auto &object = session.layouts().instance(instruction.info.type);
        if (instruction.constructorTarget || !instruction.operands.empty() ||
            instruction.constructorKind != ConstructorKind::Ordinary ||
            object.defaultConstructor != SpecialMemberStatus::Generated ||
            object.generatedDefaultRequiresSchedule)
          throw BackendUnsupportedError(
              "LLVM vacant storage requires exact generated reservation");
        result = target;
        break; // No payload lifetime, zeroing, state byte or cleanup is begun.
      }
      lifetimes.active(ir, target, instruction.info.type, false);
      if (instruction.constructorTarget) {
        result = call(instruction, target);
      } else if (mir_detail::passiveUnionFieldConstruction(
                     session.program().mir(), instruction)) {
        const auto &object = session.layouts().instance(instruction.info.type);
        const auto index = session.layouts().fieldIndex(
            object.type, object.declaredFields.front().symbol);
        // The sealed union projection retains the same address at offset zero.
        // Initialize only the selected field, including its typed copy width.
        const auto &field = object.declaredFields.at(index);
        store(operand(instruction.operands.front()), target, field.type);
        lifetimes.active(ir, target, object.type, true);
        result = target;
      } else if (instruction.constructorKind == ConstructorKind::Ordinary &&
                 instruction.operands.empty() &&
                 instruction.info.type.kind == SemanticType::Class) {
        if (containsHierarchyStorage(session.program().mir(),
                                     instruction.info.type)) {
          lifetimes.generatedEmpty(ir, target, input, instruction);
        } else {
          const auto &object =
              session.layouts().instance(instruction.info.type);
          if (object.defaultConstructor != SpecialMemberStatus::Generated ||
              object.generatedDefaultRequiresSchedule ||
              (!object.declaredFields.empty() && !object.unionLayout) ||
              !object.bases.empty())
            throw BackendUnsupportedError(
                "LLVM targetless default construction requires an exact empty "
                "generated object");
          ir.CreateStore(llvm::Constant::getNullValue(
                             session.layouts().get(object.type).storage),
                         target);
          lifetimes.active(ir, target, object.type, true);
        }
        result = target;
      } else if (instruction.info.type.kind == SemanticType::Class &&
                 instruction.operands.size() == 1 &&
                 instruction.constructorKind != ConstructorKind::Ordinary) {
        auto *source = operand(instruction.operands[0]);
        store(source, target, instruction.info.type);
        if (instruction.constructorKind == ConstructorKind::Move)
          lifetimes.moved(ir, source, instruction.info.type);
        result = target;
      } else {
        throw BackendUnsupportedError("LLVM construction requires a selected "
                                      "constructor or generated transfer");
      }
      break;
    }
    case MirInstructionKind::Drop:
    case MirInstructionKind::CleanupBeforeAssign:
      drop(instruction);
      break;
    case MirInstructionKind::Call:
    case MirInstructionKind::CallBody:
      result = call(instruction);
      if (instruction.loan && result && result->getType()->isPointerTy())
        memory.publish(*instruction.loan, result);
      break;
    case MirInstructionKind::Borrow: {
      const auto &loan = *input.findLoan(*instruction.loan);
      const auto publication = fieldPublication(loan);
      if (publication.matched() &&
          publication.kind == StoredFieldPublicationKind::ProvenanceOnly)
        break;
      if (const auto &selection = loan.referenceSelection) {
        result = values.at(selection->value);
        if (!result)
          throw std::logic_error(
              "LLVM payload reference lost its dominating selected address");
      } else {
        result = memory.selected(instruction.operands.at(0));
      }
      memory.publish(*instruction.loan, result);
      publishStoredField(loan, result);
      break;
    }
    case MirInstructionKind::Lifecycle:
    case MirInstructionKind::EndBorrow:
    case MirInstructionKind::ActivateBorrow:
    case MirInstructionKind::ParkRangeCursor:
    case MirInstructionKind::RestoreRangeCursor:
      break; // Scalar permission/availability transitions have no runtime
             // state.
    case MirInstructionKind::BeginThreadLocals:
      ir.CreateCall(
          declareRuntimeService(session, RuntimeService::BeginThreadLocals));
      break;
    case MirInstructionKind::EnsureThreadLocal: {
      const auto *storage = session.program().mir().findThreadLocalStorage(
          instruction.threadLocalStorage);
      if (storage && storage->dataInitializer)
        break;
      const auto &term = current->terminator;
      if (term.kind != MirTerminatorKind::Invoke ||
          term.invokeInstruction != instruction.id)
        throw std::logic_error("LLVM dynamic TLS access lost its Invoke");
      threadLocals.ensure(
          ir, instruction.threadLocalStorage, channels.at(term.failureRecord),
          [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
            guard(condition, outcome);
          },
          [&] { failedOperation(); });
      break;
    }
    case MirInstructionKind::DrainThreadLocals: {
      auto *null = llvm::ConstantPointerNull::get(session.layouts().pointer());
      auto *channel = current->activeFailure
                          ? channels.at(current->activeFailure)
                          : nullptr;
      ir.CreateCall(
          declareRuntimeService(session, RuntimeService::DrainThreadLocals),
          {channel ? failures.record(ir, channel) : null,
           channel ? failures.origin(ir, channel) : null});
      break;
    }
    default:
      throw BackendUnsupportedError(
          "LLVM instruction shape is not implemented");
    }
    if (instruction.result && result)
      values.at(*instruction.result) = result;
    if (published.matched()) {
      if (!result)
        throw std::logic_error(
            "LLVM constructor publication lost its source result");
      if (published.transport ==
          UnstagedConstructorFieldPublicationTransport::ExactValue) {
        store(result, publishedAddress, published.targetType);
      } else if (published.transport ==
                 UnstagedConstructorFieldPublicationTransport::
                     NullUniqueOwner) {
        auto *storage = llvm::dyn_cast<llvm::PointerType>(
            session.layouts().get(published.targetType).storage);
        if (!storage)
          throw std::logic_error(
              "LLVM null owner publication lost its pointer storage");
        store(llvm::ConstantPointerNull::get(storage), publishedAddress,
              published.targetType);
      } else {
        throw std::logic_error(
            "LLVM constructor publication lost its exact transport");
      }
    }
    lifecycle(instruction.lifecycle);
  }

  void terminator(const MirBlock &block) {
    if (!ir.GetInsertBlock()->empty() &&
        ir.GetInsertBlock()->back().isTerminator())
      return;
    const auto &term = block.terminator;
    switch (term.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Invoke:
      lifecycle(term.successLifecycle);
      jump(term.target, term.targetArguments);
      break;
    case MirTerminatorKind::ContinueFailure: {
      auto *channel = channels.at(term.failureRecord);
      ir.CreateStore(ir.CreateLoad(session.abi().failureChannel(),
                                   channels.at(block.activeFailure)),
                     channel);
      jump(term.target, term.targetArguments);
      break;
    }
    case MirTerminatorKind::Branch: {
      auto *condition = operand(*term.value);
      auto *taken = edge(term.target, term.targetArguments);
      auto *alternate = edge(term.elseTarget, term.elseArguments);
      ir.CreateCondBr(condition, taken, alternate);
      break;
    }
    case MirTerminatorKind::Switch: {
      auto *value = operand(*term.value);
      const auto &type = term.value->type.kind == SemanticType::Reference
                             ? term.value->type.arguments.at(0)
                             : term.value->type;
      if (session.program().mir().findPayloadEnum(type)) {
        const auto &layout = session.layouts().payload(type);
        value = ir.CreateLoad(ir.getInt32Ty(),
                              payloadTagAddress(ir, layout, value));
      }
      llvm::BasicBlock *fallback = nullptr;
      for (const auto &target : term.switchTargets)
        if (!target.value) {
          fallback = edge(target.target, target.arguments);
        }
      if (!fallback) {
        fallback = llvm::BasicBlock::Create(session.context(),
                                            "switch.exhaustive", &function);
        llvm::IRBuilder<> dead(fallback);
        dead.CreateUnreachable();
      }
      auto *output = ir.CreateSwitch(value, fallback);
      for (const auto &target : term.switchTargets)
        if (target.value) {
          const auto &constant = target.value->value;
          auto *bits = llvm::ConstantInt::get(
              llvm::cast<llvm::IntegerType>(value->getType()),
              constant.negative ? uint64_t(0) - constant.magnitude
                                : constant.magnitude);
          output->addCase(bits, edge(target.target, target.arguments));
        }
      break;
    }
    case MirTerminatorKind::Return: {
      if (input.kind == MirBodyKind::Constructor && owner)
        lifetimes.active(ir, thisAddress, owner->type, true);
      auto *value = term.value ? operand(*term.value) : nullptr;
      if (term.value) {
        const auto conversion =
            mir_detail::scalarReturnConversion(input, *term.value);
        if (conversion == mir_detail::MirScalarReturnConversion::Invalid)
          throw std::logic_error(
              "LLVM scalar return lost its verified conversion");
        if (conversion == mir_detail::MirScalarReturnConversion::Initialization)
          value =
              scalars.convert(value, term.value->type, input.returnType, false);
      }
      if (input.kind == MirBodyKind::HostedStartup) {
        const auto &plan = session.program().mir().hostedStartupPlan();
        const auto *operation =
            plan ? plan->findOperation(term.hostedStartupOperation) : nullptr;
        if (!operation || operation->block != block.id)
          throw std::logic_error(
              "LLVM hosted return lost its sealed startup operation");
        value = hostedReturnStatus(ir, *operation, value);
      }
      if (returnDestination && value &&
          !(term.value->kind == MirOperandKind::Value &&
            placement.target(term.value->value) &&
            placement.target(term.value->value)->returned))
        store(value, returnDestination, input.returnType);
      if (abi.statusResult)
        ir.CreateRet(ir.getTrue());
      else if (returnDestination || !value)
        ir.CreateRetVoid();
      else
        ir.CreateRet(value);
      break;
    }
    case MirTerminatorKind::PropagateFailure:
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      // The evaluator permits no cleanup in any traversed GTI frame. The
      // test-owned shim copies the channel before unwinding to its outer catch.
      // This is not an ordinary runtime C import or a production unwind edge.
      leaveFailureCleanup();
      ir.CreateCall(
          session.module().getOrInsertFunction(
              "__gti_c04_evaluate_raise",
              llvm::FunctionType::get(ir.getVoidTy(),
                                      {session.layouts().pointer()}, false)),
          {channels.at(term.failureRecord)});
      ir.CreateUnreachable();
      break;
#endif
      if (!failureDestination)
        throw std::logic_error("LLVM failure propagation has no destination");
      ir.CreateStore(ir.CreateLoad(session.abi().failureChannel(),
                                   channels.at(term.failureRecord)),
                     failureDestination);
      leaveFailureCleanup();
      ir.CreateRet(ir.getFalse());
      break;
    case MirTerminatorKind::ContainFailure:
      leaveFailureCleanup();
      failures.terminate(ir, channels.at(term.failureRecord));
      break;
    case MirTerminatorKind::TerminateCleanupFailure:
      failures.terminateCleanup(ir, channels.at(block.activeFailure),
                                channels.at(term.failureRecord));
      break;
    case MirTerminatorKind::Exit:
      if (abi.statusResult)
        ir.CreateRet(ir.getTrue());
      else
        ir.CreateRetVoid();
      break;
    case MirTerminatorKind::Unreachable:
      ir.CreateUnreachable();
      break;
    case MirTerminatorKind::None:
      throw std::logic_error("LLVM body has an unterminated MIR block");
    }
  }

  void emit(std::span<const ParameterLane> parameters) {
    if (constructor && owner && owner->polymorphic &&
        (!constructor->dispatchState ||
         constructor->definitionKind != MirDefinitionKind::Source))
      throw BackendUnsupportedError("LLVM polymorphic construction requires "
                                    "its verified active-class schedule");
    auto *entry =
        llvm::BasicBlock::Create(session.context(), "entry", &function);
    ir.SetInsertPoint(entry);
    if (std::any_of(input.blocks.begin(), input.blocks.end(),
                    [](const MirBlock &block) { return block.activeFailure != 0; })) {
      // A defined-failure chain cannot rejoin normal execution. One token
      // spans that invocation's cleanup, with no TLS work on successful paths.
      cleanupToken = ir.CreateAlloca(session.layouts().pointer());
      ir.CreateStore(llvm::ConstantPointerNull::get(session.layouts().pointer()),
                     cleanupToken);
    }
    for (const auto &block : input.blocks) {
      blocks.at(block.id) = llvm::BasicBlock::Create(
          session.context(), "bb" + std::to_string(block.id), &function);
      llvm::IRBuilder<> header(blocks.at(block.id));
      for (const auto &parameter : block.parameters)
        values.at(parameter.value) =
            header.CreatePHI(session.layouts().get(parameter.type).value, 0);
    }
    memory.allocate(globals);
    if (input.kind == MirBodyKind::HostedStartup &&
        function.getFunctionType() != abi.type)
      throw std::logic_error(
          "LLVM hosted declaration/body native ABI disagrees");
    if (input.kind == MirBodyKind::HostedStartup
            ? !parameters.empty()
            : parameters.size() !=
                  static_cast<std::size_t>(std::count_if(
                      abi.parameters.begin(), abi.parameters.end(),
                      [](const auto &parameter) {
                        return parameter.channel == AbiChannel::Argument;
                      })))
      throw std::logic_error(
          "LLVM incoming ABI arguments differ from sealed parameter bindings");
    for (std::size_t index = 0; index < abi.parameters.size(); ++index) {
      auto *argument = function.getArg(index);
      const auto &parameter = abi.parameters[index];
      switch (parameter.channel) {
      case AbiChannel::ResultDestination:
        returnDestination = argument;
        break;
      case AbiChannel::FailureDestination:
        failureDestination = argument;
        break;
      case AbiChannel::CallerSite:
        callerSite = argument;
        break;
      case AbiChannel::CallerArtifact:
        callerArtifact = argument;
        break;
      case AbiChannel::Argument:
        if (input.kind == MirBodyKind::HostedStartup) {
          if (parameter.sourceIndex == 0 &&
              argument->getType()->isIntegerTy(32))
            nativeArgumentCount = argument;
          else if (parameter.sourceIndex == 1 &&
                   argument->getType()->isPointerTy())
            nativeArguments = argument;
          else
            throw std::logic_error(
                "LLVM hosted arguments differ from the native C plan");
          break;
        }
        if (parameter.sourceIndex >= parameters.size())
          throw std::logic_error(
              "LLVM ABI argument exceeds its exact parameter map");
        memory.parameter(parameters[parameter.sourceIndex], argument,
                         parameter.indirect);
        break;
      case AbiChannel::Receiver:
        if (!receiverType)
          throw std::logic_error("LLVM receiver has no sealed object type");
        thisAddress = argument;
        memory.receiver(argument, *receiverType);
        break;
      }
    }
    if (input.kind == MirBodyKind::HostedStartup &&
        session.program().mir().hostedStartupPlan()->kind ==
            ProgramEntryKind::OwnedArguments &&
        (!nativeArgumentCount || !nativeArguments))
      throw std::logic_error(
          "LLVM hosted arguments were not supplied by the native entry");
    for (const auto &record : input.failureRecords)
      channels.at(record.id) = ir.CreateAlloca(session.abi().failureChannel());
    if (constructor && constructor->dispatchState &&
        constructor->dispatchState->afterBaseInitialization == 0)
      dispatch.install(ir, thisAddress,
                       constructor->dispatchState->activeClass);
    // A constructor's entry Stored loans are the sealed field-publication
    // contract. Their referent address, not the Load of its scalar contents,
    // initializes the physical reference field.
    if (input.kind == MirBodyKind::Constructor && owner)
      for (const auto &loan : input.loans)
        if (loan.entry && loan.kind == MirLoanKind::Stored &&
            loan.storedField) {
          const auto publication = fieldPublication(loan);
          if (publication.kind == StoredFieldPublicationKind::ProvenanceOnly)
            continue;
          const auto &source = *input.findPlace(loan.source);
          auto *address = memory.address(source.id);
          if (source.type.kind == SemanticType::Reference)
            address = load(address, source.type);
          publishStoredField(loan, address);
        }
    for (const auto &obligation : input.dropObligations) {
      live.at(obligation.id) = ir.CreateAlloca(ir.getInt1Ty());
      ir.CreateStore(ir.getInt1(obligation.initiallyActive),
                     live.at(obligation.id));
    }
    ir.CreateBr(blocks.at(input.entry));

    // RPO ensures ordinary SSA definitions precede their uses. Loop headers
    // already have PHIs, whose backedge inputs are completed by predecessor
    // emission.
    std::set<MirBlockId> visited;
    std::vector<MirBlockId> order;
    std::function<void(MirBlockId)> visit = [&](MirBlockId id) {
      if (!id || !visited.insert(id).second)
        return;
      const auto &term = input.findBlock(id)->terminator;
      visit(term.target);
      visit(term.elseTarget);
      for (const auto &target : term.switchTargets)
        visit(target.target);
      order.push_back(id);
    };
    visit(input.entry);
    std::reverse(order.begin(), order.end());
    for (auto id : order) {
      current = input.findBlock(id);
      ir.SetInsertPoint(blocks.at(id));
      for (const auto &row : current->instructions)
        instruction(row);
      terminator(*current);
    }
    for (const auto &block : input.blocks)
      if (!visited.contains(block.id)) {
        ir.SetInsertPoint(blocks.at(block.id));
        ir.CreateUnreachable();
      }
  }
};

const MirClassInstance *Bodies::owner(MirBodyAddress address) {
  const auto &program = session.program().mir();
  if (address.kind == MirBodyKind::FieldInitializers)
    return program.findClassInstance(address.owner);
  if (address.kind == MirBodyKind::Constructor)
    return program.findClassInstance(
        program.findConstructorInstance(address.owner)->owner);
  if (address.kind == MirBodyKind::Destructor)
    return program.findClassInstance(
        program.findDestructorInstance(address.owner)->owner);
  if (address.kind == MirBodyKind::Function) {
    const auto *target = program.findFunctionInstance(address.owner);
    if (target->owner && !target->staticMember)
      return program.findClassInstance(*target->owner);
  }
  return nullptr;
}
FunctionAbi Bodies::signature(MirBodyAddress address) {
  const auto &program = session.program().mir();
  if (address.kind == MirBodyKind::HostedStartup)
    return hostedEntryAbi(session);
  if (address.kind == MirBodyKind::Function) {
    const auto &target = *program.findFunctionInstance(address.owner);
    return session.abi().gti(target.returnType, target.parameterTypes,
                             target.mayRaiseDefinedFailure, owner(address),
                             target.tracksCaller);
  }
  if (address.kind == MirBodyKind::Constructor) {
    const auto &target = *program.findConstructorInstance(address.owner);
    return session.abi().gti(SemanticType::Void, target.parameterTypes,
                             target.mayRaiseDefinedFailure, true);
  }
  if (address.kind == MirBodyKind::Destructor) {
    const auto &target = *program.findDestructorInstance(address.owner);
    return session.abi().gti(SemanticType::Void, {},
                             target.mayRaiseDefinedFailure, true);
  }
  if (address.kind == MirBodyKind::Lambda) {
    const auto &target = *program.findLambda(address.owner);
    return session.abi().gti(target.returnType, target.parameterTypes,
                             !target.body.failureRecords.empty(),
                             !target.captureTypes.empty());
  }
  const auto &body = *findMirBody(program, address);
  return session.abi().gti(body.returnType, {},
                           address.kind != MirBodyKind::HostedStartup &&
                               !body.failureRecords.empty(),
                           owner(address));
}
llvm::Function *Bodies::function(MirBodyAddress address) {
  const auto key = std::pair(address.kind, address.owner);
  if (const auto found = functions.find(key); found != functions.end())
    return found->second;
  llvm::Function *output = nullptr;
  if (address.kind == MirBodyKind::Function)
    output = session.abi().declareFunction(address.owner);
  else if (address.kind == MirBodyKind::HostedStartup) {
    const auto plan = hostedEntryAbi(session);
    output =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               "main", session.module());
    plan.applyTo(*output);
    session.requireDefinition(*output);
  } else if (address.kind == MirBodyKind::Module ||
             address.kind == MirBodyKind::Lambda ||
             address.kind == MirBodyKind::Constructor ||
             address.kind == MirBodyKind::Destructor ||
             address.kind == MirBodyKind::FieldInitializers ||
             address.kind == MirBodyKind::StaticFieldInitializers ||
             address.kind == MirBodyKind::ThreadLocalInitializer) {
    const auto plan = signature(address);
    output = llvm::Function::Create(
        plan.type, llvm::GlobalValue::InternalLinkage,
        "__gti_native_v1_body_" +
            std::to_string(static_cast<unsigned>(address.kind)) + "_" +
            std::to_string(address.owner),
        session.module());
    plan.applyTo(*output);
    session.requireDefinition(*output);
  } else
    throw BackendUnsupportedError("LLVM body kind is not implemented");
  functions.emplace(key, output);
  return output;
}
void Bodies::body(MirBodyAddress address) {
  const auto &input = *findMirBody(session.program().mir(), address);
  const auto plan = signature(address);
  if (address.kind == MirBodyKind::Function) {
    const auto &declaration =
        *session.program().mir().findFunctionInstance(address.owner);
    if (declaration.definitionKind != MirDefinitionKind::Source)
      return;
  }
  const auto parameters =
      parameterPlan(session.program().mir(), address, input);
  const auto *lambda = address.kind == MirBodyKind::Lambda
                           ? session.program().mir().findLambda(address.owner)
                           : nullptr;
  const auto *classOwner = owner(address);
  std::optional<SemanticType> receiverType;
  if (classOwner)
    receiverType = classOwner->type;
  else if (lambda && !lambda->captureTypes.empty())
    receiverType = lambda->type;
  Body emission(
      session, failures, dispatch, lifetimes, threadLocals, *function(address),
      address, input, plan, parameters.concrete.lanes,
      parameters.concrete.packs, parameters.sourceBindings, globals, callables,
      [&](MirBodyAddress target) { return function(target); },
      [&](MirBodyAddress target) { return signature(target); }, classOwner,
      address.kind == MirBodyKind::Constructor
          ? session.program().mir().findConstructorInstance(address.owner)
          : nullptr,
      lambda, std::move(receiverType));
}
void Bodies::lower(const LlvmAdmissionPlan &admission) {
  threadLocals.allocate(globals);
  llvm::IRBuilder<> constants(session.context());
  Scalars scalars(session, constants, [](llvm::Value *, DefinedFailureOutcome) {
    throw std::logic_error("LLVM static datum cannot perform runtime checking");
  });
  const auto *module =
      findMirBody(session.program().mir(), {MirBodyKind::Module, 0});
  for (const auto &step :
       session.program().mir().programInitializationPlan().steps) {
    const auto &type = module->findPlace(step.storagePlace)->type;
    // Admission establishes the storage family. Allocate every admitted
    // initialization-plan root, including pointer and counted-text globals.
    auto *storage = session.layouts().get(type).storage;
    llvm::Constant *initial = llvm::Constant::getNullValue(storage);
    if (step.dataConstant)
      initial = scalars.constant(*step.dataConstant, type);
    const auto layout = session.layouts().get(type);
    if (session.dataLayout().getTypeAllocSize(initial->getType()) !=
        layout.size)
      throw std::logic_error(
          "LLVM global initializer differs from its sealed extent");
    auto *global = new llvm::GlobalVariable(
        session.module(), initial->getType(), false,
        llvm::GlobalValue::InternalLinkage, initial,
        "__gti_native_v1_g" + std::to_string(step.symbol));
    global->setAlignment(llvm::Align(layout.alignment));
    globals.emplace(step.symbol, global);
  }
  declareNativeCStorage(session, globals);
  for (auto address : admission.requiredBodies())
    static_cast<void>(function(address));
  std::vector<MirNativeCallbackAdapterId> callbacks;
  for (auto address : admission.requiredBodies())
    for (const auto &block :
         findMirBody(session.program().mir(), address)->blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.nativeCallbackAdapter)
          callbacks.push_back(*stage.nativeCallbackAdapter);
  emitNativeCCallbacks(session, failures, callbacks);
  for (auto address : admission.requiredBodies())
    body(address);
}
#if defined(GTI_LLVM_C04_MIR_EVALUATION)
void Bodies::lowerEvaluation(std::span<const HirFunctionInstanceId> selected) {
  for (auto id : selected) {
    auto *output = function({MirBodyKind::Function, id});
    // Equal, explicit call boundaries for the transport microbenchmark, not
    // a claim about normal whole-program inlining or production performance.
    output->addFnAttr(llvm::Attribute::NoInline);
    output->setUWTableKind(llvm::UWTableKind::Sync);
  }
  for (auto id : selected)
    body({MirBodyKind::Function, id});
}
#endif
} // namespace

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission) {
  Bodies(session, admission).lower(admission);
}

#if defined(GTI_LLVM_C04_MIR_EVALUATION)
void lowerC04Evaluation(Session &session, const LlvmAdmissionPlan &admission,
                        std::span<const HirFunctionInstanceId> functions) {
  validateC04Evaluation(session, admission, functions);
  Bodies(session, admission).lowerEvaluation(functions);
}
#endif

} // namespace lang::native_llvm
