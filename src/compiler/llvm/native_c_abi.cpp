#include "native_c_abi.h"

#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace lang::native_llvm {
namespace {

[[noreturn]] void unsupported() {
  throw BackendUnsupportedError(
      "LLVM native C record classifier requires a flat natural passive "
      "1..16-byte record of fixed integers, raw pointers, float or double "
      "on Darwin AArch64 or Linux x86_64 System V");
}

bool integerField(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::UInt8:
  case SemanticType::Int16:
  case SemanticType::UInt16:
  case SemanticType::Int32:
  case SemanticType::UInt32:
  case SemanticType::Int64:
  case SemanticType::UInt64:
  case SemanticType::RawPointer:
    return true;
  default:
    return false;
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

} // namespace

NativeCRecordAbi classifyNativeCRecord(Session &session,
                                      const SemanticType &type,
                                      NativeCRegisterBudget available) {
  const llvm::Triple triple(session.module().getTargetTriple());
  const bool darwin = triple.getArch() == llvm::Triple::aarch64 &&
                      triple.isOSDarwin() && triple.isOSBinFormatMachO();
  const bool sysv = triple.getArch() == llvm::Triple::x86_64 &&
                    triple.isOSLinux() && triple.isOSBinFormatELF();
  if ((!darwin && !sysv) || !session.dataLayout().isLittleEndian() ||
      session.dataLayout().getPointerSize() != 8 ||
      available.integers > 6 || available.vectors > 8 ||
      type.kind != SemanticType::Class)
    unsupported();
  session.verify();
  auto &layouts = session.layouts();
  const auto &record = layouts.instance(type);
  if (!record.cAbiRecord || !record.cAbiLayout || record.unionLayout ||
      record.cOpaqueHandle || !record.bases.empty() || record.polymorphic ||
      record.requiresActiveCleanup || record.requiresActiveDropState ||
      record.cAbiLayout->fields.empty())
    unsupported();
  const auto storage = layouts.get(type);
  if (!storage.size || storage.size > 16 || storage.alignment > 8)
    unsupported();

  bool homogeneous = true;
  const auto &fields = record.cAbiLayout->fields;
  const auto firstKind = fields.front().type.kind;
  for (const auto &field : fields) {
    if (!integerField(field.type) && field.type.kind != SemanticType::Float &&
        field.type.kind != SemanticType::Double)
      unsupported();
    if (field.offsetBytes % field.abiAlignmentBytes != 0)
      unsupported();
    homogeneous &= field.type.kind == firstKind;
  }
  homogeneous &= (firstKind == SemanticType::Float ||
                  firstKind == SemanticType::Double) && fields.size() <= 4;

  NativeCRecordAbi plan;
  plan.storage = storage;
  plan.remaining = available;
  auto &context = session.context();
  if (darwin) {
    // AAPCS64 6.8 B/C and 6.9, with Darwin's natural stack alignment.
    // Keep the aggregate argument grouped: LLVM owns register/stack allocation.
    // See https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
    if (homogeneous) {
      auto *element = layouts.get(fields.front().type).storage;
      plan.argument.coercion = llvm::ArrayType::get(element, fields.size());
      plan.result.coercion = storage.storage;
    } else {
      auto *i64 = llvm::Type::getInt64Ty(context);
      plan.argument.coercion = storage.size <= 8
                                   ? i64
                                   : llvm::ArrayType::get(i64, 2);
      plan.result.coercion = storage.size <= 8
                                 ? llvm::IntegerType::get(context, storage.size * 8)
                                 : llvm::ArrayType::get(i64, 2);
    }
    plan.argument.pieces = {{plan.argument.coercion, 0}};
    plan.result.pieces = {{plan.result.coercion, 0}};
    return plan;
  }

  // AMD64 psABI 3.2.3: classify independent eightbytes, INTEGER dominates SSE.
  // This bound has no unaligned fields, X87, SSEUP or >2-eightbyte aggregates.
  // https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex
  struct Eightbyte {
    bool integer = false;
    bool doubleField = false;
    unsigned floats = 0;
    std::uint64_t end = 0;
  };
  std::array<Eightbyte, 2> chunks{};
  for (const auto &field : fields) {
    const auto index = field.offsetBytes / 8;
    auto &chunk = chunks.at(index);
    chunk.integer |= integerField(field.type);
    chunk.doubleField |= field.type.kind == SemanticType::Double;
    chunk.floats += field.type.kind == SemanticType::Float;
    chunk.end = std::max(chunk.end, field.offsetBytes + field.sizeBytes);
  }
  unsigned integers = 0, vectors = 0;
  for (std::uint64_t index = 0; index < (storage.size + 7) / 8; ++index) {
    const auto &chunk = chunks[index];
    llvm::Type *carrier = nullptr;
    if (chunk.integer) {
      ++integers;
      carrier = llvm::IntegerType::get(
          context, static_cast<unsigned>((chunk.end - index * 8) * 8));
    } else {
      ++vectors;
      carrier = chunk.doubleField
                    ? llvm::Type::getDoubleTy(context)
                : chunk.floats == 2
                    ? static_cast<llvm::Type *>(llvm::FixedVectorType::get(
                          llvm::Type::getFloatTy(context), 2))
                    : llvm::Type::getFloatTy(context);
    }
    plan.result.pieces.push_back({carrier, index * 8});
  }
  plan.result.coercion = joined(context, plan.result.pieces);
  if (integers > available.integers || vectors > available.vectors) {
    // Whole-aggregate rollback: byval copies to the outgoing stack, without
    // consuming a pointer register. Never substitute an ordinary pointer here.
    plan.argument.coercion = layouts.pointer();
    plan.argument.byValue = true;
    plan.argument.stackAlignment = std::max<std::uint64_t>(8, storage.alignment);
  } else {
    plan.argument = plan.result;
    plan.argument.expand = true;
    plan.remaining.integers -= integers;
    plan.remaining.vectors -= vectors;
  }
  return plan;
}

} // namespace lang::native_llvm
