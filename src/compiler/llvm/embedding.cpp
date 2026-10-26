#include "embedding.h"

#include "abi.h"
#include "failure.h"
#include "gti/native_header.h"
#include "native_c_abi.h"
#include "reachability.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/SHA256.h>

namespace lang::native_llvm {

bool infallibleEmbeddingContract(const LoweredProgram &program) {
  const auto &target = program.target();
  if (!((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")))
    return false;
  if (!program.embeddingPlan() || program.embeddingPlan()->exports.empty() ||
      program.embeddingPlan()->abiVersion != 1 ||
      !program.mir().nativeCallbackAdapters().empty() ||
      !program.mir().threadLocalStorage().empty())
    return false;
  for (const auto &step : program.mir().programInitializationPlan().steps)
    if (step.requiresActiveCleanup)
      return false;
  const auto closure = reachableProgram(program);
  if (closure.conservative)
    return false;
  for (auto address : closure.bodies) {
    const auto *body = findMirBody(program.mir(), address);
    if (!body || !body->failureRecords.empty())
      return false;
    if (address.kind == MirBodyKind::Function) {
      const auto *function = program.mir().findFunctionInstance(address.owner);
      if (!function || function->definitionKind != MirDefinitionKind::Source ||
          function->mayRaiseDefinedFailure || function->tracksCaller ||
          function->linkage == LanguageLinkage::C)
        return false;
    }
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        // No native unwind route is admitted before a reviewed outbound
        // firewall can contain it without unwinding through GTI frames.
        if (stage.nativeCalleeType || stage.nativeCallbackAdapter ||
            stage.threadTaskAdapter || !stage.scopedThreadTasks.empty())
          return false;
      }
  }
  return true;
}

namespace {
llvm::FunctionCallee service(Session &session, llvm::IRBuilder<> &,
                             const char *name, llvm::Type *result,
                             std::initializer_list<llvm::Type *> parameters) {
  auto callee = session.module().getOrInsertFunction(
      name, llvm::FunctionType::get(result, parameters, false));
  auto *function = llvm::cast<llvm::Function>(callee.getCallee());
  function->addFnAttr(llvm::Attribute::NoUnwind);
  function->setVisibility(llvm::GlobalValue::HiddenVisibility);
  return callee;
}

llvm::Function *exported(Session &session, const std::string &name,
                         llvm::FunctionType *type) {
  if (session.module().getNamedValue(name))
    throw std::logic_error("embedding public symbol collision: " + name);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, session.module());
  function->addFnAttr(llvm::Attribute::NoUnwind);
  session.requireDefinition(*function);
  return function;
}

void continueOnSuccess(Session &session, llvm::IRBuilder<> &ir,
                       llvm::Value *status) {
  auto *function = ir.GetInsertBlock()->getParent();
  auto *success =
      llvm::BasicBlock::Create(session.context(), "admitted", function);
  auto *reject =
      llvm::BasicBlock::Create(session.context(), "rejected", function);
  ir.CreateCondBr(ir.CreateICmpEQ(status, ir.getInt32(0)), success, reject);
  ir.SetInsertPoint(reject);
  ir.CreateRet(status);
  ir.SetInsertPoint(success);
}

llvm::Constant *constant(Session &session, llvm::Constant *value,
                         const char *name) {
  return new llvm::GlobalVariable(session.module(), value->getType(), true,
                                  llvm::GlobalValue::PrivateLinkage, value,
                                  name);
}
} // namespace

void emitEmbedding(Session &session, Failures &failures,
                   llvm::Function &initialize) {
  const auto &program = session.program();
  if (!infallibleEmbeddingContract(program))
    throw BackendUnsupportedError(
        "LLVM embedding requires an infallible, "
        "native-unwind-free closure; fallible containment awaits C04");
  auto &context = session.context();
  llvm::IRBuilder<> ir(context);
  auto *ptr = session.layouts().pointer();
  auto *i32 = ir.getInt32Ty();
  auto *voidType = ir.getVoidTy();
  auto *create =
      exported(session, "gti_context_create_v1",
               llvm::FunctionType::get(i32, {i32, ptr, ptr, ptr, ptr}, false));
  ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", create));
  std::vector<llvm::Value *> arguments;
  for (auto &arg : create->args())
    arguments.push_back(&arg);
  auto *status =
      ir.CreateCall(service(session, ir, "gti_rt_embed_create_begin_v1", i32,
                            {i32, ptr, ptr, ptr, ptr}),
                    arguments);
  continueOnSuccess(session, ir, status);
  if (!initialize.arg_empty() || !initialize.getReturnType()->isVoidTy())
    throw std::logic_error("infallible module initializer has hidden channels");
  ir.CreateCall(&initialize);
  ir.CreateCall(
      service(session, ir, "gti_rt_embed_create_end_v1", voidType, {ptr}),
      {create->getArg(3)});
  ir.CreateRet(ir.getInt32(0));

  auto *destroy = exported(session, "gti_context_destroy_v1",
                           llvm::FunctionType::get(i32, {ptr, ptr}, false));
  ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", destroy));
  ir.CreateRet(ir.CreateCall(
      service(session, ir, "gti_rt_embed_destroy_v1", i32, {ptr, ptr}),
      {destroy->getArg(0), destroy->getArg(1)}));

  std::vector<llvm::Constant *> descriptors;
  auto *identityType = llvm::ArrayType::get(ir.getInt8Ty(), 32);
  auto *exportType =
      llvm::StructType::get(session.abi().textView(), identityType);
  std::string moduleIdentity = "gti-embedding-module-v1;";
  const auto &artifactId =
      program.mir().failureMetadata().artifactIdentity().bytes;
  moduleIdentity.append(reinterpret_cast<const char *>(artifactId.data()),
                        artifactId.size());
  for (const auto &selected : program.embeddingPlan()->exports) {
    const auto signature = nativeEmbeddingSignature(program, selected);
    const auto digest = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(signature.data()), signature.size()));
    moduleIdentity +=
        std::to_string(selected.symbol.size()) + ':' + selected.symbol;
    moduleIdentity.append(reinterpret_cast<const char *>(digest.data()),
                          digest.size());
    auto *name = constant(
        session,
        llvm::ConstantDataArray::getString(context, selected.symbol, false),
        "embed.symbol");
    auto *view = llvm::ConstantStruct::get(
        session.abi().textView(), {name, ir.getInt64(selected.symbol.size())});
    descriptors.push_back(llvm::ConstantStruct::get(
        exportType, {view, llvm::ConstantDataArray::get(context, digest)}));

    // Synthetic context/output pointers are representation lanes, not source
    // type admission. The source parameters come only from the sealed plan.
    const auto pointerType = SemanticType::rawPointerTo(SemanticType::Void);
    std::vector<SemanticType> parameters{pointerType};
    parameters.insert(parameters.end(), selected.parameterTypes.begin(),
                      selected.parameterTypes.end());
    if (selected.returnType != SemanticType::Void)
      parameters.push_back(pointerType);
    parameters.push_back(pointerType);
    const auto native =
        planNativeCFunction(session, SemanticType::UInt32, parameters);
    auto *wrapper = exported(session, selected.symbol, native.signature.type);
    native.signature.applyTo(*wrapper);
    ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", wrapper));
    const auto inputs = decodeNativeCArguments(session, ir, native, *wrapper);
    if (selected.returnType != SemanticType::Void) {
      auto *valid = ir.CreateICmpNE(inputs[inputs.size() - 2],
                                    llvm::ConstantPointerNull::get(ptr));
      continueOnSuccess(session, ir,
                        ir.CreateSelect(valid, ir.getInt32(0), ir.getInt32(2)));
    }
    continueOnSuccess(
        session, ir,
        ir.CreateCall(
            service(session, ir, "gti_rt_embed_enter_v1", i32, {ptr, ptr}),
            {inputs.front(), inputs.back()}));
    const auto gti =
        session.abi().gti(selected.returnType, selected.parameterTypes, false);
    llvm::Value *output = nullptr;
    if (gti.resultDestination) {
      const auto layout = session.layouts().get(selected.returnType);
      auto *slot = ir.CreateAlloca(layout.storage);
      slot->setAlignment(llvm::Align(layout.alignment));
      output = slot;
    }
    std::vector<llvm::Value *> callArguments;
    for (const auto &parameter : gti.parameters) {
      if (parameter.channel == AbiChannel::ResultDestination)
        callArguments.push_back(output);
      else if (parameter.channel == AbiChannel::Argument)
        callArguments.push_back(inputs.at(parameter.sourceIndex + 1));
      else
        throw std::logic_error(
            "infallible embedding acquired a hidden channel");
    }
    auto *call = ir.CreateCall(session.abi().declareFunction(selected.function),
                               callArguments);
    gti.applyTo(*call);
    if (selected.returnType != SemanticType::Void) {
      const auto layout = session.layouts().get(selected.returnType);
      auto *destination = inputs[inputs.size() - 2];
      if (output)
        ir.CreateMemCpy(destination, llvm::Align(layout.alignment), output,
                        llvm::Align(layout.alignment), layout.size);
      else
        ir.CreateStore(call, destination);
    }
    ir.CreateCall(
        service(session, ir, "gti_rt_embed_leave_v1", voidType, {ptr}),
        {inputs.front()});
    ir.CreateRet(ir.getInt32(0));
  }
  const auto digest = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(moduleIdentity.data()),
      moduleIdentity.size()));
  auto *exports = constant(
      session,
      llvm::ConstantArray::get(
          llvm::ArrayType::get(exportType, descriptors.size()), descriptors),
      "embed.exports");
  auto *descriptorType =
      llvm::StructType::get(i32, i32, identityType, ptr, ptr, i32, i32);
  auto *descriptor = constant(
      session,
      llvm::ConstantStruct::get(
          descriptorType,
          {ir.getInt32(1),
           ir.getInt32(session.dataLayout().getTypeAllocSize(descriptorType)),
           llvm::ConstantDataArray::get(context, digest), failures.artifact(),
           exports, ir.getInt32(descriptors.size()), ir.getInt32(0)}),
      "embed.descriptor");
  auto *get = exported(session, "gti_get_module_descriptor_v1",
                       llvm::FunctionType::get(ptr, {}, false));
  ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", get));
  ir.CreateRet(descriptor);
  // GTI implementation symbols never form part of the host ABI.
  for (auto &function : session.module())
    if (function.getName().starts_with("__gti_native_") &&
        !function.isDeclaration())
      function.setLinkage(llvm::GlobalValue::InternalLinkage);
}
} // namespace lang::native_llvm
