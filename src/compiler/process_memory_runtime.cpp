#include "gti/process_memory_runtime.h"

#include <algorithm>
#include <array>
#include <vector>

namespace lang {
namespace {

[[nodiscard]] const std::vector<ProcessMemoryRuntimeSignature> &signatures() {
  static const std::vector<ProcessMemoryRuntimeSignature> table = [] {
    const SemanticType address = SemanticType::rawPointerTo(SemanticType::Void);
    const SemanticType observedAddress =
        SemanticType::rawPointerTo(SemanticType::Void, AccessMode::ReadOnly);
    const SemanticType status = SemanticType::rawPointerTo(SemanticType::Int32);
    return std::vector<ProcessMemoryRuntimeSignature>{
        {ProcessMemoryRuntimeOperation::NativeSizeMax,
         "gti_rt_native_size_max_v1",
         SemanticType::UInt64,
         {}},
        {ProcessMemoryRuntimeOperation::AddressIsAligned,
         "gti_rt_address_is_aligned_v1",
         SemanticType::Int32,
         {observedAddress, SemanticType::UInt64}},
        {ProcessMemoryRuntimeOperation::PosixPointerSize,
         "gti_rt_posix_pointer_size_v1",
         SemanticType::UInt64,
         {}},
        {ProcessMemoryRuntimeOperation::PosixOutOfMemoryStatus,
         "gti_rt_posix_enomem_v1",
         SemanticType::Int32,
         {}},
        {ProcessMemoryRuntimeOperation::PosixInvalidArgumentStatus,
         "gti_rt_posix_einval_v1",
         SemanticType::Int32,
         {}},
        {ProcessMemoryRuntimeOperation::PosixAllocate,
         "gti_rt_posix_memalign_v1",
         address,
         {SemanticType::UInt64, SemanticType::UInt64, status}},
        {ProcessMemoryRuntimeOperation::PosixRelease,
         "gti_rt_posix_free_v1",
         SemanticType::Void,
         {address}},
        {ProcessMemoryRuntimeOperation::AlignedAllocate,
         "gti_rt_aligned_malloc_v1",
         address,
         {SemanticType::UInt64, SemanticType::UInt64}},
        {ProcessMemoryRuntimeOperation::AlignedRelease,
         "gti_rt_aligned_free_v1",
         SemanticType::Void,
         {address}},
    };
  }();
  return table;
}

} // namespace

const ProcessMemoryRuntimeSignature *
processMemoryRuntimeSignature(ProcessMemoryRuntimeOperation operation) {
  for (const auto &signature : signatures()) {
    if (signature.operation == operation) {
      return &signature;
    }
  }
  return nullptr;
}

std::string_view
processMemoryRuntimeSymbol(ProcessMemoryRuntimeOperation operation) {
  if (const auto *signature = processMemoryRuntimeSignature(operation))
    return signature->symbol;
  return {};
}

ProcessMemoryRuntimeOperation
exactProcessMemoryRuntimeOperation(std::string_view symbol,
                                   const SemanticType &returnType,
                                   std::span<const SemanticType> parameters) {
  for (const auto &signature : signatures()) {
    if (signature.symbol == symbol && signature.returnType == returnType &&
        std::ranges::equal(signature.parameters, parameters)) {
      return signature.operation;
    }
  }
  return ProcessMemoryRuntimeOperation::None;
}

bool processMemoryRuntimeSelectedForTarget(
    ProcessMemoryRuntimeOperation operation, std::string_view os) {
  const bool windows = os == "windows";
  switch (operation) {
  case ProcessMemoryRuntimeOperation::NativeSizeMax:
  case ProcessMemoryRuntimeOperation::AddressIsAligned:
    return true;
  case ProcessMemoryRuntimeOperation::PosixPointerSize:
  case ProcessMemoryRuntimeOperation::PosixOutOfMemoryStatus:
  case ProcessMemoryRuntimeOperation::PosixInvalidArgumentStatus:
  case ProcessMemoryRuntimeOperation::PosixAllocate:
  case ProcessMemoryRuntimeOperation::PosixRelease:
    return !windows;
  case ProcessMemoryRuntimeOperation::AlignedAllocate:
  case ProcessMemoryRuntimeOperation::AlignedRelease:
    return windows;
  case ProcessMemoryRuntimeOperation::None:
  case ProcessMemoryRuntimeOperation::Count:
    return false;
  }
  return false;
}

} // namespace lang
