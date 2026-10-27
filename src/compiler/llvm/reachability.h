#pragma once

#include "gti/lowered_program.h"

#include <optional>

namespace lang::native_llvm {

struct Reachability {
  std::vector<MirBodyAddress> bodies;
  std::vector<LoweredGeneratedItemIdentity> generated;
  bool conservative = false;
  [[nodiscard]] bool contains(MirBodyAddress address) const;
  [[nodiscard]] bool contains(LoweredGeneratedItemIdentity identity) const;
};

// Exact source implementations already selected by the sealed active-class
// contract. This is not overload/override lookup or a native table graph.
// Every demanded Source target must have its own sealed executable body.
[[nodiscard]] std::vector<MirBodyAddress>
dispatchSourceBodies(const LoweredProgram &program,
                     HirClassInstanceId activeClass);

// The concrete polymorphic class whose complete object this exact verified
// allocation creates as a unique-owner pointee. Owning such an object later
// selects its cleanup through the installed dispatch descriptor, never through
// a statically named base or interface.
[[nodiscard]] std::optional<HirClassInstanceId>
dynamicOwnerAllocation(const MirProgram &program,
                       const MirInstruction &instruction);

// Unknown indirect dispatch conservatively retains the complete executable
// inventory. Omission is never inferred from a source spelling or visibility.
[[nodiscard]] Reachability reachableProgram(const LoweredProgram &program);

} // namespace lang::native_llvm
