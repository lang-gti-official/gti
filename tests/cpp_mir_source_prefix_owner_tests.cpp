#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "private_stdlib_fixture.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

// C++ spelling of source prefix-owner cleanup recipes. The backend-neutral
// contract lives in source_prefix_owner_tests.cpp.
int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      gti_test::analyzePrivateMemoryFixture("source-prefix-contract.gti", R"gti(
import std.memory;
int main() {
  mut std::detail::prefix_owner<int> source{uint64_t(4)};
  source.append_value(3);
  source.append_value(7);
  mut std::detail::prefix_owner<int> destination{uint64_t(8)};
  source.relocate_to(destination);
  destination.insert(uint64_t(1), 5);
  destination.erase(uint64_t(0));
  return destination.read(uint64_t(0)) == 5 ? 0 : 1;
}
)gti");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto cpp = gti_test::emitCppText(frontend);
  if (cpp.find("// GTI verified-MIR body:") == std::string::npos)
    return 1;
  const auto hosted = lang::Frontend().analyze(
      root / "source-prefix-hosted-contract.gti", R"gti(
import std.string;
import std.vector;
int main(int argc, std::vector<std::string> argv) { return argc; }
)gti",
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!hosted.canGenerateCode()) {
    std::cerr << "source prefix hosted argument contract did not lower\n";
    return 1;
  }
  const auto hostedText = gti_test::emitCppText(hosted);
  if (hostedText.find("__gti_cleanup_context(&__gti_failure_record") ==
          std::string::npos ||
      hostedText.find(
          "__gti_arguments).template __gti_mir_unique_cleanup_failure<") ==
          std::string::npos) {
    std::cerr << "hosted argument cleanup did not consume its MIR recipe\n";
    return 1;
  }
  const auto forwardedPath =
      root / "tests/fixtures/source_prefix_forwarded_result_cleanup.gti";
  std::ifstream forwardedInput(forwardedPath);
  const std::string forwardedSource{
      std::istreambuf_iterator<char>(forwardedInput), {}};
  const auto forwarded = gti_test::analyzePrivateMemoryFixture(
      forwardedPath.string(), forwardedSource);
  if (!forwarded.canGenerateCode() ||
      gti_test::emitCppText(forwarded).find(
          "GTI MIR unpublished return cleanup") == std::string::npos) {
    std::cerr
        << "forwarded expected result did not retain unpublished cleanup\n";
    return 1;
  }
  const auto payloadPath =
      root / "tests/fixtures/source_prefix_payload_return_cleanup.gti";
  std::ifstream payloadInput(payloadPath);
  const std::string payloadSource{std::istreambuf_iterator<char>(payloadInput),
                                  {}};
  const auto payload = gti_test::analyzePrivateMemoryFixture(
      payloadPath.string(), payloadSource);
  if (!payload.canGenerateCode() ||
      gti_test::emitCppText(payload).find(
          "GTI MIR unpublished return cleanup") == std::string::npos) {
    std::cerr << "owning enum return lost its unpublished cleanup recipe\n";
    return 1;
  }
  std::cout << "source prefix C++ cleanup spelling: passed\n";
  return 0;
}
