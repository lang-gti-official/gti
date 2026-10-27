#include "callables.h"
#include "parameter_packs.h"

#include "../mir_verification_queries.h"

#include <llvm/IR/IRBuilder.h>

#include <stdexcept>

namespace lang::native_llvm {

Callables::Callables(Session &session, FindBody findBody, FindAbi findAbi)
    : session(session), findBody(std::move(findBody)),
      findAbi(std::move(findAbi)) {}

const MirLambdaInstance &Callables::lambda(HirLambdaId id) const {
  const auto *result = session.program().mir().findLambda(id);
  if (!result)
    throw std::logic_error("LLVM callable lost its exact lambda identity");
  return *result;
}

CallableSignature Callables::pointerSignature(const SemanticType &type) const {
  const auto *signature =
      mir_detail::functionPointerSignature(session.program().mir(), type);
  if (!signature || signature->arguments.empty())
    throw std::logic_error(
        "LLVM function pointer lost its verified invocation signature");
  CallableSignature result{.result = signature->arguments.front()};
  result.parameters =
      flattenConcreteParameters(std::span(signature->arguments).subspan(1));
  return result;
}

FunctionAbi Callables::pointerAbi(const SemanticType &type) const {
  const auto signature = pointerSignature(type);
  // The private invocation-pointer capability has one stable checked ABI.
  // Failure-free targets are adapted into this form below.
  return session.abi().gti(signature.result, signature.parameters, true);
}

llvm::Function *Callables::pointerAdapter(const MirInstruction &formation) {
  if (!mir_detail::functionPointerOperation(session.program().mir(), formation))
    throw std::logic_error(
        "LLVM function-pointer formation lost its verified target");
  const MirBodyAddress target =
      formation.functionPointerFunctionTarget
          ? MirBodyAddress{MirBodyKind::Function,
                           *formation.functionPointerFunctionTarget}
          : MirBodyAddress{MirBodyKind::Lambda,
                           *formation.functionPointerLambdaTarget};
  const auto key = std::pair(target.kind, target.owner);
  if (const auto found = pointerAdapters.find(key);
      found != pointerAdapters.end())
    return found->second;

  const auto outward = pointerAbi(formation.info.type);
  const auto inward = findAbi(target);
  auto *adapter = llvm::Function::Create(
      outward.type, llvm::GlobalValue::InternalLinkage,
      "__gti_native_v" + std::to_string(executionAbiVersion) +
          "_function_pointer_" +
          std::to_string(static_cast<unsigned>(target.kind)) + "_" +
          std::to_string(target.owner),
      session.module());
  outward.applyTo(*adapter);
  pointerAdapters.emplace(key, adapter);

  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", adapter));
  const auto argument = [&](AbiChannel channel,
                            std::size_t source = 0) -> llvm::Value * {
    for (std::size_t index = 0; index < outward.parameters.size(); ++index)
      if (outward.parameters[index].channel == channel &&
          (channel != AbiChannel::Argument ||
           outward.parameters[index].sourceIndex == source))
        return adapter->getArg(index);
    return nullptr;
  };

  std::vector<llvm::Value *> inputs;
  inputs.reserve(inward.parameters.size());
  for (const auto &parameter : inward.parameters) {
    llvm::Value *value = nullptr;
    switch (parameter.channel) {
    case AbiChannel::ResultDestination:
      value = argument(AbiChannel::ResultDestination);
      break;
    case AbiChannel::FailureDestination:
      value = argument(AbiChannel::FailureDestination);
      break;
    case AbiChannel::Argument:
      value = argument(AbiChannel::Argument, parameter.sourceIndex);
      break;
    case AbiChannel::Receiver:
    case AbiChannel::CallerSite:
    case AbiChannel::CallerArtifact:
      throw std::logic_error(
          "verified function-pointer target acquired an incompatible ABI "
          "channel");
    }
    if (!value)
      throw std::logic_error(
          "LLVM function-pointer adapter lost a planned ABI channel");
    inputs.push_back(value);
  }

  auto *call = ir.CreateCall(findBody(target), inputs);
  inward.applyTo(*call);
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
  if (inward.type->getReturnType() != outward.type->getReturnType())
    throw std::logic_error("C04 invocation adapter return plans disagree");
  if (call->getType()->isVoidTy())
    ir.CreateRetVoid();
  else
    ir.CreateRet(call);
  return adapter;
#endif
  if (inward.statusResult) {
    ir.CreateRet(call);
    return adapter;
  }

  auto *destination = argument(AbiChannel::ResultDestination);
  if (!call->getType()->isVoidTy()) {
    if (!destination)
      throw std::logic_error(
          "LLVM function-pointer result lost its checked destination");
    llvm::Value *value = call;
    if (pointerSignature(formation.info.type).result == SemanticType::Bool)
      value = ir.CreateZExt(value, llvm::Type::getInt8Ty(session.context()));
    ir.CreateStore(value, destination);
  }
  ir.CreateRet(ir.getTrue());
  return adapter;
}

} // namespace lang::native_llvm
