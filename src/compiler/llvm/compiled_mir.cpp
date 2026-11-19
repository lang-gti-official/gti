#include "compiled_mir.h"
#include "gti/compiled_mir.h"
#include "scalar.h"

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {
namespace {
Literal literal(const CompiledMirStep &step) {
  if (step.type == SemanticType::Float || step.type == SemanticType::Double)
    return BinaryFloat{step.literalBits, step.type == SemanticType::Float
                                             ? BinaryFloatFormat::Binary32
                                             : BinaryFloatFormat::Binary64};
  if (step.type == SemanticType::Bool)
    return step.literalBits != 0;
  return step.literalBits;
}

void emit(Session &session, const CompiledScalarBinding &binding,
          llvm::Function &native, std::span<llvm::CallBase *const> uses) {
  const auto &fn =
      *session.program().mir().findFunctionInstance(binding.function);
  const auto abi = session.abi().gti(fn.returnType, fn.parameterTypes, true);
  auto &context = session.context();
  auto *copy =
      llvm::Function::Create(abi.type, llvm::GlobalValue::InternalLinkage,
                             binding.symbol + ".mir", session.module());
  session.requireDefinition(*copy);
  abi.applyTo(*copy);
  applyCodegenAttributes(*copy, binding.codegenAttributes);
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", copy));
  std::vector<llvm::Value *> values;
  for (unsigned i = 0; i < abi.parameters.size(); ++i) {
    if (abi.parameters[i].channel == AbiChannel::Argument)
      values.push_back(copy->getArg(i));
  }
  std::vector<SemanticType> types = fn.parameterTypes;
  llvm::BasicBlock *fallback = nullptr;
  Scalars scalars(
      session, ir, [&](llvm::Value *invalid, DefinedFailureOutcome) {
        // Replaying a pure body preserves the producer's failure record, source
        // location and failure ABI. The successful path stays entirely in MIR.
        if (!fallback)
          fallback = llvm::BasicBlock::Create(context, "native.failure", copy);
        auto *success = llvm::BasicBlock::Create(context, "ok", copy);
        ir.CreateCondBr(invalid, fallback, success);
        ir.SetInsertPoint(success);
      });
  for (const auto &step : binding.optimizationBody->steps) {
    MirInstruction instruction;
    instruction.info.type = step.type;
    instruction.operation = step.operation;
    instruction.intrinsic = step.intrinsic;
    if (step.operation == MirOperation::Literal)
      instruction.literal = literal(step);
    std::vector<llvm::Value *> inputs;
    for (auto slot : step.inputs) {
      inputs.push_back(values.at(slot));
      MirOperand operand;
      operand.type = types.at(slot);
      instruction.operands.push_back(std::move(operand));
    }
    values.push_back(scalars.compute(instruction, inputs));
    types.emplace_back(step.type);
  }
  ir.CreateStore(values.at(binding.optimizationBody->returned),
                 copy->getArg(*abi.resultDestination));
  ir.CreateRet(ir.getTrue());
  if (fallback) {
    ir.SetInsertPoint(fallback);
    std::vector<llvm::Value *> args;
    for (auto &arg : copy->args())
      args.push_back(&arg);
    auto *call = ir.CreateCall(&native, args);
    abi.applyTo(*call);
    ir.CreateRet(call);
  }
  for (auto *call : uses)
    call->setCalledFunction(copy);
}
} // namespace

void emitCompiledMir(Session &session) {
  const auto options = session.options();
  if (options.optimization == LlvmCodegenOptimization::None ||
      options.debugInfo ||
      options.instrumentation != LlvmInstrumentationMode::None)
    return;
  for (const auto &binding : session.program().compiledScalarBindings()) {
    if (!binding.imported || !binding.optimizationBody)
      continue;
    auto *native = session.module().getFunction(binding.symbol);
    if (!native)
      continue;
    std::vector<llvm::CallBase *> calls;
    for (auto *user : native->users())
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(user);
          call && call->getCalledFunction() == native)
        calls.push_back(call);
    if (!calls.empty())
      emit(session, binding, *native, calls);
  }
}
} // namespace lang::native_llvm
