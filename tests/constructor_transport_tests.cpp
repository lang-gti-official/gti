#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <string_view>

int main() {
  int failures = 0;
  const auto check = [&](std::string_view source, bool valid,
                         std::string_view code) {
    auto result = lang::Frontend().analyze("constructor-transport.gti",
                                           std::string(source));
    const bool diagnosed =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &d) {
                      return d.code == code && d.primary.end > d.primary.start;
                    });
    if (result.canGenerateCode() != valid || (!valid && !diagnosed)) {
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      ++failures;
    }
    return result;
  };
  auto result = check(R"gti(
struct Box {
  int value = 4;
  Box() {}
  Box<Other>(Other source, int policy = 0) {}
};
int main() {
  Box original{};
  Box copy{original};
  Box converted{7};
  return copy.value - converted.value;
}
)gti",
                      true, {});
  if (result.canGenerateCode()) {
    bool copied = false, converted = false;
    for (const auto &function : result.hir.functionInstances()) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      for (const auto &value : function.body.values) {
        if (!value.source)
          continue;
        const auto *construction =
            result.semantics.findConstruction(*value.source);
        if (!construction)
          continue;
        copied |= construction->kind == lang::ConstructorKind::Copy &&
                  construction->typeArguments.empty();
        converted |=
            construction->kind == lang::ConstructorKind::Ordinary &&
            construction->typeArguments ==
                std::vector<lang::SemanticType>{lang::SemanticType::Int32};
      }
    }
    if (!copied || !converted) {
      std::cerr << "same-owner copy or unrelated conversion lost its selected "
                   "semantic identity\n";
      ++failures;
    }
  }
  check(R"gti(
struct Box {
  int value = 4;
  Box() {}
  Box(Box& source) = delete;
  Box<Other>(Other source) {}
};
int main() { Box original{}; Box forbidden{original}; return 0; }
)gti",
        false, "GTI-S2020");
  check(R"gti(
class Box {
  int value = 4;
  Box(Box& source) {}
public:
  Box() {}
  Box<Other>(Other source) {}
};
int main() { Box original{}; Box forbidden{original}; return 0; }
)gti",
        false, "GTI-S2007");
  return failures ? 1 : 0;
}
