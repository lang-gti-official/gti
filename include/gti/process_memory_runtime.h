#pragma once

#include "gti/semantic_core.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace lang {

// Declared contract of GTI's own process-allocator runtime ABI
// (`runtime/include/gti/memory.h`). Semantics selects an operation only for an
// exact compiler-private C declaration in the trusted `std/memory` library
// unit; an application declaration or public spelling of the same symbol
// stays an ordinary native declaration. Calls remain ordinary native calls:
// the classification lets MIR state each declaration's effects precisely.
enum class ProcessMemoryRuntimeOperation : std::uint8_t {
  None,
  NativeSizeMax,
  AddressIsAligned,
  PosixPointerSize,
  PosixOutOfMemoryStatus,
  PosixInvalidArgumentStatus,
  PosixAllocate,
  PosixRelease,
  AlignedAllocate,
  AlignedRelease,
  Count
};

// The exact versioned runtime symbol of an operation; empty for None/Count.
[[nodiscard]] std::string_view
processMemoryRuntimeSymbol(ProcessMemoryRuntimeOperation operation);

// The operation whose exact symbol and native signature both match, or None.
// Semantic classification and MIR verification share this one table.
[[nodiscard]] ProcessMemoryRuntimeOperation
exactProcessMemoryRuntimeOperation(std::string_view symbol,
                                   const SemanticType &returnType,
                                   std::span<const SemanticType> parameters);

// `std/memory` declares the aligned CRT pair on Windows and the POSIX family
// on every other target; the two common queries exist on both.
[[nodiscard]] bool
processMemoryRuntimeSelectedForTarget(ProcessMemoryRuntimeOperation operation,
                                      std::string_view os);

} // namespace lang
