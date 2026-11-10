#include "gti/ast_printer.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
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

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void testBinary64Representation() {
  const lang::FrontendResult result = analyze(
      "binary64-pipeline.gti",
      "T floating_identity<std::floating_point T>(T value) { return value; }\n"
      "extern \"C\" { double native_scale(double value); }\n"
      "constexpr double tenth = 0.1d;\n"
      "constexpr double sum = tenth + 0.2D;\n"
      "constexpr double mixed = 1.0 + 0.25d;\n"
      "constexpr double widened = 0.1;\n"
      "constexpr double grouped = ((0.5d));\n"
      "constexpr float grouped32 = ((0.25));\n"
      "constexpr float narrowed = float(sum);\n"
      "constexpr int32_t truncated = int32_t(3.9d);\n"
      "constexpr uint64_t double_size = sizeof(double);\n"
      "constexpr uint64_t double_alignment = alignof(double);\n"
      "int main() {\n"
      "  double generic = floating_identity<double>(mixed);\n"
      "  double runtime = generic + 1;\n"
      "  return runtime == 2.25d && narrowed > 0.0 && truncated == 3 && "
      "double_size == 8 && double_alignment == 8 ? 0 : 1;\n"
      "}\n");
  expect(result.canGenerateCode(),
         "binary64 should compile through syntax, semantics, HIR, and MIR");
  if (result.canGenerateCode()) {
    const std::string generated = gti_test::emitCppText(result);
    expect(generated.find("std::bit_cast<double>(std::uint64_t{"
                          "0x3fb999999999999aULL})") != std::string::npos &&
               generated.find("static_assert(__gti_strict_ieee754 == 1") !=
                   std::string::npos &&
               generated.find("numeric_limits<double>::digits == 53") !=
                   std::string::npos,
           "the backend should emit retained binary64 bits under the strict "
           "IEEE policy");
  }
}

} // namespace

int main() {
  testBinary64Representation();
  if (failures != 0) {
    return 1;
  }
  std::cout << "binary64 C++ representation tests passed\n";
  return 0;
}
