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
[[borrowed_from(owner)]] c_int& claim(c_int* address, c_int& owner) {
  unsafe { return *address; }
}
void transport() {
  c_int snapshot = unsafe { limit };
  auto scalar_address = &counter;
  auto pointer_slot_address = &pointer;
  auto field_address = &record.field;
  c_int witness = c_int(0);
  unsafe {
    c_int& checked = claim(pointer, witness);
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
void testConstNativeRecordAssignment() {
  for (const std::string &shape : std::vector<std::string>{
           "[[c_abi]] struct R { c_int value; };",
           "[[c_abi]] struct R { c_int values[2]; };",
           "[[c_abi]] struct Inner { c_int value; }; "
           "[[c_abi]] struct R { mut Inner inner; };",
           "[[c_abi]] struct Inner { c_int value; }; "
           "[[c_abi]] struct R { mut Inner values[2]; };",
       }) {
    const std::string globals =
        shape + "extern \"C\" { mut R a; mut R b; R fresh(); }";
    for (const std::string &operation : std::vector<std::string>{
             "void use() { unsafe { a = b; } }",
             "void use() { unsafe { a = fresh(); } }",
         }) {
      const auto result =
          analyze(globals + operation + "int main() { return 0; }");
      const bool assignmentError =
          std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2003" &&
                               diagnostic.message.find("Cannot assign") !=
                                   std::string::npos;
                      });
      expect(!result.canGenerateCode() && assignmentError,
             "readonly foreign subobjects forbid copy and move replacement",
             result);
    }
    const auto copy =
        analyze(globals + "void take(R value) {} void use() { unsafe { mut R "
                          "copy = a; take(a); copy = b; copy = fresh(); } }"
                          "int main() { return 0; }");
    expect(copy.canGenerateCode(),
           "readonly ABI fields preserve local replacement and value transport",
           copy);
  }
  const auto ordinary =
      analyze("struct Plain { int value; }; "
              "void use(mut Plain target, Plain source) { target = source; } "
              "int main() { return 0; }");
  expect(ordinary.canGenerateCode(),
         "ordinary GTI immutable fields retain replacement policy", ordinary);
}
} // namespace

int main() {
  testRejectedBorrows();
  testRawAccessAndWitness();
  testConstNativeRecordAssignment();
  return failures == 0 ? 0 : 1;
}
