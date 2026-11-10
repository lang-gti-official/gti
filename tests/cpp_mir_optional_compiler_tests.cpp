// Frozen C++-backend coverage moved from optional_compiler_tests.cpp: native
// emission of a fallible optional reset, which the LLVM backend does not yet
// lower (O-LLVM-06).
#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

int main() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend =
      lang::Frontend().analyze("optional-reset-cleanup.gti", R"(
import std.optional;
class Resource {
  int divisor;
public:
  Resource(int divisor) : divisor(divisor) {}
  ~Resource() { int checked = 12 / this.divisor; }
};
void destroy(int divisor) {
  mut std::optional<Resource> slot{Resource(divisor)};
  slot.reset();
}
)",
                               {library / "prelude.gti"}, {}, {library});
  expect(frontend.canGenerateCode(),
         "fallible payload reset must have independently verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const auto artifact = gti_test::emitCpp(frontend);
  bool ordinaryReset = !artifact.contents.empty();
  for (const auto &instance : frontend.mir.classInstances())
    ordinaryReset &= instance.compilerCapability !=
                     lang::CompilerCapabilityTypeKind::OptionalStorage;
  expect(ordinaryReset, "source reset emits without any instantiated private "
                        "optional storage or operation");
  return failures == 0 ? 0 : 1;
}
