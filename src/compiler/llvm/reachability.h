#pragma once

#include "gti/lowered_program.h"

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

// Unknown indirect dispatch conservatively retains the complete executable
// inventory. Omission is never inferred from a source spelling or visibility.
[[nodiscard]] Reachability reachableProgram(const LoweredProgram &program);

} // namespace lang::native_llvm
