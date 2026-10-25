#include "native_c_abi.h"

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

} // namespace lang::native_llvm
