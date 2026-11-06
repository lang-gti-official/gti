#include "closure_environment.h"
#include "../mir_lifetimes.h"

#include <utility>

namespace lang::native_llvm {

ClosureEnvironmentLayout
closureEnvironmentLayout(const MirLambdaInstance &instance) {
  return {.captureTypes = instance.captureTypes,
          .captureModes = instance.captureModes,
          .captureRequiresActiveCleanup =
              instance.captureRequiresActiveCleanup};
}

std::optional<ClosureEnvironmentLayout>
closureEnvironmentLayout(const MirProgram &program, const SemanticType &type) {
  std::optional<ClosureEnvironmentLayout> result;
  for (const auto &candidate : program.lambdaInstances()) {
    const auto view = mirLambdaLifetimeView(candidate, type);
    if (!view) continue;
    auto current = closureEnvironmentLayout(candidate);
    current.captureTypes.assign(type.lambdaCaptureTypes().begin(),
                                 type.lambdaCaptureTypes().end());
    if (result && *result != current)
      return std::nullopt;
    result = std::move(current);
  }
  return result;
}

const MirLambdaInstance *closureCleanupInstance(const MirProgram &program,
                                                const MirDropType &recipe) {
  if (!recipe.lambdaInstance || recipe.type.kind != SemanticType::Lambda ||
      recipe.classInstance || recipe.destructor)
    return nullptr;
  const auto *instance = program.findLambda(*recipe.lambdaInstance);
  return instance && mirLambdaLifetimeView(*instance, recipe.type) ? instance
                                                                  : nullptr;
}

} // namespace lang::native_llvm
