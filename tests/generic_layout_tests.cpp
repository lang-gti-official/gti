#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(std::string_view source, bool valid, std::string_view label,
           std::string_view code = {}) {
  auto result =
      lang::Frontend().analyze("generic-layout.gti", std::string(source));
  const bool diagnostic = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.code == code && d.primary.end > d.primary.start &&
               d.primary.line != 0;
      });
  if (result.canGenerateCode() != valid ||
      (!valid && !code.empty() && !diagnostic)) {
    std::cerr << "FAIL " << label << '\n';
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    ++failures;
  }
}
} // namespace

int main() {
  constexpr std::string_view layers = R"gti(
namespace shapes {
struct Layer<uint64_t N> { Layer<N - 1> inner; int32_t value = 1; };
}
struct shapes::Layer<0> { int32_t value = 100; };
)gti";
  check(std::string(layers) + R"gti(
int main() { shapes::Layer<2> stack{}; return stack.inner.inner.value - 100; }
)gti",
        true,
        "finite concrete value recursion reaches its exact specialization");
  check(std::string(layers) + R"gti(
int main() { shapes::Layer<63> stack{}; return stack.value - 1; }
)gti",
        true, "finite concrete layout at the positive depth boundary");
  check(std::string(layers) + R"gti(
using Previous<uint64_t N> = shapes::Layer<N - 1>;
struct Box<uint64_t N = 2> { Previous<N> inner; int[N + 1] values = {}; };
int main() { Box<> object{}; return object.inner.inner.value - 100; }
)gti",
        true,
        "aliases, generic defaults and field extents retain concrete identity");
  check(
      R"gti(
struct Inner { int32_t value = 17; };
struct Outer { Inner inner; };
int main() { Outer object{}; return object.inner.value - 17; }
)gti",
      true,
      "unrelated omitted nominal field receives checked default construction");
  check(R"gti(
class Inner { int field; public: Inner(int value) : field(value) {} };
class Outer { public: Outer(Inner value) : inner(value) {} private: Inner inner; };
int main() { Inner value{3}; Outer object{value}; return 0; }
)gti",
        true,
        "explicit field construction still works without a zero-argument "
        "operation");
  check(R"gti(
class Inner { int field; public: Inner(int value) : field(value) {} };
struct Outer { Inner inner; };
int main() { Outer object{}; return 0; }
)gti",
        false,
        "a missing zero-argument operation is not a synthetic empty state",
        "GTI-S2012");
  check(R"gti(
struct Outer { int32_t value; };
int main() { Outer object{}; return 0; }
)gti",
        false,
        "omitted scalar fields do not acquire implicit zero initialization");
  check(R"gti(
struct Node { Node next; };
int main() { Node object{}; return 0; }
)gti",
        false, "a genuine by-value cycle remains invalid", "GTI-S2082");
  check(R"gti(
struct Growing<uint64_t N> { Growing<N + 1> next; int value = 1; };
int main() { Growing<0> object{}; return 0; }
)gti",
        false,
        "unbounded increasing concrete layouts receive a located resource "
        "diagnostic",
        "GTI-S2105");
  check(R"gti(
struct Underflow<uint64_t N> { Underflow<N - 1> next; int value = 1; };
int main() { Underflow<0> object{}; return 0; }
)gti",
        false, "closed substituted arithmetic remains checked", "GTI-S2026");
  std::optional<lang::CompileTimeValue> retained;
  {
    auto snapshot = lang::Frontend().analyze(
        "owned-expression.gti",
        std::string(layers) +
            "int main() { shapes::Layer<2> value{}; return value.value - 1; }");
    for (const auto &instance : snapshot.hir.classes) {
      const auto *owner =
          snapshot.semantics.findClassType(instance.declaration);
      if (owner != nullptr && !owner->fields.empty() &&
          !owner->fields.front().type.valueArguments.empty() &&
          owner->fields.front().type.valueArguments.front().expression)
        retained = owner->fields.front().type.valueArguments.front();
    }
  }
  if (!retained || !retained->expression || !retained->expression->left ||
      lang::substituteCompileTimeValue(
          *retained, {{retained->expression->left->parameterId,
                       lang::CompileTimeValue::uint64(2)}}) !=
          lang::CompileTimeValue::uint64(1)) {
    std::cerr
        << "FAIL owned expression outlives the Frontend and Program snapshot\n";
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "generic layout tests passed\n";
  return 0;
}
