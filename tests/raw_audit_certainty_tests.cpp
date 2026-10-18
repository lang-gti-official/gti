#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void check(const std::string &name, const std::string &source, bool definite) {
  for (const auto mode :
       {lang::RawPointerAuditMode::Off, lang::RawPointerAuditMode::Warn,
        lang::RawPointerAuditMode::Strict}) {
    lang::FrontendOptions options;
    options.stopAfter = lang::FrontendPhase::Semantics;
    options.rawPointerAudit = mode;
    const auto result =
        lang::Frontend(options).analyze("raw-certainty.gti", source);
    const auto count = [&](const std::string &code) {
      return std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto &item) { return item.code == code; });
    };
    const bool audit = mode != lang::RawPointerAuditMode::Off;
    const bool valid = mode != lang::RawPointerAuditMode::Strict;
    const bool findings =
        audit ? (definite ? count("GTI-S2101") > 0
                          : count("GTI-S2101") == 0 && count("GTI-S2102") > 0)
              : result.diagnostics.empty();
    const auto expectedCode = definite ? "GTI-S2101" : "GTI-S2102";
    const bool details = std::all_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &item) {
          return item.code != expectedCode ||
                 (!item.related.empty() && !item.hints.empty() &&
                  item.severity == (mode == lang::RawPointerAuditMode::Strict
                                        ? lang::DiagnosticSeverity::Error
                                        : lang::DiagnosticSeverity::Warning));
        });
    if (result.semanticValid == valid && findings && details)
      continue;
    ++failures;
    std::cerr << "FAIL: " << name
              << " mode=" << lang::rawPointerAuditModeName(mode) << '\n';
    for (const auto &item : result.diagnostics)
      std::cerr << item.code << ": " << item.message << '\n';
  }
}
} // namespace

int main() {
  check("implicit return construction retains unresolved raw-field origins",
        R"gti(
struct Handle {
  const int* pointer;
  implicit Handle(const int* input) : pointer(input) {}
};
Handle handle(const int* pointer) { return pointer; }
int inspect() {
  int local = 7;
  Handle result = handle(&local);
  unsafe { return *result.pointer; }
}
int main() { return 0; }
)gti",
        false);
  const std::string start = R"(
int inspect(bool choice) {
  int live = 1;
  mut const int* pointer = nullptr;
  { int dead = 2; pointer = &dead; }
)";
  const std::string end = "}\nint main() { return 0; }\n";
  check("linear exact expired alias",
        start + "const int* copy = pointer; unsafe { return *copy; }" + end,
        true);
  check("live or expired conditional",
        start +
            "const int* selected = choice ? pointer : &live; unsafe { return "
            "*selected; }" +
            end,
        false);
  check("foreign or expired conditional",
        "extern \"C\" { const int* foreign(); }\n" + start +
            "unsafe { const int* selected = choice ? pointer : foreign(); "
            "return *selected; }" +
            end,
        false);
  check("empty alternative at branch join",
        "extern \"C\" { const int* foreign(); }\n" + start +
            "if (choice) { unsafe { pointer = foreign(); } } unsafe { return "
            "*pointer; }" +
            end,
        false);
  check("independent call result input fallback",
        "const int* independent(const int* unused, const int* live) { return "
        "live; }\n" +
            start +
            "const int* selected = independent(pointer, &live); unsafe { "
            "return *selected; }" +
            end,
        false);
  check("source summary access may not execute",
        "unsafe int read(bool choice, const int* pointer) { unsafe { if "
        "(choice) { "
        "return *pointer; } } return 0; }\n" +
            start + "unsafe { return read(choice, pointer); }" + end,
        false);
  check("uncertain owner preserves inner field uncertainty", R"(
struct Address { mut const int* pointer = nullptr; };
int inspect(bool choice, const Address* other) {
  mut Address address{};
  { int dead = 2; address.pointer = &dead; }
  unsafe {
    const Address* owner = choice ? &address : other;
    const int* selected = owner->pointer;
    return *selected;
  }
}
int main() { return 0; }
)",
        false);
  if (failures)
    return 1;
  std::cout << "Raw audit certainty tests passed\n";
}
