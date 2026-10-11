#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto stdlib = root / "stdlib";
  for (const std::string os : {"linux", "macos", "windows", "unknown"}) {
    lang::FrontendOptions options;
    options.target = lang::TargetInfo::host();
    options.target.os = os;
    const auto result =
        lang::Frontend(std::move(options))
            .analyze(
                root / "tests/fixtures/environment-target.gti",
                std::string{
                    "#include <std/environment>\nint main() { return 0; }\n"},
                {stdlib / "prelude.gti"}, {}, {stdlib});
    const bool supported = os == "linux" || os == "macos";
    if (result.canGenerateCode() != supported) {
      std::cerr << "wrong environment target availability for " << os << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 1;
    }
    if (!supported) {
      const std::string expected =
          os == "windows"
              ? "std/environment is not yet available on Windows."
              : "std/environment requires a supported POSIX target.";
      if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto &diagnostic) {
                         return diagnostic.message.find(expected) !=
                                std::string::npos;
                       })) {
        std::cerr << "missing explicit environment target diagnostic for " << os
                  << '\n';
        return 1;
      }
    }
  }
  return 0;
}
