#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(const std::string &source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("qualified_type_members.gti", source,
                                {library / "prelude.gti"}, {}, {library});
}

void reject(const std::string &source, std::string_view code,
            std::string_view message) {
  const auto result = analyze(source);
  check(result.syntaxValid, "invalid member types have valid type-path syntax");
  check(!result.semanticValid && !result.canGenerateCode(), message);
  check(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &item) { return item.code == code; }),
        "invalid type path has a frontend diagnostic");
}
}

int main() {
  const std::string source = R"(
namespace user {
struct Inner<T> { using value_type = T; };
struct Box<T = int> {
  using value_type = T;
  using nested = Inner<T>;
};
struct Fixed<T, uint64_t N> { using array_type = T[N]; };
}
using Value = user::Box<int>::nested::value_type;
using Array = user::Fixed<int, 2>::array_type;
user::Box<T>::value_type identity<T>(user::Box<T>::value_type value) {
  return value;
}
int main() {
  Value value = 7;
  user::Box<>::value_type defaulted = 1;
  Array items = {2, 3};
  user::Box<uint64_t>::value_type wide = uint64_t(9);
  return identity<int>(value) + defaulted + items[0] - 10;
}
)";
  const auto valid = analyze(source);
  if (!valid.canGenerateCode()) {
    for (const auto &item : valid.diagnostics) {
      std::cerr << item.code << ": " << item.message << '\n';
    }
  }
  check(valid.canGenerateCode(),
        "qualified aliases preserve namespaces, nesting, defaults and value generics through MIR");

  reject("class Box<T> { using hidden = T; }; using Bad = Box<int>::hidden;",
         "GTI-S2007", "private member aliases stay private");
  reject("struct Inner<T> { using value_type = T; }; class Box<T> { using hidden = Inner<T>; }; "
         "using Bad = Box<int>::hidden::value_type;",
         "GTI-S2007", "private intermediate aliases cannot leak public members");
  reject("struct Box<T> { int value = 0; }; using Bad = Box<int>::value;",
         "GTI-S2030", "a value member cannot be used as a type");
  reject("struct Box<T> {}; using Bad = Box<int>::missing;",
         "GTI-S2030", "missing member aliases stop in semantics");
  reject("struct Box<T> { using scalar = int; }; using Bad = Box<int>::scalar::missing;",
         "GTI-S2030", "a primitive alias is not a class-like qualifier");
  reject("struct Box<T> { using value_type = T; }; using Bad = Box<int>::value_type<int>;",
         "GTI-S2030", "member aliases are not alias templates");
  reject("struct Box<T> { using recursive = Box<T>::recursive; };",
         "GTI-S2030", "qualified alias cycles are diagnosed");

  const auto constrained = analyze(R"(
struct Box<std::integral T> { using value_type = T; };
using Bad = Box<double>::value_type;
)");
  check(!constrained.semanticValid,
        "an owner constraint cannot be bypassed through a type member");
  const auto arity = analyze("struct Box<T> { using value_type = T; }; using Bad = Box<int, bool>::value_type;");
  check(!arity.semanticValid, "specialized owners retain exact generic arity");

  const std::string completionSource = R"(
class Box<T> {
  using value_hidden = T;
public:
  using value_type = T;
};
using Selected = Box<uint64_t>::val;
)";
  const auto completion = lang::LanguageQueries().complete(
      {.entryPath = "qualified_completion.gti",
       .source = completionSource,
       .byteOffset = completionSource.rfind("val") + 3});
  check(std::any_of(completion.candidates.begin(), completion.candidates.end(),
                    [](const auto &candidate) {
                      return candidate.label == "value_type" &&
                             candidate.detail == "using Box<uint64_t>::value_type = uint64_t";
                    }),
        "completion substitutes the written owner and alias target");
  check(std::none_of(completion.candidates.begin(), completion.candidates.end(),
                     [](const auto &candidate) { return candidate.label == "value_hidden"; }),
        "completion filters private member aliases");

  std::cout << "Qualified type member tests: " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
