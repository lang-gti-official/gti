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
} // namespace

int main() {
  const std::string source = R"(
namespace user {
struct Inner<T> { using value_type = T;  public: Inner() = default; };
struct Box<T = int> {
  using value_type = T;
  using nested = Inner<T>;
public:
  Box() = default;
};
struct Fixed<T, uint64_t N> { using array_type = T[N];  public: Fixed() = default; };
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
        "qualified aliases preserve namespaces, nesting, defaults and value "
        "generics through MIR");

  reject(R"(
namespace user { struct Select<T> { using value_type = int;  public: Select() = default; }; }
struct user::Select<bool> { using value_type = uint64_t;  public: Select() = default; };
)",
         "GTI-S2030",
         "exact-definition aliases retain their existing restriction");
  const auto defaults = analyze(R"(
struct Box<T> { using value_type = T;  public: Box() = default; };
struct Later<T = Box<U>::value_type, U = int> { public: Later() = default; };
)");
  check(!defaults.semanticValid &&
            std::any_of(
                defaults.diagnostics.begin(), defaults.diagnostics.end(),
                [](const auto &item) { return item.code == "GTI-S2023"; }),
        "owner arguments cannot hide references to later default parameters");

  const auto moveOnly = analyze(R"(
import std.memory;
struct Box<T> { using value_type = T;  public: Box() = default; };
int main() {
  Box<std::unique_ptr<int>>::value_type first = std::make_unique<int>(7);
  Box<std::unique_ptr<int>>::value_type second = first;
  return 0;
}
)");
  check(!moveOnly.semanticValid && !moveOnly.canGenerateCode() &&
            std::any_of(
                moveOnly.diagnostics.begin(), moveOnly.diagnostics.end(),
                [](const auto &item) { return item.code == "GTI-S2003"; }),
        "type aliases cannot make a unique owner copyable");
  const std::string borrowedSource = R"(
import std.vector;
int main() {
  mut std::vector<int> values{};
  values.push_back(1);
  std::vector<int>::const_iterator cursor = values.begin();
  values.push_back(2);
  return *cursor;
}
)";
  const auto borrowed = analyze(borrowedSource);
  const auto mutation = borrowedSource.find("values.push_back(2)") +
                        std::string_view("values.push_back(2").size();
  check(!borrowed.semanticValid && !borrowed.canGenerateCode() &&
            borrowed.diagnostics.size() == 1 &&
            borrowed.diagnostics.front().code == "GTI-S2017" &&
            borrowed.diagnostics.front().primary.start == mutation &&
            borrowed.diagnostics.front().primary.end == mutation + 1 &&
            borrowed.diagnostics.front().related.size() == 2,
        "public iterator aliases reject owner mutation at the actual call, "
        "with borrow-origin and later-use locations");
  const auto wrapper = analyze(R"(
import std.vector;
struct CursorTraits<std::vector_element T> {
  using iterator = std::vector<T>::const_iterator;
  using sentinel = std::vector<T>::sentinel;
public:
  CursorTraits() = default;
};
int main() {
  mut std::vector<int> values{};
  values.push_back(7);
  CursorTraits<int>::iterator cursor = values.begin();
  CursorTraits<int>::sentinel finish = values.end();
  if (cursor != finish) { return *cursor - 7; }
  return 1;
}
)");
  check(wrapper.canGenerateCode(),
        "user generic classes can alias public specialized iterator types");

  const lang::Formatter formatter;
  const auto formatted = formatter.format(source);
  check(analyze(formatted).canGenerateCode() &&
            formatter.format(formatted) == formatted &&
            formatted.find("user::Box<T>::value_type") != std::string::npos,
        "qualified type paths survive idempotent formatting and reanalysis");
  const auto unit = valid.sourceGraph.entryUnit();
  const auto offset = source.find("Box<int>::nested::value_type") +
                      std::string("Box<int>::nested::").size();
  const lang::LanguageQueries queries;
  const auto definition = queries.definition(valid, unit, offset);
  check(definition && definition->target.start == source.find("value_type = T"),
        "nested type members navigate to the original alias declaration");

  const std::string missing = "struct Box<T> { public: Box() = default; }; "
                              "using Bad = Box<int>::missing;";
  const auto missingResult = analyze(missing);
  check(std::any_of(missingResult.diagnostics.begin(),
                    missingResult.diagnostics.end(),
                    [&](const auto &item) {
                      return item.code == "GTI-S2030" &&
                             item.message.find("no direct type alias") !=
                                 std::string::npos &&
                             item.primary.start == missing.find("missing") &&
                             item.primary.end == missing.find("missing") + 7;
                    }),
        "missing aliases highlight the member name rather than its owner");
  const auto recovered =
      analyze("struct Box<T> { using value_type = T; }; "
              "using Bad = Box<int>::; int following() { return 1; }");
  check(!recovered.syntaxValid && !recovered.canGenerateCode() &&
            std::any_of(recovered.program.declarations().begin(),
                        recovered.program.declarations().end(),
                        [](const auto &statement) {
                          const auto *function =
                              lang::stmtAs<lang::FunctionDecl>(statement.get());
                          return function &&
                                 function->name().lexeme == "following";
                        }),
        "incomplete qualified paths recover without entering the backend");
  std::string tooDeep = "using Bad = Box<int>";
  for (int index = 0; index < 600; ++index) {
    tooDeep += "::next";
  }
  tooDeep += ";";
  const auto bounded = analyze(tooDeep);
  check(!bounded.syntaxValid && !bounded.canGenerateCode(),
        "qualified type chains retain the parser resource bound");

  reject("class Box<T> { using hidden = T;  public: Box() = default; }; using "
         "Bad = Box<int>::hidden;",
         "GTI-S2007", "private member aliases stay private");
  reject("struct Inner<T> { using value_type = T;  public: Inner() = default; "
         "}; class Box<T> { using "
         "hidden = Inner<T>;  public: Box() = default; }; "
         "using Bad = Box<int>::hidden::value_type;",
         "GTI-S2007",
         "private intermediate aliases cannot leak public members");
  reject("struct Box<T> { int value = 0;  public: Box() = default; }; using "
         "Bad = Box<int>::value;",
         "GTI-S2030", "a value member cannot be used as a type");
  reject("struct Box<T> { public: Box() = default; }; using Bad = "
         "Box<int>::missing;",
         "GTI-S2030", "missing member aliases stop in semantics");
  reject("struct Box<T> { using scalar = int;  public: Box() = default; }; "
         "using Bad = "
         "Box<int>::scalar::missing;",
         "GTI-S2030", "a primitive alias is not a class-like qualifier");
  reject("struct Box<T> { using value_type = T;  public: Box() = default; }; "
         "using Bad = "
         "Box<int>::value_type<int>;",
         "GTI-S2030", "member aliases are not alias templates");
  reject("struct Box<T> { using recursive = Box<T>::recursive;  public: Box() "
         "= default; };",
         "GTI-S2030", "qualified alias cycles are diagnosed");

  const auto constrained = analyze(R"(
struct Box<std::integral T> { using value_type = T;  public: Box() = default; };
using Bad = Box<double>::value_type;
)");
  check(!constrained.semanticValid,
        "an owner constraint cannot be bypassed through a type member");
  const auto arity = analyze("struct Box<T> { using value_type = T;  public: "
                             "Box() = default; }; using "
                             "Bad = Box<int, bool>::value_type;");
  check(!arity.semanticValid, "specialized owners retain exact generic arity");

  const std::string completionSource = R"(
class Box<T> {
  using value_hidden = T;
public:
  using value_type = T;
public:
  Box() = default;
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
                             candidate.detail ==
                                 "using Box<uint64_t>::value_type = uint64_t";
                    }),
        "completion substitutes the written owner and alias target");
  check(std::none_of(completion.candidates.begin(), completion.candidates.end(),
                     [](const auto &candidate) {
                       return candidate.label == "value_hidden";
                     }),
        "completion filters private member aliases");
  const std::string staticCompletionSource =
      "struct Box<T> { static int flag = 0;  public: Box() = default; }; "
      "int main() { return Box<int>::fl; }";
  const auto staticCompletion = lang::LanguageQueries().complete(
      {.entryPath = "qualified_static_completion.gti",
       .source = staticCompletionSource,
       .byteOffset = staticCompletionSource.rfind("fl") + 2});
  check(std::any_of(
            staticCompletion.candidates.begin(),
            staticCompletion.candidates.end(),
            [](const auto &candidate) { return candidate.label == "flag"; }),
        "type-member completion does not hide existing expression static "
        "members");

  std::cout << "Qualified type member tests: " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
