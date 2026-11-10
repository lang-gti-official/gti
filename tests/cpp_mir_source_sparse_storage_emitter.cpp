#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "private_stdlib_fixture.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

// Emits the source sparse-slot fixture as C++ for
// source_sparse_storage_runtime_test.py:
//   gti_cpp_mir_source_sparse_storage_emitter OUTPUT c++20|c++23 O0|O1|O3
int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: " << argv[0] << " OUTPUT STANDARD LEVEL\n";
    return 2;
  }
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream input(root / "tests/fixtures/source_sparse_slots.gti");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "source_sparse_slots.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto standard = std::string(argv[2]) == "c++20"
                            ? lang::CppStandard::Cpp20
                            : lang::CppStandard::Cpp23;
  const auto level = std::string(argv[3]) == "O0" ? lang::OptimizationLevel::O0
                     : std::string(argv[3]) == "O1"
                         ? lang::OptimizationLevel::O1
                         : lang::OptimizationLevel::O3;
  std::ofstream output(argv[1]);
  output << gti_test::emitCppText(frontend, standard, lang::TargetInfo::host(),
                                  level);
  return output ? 0 : 1;
}
