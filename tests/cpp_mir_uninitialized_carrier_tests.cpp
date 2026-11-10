#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

// C++ helper spelling of bulk raw relocation. The backend-neutral contract
// lives in uninitialized_carrier_tests.cpp.
namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "uninitialized-carrier-test.gti";
  std::unordered_map<std::string, std::string> overrides{
      {std::filesystem::weakly_canonical(entry).string(), source}};
  return lang::Frontend().analyze(entry, source, {root / "stdlib/prelude.gti"},
                                  overrides, {root / "stdlib"});
}
} // namespace

int main() {
  const std::string prefix = R"(
import std.memory;
import std.string;
struct Plain { int value = 8;  public: Plain() = default; };
class Stationary {
public:
  Stationary(int value) {}
  Stationary(Stationary& other) = delete;
  Stationary(Stationary&& other) = delete;
public:
  Stationary() = default;
};
class Holder<std::layout_queryable T> {
  mut std::uninitialized<T> bytes{};
public:
  Holder() {}
};
)";
  const auto range = analyze(prefix + R"(
void transfer(Plain* destination, Plain* source, std::size_t count) {
  unsafe { std::relocate_range_at(destination, source, count); }
}
int main() { transfer(nullptr, nullptr, std::size_t(0)); return 0; }
)");
  expect(range.canGenerateCode(),
         "bitwise range relocation forms verified MIR");
  if (range.canGenerateCode()) {
    const auto emitted = gti_test::emitCppText(range);
    expect(emitted.find(
               "::gti_internal::backend::mir_object_relocate_range_v1(") !=
               std::string::npos,
           "production MIR emitter consumes bulk relocation");
  }
  return failures == 0 ? 0 : 1;
}
