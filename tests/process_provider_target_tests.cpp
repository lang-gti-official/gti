#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <filesystem>
#include <iostream>
#include <string>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  for (const auto *os : {"macos", "linux", "windows"}) {
    lang::FrontendOptions options;
    options.target.os = os;
    options.target.vendor = std::string(os) == "macos" ? "apple" : "pc";
    options.target.arch = "x86_64";
    const auto result = lang::Frontend(options).analyze(
        root / "tests/fixtures/process_resource_policy.gti", std::nullopt,
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    if (!result.canGenerateCode()) {
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 1;
    }
    const auto text =
        gti_test::emitCppText(result, lang::CppStandard::Cpp23, options.target);
    const bool windows = std::string(os) == "windows";
    const auto selected =
        windows ? "gti_rt_aligned_malloc_v1" : "gti_rt_posix_memalign_v1";
    const auto absent =
        windows ? "gti_rt_posix_memalign_v1" : "gti_rt_aligned_malloc_v1";
    if (text.find(selected) == std::string::npos ||
        text.find(absent) != std::string::npos ||
        text.find("gti_rt_memory_allocate_v1") != std::string::npos) {
      std::cerr << "Wrong source provider selection for " << os << '\n';
      return 2;
    }
  }
  return 0;
}
