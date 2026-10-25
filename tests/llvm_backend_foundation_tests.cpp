#include "gti/frontend.h"
#include "gti/llvm_backend.h"

#include "cpp_backend_test_support.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testFoundationConsumesVerifiedLoweredProgram() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("llvm-backend-foundation.gti", R"(
int add(int left, int right) { return left + right; }
int main() { return add(20, 22) - 42; }
)");
  expect(frontend.canGenerateCode(),
         "the LLVM backend fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::LlvmBackend backend(
      {.optimization = lang::LlvmCodegenOptimization::Default,
       .debugInfo = false});
  expect(backend.name() == "llvm",
         "the native backend should expose a stable driver identity");

  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto artifact = backend.generate(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir));
    expect(artifact.kind == lang::BackendArtifactKind::Object &&
               !artifact.contents.empty(),
           "a verified scalar program produces an independent native object");
  }
}

} // namespace

int main() {
  testFoundationConsumesVerifiedLoweredProgram();
  if (failures != 0) {
    std::cerr << failures << " LLVM backend foundation test(s) failed\n";
    return 1;
  }
  std::cout << "LLVM backend foundation tests passed\n";
  return 0;
}
