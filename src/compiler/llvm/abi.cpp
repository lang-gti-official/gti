#include "abi.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Metadata.h>

#include <stdexcept>

namespace lang::native_llvm {
namespace {

AbiExtension nativeExtension(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Bool:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
    return AbiExtension::Unsigned;
  case SemanticType::Int8:
  case SemanticType::Int16:
    return AbiExtension::Signed;
  default:
    return AbiExtension::None;
  }
}

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
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto input = layouts.get(parameters[index]);
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
  const auto scalar = [&](const SemanticType &type,
                          bool allowVoid) -> llvm::Type * {
    switch (type.kind) {
    case SemanticType::Void:
      if (allowVoid)
        return llvm::Type::getVoidTy(session.context());
      break;
    case SemanticType::Bool:
      return llvm::Type::getInt1Ty(session.context());
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::RawPointer:
    case SemanticType::CString:
    case SemanticType::NativeFunction:
      return layouts.get(type).value;
    default:
      break;
    }
    throw BackendUnsupportedError(
        "LLVM native C ABI supports only exact scalars/pointers here; "
        "aggregate classification is pending O-LLVM-11");
  };
  FunctionAbi plan;
  auto *returnType = scalar(result, true);
  plan.resultExtension = nativeExtension(result);
  std::vector<llvm::Type *> types;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    types.push_back(scalar(parameters[index], false));
    plan.parameters.push_back({AbiChannel::Argument, index, false,
                               nativeExtension(parameters[index])});
  }
  plan.type = llvm::FunctionType::get(returnType, types, false);
  return plan;
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
  const auto name = native ? function->externalSymbol
                           : "__gti_native_v" +
                                 std::to_string(executionAbiVersion) + "_f" +
                                 std::to_string(id);
  if (name.empty())
    throw std::logic_error(
        "LLVM native declaration has no exact external symbol");
  if (auto *existing = session.module().getNamedValue(name)) {
    auto *previous = llvm::dyn_cast<llvm::Function>(existing);
    if (!previous || previous->getFunctionType() != plan.type)
      throw std::logic_error("conflicting LLVM symbol declaration: " + name);
    if (function->definitionKind == MirDefinitionKind::Source)
      session.requireSourceDefinition(id, *previous);
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
