#include "body.h"
#include "atomics.h"
#include "failure.h"
#include "lifetimes.h"
#include "placement.h"
#include "places.h"
#include "runtime_abi.h"
#include "scalar.h"
#include "text.h"
#include "thread_locals.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace lang::native_llvm {
namespace {

class Bodies final {
public:
  explicit Bodies(Session &session)
      : session(session), failures(session),
        lifetimes(session, failures,
                  [&](MirBodyAddress address) { return function(address); }),
        threadLocals(
            session, failures, lifetimes,
            [&](MirBodyAddress address) { return function(address); },
            [&](MirBodyAddress address) { return signature(address); }) {}
  void lower(const LlvmAdmissionPlan &admission);

private:
  Session &session;
  Failures failures;
  Lifetimes lifetimes;
  ThreadLocals threadLocals;
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  std::map<std::pair<MirBodyKind, std::size_t>, llvm::Function *> functions;
  [[nodiscard]] llvm::Function *function(MirBodyAddress address);
  void body(MirBodyAddress address);
  [[nodiscard]] FunctionAbi signature(MirBodyAddress address);
  [[nodiscard]] const MirClassInstance *owner(MirBodyAddress address);
};

class Body final {
public:
  Body(Session &session, Failures &failures, Lifetimes &lifetimes,
       ThreadLocals &threadLocals, llvm::Function &function,
       MirBodyAddress address, const MirBody &body, FunctionAbi abi,
       std::span<const HirBindingId> parameters,
       const std::map<SymbolId, llvm::GlobalVariable *> &globals,
       std::function<llvm::Function *(MirBodyAddress)> findFunction,
       std::function<FunctionAbi(MirBodyAddress)> bodyAbi,
       const MirClassInstance *owner)
      : session(session), failures(failures), function(function),
        address(address), input(body), abi(std::move(abi)), globals(globals),
        ir(session.context()), lifetimes(lifetimes), threadLocals(threadLocals),
        bodyAbi(std::move(bodyAbi)), owner(owner),
        scalars(session, ir,
                [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
                  guard(condition, outcome);
                }),
        atomics(session, ir), findFunction(std::move(findFunction)),
        values(body.values.size() + 1),
        memory(session, ir, body, values,
               [&](llvm::Value *condition, DefinedFailureOutcome outcome) {
                 guard(condition, outcome);
               }),
        channels(body.failureRecords.size() + 1),
        blocks(body.blocks.size() + 1), live(body.dropObligations.size() + 1),
        placement(body) {
    emit(parameters);
  }

private:
  Session &session;
  Failures &failures;
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
  Scalars scalars;
  Atomics atomics;
  std::function<llvm::Function *(MirBodyAddress)> findFunction;
  std::vector<llvm::Value *> values;
  Places memory;
  std::vector<llvm::Value *> channels;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<llvm::Value *> live;
  llvm::Value *thisAddress = nullptr;
  Placement placement;
  std::map<MirValueId, llvm::Value *> temporaries;
  llvm::Value *returnDestination = nullptr, *failureDestination = nullptr;
  llvm::Value *callerSite = nullptr, *callerArtifact = nullptr;
  const MirBlock *current = nullptr;
  const MirInstruction *stage = nullptr;

  void publishStoredField(const MirLoan &loan, llvm::Value *address) {
    if (loan.kind != MirLoanKind::Stored || !loan.storedField)
      return;
    if (input.kind != MirBodyKind::Constructor || !owner)
      throw std::logic_error(
          "LLVM stored field publication lost its constructor");
    ir.CreateStore(
        address,
        ir.CreateStructGEP(
            session.layouts().get(owner->type).storage, thisAddress,
            session.layouts().fieldIndex(owner->type, loan.storedField)));
  }

  [[nodiscard]] llvm::Value *destination(MirValueId value) {
    if (const auto *target = placement.target(value)) {
      if (target->place)
        return memory.address(target->place);
      if (target->returned) {
        if (!returnDestination)
          throw std::logic_error("LLVM object return lost its ABI destination");
        return returnDestination;
      }
      const auto &type = input.findValue(target->aggregate)->info.type;
      return ir.CreateGEP(session.layouts().get(type).storage,
                          destination(target->aggregate),
                          {ir.getInt64(0), ir.getInt64(target->element)});
    }
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
  [[nodiscard]] llvm::Value *operand(const MirOperand &operand) {
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
      if (addressValue(operand.type)) {
        auto *target = memory.temporary(operand.type);
        store(value, target, operand.type);
        if (operand.kind == MirOperandKind::Move)
          lifetimes.moved(ir, source, operand.type);
        return target;
      }
      if (operand.kind == MirOperandKind::Move &&
          operand.type.kind == SemanticType::Array)
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
  [[nodiscard]] llvm::Value *call(const MirInstruction &instruction,
                                  llvm::Value *constructed = nullptr) {
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
      if (Atomics::handles(instruction.intrinsic)) {
        std::vector<llvm::Value *> inputs;
        for (const auto &source : instruction.operands)
          inputs.push_back(operand(source));
        return atomics.emit(input, instruction, inputs);
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
    const auto constructor = instruction.constructorTarget;
    if (!target && !instruction.bodyTarget && !constructor)
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
                                  target->mayRaiseDefinedFailure,
                                  target->owner && !target->staticMember,
                                  target->tracksCaller);
      callee = session.abi().declareFunction(target->id);
    } else {
      const auto address =
          constructor ? MirBodyAddress{MirBodyKind::Constructor, *constructor}
                      : *instruction.bodyTarget;
      signature = bodyAbi(address);
      callee = findFunction(address);
    }
    const auto &resultType =
        target ? target->returnType : instruction.info.type;
    llvm::Value *resultSlot = nullptr;
    if (signature.resultDestination) {
      llvm::IRBuilder<> entry(&function.getEntryBlock(),
                              function.getEntryBlock().begin());
      resultSlot =
          instruction.result && addressValue(resultType)
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
        auto *value = operand(instruction.operands.at(parameter.sourceIndex));
        if (parameter.indirect &&
            !addressValue(
                instruction.operands.at(parameter.sourceIndex).type)) {
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
        arguments.push_back(constructed ? constructed
                            : instruction.receiver
                                ? memory.selected(*instruction.receiver)
                                : thisAddress);
        if (!arguments.back())
          throw std::logic_error("LLVM call lost its sealed receiver");
        break;
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
    auto *status = ir.CreateCall(
        lifetimes.cleanup(*recipe),
        {address, channel, ir.getInt1(current->activeFailure != 0)});
    if (checked) {
      auto *bad =
          llvm::BasicBlock::Create(session.context(), "drop.failed", &function);
      ir.CreateCondBr(status, done, bad);
      ir.SetInsertPoint(bad);
      jump(term.elseTarget, term.elseArguments);
    } else {
      ir.CreateBr(done);
    }
    ir.SetInsertPoint(done);
  }
  void instruction(const MirInstruction &instruction) {
    stage = &instruction;
    if (instruction.tombstone)
      return;
    llvm::Value *result = nullptr;
    switch (instruction.kind) {
    case MirInstructionKind::Compute: {
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
        if (addressValue(type)) {
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
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Move:
      result = operand(instruction.operands.at(0));
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
        if (addressValue(place.type))
          result = destination;
      }
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
      result = operand(instruction.operands.at(0));
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
    case MirInstructionKind::Construct: {
      auto *target = destination(*instruction.result);
      lifetimes.active(ir, target, instruction.info.type, false);
      if (instruction.constructorTarget) {
        result = call(instruction, target);
      } else if (instruction.constructorKind == ConstructorKind::Ordinary &&
                 instruction.operands.empty() &&
                 instruction.info.type.kind == SemanticType::Class) {
        const auto &object = session.layouts().instance(instruction.info.type);
        if (object.defaultConstructor != SpecialMemberStatus::Generated ||
            object.generatedDefaultRequiresSchedule ||
            !object.declaredFields.empty() || !object.bases.empty())
          throw BackendUnsupportedError(
              "LLVM targetless default construction requires an exact empty "
              "generated object");
        ir.CreateStore(llvm::Constant::getNullValue(
                           session.layouts().get(object.type).storage),
                       target);
        lifetimes.active(ir, target, object.type, true);
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
    case MirInstructionKind::Borrow:
      result = memory.selected(instruction.operands.at(0));
      memory.publish(*instruction.loan, result);
      publishStoredField(*input.findLoan(*instruction.loan), result);
      break;
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
          [&] { jump(term.elseTarget, term.elseArguments); });
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
    memory.allocate(globals);
    llvm::Value *atomicArgument = nullptr;
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
        memory.parameter(parameters[parameter.sourceIndex], argument,
                         parameter.indirect);
        if (parameter.sourceIndex == 0 && !parameter.indirect)
          atomicArgument = argument;
        break;
      case AbiChannel::Receiver:
        if (!owner)
          throw std::logic_error("LLVM receiver has no sealed class instance");
        thisAddress = argument;
        memory.receiver(argument, owner->type);
        break;
      }
    }
    if (input.kind == MirBodyKind::Constructor && owner &&
        owner->compilerCapability ==
            CompilerCapabilityTypeKind::AtomicStorage) {
      const auto *constructor =
          session.program().mir().findConstructorInstance(address.owner);
      if (address.kind != MirBodyKind::Constructor || !constructor ||
          &constructor->body != &input || !thisAddress || !atomicArgument)
        throw BackendUnsupportedError(
            "LLVM atomic initialization lost its exact constructor ABI lane");
      atomics.initialize(*constructor, thisAddress, atomicArgument);
    }
    for (const auto &record : input.failureRecords)
      channels.at(record.id) = ir.CreateAlloca(session.abi().failureChannel());
    // A constructor's entry Stored loans are the sealed field-publication
    // contract. Their referent address, not the Load of its scalar contents,
    // initializes the physical reference field.
    if (input.kind == MirBodyKind::Constructor && owner)
      for (const auto &loan : input.loans)
        if (loan.entry && loan.kind == MirLoanKind::Stored &&
            loan.storedField) {
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
  } else if (address.kind == MirBodyKind::Module ||
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
  std::span<const HirBindingId> parameters;
  if (address.kind == MirBodyKind::Function) {
    const auto &declaration =
        *session.program().mir().findFunctionInstance(address.owner);
    if (declaration.definitionKind != MirDefinitionKind::Source)
      return;
    parameters = declaration.parameterBindings;
  } else if (address.kind == MirBodyKind::Constructor)
    parameters = session.program()
                     .mir()
                     .findConstructorInstance(address.owner)
                     ->parameterBindings;
  Body emission(
      session, failures, lifetimes, threadLocals, *function(address), address,
      input, plan, parameters, globals,
      [&](MirBodyAddress target) { return function(target); },
      [&](MirBodyAddress target) { return signature(target); }, owner(address));
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
