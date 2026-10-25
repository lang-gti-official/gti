#include "parameter_packs.h"

#include "gti/backend.h"

#include <algorithm>

namespace lang::native_llvm {

std::vector<SemanticType>
flattenConcreteParameters(std::span<const SemanticType> parameters) {
  std::vector<SemanticType> flattened;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    if (parameter.kind != SemanticType::TypePack) {
      flattened.push_back(parameter);
      continue;
    }
    if (!parameter.concretePack || index + 1 != parameters.size() ||
        std::any_of(parameter.arguments.begin(), parameter.arguments.end(),
                    [](const SemanticType &element) {
                      return element.kind == SemanticType::TypePack;
                    }))
      throw BackendUnsupportedError(
          "LLVM GTI ABI requires an exact trailing concrete parameter pack");
    flattened.insert(flattened.end(), parameter.arguments.begin(),
                     parameter.arguments.end());
  }
  return flattened;
}

} // namespace lang::native_llvm
