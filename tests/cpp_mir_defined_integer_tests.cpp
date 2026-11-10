#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
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

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void testPrivateHelperLowering() {
  const std::string source = R"(
import std.numeric;

constexpr int8_t wrap_i8 = std::wrapping_add(int8_t(127), int8_t(1));
constexpr int16_t wrap_i16 = std::wrapping_add(int16_t(32767), int16_t(1));
constexpr int32_t wrap_i32 = std::wrapping_add(int32_t(2147483647), int32_t(1));
constexpr int64_t wrap_i64 = std::wrapping_add(int64_t(9223372036854775807), int64_t(1));
constexpr uint8_t wrap_u8 = std::wrapping_add(uint8_t(255), uint8_t(1));
constexpr uint16_t wrap_u16 = std::wrapping_add(uint16_t(65535), uint16_t(1));
constexpr uint32_t wrap_u32 = std::wrapping_add(uint32_t(4294967295), uint32_t(1));
constexpr uint64_t wrap_u64 = std::wrapping_add(uint64_t(18446744073709551615), uint64_t(1));

constexpr int8_t wrap_sub = std::wrapping_sub(int8_t(-128), int8_t(1));
constexpr int8_t wrap_mul = std::wrapping_mul(int8_t(100), int8_t(2));
constexpr int8_t sat_add = std::saturating_add(int8_t(120), int8_t(20));
constexpr int8_t sat_sub = std::saturating_sub(int8_t(-120), int8_t(20));
constexpr int8_t sat_mul = std::saturating_mul(int8_t(-100), int8_t(2));
constexpr expected<int8_t, std::arithmetic_errc> checked_fit =
    std::checked_add(int8_t(40), int8_t(2));
constexpr expected<int8_t, std::arithmetic_errc> checked_overflow =
    std::checked_add(int8_t(127), int8_t(1));
constexpr bool checked_fit_has_value = checked_fit.has_value();
constexpr bool checked_overflow_has_value = checked_overflow.has_value();
constexpr int8_t checked_recovered = checked_overflow.value_or(int8_t(9));
constexpr int8_t checked_difference =
    std::checked_sub(int8_t(5), int8_t(3)).value_or(int8_t(0));

int8_t runtime_wrap(int8_t left, int8_t right) {
  int8_t added = std::wrapping_add(left, right);
  int8_t subtracted = std::wrapping_sub(added, right);
  return std::wrapping_mul(subtracted, int8_t(1));
}

int8_t runtime_saturate(int8_t left, int8_t right) {
  int8_t added = std::saturating_add(left, right);
  int8_t subtracted = std::saturating_sub(added, right);
  return std::saturating_mul(subtracted, int8_t(2));
}

expected<int8_t, std::arithmetic_errc> runtime_checked(
    int8_t left, int8_t right) {
  return std::checked_mul(left, right);
}

int main() {
  expected<int8_t, std::arithmetic_errc> checked =
      runtime_checked(int8_t(100), int8_t(2));
  return runtime_wrap(int8_t(127), int8_t(1)) == int8_t(-128) and
                 runtime_saturate(int8_t(100), int8_t(40)) == int8_t(127) and
                 !checked.has_value() and
                 checked.error() ==
                     std::arithmetic_errc::result_out_of_range
             ? 0
             : 1;
}
)";
  lang::FrontendResult result = analyze("defined-integer-valid.gti", source);
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode(),
         "public wrapping/saturating source should lower completely");

  if (result.canGenerateCode()) {
    const std::string generated = gti_test::emitCppText(result);
    for (const std::string_view helper :
         {"wrapping_add", "wrapping_sub", "wrapping_mul", "saturating_add",
          "saturating_sub", "saturating_mul", "integer_add_overflow_flag",
          "integer_subtract_overflow_flag", "integer_multiply_overflow_flag"}) {
      expect(generated.find("::gti_internal::backend::" + std::string(helper) +
                            "(") != std::string::npos,
             "production MIR emission should lower each ordinary std wrapper "
             "through the selected private helper");
    }
  }
}

void testHelpersRequireExpectedSurface() {
  const lang::FrontendResult result =
      analyze("defined-integer-unrelated.gti", "int main() { return 0; }");
  expect(result.canGenerateCode(),
         "an unrelated program should remain valid without expected support");
  for (const lang::CppStandard standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const std::string generated = gti_test::emitCppText(result, standard);
    expect(generated.find("#include <expected>") == std::string::npos &&
               generated.find("#include <nonstd/expected.hpp>") ==
                   std::string::npos &&
               generated.find("using expected_result =") == std::string::npos &&
               generated.find("inline constexpr expected_result<") ==
                   std::string::npos,
           "programs without expected values should not emit checked-result "
           "helpers or their native dependency");
  }
}

} // namespace

int main() {
  testPrivateHelperLowering();
  testHelpersRequireExpectedSurface();
  if (failures != 0) {
    std::cerr << failures << " defined integer C++ test(s) failed\n";
    return 1;
  }
  std::cout << "All defined integer C++ representation tests passed\n";
  return 0;
}
