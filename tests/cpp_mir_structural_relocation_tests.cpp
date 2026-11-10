#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"

// C++ helper spelling of typed source-storage relocation. The
// backend-neutral contract lives in structural_relocation_tests.cpp.
int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/structural_relocation.gti";
  std::ifstream input(path);
  expect(input.good(), "structural relocation fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "structural concepts and public relocation analyze");
  if (!frontend.canGenerateCode())
    return 1;
  const auto cpp = gti_test::emitCppText(frontend);
  expect(cpp.find("mir_storage_relocate_v1") == std::string::npos &&
             cpp.find("::gti_internal::backend::mir_object_relocate_v1(") !=
                 std::string::npos &&
             cpp.find("mir_prefix_relocate_v1") == std::string::npos,
         "sparse and prefix source storage both use general typed relocation "
         "without a private prefix fallback");
  return failures == 0 ? 0 : 1;
}
