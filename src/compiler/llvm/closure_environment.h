#pragma once

#include "gti/mir.h"

#include <optional>

namespace lang::native_llvm {

// Physical closure storage is a property of the fully concrete lambda type.
// Body identity is deliberately absent: formation, invocation and cleanup keep
// their exact MirLambdaInstance IDs even when instantiated bodies share this
// representation.
struct ClosureEnvironmentLayout final {
  std::vector<SemanticType> captureTypes;
  std::vector<LambdaCaptureMode> captureModes;
  std::vector<bool> captureRequiresActiveCleanup;

  friend bool operator==(const ClosureEnvironmentLayout &,
                         const ClosureEnvironmentLayout &) = default;
};

[[nodiscard]] ClosureEnvironmentLayout
closureEnvironmentLayout(const MirLambdaInstance &instance);

// Returns one representation only when every exact instance of the concrete
// type agrees. Missing or incompatible inventories fail closed before object
// publication; no instance is selected as callable or cleanup authority.
[[nodiscard]] std::optional<ClosureEnvironmentLayout>
closureEnvironmentLayout(const MirProgram &program, const SemanticType &type);

// Cleanup identity is never representation-canonicalized. The recipe's exact
// instance remains the authority for capture selectors and nested drop plans.
[[nodiscard]] const MirLambdaInstance *
closureCleanupInstance(const MirProgram &program, const MirDropType &recipe);

} // namespace lang::native_llvm
