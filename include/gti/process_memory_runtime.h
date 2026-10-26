#pragma once

#include "gti/semantic_core.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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

// Immutable fixed-width C contract shared by semantic classification, MIR
// verification and compiler-generated private allocation mechanisms. Builtin
// types here carry no snapshot-local declaration IDs or mutable LLVM state.
struct ProcessMemoryRuntimeSignature {
  ProcessMemoryRuntimeOperation operation;
  std::string_view symbol;
  SemanticType returnType;
  std::vector<SemanticType> parameters;
};

// Stable table view, or nullptr for None/Count/unknown values. This query does
// not authorize application declarations or choose source allocation policy.
[[nodiscard]] const ProcessMemoryRuntimeSignature *
processMemoryRuntimeSignature(ProcessMemoryRuntimeOperation operation);

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
