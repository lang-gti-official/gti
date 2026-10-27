#include "closure_environment.h"

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
    if (candidate.type != type)
      continue;
    auto current = closureEnvironmentLayout(candidate);
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
  return instance != nullptr && instance->type == recipe.type ? instance
                                                              : nullptr;
}

} // namespace lang::native_llvm
