#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/mir_backend.h"
#include "gti/native_header.h"

#include "cpp_backend_test_support.h"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lang {

struct LoweredProgramTestAccess {
  static std::vector<LoweredDeclaration> &
  declarations(LoweredProgram &program) {
    return program.declarations_;
  }

  static std::vector<LoweredGeneratedItem> &
  generatedItems(LoweredProgram &program) {
    return program.generatedItems_;
  }
};

} // namespace lang

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectRejected(lang::Backend &backend, const lang::LoweredProgram &program,
                    std::string_view mutation) {
  bool rejected = false;
  try {
    static_cast<void>(backend.generate(program));
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected, std::string(backend.name()) + " backend should reject the " +
                       std::string(mutation) + " lowered-program mutation");
}

void testBackendsRejectMalformedLoweredPrograms() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("backend-lowered-program-rejection.gti", R"(
[[c_abi]] struct NativeValue {
  int32_t value;
};

extern "C" {
  NativeValue native_identity(NativeValue value);
}

int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "the backend rejection fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::LoweredProgram original =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  lang::CppBackend cpp;
  lang::MirBackend mir;
  lang::NativeHeaderBackend nativeHeader;
  expect(!cpp.generate(original).contents.empty() &&
             !mir.generate(original).contents.empty() &&
             !nativeHeader.generate(original).contents.empty(),
         "all executable backend implementations should accept the same "
         "verified lowered program");

  lang::LoweredProgram staleSeal = original;
  auto &declarations = lang::LoweredProgramTestAccess::declarations(staleSeal);
  expect(!declarations.empty(),
         "the backend mutation fixture should contain declarations");
  if (!declarations.empty()) {
    declarations.front().name += "_forged";
    expectRejected(cpp, staleSeal, "stale construction seal");
    expectRejected(mir, staleSeal, "stale construction seal");
    expectRejected(nativeHeader, staleSeal, "stale construction seal");
  }

  lang::LoweredProgram missingGeneratedItem = original;
  auto &generated =
      lang::LoweredProgramTestAccess::generatedItems(missingGeneratedItem);
  expect(!generated.empty(),
         "the backend mutation fixture should contain generated items");
  if (!generated.empty()) {
    generated.pop_back();
    expectRejected(cpp, missingGeneratedItem, "missing generated item");
    expectRejected(mir, missingGeneratedItem, "missing generated item");
    expectRejected(nativeHeader, missingGeneratedItem,
                   "missing generated item");
  }
}

} // namespace

int main() {
  testBackendsRejectMalformedLoweredPrograms();
  if (failures != 0) {
    std::cerr << failures << " backend rejection test(s) failed\n";
    return 1;
  }
  std::cout << "backend lowered-program rejection tests passed\n";
  return 0;
}
