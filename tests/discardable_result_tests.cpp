#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void valid(const std::string &source) {
  const auto frontend = lang::Frontend().analyze("discardable.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), source);
}

void invalid(const std::string &source, std::string_view code,
             std::string_view message) {
  const auto frontend = lang::Frontend().analyze("discardable.gti", source);
  expect(!frontend.canGenerateCode(), "invalid discard contract must reject");
  for (const auto &diagnostic : frontend.diagnostics) {
    if (diagnostic.code == code &&
        diagnostic.message.find(message) != std::string::npos) {
      expect(diagnostic.primary.source.ends_with("discardable.gti"),
             "diagnostic source");
      expect(diagnostic.primary.end > diagnostic.primary.start,
             "diagnostic span");
      return;
    }
  }
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(false, source);
}
} // namespace

int main() {
  valid(R"(
int main() { put(3); [[discard]] put(4); return put(5); }
[[discardable]] int put(int value) { return value; }
)");
  valid(R"(
[[discardable]] int value() { return 7; }
int main() { auto fn = value; fn(); return 0; }
)");
  valid(R"(
[[discardable]] T identity<T>(T value) { return value; }
struct Counter {
  mut int value = 0;
  [[discardable, borrowed_from(this)]] mut int& add(int amount) mut {
    this.value += amount; return this.value;
  }
  [[discardable]] static int constant() { return 7; }
};
int main() { mut Counter c{}; c.add(1); c.add(2); Counter::constant(); identity(3); return c.value - 3; }
)");
  valid(R"(
struct Base { [[discardable]] virtual int get() { return 1; } };
struct Child : public Base { int get() override { return 2; } };
void use(Base& value) { value.get(); }
int main() { Child child{}; use(child); return 0; }
)");
  valid(R"(
extern "C" { [[discardable]] int native_value(); }
int main() { native_value(); return 0; }
)");
  valid(R"(
extern "C" {
  [[discardable]] [[link_name("native_first")]] int first();
  [[link_name("native_second")]] [[discardable]] int second();
}
int main() { first(); second(); return 0; }
)");
  invalid("int value() { return 1; } int main() { value(); return 0; }",
          "GTI-S2009", "must be used");
  invalid(R"(
[[discardable]] int select(int value) { return value; }
bool select(bool value) { return value; }
int main() { select(1); select(true); return 0; }
)",
          "GTI-S2009", "must be used");
  invalid("[[discardable]] void action() {} int main() { return 0; }",
          "GTI-S2099", "non-void");
  invalid("[[discardable, discardable]] int value() { return 1; }", "GTI-S2099",
          "Duplicate");
  invalid("[[discardable(1)]] int value() { return 1; }", "GTI-P0001",
          "Unknown function attribute");
  invalid("[[discardable]] int value = 1;", "GTI-P0001", "function");
  valid(R"(
struct Callable { [[discardable]] int operator()() { return 1; } };
int main() { Callable fn{}; fn(); return 0; }
)");
  invalid(R"(
struct Base { virtual int get() { return 1; } };
struct Child : public Base { [[discardable]] int get() override { return 2; } };
void use(Base& value) { value.get(); }
int main() { Child child{}; child.get(); use(child); return 0; }
)",
          "GTI-S2009", "must be used");
  invalid(R"(
[[discardable]] int value() { return 7; }
int main() { auto callback = []() -> int { return value(); }; callback(); return 0; }
)",
          "GTI-S2009", "must be used");
  invalid("[[discardable]] int& bad() { int local = 1; return local; }",
          "GTI-S2017", "reference");
  invalid(R"(
struct Counter {
  mut int value = 0;
  [[discardable, borrowed_from(this)]] mut int& add() mut { return this.value; }
};
int main() { mut Counter c{}; mut int& loan = c.add(); c.add(); return loan; }
)",
          "GTI-S2017", "borrow");

  const std::string source = "[[discardable]] int value(){return 3;}\n"
                             "int main(){value();return 0;}\n";
  const auto frontend = lang::Frontend().analyze("hover.gti", source);
  const lang::LanguageQueries queries;
  for (auto offset : {source.find("value()"), source.rfind("value()")}) {
    const auto hover =
        queries.hover(frontend, frontend.sourceGraph.entryUnit(), offset + 1);
    expect(hover && hover->signature.starts_with("[[discardable]]"),
           "declaration and selected-call hover must expose discard policy");
  }
  const lang::Formatter formatter;
  const auto formatted = formatter.format(source);
  expect(formatted.find("[[discardable]]") != std::string::npos &&
             formatter.format(formatted) == formatted,
         "discardable formatting must preserve syntax and be idempotent");
  valid(formatted);
}
