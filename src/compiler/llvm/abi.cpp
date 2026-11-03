#include "abi.h"
#include "native_c_abi.h"
#include "parameter_packs.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <stdexcept>

namespace lang::native_llvm {

VirtualRootAbi
Abi::virtualRoot(HirFunctionInstanceId rootId,
                 std::span<const HirFunctionInstanceId> implementations) {
  if (!std::is_sorted(implementations.begin(), implementations.end()) ||
      std::adjacent_find(implementations.begin(), implementations.end()) !=
          implementations.end())
    throw std::invalid_argument("LLVM virtual ABI target set is not canonical");
  if (const auto found = virtualRoots_.find(rootId);
      found != virtualRoots_.end()) {
    if (!std::equal(implementations.begin(), implementations.end(),
                    found->second.implementations.begin(),
                    found->second.implementations.end()))
      throw std::invalid_argument("LLVM virtual ABI target set changed");
    return found->second;
  }
  const auto &program = session.program().mir();
  const auto *root = program.findFunctionInstance(rootId);
  if (!root || !root->owner || !root->virtualMethod || root->overrideMethod ||
      root->staticMember || root->linkage != LanguageLinkage::Gti)
    throw std::invalid_argument("LLVM virtual ABI lacks an exact root");
  std::vector<HirFunctionInstanceId> expected;
  for (const auto &owner : program.classInstances())
    for (const auto &selection : owner.virtualDispatch)
      if (selection.root == rootId) {
        const auto *target = program.findFunctionInstance(selection.target);
        if (!target)
          throw std::invalid_argument("LLVM virtual ABI lost a sealed target");
        if (!target->pureVirtual)
          expected.push_back(target->id);
      }
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  if (!std::equal(implementations.begin(), implementations.end(),
                  expected.begin(), expected.end()))
    throw std::invalid_argument("LLVM virtual ABI target set is incomplete");
  if (root->tracksCaller)
    throw BackendUnsupportedError(
        "LLVM virtual caller attribution has no admitted source contract");
  for (const auto id : expected) {
    const auto &target = *program.findFunctionInstance(id);
    if (target.returnType != root->returnType ||
        target.parameterTypes != root->parameterTypes ||
        target.receiverMutability != root->receiverMutability ||
        target.overloadedOperator != root->overloadedOperator ||
        target.returnBorrowOrigin != root->returnBorrowOrigin ||
        target.returnBorrowParameter != root->returnBorrowParameter ||
        target.returnBorrowAccess != root->returnBorrowAccess ||
        target.definitionKind != MirDefinitionKind::Source ||
        target.tracksCaller)
      throw BackendUnsupportedError(
          "LLVM virtual target contradicts its verified root contract");
  }
  // A body effect is distinct from the virtual invocation contract. Verified
  // MIR retains VirtualCall propagation even when the named root body cannot
  // fail. Keep this interface independent of reachable target subsets; exact
  // bodies/devirtualized calls retain their own plans. Physical transport is
  // still entirely Abi::gti's responsibility while C04 remains unresolved.
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    if (!body)
      throw std::logic_error("LLVM virtual ABI lost its sealed body");
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone ||
            instruction.dispatch != CallDispatch::Virtual ||
            !instruction.functionTarget)
          continue;
        const auto *selected =
            program.findFunctionInstance(*instruction.functionTarget);
        const auto *owner = selected && selected->owner
                                ? program.findClassInstance(*selected->owner)
                                : nullptr;
        if (!owner ||
            std::none_of(owner->virtualDispatch.begin(),
                         owner->virtualDispatch.end(),
                         [&](const auto &row) { return row.root == rootId; }))
          continue;
        if (instruction.definedFailure.propagation !=
                FailurePropagationKind::VirtualCall ||
            block.terminator.kind != MirTerminatorKind::Invoke ||
            block.terminator.invokeInstruction != instruction.id ||
            !body->findFailureRecord(block.terminator.failureRecord))
          throw BackendUnsupportedError(
              "LLVM virtual invocation lacks its verified failure boundary");
      }
  }
  VirtualRootAbi result{.root = rootId,
                        .implementations = std::move(expected),
                        .call = gti(root->returnType, root->parameterTypes,
                                    true, true, root->tracksCaller),
                        .mayFail = true,
                        .tracksCaller = root->tracksCaller};
  virtualRoots_.emplace(rootId, result);
  return result;
}

VirtualAdjustmentAbi
Abi::virtualAdjustment(const VirtualRootAbi &root,
                       HirFunctionInstanceId implementation) {
  const auto authoritative = virtualRoot(root.root, root.implementations);
  if (root != authoritative ||
      !std::binary_search(root.implementations.begin(),
                          root.implementations.end(), implementation))
    throw std::invalid_argument(
        "LLVM virtual adjustment has a mismatched plan");
  const auto *target =
      session.program().mir().findFunctionInstance(implementation);
  return {.entry = authoritative.call,
          .implementation =
              gti(target->returnType, target->parameterTypes,
                  target->mayRaiseDefinedFailure, true, target->tracksCaller)};
}

namespace {

template <typename Callable>
void applyExtensions(Callable &function, const FunctionAbi &abi) {
  const auto attribute = [](AbiExtension extension) {
    return extension == AbiExtension::Signed ? llvm::Attribute::SExt
                                             : llvm::Attribute::ZExt;
  };
  if (abi.resultExtension != AbiExtension::None)
    function.addRetAttr(attribute(abi.resultExtension));
  for (unsigned index = 0; index < abi.parameters.size(); ++index)
    if (abi.parameters[index].extension != AbiExtension::None)
      function.addParamAttr(index, attribute(abi.parameters[index].extension));
  for (const auto &memory : abi.nativeMemory) {
    if (memory.result)
      function.addParamAttr(memory.index,
                            llvm::Attribute::getWithStructRetType(
                                abi.type->getContext(), memory.storage));
    else if (memory.byValue)
      function.addParamAttr(memory.index,
                            llvm::Attribute::getWithByValType(
                                abi.type->getContext(), memory.storage));
    function.addParamAttr(memory.index, llvm::Attribute::getWithAlignment(
                                            abi.type->getContext(),
                                            llvm::Align(memory.alignment)));
  }
}

} // namespace

void FunctionAbi::applyTo(llvm::Function &function) const {
  if (function.getFunctionType() != type)
    throw std::logic_error("LLVM function disagrees with planned ABI");
  applyExtensions(function, *this);
}
void FunctionAbi::applyTo(llvm::CallBase &call) const {
  if (call.getFunctionType() != type)
    throw std::logic_error("LLVM call disagrees with planned ABI");
  applyExtensions(call, *this);
}

Abi::Abi(Session &session, Layouts &layouts)
    : session(session), layouts(layouts) {
  auto &c = session.context();
  auto *i8 = llvm::Type::getInt8Ty(c);
  auto *i16 = llvm::Type::getInt16Ty(c);
  auto *i32 = llvm::Type::getInt32Ty(c);
  auto *i64 = llvm::Type::getInt64Ty(c);
  auto *ptr = layouts.pointer();
  textView_ =
      llvm::cast<llvm::StructType>(layouts.get(SemanticType::StringView).value);
  failureRecord_ = llvm::StructType::create(
      c,
      {i32, i16, i16, i32, i32, llvm::ArrayType::get(i8, 32), i32, i32,
       llvm::ArrayType::get(i8, 512)},
      "gti.c.failure_record.v2");
  failureArtifact_ = llvm::StructType::create(
      c, {i32, i32, llvm::ArrayType::get(i8, 32), ptr, i32, i32, ptr, i64},
      "gti.c.failure_artifact.v1");
  failureChannel_ = llvm::StructType::create(c, {failureRecord_, ptr},
                                             "gti.failure_channel.v1");
  failureOutcome_ =
      llvm::StructType::create(c, {i16, i16, i32}, "gti.c.failure_outcome.v1");
  failureSite_ = llvm::StructType::create(
      c, {textView_, i64, i64, i64, ptr, i32, i32}, "gti.c.failure_site.v1");
  failureEmergency_ =
      llvm::StructType::create(c, {i32, i32, failureRecord_, failureRecord_},
                               "gti.c.failure_emergency.v2");
}

FunctionAbi Abi::gti(const SemanticType &result,
                     std::span<const SemanticType> parameters, bool mayFail,
                     bool receiver, bool tracksCaller) {
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
#if !defined(GTI_LLVM_C04_MIR_EVALUATION)
#error "C04 native transport is restricted to the isolated evaluation target"
#endif
  // The test entry validates its bounded lifecycle/call closure before
  // emission. This changes only physical transport; MIR retains its failure
  // edges.
  mayFail = false;
#endif
  const auto concreteParameters = flattenConcreteParameters(parameters);
  const auto output = layouts.get(result);
  FunctionAbi plan;
  std::vector<llvm::Type *> types;
  const auto append = [&](llvm::Type *type, AbiChannel channel,
                          std::size_t index = 0, bool indirect = false) {
    types.push_back(type);
    plan.parameters.push_back({channel, index, indirect});
  };
  const bool returnsValue = !output.value->isVoidTy();
  if (returnsValue && (mayFail || output.addressOnly)) {
    plan.resultDestination = static_cast<unsigned>(types.size());
    append(layouts.pointer(), AbiChannel::ResultDestination, 0, true);
  }
  if (mayFail) {
    plan.failureDestination = static_cast<unsigned>(types.size());
    append(layouts.pointer(), AbiChannel::FailureDestination, 0, true);
  }
  if (receiver)
    append(layouts.pointer(), AbiChannel::Receiver, 0, true);
  if (tracksCaller) {
    append(llvm::Type::getInt32Ty(session.context()), AbiChannel::CallerSite);
    append(layouts.pointer(), AbiChannel::CallerArtifact);
  }
  for (std::size_t index = 0; index < concreteParameters.size(); ++index) {
    const auto input = layouts.get(concreteParameters[index]);
    if (input.value->isVoidTy())
      throw std::logic_error("void GTI ABI parameter");
    append(input.addressOnly ? layouts.pointer() : input.value,
           AbiChannel::Argument, index, input.addressOnly);
  }
  plan.statusResult = mayFail;
  auto *returnType = mayFail ? llvm::Type::getInt1Ty(session.context())
                     : output.addressOnly
                         ? llvm::Type::getVoidTy(session.context())
                         : output.value;
  plan.type = llvm::FunctionType::get(returnType, types, false);
  return plan;
}

FunctionAbi Abi::nativeC(const SemanticType &result,
                         std::span<const SemanticType> parameters) {
  return planNativeCFunction(session, result, parameters).signature;
}

llvm::Function *Abi::declareFunction(HirFunctionInstanceId id) {
  const auto *function = session.program().mir().findFunctionInstance(id);
  if (!function)
    throw std::logic_error(
        "LLVM declaration requires an exact lowered function identity");
  if (function->definitionKind == MirDefinitionKind::RuntimeBinding)
    throw BackendUnsupportedError(
        "LLVM runtime binding requires an explicit native service adapter");
  const auto *declaration =
      session.program().findFunctionDeclaration(function->declaration);
  if (!declaration)
    throw std::logic_error("LLVM declaration has no sealed source declaration");
  const bool native = function->linkage == LanguageLinkage::C;
  const auto plan =
      native ? nativeC(function->returnType, function->parameterTypes)
             : gti(function->returnType, function->parameterTypes,
                   function->mayRaiseDefinedFailure,
                   function->owner.has_value() && !function->staticMember,
                   function->tracksCaller);
  constexpr const char *privatePrefix =
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      "__gti_c04_unwind_v";
#elif defined(GTI_LLVM_C04_MIR_EVALUATION)
      "__gti_c04_status_v";
#else
      "__gti_native_v";
#endif
  const auto *compiled = session.program().compiledScalarBinding(id);
  const auto name = compiled && compiled->imported ? compiled->symbol
                    : native
                        ? function->externalSymbol
                        : privatePrefix + std::to_string(executionAbiVersion) +
                              "_f" + std::to_string(id);
  if (name.empty())
    throw std::logic_error(
        "LLVM native declaration has no exact external symbol");
  if (auto *existing = session.module().getNamedValue(name)) {
    auto *previous = llvm::dyn_cast<llvm::Function>(existing);
    if (!previous || previous->getFunctionType() != plan.type)
      throw std::logic_error("conflicting LLVM symbol declaration: " + name);
    if (function->definitionKind == MirDefinitionKind::Source)
      session.requireSourceDefinition(id, *previous);
    if (native)
      verifyNativeCImport(*previous, plan);
    return previous;
  }
  auto *output = llvm::Function::Create(
      plan.type, llvm::GlobalValue::ExternalLinkage, name, session.module());
  plan.applyTo(*output);
  if (function->definitionKind == MirDefinitionKind::Source)
    session.requireSourceDefinition(id, *output);
  // Provenance survives independently of debug-info generation. This does not
  // manufacture DWARF or change function semantics.
  if (const auto *source = session.program().findFunctionInstance(id)) {
    const auto &span = source->source;
    output->setMetadata(
        "gti.source",
        llvm::MDNode::get(
            session.context(),
            {llvm::MDString::get(session.context(), span.source),
             llvm::MDString::get(session.context(), std::to_string(span.start)),
             llvm::MDString::get(session.context(),
                                 std::to_string(span.end))}));
  }
  return output;
}

void Abi::declareFunctions(std::span<const HirFunctionInstanceId> ids) {
  for (const auto id : ids)
    static_cast<void>(declareFunction(id));
}

} // namespace lang::native_llvm
