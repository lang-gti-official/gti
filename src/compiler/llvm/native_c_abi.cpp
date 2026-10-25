#include "native_c_abi.h"
#include "failure.h"

#include <llvm/IR/Constants.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <array>
#include <functional>

namespace lang::native_llvm {
namespace {

[[noreturn]] void unsupported() {
  throw BackendUnsupportedError(
      "LLVM native C ABI requires a verified natural passive C value on "
      "Darwin AArch64 or Linux x86_64 System V");
}

bool integerField(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Bool:
  case SemanticType::Int8:
  case SemanticType::UInt8:
  case SemanticType::Int16:
  case SemanticType::UInt16:
  case SemanticType::Int32:
  case SemanticType::UInt32:
  case SemanticType::Int64:
  case SemanticType::UInt64:
  case SemanticType::RawPointer:
  case SemanticType::CString:
  case SemanticType::NativeFunction:
    return true;
  default:
    return false;
  }
}

bool darwinTarget(Session &session) {
  const llvm::Triple triple(session.module().getTargetTriple());
  const auto &target = session.program().target();
  const bool darwin = triple.getArch() == llvm::Triple::aarch64 &&
                      triple.isOSDarwin() && triple.isOSBinFormatMachO() &&
                      target.os == "macos" && target.arch == "arm64";
  const bool sysv = triple.getArch() == llvm::Triple::x86_64 &&
                    triple.isOSLinux() && triple.isOSBinFormatELF() &&
                    target.os == "linux" && target.arch == "x86_64";
  if ((!darwin && !sysv) || !session.dataLayout().isLittleEndian() ||
      session.dataLayout().getPointerSize() != 8)
    unsupported();
  // A caller may still have an unterminated IR block. Session verifies the
  // completed module; classification only checks immutable target/layout facts.
  verifyTargetLayout(target, session.dataLayout(), session.context());
  return darwin;
}

struct Homogeneous {
  llvm::Type *base = nullptr;
  unsigned count = 0;
};

Homogeneous inspect(Session &session, const SemanticType &type,
                    unsigned depth = 0) {
  if (depth > 256)
    unsupported();
  if (integerField(type))
    return {};
  if (type.kind == SemanticType::Float || type.kind == SemanticType::Double)
    return {session.layouts().get(type).storage, 1};
  if (type.kind == SemanticType::StringView)
    return {};
  if (type.kind == SemanticType::Array) {
    if (type.arguments.size() != 1 || !type.arrayLength ||
        type.arrayLengthParameterId)
      unsupported();
    const auto element = inspect(session, type.arguments[0], depth + 1);
    if (!element.base || type.arrayLength > 4 / element.count)
      return {};
    return {element.base,
            static_cast<unsigned>(type.arrayLength * element.count)};
  }
  if (type.kind != SemanticType::Class)
    unsupported();
  const auto &record = session.layouts().instance(type);
  if (!record.cAbiRecord || !record.cAbiLayout || record.cOpaqueHandle ||
      !record.bases.empty() || !record.structuralBases.empty() ||
      record.polymorphic || record.requiresActiveCleanup ||
      record.requiresActiveDropState || record.declaredFields.empty())
    unsupported();
  const auto storage = session.layouts().get(type);
  if (!storage.size || storage.alignment > 8)
    unsupported();
  Homogeneous hfa;
  bool homogeneous = true;
  for (const auto &field : record.declaredFields) {
    const auto next = inspect(session, field.type, depth + 1);
    if (!next.base || (hfa.base && hfa.base != next.base))
      homogeneous = false;
    if (!hfa.base)
      hfa.base = next.base;
    hfa.count = record.unionLayout ? std::max(hfa.count, next.count)
                                   : hfa.count + next.count;
    if (hfa.count > 4)
      homogeneous = false;
  }
  if (!homogeneous || !hfa.base || !hfa.count ||
      session.dataLayout().getTypeAllocSize(hfa.base).getFixedValue() *
              hfa.count !=
          storage.size)
    return {};
  return hfa;
}

void leaves(
    Session &session, const SemanticType &type, std::uint64_t at,
    const std::function<void(const SemanticType &, std::uint64_t)> &visit) {
  if (type.kind == SemanticType::Array) {
    const auto stride = session.layouts().get(type).stride;
    for (std::uint64_t i = 0; i < type.arrayLength; ++i)
      leaves(session, type.arguments[0], at + stride * i, visit);
  } else if (type.kind == SemanticType::Class) {
    const auto &record = session.layouts().instance(type);
    const auto storage = session.layouts().get(type);
    for (std::size_t i = 0; i < record.declaredFields.size(); ++i)
      leaves(session, record.declaredFields[i].type,
             at + storage.fieldOffsets.at(i), visit);
  } else if (type.kind == SemanticType::StringView) {
    visit(SemanticType::rawPointerTo(SemanticType::Void), at);
    visit(SemanticType::UInt64, at + 8);
  } else {
    visit(type, at);
  }
}

llvm::Type *joined(llvm::LLVMContext &context,
                   const std::vector<NativeCAbiPiece> &pieces) {
  if (pieces.size() == 1)
    return pieces.front().type;
  std::vector<llvm::Type *> types;
  for (const auto &piece : pieces)
    types.push_back(piece.type);
  return llvm::StructType::get(context, types);
}

NativeCRecordAbi aggregate(Session &session, const SemanticType &type,
                           NativeCRegisterBudget available) {
  const bool darwin = darwinTarget(session);
  if (available.integers > 6 || available.vectors > 8)
    unsupported();
  const auto hfa = inspect(session, type);
  NativeCRecordAbi plan;
  plan.storage = session.layouts().get(type);
  plan.remaining = available;
  auto &context = session.context();
  if (darwin && hfa.base) {
    // AAPCS64 6.8/6.9: flatten HFAs only after the complete aggregate layout.
    plan.argument.coercion = llvm::ArrayType::get(hfa.base, hfa.count);
    plan.result.coercion = plan.argument.coercion;
  } else if (plan.storage.size > 16) {
    plan.argument.coercion = session.layouts().pointer();
    plan.argument.indirect = true;
    plan.argument.byValue = !darwin;
    plan.argument.stackAlignment =
        darwin ? plan.storage.alignment
               : std::max<std::uint64_t>(8, plan.storage.alignment);
    plan.result.coercion = llvm::Type::getVoidTy(context);
    plan.result.indirect = true;
    return plan;
  } else if (darwin) {
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    plan.argument.coercion =
        plan.storage.size <= 8 ? i64 : llvm::ArrayType::get(i64, 2);
    plan.result.coercion =
        plan.storage.size <= 8
            ? static_cast<llvm::Type *>(
                  llvm::IntegerType::get(context, plan.storage.size * 8))
            : llvm::ArrayType::get(i64, 2);
  } else {
    // AMD64 psABI 3.2.3: INTEGER dominates SSE, including overlapping union
    // alternatives. Track occupied float lanes, not number of source fields.
    struct Eightbyte {
      bool integer = false;
      bool doubleField = false;
      std::array<bool, 2> floats{};
      std::uint64_t end = 0;
    };
    std::array<Eightbyte, 2> chunks{};
    leaves(session, type, 0, [&](const SemanticType &leaf, std::uint64_t at) {
      auto &chunk = chunks.at(at / 8);
      const auto layout = session.layouts().get(leaf);
      if (at % layout.alignment != 0 || at % 8 + layout.size > 8)
        unsupported();
      chunk.integer |= integerField(leaf);
      chunk.doubleField |= leaf.kind == SemanticType::Double;
      if (leaf.kind == SemanticType::Float)
        chunk.floats[(at % 8) / 4] = true;
      chunk.end = std::max(chunk.end, at + layout.size);
    });
    unsigned integers = 0, vectors = 0;
    for (std::uint64_t index = 0; index < (plan.storage.size + 7) / 8;
         ++index) {
      const auto &chunk = chunks[index];
      llvm::Type *carrier = nullptr;
      std::uint64_t at = index * 8;
      if (chunk.integer) {
        ++integers;
        carrier = llvm::IntegerType::get(context, (chunk.end - at) * 8);
      } else {
        ++vectors;
        if (chunk.doubleField)
          carrier = llvm::Type::getDoubleTy(context);
        else if (chunk.floats[0] && chunk.floats[1])
          carrier =
              llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 2);
        else if (chunk.floats[0] || chunk.floats[1]) {
          carrier = llvm::Type::getFloatTy(context);
          if (!chunk.floats[0])
            at += 4;
        } else
          unsupported();
      }
      plan.result.pieces.push_back({carrier, at});
    }
    plan.result.coercion = joined(context, plan.result.pieces);
    if (integers > available.integers || vectors > available.vectors) {
      plan.argument.coercion = session.layouts().pointer();
      plan.argument.indirect = true;
      plan.argument.byValue = true;
      plan.argument.stackAlignment =
          std::max<std::uint64_t>(8, plan.storage.alignment);
    } else {
      plan.argument = plan.result;
      plan.argument.expand = true;
      plan.remaining.integers -= integers;
      plan.remaining.vectors -= vectors;
    }
    return plan;
  }
  plan.argument.pieces = {{plan.argument.coercion, 0}};
  plan.result.pieces = {{plan.result.coercion, 0}};
  return plan;
}

AbiExtension extension(const SemanticType &type) {
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

llvm::AllocaInst *temporary(llvm::IRBuilder<> &ir, llvm::Type *type,
                            llvm::Align alignment = llvm::Align(1)) {
  auto &entry = ir.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> allocate(&entry, entry.begin());
  auto *slot = allocate.CreateAlloca(type);
  slot->setAlignment(alignment);
  return slot;
}

llvm::Value *addressAt(llvm::IRBuilder<> &ir, llvm::Value *address,
                       std::uint64_t at) {
  return ir.CreateGEP(ir.getInt8Ty(), address, ir.getInt64(at));
}

llvm::Value *pack(Session &session, llvm::IRBuilder<> &ir, llvm::Value *storage,
                  const NativeCAbiPiece &piece, std::uint64_t size) {
  auto *slot = temporary(ir, piece.type,
                         session.dataLayout().getABITypeAlign(piece.type));
  ir.CreateStore(llvm::Constant::getNullValue(piece.type), slot);
  const auto bytes =
      session.dataLayout().getTypeStoreSize(piece.type).getFixedValue();
  ir.CreateMemCpy(slot, llvm::MaybeAlign(1),
                  addressAt(ir, storage, piece.offset), llvm::MaybeAlign(1),
                  std::min(bytes, size - piece.offset));
  return ir.CreateLoad(piece.type, slot);
}

void unpack(Session &session, llvm::IRBuilder<> &ir, llvm::Value *storage,
            llvm::Value *value, std::uint64_t at, std::uint64_t size) {
  auto *slot =
      temporary(ir, value->getType(),
                session.dataLayout().getABITypeAlign(value->getType()));
  ir.CreateStore(value, slot);
  const auto bytes =
      session.dataLayout().getTypeStoreSize(value->getType()).getFixedValue();
  ir.CreateMemCpy(addressAt(ir, storage, at), llvm::MaybeAlign(1), slot,
                  llvm::MaybeAlign(1), std::min(bytes, size - at));
}

} // namespace

NativeCRecordAbi classifyNativeCRecord(Session &session,
                                       const SemanticType &type,
                                       NativeCRegisterBudget available) {
  if (type.kind != SemanticType::Class)
    unsupported();
  return aggregate(session, type, available);
}

NativeCFunctionAbi
planNativeCFunction(Session &session, const SemanticType &result,
                    std::span<const SemanticType> parameters) {
  const bool darwin = darwinTarget(session);
  NativeCFunctionAbi plan;
  NativeCRegisterBudget available;
  std::vector<llvm::Type *> types;
  const auto describe = [&](const SemanticType &type, bool output) {
    NativeCValueAbi value;
    value.type = type;
    value.storage = session.layouts().get(type);
    value.aggregate = type.kind == SemanticType::Class ||
                      type.kind == SemanticType::StringView;
    if (value.aggregate) {
      if (output && type.kind == SemanticType::StringView)
        unsupported();
      const auto record = aggregate(session, type, available);
      value.transport = output ? record.result : record.argument;
      if (!output)
        available = record.remaining;
    } else {
      if (!integerField(type) && type.kind != SemanticType::Float &&
          type.kind != SemanticType::Double &&
          !(output && type.kind == SemanticType::Void))
        unsupported();
      value.transport.coercion = value.storage.value;
      if (!output && !darwin) {
        auto &count = type.kind == SemanticType::Float ||
                              type.kind == SemanticType::Double
                          ? available.vectors
                          : available.integers;
        if (count)
          --count;
      }
    }
    return value;
  };
  plan.result = describe(result, true);
  if (plan.result.transport.indirect) {
    plan.signature.resultDestination = 0;
    types.push_back(session.layouts().pointer());
    plan.signature.parameters.push_back(
        {AbiChannel::ResultDestination, 0, true});
    plan.signature.nativeMemory.push_back({0, plan.result.storage.storage,
                                           plan.result.storage.alignment, true,
                                           false});
    if (!darwin)
      --available.integers;
  } else if (!plan.result.aggregate) {
    plan.signature.resultExtension = extension(result);
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    auto value = describe(parameters[index], false);
    value.firstParameter = types.size();
    if (value.transport.expand) {
      for (const auto &piece : value.transport.pieces) {
        types.push_back(piece.type);
        plan.signature.parameters.push_back({AbiChannel::Argument, index});
      }
    } else {
      types.push_back(value.transport.coercion);
      plan.signature.parameters.push_back(
          {AbiChannel::Argument, index, value.transport.indirect,
           value.aggregate ? AbiExtension::None : extension(value.type)});
    }
    if (value.transport.indirect)
      plan.signature.nativeMemory.push_back(
          {value.firstParameter, value.storage.storage,
           value.transport.stackAlignment, false, value.transport.byValue});
    plan.arguments.push_back(std::move(value));
  }
  plan.signature.type =
      llvm::FunctionType::get(plan.result.transport.coercion, types, false);
  return plan;
}

llvm::Value *emitNativeCCall(Session &session, llvm::IRBuilder<> &ir,
                             const NativeCFunctionAbi &plan,
                             llvm::Value *callee,
                             std::span<llvm::Value *const> arguments,
                             llvm::Value *resultDestination) {
  if (arguments.size() != plan.arguments.size())
    throw std::logic_error(
        "native C call disagrees with its exact argument plan");
  if (plan.result.aggregate && !resultDestination)
    resultDestination = temporary(ir, plan.result.storage.storage,
                                  llvm::Align(plan.result.storage.alignment));
  std::vector<llvm::Value *> lowered;
  if (plan.result.transport.indirect)
    lowered.push_back(resultDestination);
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &value = plan.arguments[index];
    auto *input = arguments[index];
    if (!value.aggregate) {
      lowered.push_back(input);
      continue;
    }
    if (value.type.kind == SemanticType::StringView) {
      auto *slot = temporary(ir, value.storage.storage,
                             llvm::Align(value.storage.alignment));
      ir.CreateStore(input, slot);
      input = slot;
    }
    if (value.transport.indirect) {
      // An indirect C value is still a copy, not a borrow of the GTI source.
      const auto alignment =
          std::max(value.storage.alignment, value.transport.stackAlignment);
      auto *copy = temporary(ir, value.storage.storage, llvm::Align(alignment));
      ir.CreateMemCpy(copy, llvm::MaybeAlign(alignment), input,
                      llvm::MaybeAlign(1), value.storage.size);
      lowered.push_back(copy);
    } else {
      for (const auto &piece : value.transport.pieces)
        lowered.push_back(pack(session, ir, input, piece, value.storage.size));
    }
  }
  auto *call = ir.CreateCall(plan.signature.type, callee, lowered);
  plan.signature.applyTo(*call);
  if (!plan.result.aggregate)
    return call->getType()->isVoidTy() ? nullptr : call;
  if (!plan.result.transport.indirect) {
    const auto &pieces = plan.result.transport.pieces;
    if (pieces.size() == 1)
      unpack(session, ir, resultDestination, call, pieces[0].offset,
             plan.result.storage.size);
    else
      for (unsigned i = 0; i < pieces.size(); ++i)
        unpack(session, ir, resultDestination, ir.CreateExtractValue(call, i),
               pieces[i].offset, plan.result.storage.size);
  }
  return resultDestination;
}

std::vector<llvm::Value *>
decodeNativeCArguments(Session &session, llvm::IRBuilder<> &ir,
                       const NativeCFunctionAbi &plan,
                       llvm::Function &function) {
  if (function.getFunctionType() != plan.signature.type)
    throw std::logic_error("native C entry disagrees with its argument plan");
  std::vector<llvm::Value *> values;
  for (const auto &argument : plan.arguments) {
    auto *input = function.getArg(argument.firstParameter);
    if (!argument.aggregate) {
      values.push_back(input);
      continue;
    }
    auto *storage = temporary(ir, argument.storage.storage,
                              llvm::Align(argument.storage.alignment));
    if (argument.transport.indirect) {
      ir.CreateMemCpy(storage, llvm::MaybeAlign(argument.storage.alignment),
                      input,
                      llvm::MaybeAlign(argument.transport.stackAlignment),
                      argument.storage.size);
    } else {
      ir.CreateStore(llvm::Constant::getNullValue(argument.storage.storage),
                     storage);
      for (unsigned index = 0; index < argument.transport.pieces.size();
           ++index)
        unpack(session, ir, storage,
               function.getArg(argument.firstParameter + index),
               argument.transport.pieces[index].offset, argument.storage.size);
    }
    llvm::Value *bodyValue = storage;
    if (argument.type.kind == SemanticType::StringView)
      bodyValue = ir.CreateLoad(argument.storage.value, storage);
    values.push_back(bodyValue);
  }
  return values;
}

void emitNativeCReturn(Session &session, llvm::IRBuilder<> &ir,
                       const NativeCFunctionAbi &plan, llvm::Function &function,
                       llvm::Value *value) {
  if (function.getFunctionType() != plan.signature.type)
    throw std::logic_error("native C entry disagrees with its return plan");
  if (!plan.result.aggregate) {
    if (plan.signature.type->getReturnType()->isVoidTy())
      ir.CreateRetVoid();
    else if (value)
      ir.CreateRet(value);
    else
      throw std::logic_error("native C return lost its scalar result");
    return;
  }
  if (!value)
    throw std::logic_error("native C return lost its aggregate storage");
  if (plan.result.transport.indirect) {
    ir.CreateMemCpy(function.getArg(*plan.signature.resultDestination),
                    llvm::MaybeAlign(plan.result.storage.alignment), value,
                    llvm::MaybeAlign(plan.result.storage.alignment),
                    plan.result.storage.size);
    ir.CreateRetVoid();
    return;
  }
  const auto &pieces = plan.result.transport.pieces;
  if (pieces.size() == 1) {
    ir.CreateRet(
        pack(session, ir, value, pieces.front(), plan.result.storage.size));
    return;
  }
  llvm::Value *result = llvm::PoisonValue::get(plan.result.transport.coercion);
  for (unsigned index = 0; index < pieces.size(); ++index)
    result = ir.CreateInsertValue(
        result,
        pack(session, ir, value, pieces[index], plan.result.storage.size),
        index);
  ir.CreateRet(result);
}

void declareNativeCStorage(
    Session &session, std::map<SymbolId, llvm::GlobalVariable *> &globals) {
  for (const auto &foreign :
       session.program().mir().foreignStorageDeclarations()) {
    if (foreign.externalSymbol.empty() || globals.contains(foreign.symbol) ||
        session.module().getNamedValue(foreign.externalSymbol))
      throw std::logic_error("foreign C storage lost its unique sealed symbol");
    if (foreign.type.kind == SemanticType::Class)
      static_cast<void>(inspect(session, foreign.type));
    else if (!integerField(foreign.type) &&
             foreign.type.kind != SemanticType::Float &&
             foreign.type.kind != SemanticType::Double)
      unsupported();
    const auto layout = session.layouts().get(foreign.type);
    // Even a read-only source declaration does not prove native value
    // stability.
    auto *storage = new llvm::GlobalVariable(
        session.module(), layout.storage, false,
        llvm::GlobalValue::ExternalLinkage, nullptr, foreign.externalSymbol);
    storage->setAlignment(llvm::Align(layout.alignment));
    globals.emplace(foreign.symbol, storage);
  }
}

namespace {

std::string callbackName(MirNativeCallbackAdapterId identity) {
  return "__gti_native_c_callback_" + std::to_string(identity);
}

llvm::Function *nativeService(Session &session, const char *name,
                              llvm::Type *result,
                              llvm::ArrayRef<llvm::Type *> parameters) {
  auto *type = llvm::FunctionType::get(result, parameters, false);
  if (auto *previous = session.module().getNamedValue(name)) {
    auto *function = llvm::dyn_cast<llvm::Function>(previous);
    if (!function || function->getFunctionType() != type)
      throw std::logic_error("native callback runtime signature conflict");
    return function;
  }
  return llvm::Function::Create(type, llvm::Function::ExternalLinkage, name,
                                session.module());
}

void abortNativeBoundary(Session &session, llvm::IRBuilder<> &ir) {
  auto *abort = nativeService(session, "abort", ir.getVoidTy(), {});
  ir.CreateCall(abort);
  ir.CreateUnreachable();
}

llvm::GlobalVariable *callbackOwner(Session &session,
                                    MirNativeCallbackAdapterId identity) {
  const auto name = callbackName(identity) + ".owner";
  if (auto *previous = session.module().getNamedGlobal(name))
    return previous;
  auto *type = llvm::Type::getInt64Ty(session.context());
  auto *owner = new llvm::GlobalVariable(session.module(), type, false,
                                         llvm::GlobalValue::InternalLinkage,
                                         llvm::ConstantInt::get(type, 0), name);
  owner->setAlignment(llvm::Align(8));
  return owner;
}

void callbackAttachment(Session &session, llvm::IRBuilder<> &ir,
                        const MirNativeCallbackAdapter &adapter,
                        llvm::Function &wrapper, bool entering) {
  const char *symbol = nullptr;
  llvm::Value *argument = &wrapper;
  switch (adapter.cThreadCallback) {
  case CThreadCallbackKind::None:
    return;
  case CThreadCallbackKind::ThreadEntry:
    symbol = entering ? "gti_rt_cthread_enter_v1" : "gti_rt_cthread_leave_v1";
    break;
  case CThreadCallbackKind::OnceInitializer:
    symbol = entering ? "gti_rt_once_enter_v1" : "gti_rt_once_leave_v1";
    break;
  case CThreadCallbackKind::ThreadConfined:
    if (!entering) {
      ir.CreateCall(nativeService(session, "gti_rt_confined_callback_leave_v1",
                                  ir.getVoidTy(), {}));
      return;
    }
    symbol = "gti_rt_confined_callback_enter_v1";
    auto *owner =
        ir.CreateLoad(ir.getInt64Ty(), callbackOwner(session, adapter.id));
    owner->setAtomic(llvm::AtomicOrdering::Monotonic);
    argument = owner;
    break;
  }
  // Signatures are the versioned C declarations in
  // runtime/include/gti/cthread.h.
  ir.CreateCall(
      nativeService(session, symbol, ir.getVoidTy(), {argument->getType()}),
      {argument});
}

} // namespace

void emitNativeCCallbacks(Session &session, Failures &failures) {
  static_cast<void>(darwinTarget(session));
  for (const auto &adapter : session.program().mir().nativeCallbackAdapters()) {
    const auto *target =
        session.program().mir().findFunctionInstance(adapter.target);
    const auto *resultType = adapter.type.nativeFunctionReturnType();
    const auto parameters = adapter.type.nativeFunctionParameterTypes();
    if (!target || !resultType || target->owner || target->tracksCaller ||
        target->linkage == LanguageLinkage::C ||
        target->definitionKind != MirDefinitionKind::Source ||
        target->returnType != *resultType ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameters.begin(),
                    parameters.end()) ||
        target->mayRaiseDefinedFailure !=
            adapter.targetMayRaiseDefinedFailure ||
        adapter.failurePolicy !=
            MirNativeCallbackFailurePolicy::TerminateInvocation ||
        !adapter.catchesNativeExceptions)
      throw std::logic_error(
          "native callback lost its sealed exact target contract");
    const auto name = callbackName(adapter.id);
    if (session.module().getNamedValue(name))
      throw std::logic_error("native callback emitted more than once");
    const auto native = planNativeCFunction(session, *resultType, parameters);
    const auto gti = session.abi().gti(*resultType, parameters,
                                       target->mayRaiseDefinedFailure);
    auto *wrapper = llvm::Function::Create(native.signature.type,
                                           llvm::Function::InternalLinkage,
                                           name, session.module());
    native.signature.applyTo(*wrapper);
    wrapper->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", wrapper));
    callbackAttachment(session, ir, adapter, *wrapper, true);
    const auto inputs = decodeNativeCArguments(session, ir, native, *wrapper);
    llvm::Value *output = nullptr;
    if (gti.resultDestination)
      output = temporary(ir, native.result.storage.storage,
                         llvm::Align(native.result.storage.alignment));
    llvm::Value *failure = nullptr;
    if (gti.failureDestination) {
      failure = temporary(
          ir, session.abi().failureChannel(),
          session.dataLayout().getABITypeAlign(session.abi().failureChannel()));
      ir.CreateStore(
          llvm::Constant::getNullValue(session.abi().failureChannel()),
          failure);
    }
    std::vector<llvm::Value *> arguments;
    for (const auto &parameter : gti.parameters) {
      switch (parameter.channel) {
      case AbiChannel::ResultDestination:
        arguments.push_back(output);
        break;
      case AbiChannel::FailureDestination:
        arguments.push_back(failure);
        break;
      case AbiChannel::Argument:
        if (auto *input = inputs.at(parameter.sourceIndex);
            parameter.indirect && !input->getType()->isPointerTy()) {
          const auto layout =
              session.layouts().get(parameters[parameter.sourceIndex]);
          auto *slot =
              temporary(ir, layout.storage, llvm::Align(layout.alignment));
          ir.CreateStore(input, slot);
          arguments.push_back(slot);
        } else {
          arguments.push_back(input);
        }
        break;
      default:
        throw std::logic_error(
            "native callback acquired an unverified hidden channel");
      }
    }
    // This personality contains exceptions from native dependencies only. GTI
    // defined failure still uses the unchanged centrally planned private ABI.
    auto personality = session.module().getOrInsertFunction(
        "__gxx_personality_v0", llvm::FunctionType::get(ir.getInt32Ty(), true));
    wrapper->setPersonalityFn(
        llvm::cast<llvm::Constant>(personality.getCallee()));
    auto *normal =
        llvm::BasicBlock::Create(session.context(), "target.returned", wrapper);
    auto *caught = llvm::BasicBlock::Create(session.context(),
                                            "native.exception", wrapper);
    auto *call = ir.CreateInvoke(session.abi().declareFunction(target->id),
                                 normal, caught, arguments);
    gti.applyTo(*call);
    ir.SetInsertPoint(caught);
    auto *landingType =
        llvm::StructType::get(session.layouts().pointer(), ir.getInt32Ty());
    auto *landing = ir.CreateLandingPad(landingType, 1);
    landing->addClause(
        llvm::ConstantPointerNull::get(session.layouts().pointer()));
    ir.CreateCall(nativeService(session, "__cxa_begin_catch",
                                session.layouts().pointer(),
                                {session.layouts().pointer()}),
                  {ir.CreateExtractValue(landing, 0)});
    abortNativeBoundary(session, ir);
    ir.SetInsertPoint(normal);
    if (gti.statusResult) {
      auto *success = llvm::BasicBlock::Create(session.context(),
                                               "target.success", wrapper);
      auto *failed =
          llvm::BasicBlock::Create(session.context(), "target.failed", wrapper);
      ir.CreateCondBr(call, success, failed);
      ir.SetInsertPoint(failed);
      failures.terminate(ir, failure);
      ir.SetInsertPoint(success);
    }
    callbackAttachment(session, ir, adapter, *wrapper, false);
    llvm::Value *result = output;
    if (!native.result.storage.addressOnly && output)
      result = ir.CreateLoad(native.result.storage.value, output);
    if (!gti.resultDestination && !call->getType()->isVoidTy())
      result = call;
    emitNativeCReturn(session, ir, native, *wrapper, result);
  }
}

llvm::Value *nativeCCallbackAddress(Session &session, llvm::IRBuilder<> &ir,
                                    MirNativeCallbackAdapterId identity) {
  const auto *adapter =
      session.program().mir().findNativeCallbackAdapter(identity);
  auto *wrapper = session.module().getFunction(callbackName(identity));
  if (!adapter || !wrapper || wrapper->isDeclaration())
    throw std::logic_error(
        "native callback address requires its emitted adapter");
  if (adapter->cThreadCallback != CThreadCallbackKind::ThreadConfined)
    return wrapper;
  auto *current = ir.CreateCall(nativeService(
      session, "gti_rt_confined_callback_thread_v1", ir.getInt64Ty(), {}));
  auto *owner = callbackOwner(session, identity);
  auto *observed = ir.CreateLoad(ir.getInt64Ty(), owner);
  observed->setAtomic(llvm::AtomicOrdering::Monotonic);
  auto *function = ir.GetInsertBlock()->getParent();
  auto *ready =
      llvm::BasicBlock::Create(session.context(), "callback.bound", function);
  auto *bind =
      llvm::BasicBlock::Create(session.context(), "callback.bind", function);
  auto *invalid = llvm::BasicBlock::Create(session.context(),
                                           "callback.wrong.thread", function);
  ir.CreateCondBr(ir.CreateICmpEQ(observed, current), ready, bind);
  ir.SetInsertPoint(bind);
  auto *claim = ir.CreateAtomicCmpXchg(
      owner, ir.getInt64(0), current, llvm::MaybeAlign(8),
      llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
  ir.CreateCondBr(
      ir.CreateOr(ir.CreateExtractValue(claim, 1),
                  ir.CreateICmpEQ(ir.CreateExtractValue(claim, 0), current)),
      ready, invalid);
  ir.SetInsertPoint(invalid);
  abortNativeBoundary(session, ir);
  ir.SetInsertPoint(ready);
  return wrapper;
}

} // namespace lang::native_llvm
