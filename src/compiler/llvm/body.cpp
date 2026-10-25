#include "body.h"
#include "failure.h"
#include "runtime_abi.h"
#include "scalar.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace lang::native_llvm {
namespace {

class Bodies final {
public:
  explicit Bodies(Session &session) : session(session), failures(session) {}
  void lower(const LlvmAdmissionPlan &admission);

private:
  Session &session;
  Failures failures;
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  std::map<std::pair<MirBodyKind, std::size_t>, llvm::Function *> functions;
  [[nodiscard]] llvm::Function *function(MirBodyAddress address);
  void body(MirBodyAddress address);
};

class Body final {
public:
  Body(Session &session, Failures &failures, llvm::Function &function,
       const MirBody &body, FunctionAbi abi,
       std::span<const HirBindingId> parameters,
       const std::map<SymbolId, llvm::GlobalVariable *> &globals,
       std::function<llvm::Function *(MirBodyAddress)> findFunction)
      : session(session), failures(failures), function(function), input(body),
        abi(std::move(abi)), globals(globals), ir(session.context()),
        scalars(session, ir,
                [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
                  guard(condition, outcome);
                }),
        findFunction(std::move(findFunction)), values(body.values.size() + 1),
        places(body.places.size() + 1),
        channels(body.failureRecords.size() + 1),
        blocks(body.blocks.size() + 1) {
    emit(parameters);
  }

private:
  Session &session;
  Failures &failures;
  llvm::Function &function;
  const MirBody &input;
  FunctionAbi abi;
  const std::map<SymbolId, llvm::GlobalVariable *> &globals;
  llvm::IRBuilder<> ir;
  Scalars scalars;
  std::function<llvm::Function *(MirBodyAddress)> findFunction;
  std::vector<llvm::Value *> values, places, channels;
  std::vector<llvm::BasicBlock *> blocks;
  llvm::Value *returnDestination = nullptr, *failureDestination = nullptr;
  llvm::Value *callerSite = nullptr, *callerArtifact = nullptr;
  const MirBlock *current = nullptr;
  const MirInstruction *stage = nullptr;

  [[nodiscard]] llvm::Value *load(llvm::Value *address,
                                  const SemanticType &type) {
    const auto layout = session.layouts().get(type);
    auto *value = ir.CreateLoad(layout.storage, address);
    return type == SemanticType::Bool ? ir.CreateTrunc(value, ir.getInt1Ty())
                                      : value;
  }
  void store(llvm::Value *value, llvm::Value *address,
             const SemanticType &type) {
    if (type == SemanticType::Bool)
      value = ir.CreateZExt(value, ir.getInt8Ty());
    ir.CreateStore(value, address);
  }
  [[nodiscard]] llvm::Value *operand(const MirOperand &operand) {
    if (operand.literal)
      return scalars.literal(*operand.literal, operand.type);
    if (operand.value) {
      auto *value = values.at(operand.value);
      if (!value)
        throw std::logic_error("LLVM operand has no dominating value " +
                               std::to_string(operand.value));
      return value;
    }
    if (operand.place && (operand.kind == MirOperandKind::Copy ||
                          operand.kind == MirOperandKind::Move))
      return load(places.at(operand.place), operand.type);
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
    if (term.kind != MirTerminatorKind::Invoke ||
        term.invokeInstruction != stage->id)
      throw std::logic_error("LLVM checked operation lost its Invoke");
    auto *failed =
        llvm::BasicBlock::Create(session.context(), "check.failed", &function);
    auto *normal =
        llvm::BasicBlock::Create(session.context(), "check.success", &function);
    ir.CreateCondBr(condition, failed, normal);
    ir.SetInsertPoint(failed);
    failures.originate(ir, channels.at(term.failureRecord), *stage, outcome,
                       callerSite, callerArtifact);
    jump(term.elseTarget, term.elseArguments);
    ir.SetInsertPoint(normal);
  }
  [[nodiscard]] llvm::Value *call(const MirInstruction &instruction) {
    if (instruction.intrinsic == IntrinsicKind::Panic) {
      const auto &term = current->terminator;
      failures.originate(
          ir, channels.at(term.failureRecord), instruction,
          {DefinedFailureCode::Panic, DefinedFailureDetail::ExplicitPanic},
          callerSite, callerArtifact, operand(instruction.operands.at(0)));
      jump(term.elseTarget, term.elseArguments);
      return nullptr;
    }
    if (instruction.intrinsic != IntrinsicKind::None) {
      if (instruction.intrinsic ==
              IntrinsicKind::NumericTypeParameterConversion ||
          instruction.intrinsic == IntrinsicKind::NumericAliasConversion ||
          instruction.intrinsic ==
              IntrinsicKind::ExactTypeParameterConstruction)
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
    if (!target && !instruction.bodyTarget)
      throw BackendUnsupportedError(
          "LLVM scalar call requires an exact direct target");
    FunctionAbi signature;
    llvm::Function *callee = nullptr;
    if (target) {
      signature =
          target->linkage == LanguageLinkage::C
              ? session.abi().nativeC(target->returnType,
                                      target->parameterTypes)
              : session.abi().gti(target->returnType, target->parameterTypes,
                                  target->mayRaiseDefinedFailure, false,
                                  target->tracksCaller);
      callee = session.abi().declareFunction(target->id);
    } else {
      const auto *body =
          findMirBody(session.program().mir(), *instruction.bodyTarget);
      signature = session.abi().gti(body->returnType, {},
                                    !body->failureRecords.empty());
      callee = findFunction(*instruction.bodyTarget);
    }
    llvm::Value *resultSlot = nullptr;
    if (signature.resultDestination) {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      resultSlot = entry.CreateAlloca(
          session.layouts().get(instruction.info.type).storage);
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
        auto *value = operand(instruction.operands.at(parameter.sourceIndex));
        if (parameter.indirect) {
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
        throw BackendUnsupportedError(
            "LLVM instance receivers are pending O-LLVM-06");
      }
    }
    auto *result = ir.CreateCall(callee, arguments);
    signature.applyTo(*result);
    if (signature.statusResult) {
      auto *normal = llvm::BasicBlock::Create(session.context(), "call.success",
                                              &function);
      auto *failed =
          llvm::BasicBlock::Create(session.context(), "call.failed", &function);
      ir.CreateCondBr(result, normal, failed);
      ir.SetInsertPoint(failed);
      jump(current->terminator.elseTarget, current->terminator.elseArguments);
      ir.SetInsertPoint(normal);
    }
    return resultSlot ? load(resultSlot, instruction.info.type)
           : result->getType()->isVoidTy() ? nullptr
                                           : result;
  }
  void instruction(const MirInstruction &instruction) {
    stage = &instruction;
    if (instruction.tombstone)
      return;
    llvm::Value *result = nullptr;
    switch (instruction.kind) {
    case MirInstructionKind::Compute: {
      std::vector<llvm::Value *> inputs;
      for (const auto &value : instruction.operands)
        inputs.push_back(operand(value));
      result = scalars.compute(instruction, inputs);
      break;
    }
    case MirInstructionKind::Load:
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Move:
      result = operand(instruction.operands.at(0));
      if (instruction.kind == MirInstructionKind::Move &&
          instruction.destination)
        store(result, places.at(*instruction.destination),
              input.findPlace(*instruction.destination)->type);
      break;
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign: {
      const auto &place = *input.findPlace(*instruction.destination);
      if (instruction.operands.empty()) {
        if (input.kind != MirBodyKind::Module ||
            !globals.contains(place.symbol))
          throw std::logic_error(
              "LLVM data initialization lost its sealed global");
        break;
      }
      result = scalars.convert(operand(instruction.operands.at(0)),
                               instruction.operands[0].type, place.type, false);
      store(result, places.at(place.id), place.type);
      break;
    }
    case MirInstructionKind::Call:
    case MirInstructionKind::CallBody:
      result = call(instruction);
      break;
    case MirInstructionKind::Lifecycle:
    case MirInstructionKind::EndBorrow:
      break; // Scalar permission/availability transitions have no runtime
             // state.
    case MirInstructionKind::BeginThreadLocals:
      ir.CreateCall(
          declareRuntimeService(session, RuntimeService::BeginThreadLocals));
      break;
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
  }

  void terminator(const MirBlock &block) {
    if (!ir.GetInsertBlock()->empty() &&
        ir.GetInsertBlock()->back().isTerminator())
      return;
    const auto &term = block.terminator;
    switch (term.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Invoke:
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
      auto *value = term.value ? operand(*term.value) : nullptr;
      if (returnDestination && value)
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
      if (!failureDestination)
        throw std::logic_error("LLVM failure propagation has no destination");
      ir.CreateStore(ir.CreateLoad(session.abi().failureChannel(),
                                   channels.at(term.failureRecord)),
                     failureDestination);
      ir.CreateRet(ir.getFalse());
      break;
    case MirTerminatorKind::ContainFailure:
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

  void emit(std::span<const HirBindingId> parameters) {
    auto *entry =
        llvm::BasicBlock::Create(session.context(), "entry", &function);
    ir.SetInsertPoint(entry);
    for (const auto &block : input.blocks) {
      blocks.at(block.id) = llvm::BasicBlock::Create(
          session.context(), "bb" + std::to_string(block.id), &function);
      llvm::IRBuilder<> header(blocks.at(block.id));
      for (const auto &parameter : block.parameters)
        values.at(parameter.value) =
            header.CreatePHI(session.layouts().get(parameter.type).value, 0);
    }
    std::map<std::tuple<MirPlaceRootKind, std::size_t, std::size_t>,
             llvm::Value *>
        storage;
    for (const auto &place : input.places) {
      if (place.projections.empty() && globals.contains(place.symbol) &&
          (place.root == MirPlaceRootKind::Symbol ||
           input.kind == MirBodyKind::Module)) {
        places.at(place.id) = globals.at(place.symbol);
        continue;
      }
      if (!place.projections.empty() ||
          (place.root != MirPlaceRootKind::Binding &&
           place.root != MirPlaceRootKind::Temporary))
        throw BackendUnsupportedError(
            "LLVM scalar places require unprojected local bindings");
      const auto key =
          std::tuple(place.root,
                     place.root == MirPlaceRootKind::Binding ? place.binding
                                                             : place.temporary,
                     place.root == MirPlaceRootKind::Binding ? place.symbol
                                                             : std::size_t(0));
      auto [slot, inserted] = storage.try_emplace(key, nullptr);
      if (inserted)
        slot->second =
            ir.CreateAlloca(session.layouts().get(place.type).storage);
      places.at(place.id) = slot->second;
    }
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
        for (const auto &place : input.places)
          if (place.binding == parameters[parameter.sourceIndex])
            store(parameter.indirect ? load(argument, place.type) : argument,
                  places.at(place.id), place.type);
        break;
      case AbiChannel::Receiver:
        throw BackendUnsupportedError(
            "LLVM scalar body cannot have an instance receiver");
      }
    }
    for (const auto &record : input.failureRecords)
      channels.at(record.id) = ir.CreateAlloca(session.abi().failureChannel());
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

llvm::Function *Bodies::function(MirBodyAddress address) {
  const auto key = std::pair(address.kind, address.owner);
  if (const auto found = functions.find(key); found != functions.end())
    return found->second;
  llvm::Function *output = nullptr;
  if (address.kind == MirBodyKind::Function)
    output = session.abi().declareFunction(address.owner);
  else if (address.kind == MirBodyKind::HostedStartup) {
    if (!session.program().mir().hostedStartupPlan() ||
        session.program().mir().hostedStartupPlan()->kind !=
            ProgramEntryKind::NoArguments)
      throw BackendUnsupportedError(
          "LLVM hosted argument construction is pending O-LLVM-12");
    output = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(session.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, "main", session.module());
    session.requireDefinition(*output);
  } else if (address.kind == MirBodyKind::Module) {
    const auto *body = findMirBody(session.program().mir(), address);
    const auto signature =
        session.abi().gti(body->returnType, {}, !body->failureRecords.empty());
    output = llvm::Function::Create(
        signature.type, llvm::GlobalValue::InternalLinkage,
        "__gti_native_v1_initialize", session.module());
    session.requireDefinition(*output);
  } else
    throw BackendUnsupportedError("LLVM body kind is not implemented");
  functions.emplace(key, output);
  return output;
}
void Bodies::body(MirBodyAddress address) {
  const auto &input = *findMirBody(session.program().mir(), address);
  FunctionAbi signature;
  std::span<const HirBindingId> parameters;
  if (address.kind == MirBodyKind::Function) {
    const auto &declaration =
        *session.program().mir().findFunctionInstance(address.owner);
    if (declaration.definitionKind != MirDefinitionKind::Source)
      return;
    signature = session.abi().gti(
        declaration.returnType, declaration.parameterTypes,
        declaration.mayRaiseDefinedFailure, false, declaration.tracksCaller);
    parameters = declaration.parameterBindings;
  } else
    signature = session.abi().gti(input.returnType, {},
                                  address.kind != MirBodyKind::HostedStartup &&
                                      !input.failureRecords.empty());
  Body emission(session, failures, *function(address), input, signature,
                parameters, globals,
                [&](MirBodyAddress target) { return function(target); });
}
void Bodies::lower(const LlvmAdmissionPlan &admission) {
  llvm::IRBuilder<> constants(session.context());
  Scalars scalars(session, constants, [](llvm::Value *, DefinedFailureOutcome) {
    throw std::logic_error("LLVM static datum cannot perform runtime checking");
  });
  const auto *module =
      findMirBody(session.program().mir(), {MirBodyKind::Module, 0});
  for (const auto &step :
       session.program().mir().programInitializationPlan().steps) {
    const auto &type = module->findPlace(step.storagePlace)->type;
    if (!isPassiveScalarValueType(type) && type.kind != SemanticType::Enum)
      continue;
    auto *storage = session.layouts().get(type).storage;
    llvm::Constant *initial = llvm::Constant::getNullValue(storage);
    if (step.dataConstant) {
      const auto &value = *step.dataConstant;
      if (const auto *integer = std::get_if<ConstantInteger>(&value))
        initial = llvm::ConstantInt::get(
            storage, integer->negative ? uint64_t(0) - integer->magnitude
                                       : integer->magnitude);
      else if (const auto *enumerator = std::get_if<ConstantEnum>(&value))
        initial = llvm::ConstantInt::get(
            storage, enumerator->value.negative
                         ? uint64_t(0) - enumerator->value.magnitude
                         : enumerator->value.magnitude);
      else if (const auto *floating = std::get_if<BinaryFloat>(&value))
        initial = llvm::cast<llvm::Constant>(scalars.literal(*floating, type));
      else if (const auto *character = std::get_if<CharacterLiteral>(&value))
        initial = llvm::ConstantInt::get(storage, character->value);
      else if (const auto *boolean = std::get_if<bool>(&value))
        initial = llvm::ConstantInt::get(storage, *boolean);
      else
        throw BackendUnsupportedError(
            "LLVM static datum requires a scalar constant");
    }
    globals.emplace(step.symbol,
                    new llvm::GlobalVariable(
                        session.module(), storage, false,
                        llvm::GlobalValue::InternalLinkage, initial,
                        "__gti_native_v1_g" + std::to_string(step.symbol)));
  }
  for (auto address : admission.requiredBodies())
    static_cast<void>(function(address));
  for (auto address : admission.requiredBodies())
    body(address);
}
} // namespace

void lowerScalarProgram(Session &session, const LlvmAdmissionPlan &admission) {
  Bodies(session).lower(admission);
}

} // namespace lang::native_llvm
