#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("foreign-borrow.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}
void expect(bool condition, std::string_view message,
            const lang::FrontendResult &result) {
  if (condition)
    return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
const std::string declarations = R"(
[[c_abi]] struct Record { mut c_int field; mut c_int* pointer; };
extern "C" { mut c_int counter; c_int limit; mut c_int* pointer; mut Record record; }
)";

void testRejectedBorrows() {
  for (
      const std::string &body : std::vector<std::string>{
          "c_int& leak() { unsafe { return counter; } }",
          "c_int& leak() { unsafe { return limit; } }",
          "mut c_int& leak() { unsafe { return counter; } }",
          "void take(c_int& value) {} void use() { unsafe { take(counter); } }",
          "void take(mut c_int& value) {} void use() { unsafe { take(counter); "
          "} }",
          "void take(Record& value) {} void use() { unsafe { take(record); } }",
          "void take(c_int& value) {} void use() { unsafe { "
          "take(record.field); } }",
          "void take(c_int& value) {} void use() { auto fn = take; unsafe { "
          "fn(counter); } }",
          "void take<T>(T& value) {} void use() { unsafe { take(counter); } }",
          "void use() { unsafe { c_int& value = record.field; } }",
          "struct View { c_int& value; View(c_int& source) : value(source) {} "
          "}; void use() { unsafe { View view{counter}; } }",
      }) {
    const auto result =
        analyze(declarations + body + "\nint main() { return 0; }");
    const bool foreignBorrow =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const auto &diagnostic) {
                      return diagnostic.code == "GTI-S2054" &&
                             diagnostic.message.find("checked reference") !=
                                 std::string::npos;
                    });
    expect(!result.canGenerateCode() && foreignBorrow, body, result);
  }
}

void testRawAccessAndWitness() {
  const auto result = analyze(declarations + R"(
[[borrowed_from(owner)]] c_int& restore(c_int& owner) {
  unsafe { return *pointer; }
}
void transport() {
  auto scalar_address = &counter;
  auto pointer_slot_address = &pointer;
  auto field_address = &record.field;
  unsafe {
    auto pointee_address = &*pointer;
    auto indexed_address = &pointer[0];
    auto field_pointee_address = &record.pointer[0];
    c_int loaded = *pointer;
    Record snapshot = record;
  }
}
int main() { return 0; }
)");
  expect(result.canGenerateCode(),
         "raw access must preserve native-slot and explicit owner-witness "
         "contracts",
         result);
}
} // namespace

int main() {
  testRejectedBorrows();
  testRawAccessAndWitness();
  return failures == 0 ? 0 : 1;
}
