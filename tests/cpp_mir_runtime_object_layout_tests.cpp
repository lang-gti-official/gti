#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

// C++ spelling of runtime object-layout queries in optimized verified-MIR
// bodies. The backend-neutral contract lives in runtime_object_layout_tests.
namespace {
int failures = 0;
void expect(bool condition, std::string_view name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}
} // namespace

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream input(root / "examples/119-runtime-object-layout.gti");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = lang::Frontend().analyze(
      root / "object-layout-test.gti", source, {root / "stdlib/prelude.gti"},
      {}, {root / "stdlib"});
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "owning, immovable, nested and generic queries compile");
  if (!frontend.canGenerateCode())
    return 1;
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    const auto cpp = gti_test::emitCppText(frontend, lang::CppStandard::Cpp23,
                                           lang::TargetInfo::host(), level);
    expect(cpp.find("// GTI verified-MIR body:") != std::string::npos &&
               cpp.find("sizeof(") != std::string::npos &&
               cpp.find("alignof(") != std::string::npos,
           "optimized detached MIR emits representation queries");
  }
  return failures == 0 ? 0 : 1;
}
