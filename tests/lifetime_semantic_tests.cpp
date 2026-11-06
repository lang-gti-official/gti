#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
  const bool mir = argc > 1 && std::string_view(argv[1]) == "--mir";
  int failures = 0;
  const auto check = [&](std::string_view name, std::string_view source,
                         bool accepted, std::string_view code = {}) {
    const auto result = lang::Frontend(
        {.stopAfter = mir ? lang::FrontendPhase::Mir : lang::FrontendPhase::Semantics})
                            .analyze(std::string(name) + ".gti", std::string(source));
    const bool passed = mir ? result.canGenerateCode() : result.semanticValid;
    const bool codePresent = code.empty() ||
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &diagnostic) { return diagnostic.code == code; });
    const bool internal = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                                     [](const auto &diagnostic) {
                                       return diagnostic.code == "GTI-B0001";
                                     });
    if (passed != accepted || !codePresent || internal) {
      ++failures;
      std::cerr << name << ": expected " << (accepted ? "acceptance" : "rejection")
                << (code.empty() ? "" : std::string(" with ") + std::string(code)) << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  };
  check("explicit-identity", R"gti(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti", true);
  check("elided-identity", R"gti(
int& identity(int& value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti", true);
  check("choose-two-origins", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int main() { int first = 3; int second = 8; return choose(first, second, false) - 8; }
)gti", true);
  check("independent-regions", R"gti(
int&<a> first<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return left; }
int main() { int left = 3; int right = 8; return first(left, right) - 3; }
)gti", true);
  check("wrong-owner-region", R"gti(
int&<a> wrong<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return right; }
int main() { int left = 3; int right = 8; return wrong(left, right); }
)gti", false);
  check("subobject-preserves-owner-region", R"gti(
struct Number { int value = 7; };
int&<a> field<lifetime a>(Number&<a> number) { return number.value; }
int main() { Number number{}; return field(number) - 7; }
)gti", true);
  check("common-region-cannot-escape-as-first-input", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int&<a> wrong<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  return choose(first, second, true);
}
)gti", false, "GTI-S2127");
  check("declaration-retains-unused-supplier", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  right = 9;
  return result;
}
)gti", false);
  check("declared-suppliers-survive-alias", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  int& alias = result;
  right = 9;
  return alias;
}
)gti", false);
  check("declared-suppliers-end-at-last-use", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  int saved = result;
  right = 9;
  return saved - 3;
}
)gti", true);
  check("ambiguous-elision", R"gti(
int& choose(int& first, int& second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
)gti", false, "GTI-S2127");
  check("declared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires outlives<longer, shorter> { return input; }
)gti", true);
  check("undeclared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input) {
  return input;
}
)gti", false);
  check("named-outlives-contract", R"gti(
concept Survives<lifetime longer, lifetime shorter> = outlives<longer, shorter>;
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires Survives<longer, shorter> { return input; }
)gti", true);
  check("disjunctive-requirement-inference", R"gti(
int&<a> identity<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
int main() { int value = 7; return identity(value) - 7; }
)gti", true);
  check("disjunction-is-not-two-assumptions", R"gti(
int&<b> bad<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
)gti", false);
  check("borrowed-record-construction", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int main() { int number = 7; View view{number}; return view.value - 7; }
)gti", true);
  check("borrowed-record-escape", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
View<a> bad<lifetime a>(int&<a> outside) {
  int local = 7;
  return View<a>(local);
}
)gti", false);
  check("ordinary-input-is-not-static", R"gti(
int&<static> forever(int& input) { return input; }
)gti", false);
  check("no-local-escape", R"gti(
int&<a> escape<lifetime a>(int&<a> input) { int local = 9; return local; }
)gti", false);
  check("unknown-lifetime", R"gti(
int&<missing> bad(int& input) { return input; }
)gti", false, "GTI-S2126");
  check("lifetime-is-not-runtime-value", R"gti(
int bad<lifetime a>() { return a; }
)gti", false, "GTI-S2126");
  return failures == 0 ? 0 : 1;
}
