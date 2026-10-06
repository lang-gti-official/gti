#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(const std::string &name, const std::string &source,
           lang::RawPointerAuditMode mode, bool finding) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  options.rawPointerAudit = mode;
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto result = lang::Frontend(options).analyze(
      root / "raw-query-budget-test.gti", source, {root / "stdlib/prelude.gti"},
      {}, {root / "stdlib"});
  const auto count = std::count_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2103"; });
  const bool shouldReject =
      finding && mode == lang::RawPointerAuditMode::Strict;
  const bool expectedSeverity = std::all_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [mode](const auto &diagnostic) {
        return diagnostic.code != "GTI-S2103" ||
               diagnostic.severity == (mode == lang::RawPointerAuditMode::Warn
                                           ? lang::DiagnosticSeverity::Warning
                                           : lang::DiagnosticSeverity::Error);
      });
  if (result.semanticValid == !shouldReject && (count != 0) == finding &&
      expectedSeverity)
    return;
  ++failures;
  std::cerr << "FAIL: " << name << '\n';
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}

std::string chain(int length, bool reverse, bool generic, bool constructors) {
  std::vector<std::string> declarations;
  for (int i = 0; i < length; ++i) {
    const std::string name = std::to_string(i);
    const std::string next = std::to_string(i + 1);
    if (constructors) {
      declarations.push_back(
          "struct Relay" + name + " { mut const int* pointer = nullptr; Relay" +
          name +
          "(const int* input) { "
          "this.pointer = " +
          (i + 1 == length ? "input" : "Relay" + next + "(input).pointer") +
          "; } };\n");
    } else {
      const std::string type = generic ? "T" : "int";
      declarations.push_back(
          "const " + type + "* relay" + name + (generic ? "<T>" : "") +
          "(const " + type + "* pointer) { return " +
          (i + 1 == length
               ? "pointer"
               : "relay" + next + (generic ? "<T>" : "") + "(pointer)") +
          "; }\n");
    }
  }
  const std::string main =
      "int main() { int value = 7; const int* pointer = " +
      (constructors
           ? std::string("Relay0(&value).pointer")
           : "relay0" + std::string(generic ? "<int>" : "") + "(&value)") +
      "; unsafe { return *pointer - 7; } }\n";
  if (reverse)
    std::reverse(declarations.begin(), declarations.end());
  std::string result = reverse ? "" : main;
  for (const auto &declaration : declarations)
    result += declaration;
  return result + (reverse ? main : "");
}
} // namespace

int main() {
  for (const auto mode :
       {lang::RawPointerAuditMode::Warn, lang::RawPointerAuditMode::Strict}) {
    for (const bool reverse : {false, true}) {
      for (const int length : {8, 40}) {
        for (const bool generic : {false, true})
          check("ordinary/generic chain " + std::to_string(length) +
                    (reverse ? " reversed" : " forward"),
                chain(length, reverse, generic, false), mode, length > 32);
        check("constructor chain " + std::to_string(length) +
                  (reverse ? " reversed" : " forward"),
              chain(length, reverse, false, true), mode, length > 32);
      }
      const std::string first =
          "const int* first(const int* p) { return second(p); }\n";
      const std::string second =
          "const int* second(const int* p) { return first(p); }\n";
      check("recursive query remains unresolved",
            (reverse ? second + first : first + second) +
                "int main() { int value = 7; const int* p = first(&value); "
                "unsafe { return *p; } }",
            mode, true);
      check("recursive result transport remains inert",
            (reverse ? second + first : first + second) +
                "int main() { int value = 7; const int* p = first(&value); "
                "bool same = p == p; return 0; }",
            mode, false);
    }
  }

  // Fill the cross-demand reuse cache with distinct concrete generic keys.
  // Its capacity must not deny analysis of the subsequent live relay chain.
  std::string pressure =
      "const int* identity<uint64_t N>(const int* p) { return p; }\n"
      "const int* relay(const int* p) { return p; }\n"
      "int main() { int value = 7;\n";
  for (int i = 0; i < 2050; ++i)
    pressure += "[[discard]] identity<" + std::to_string(i) + ">(&value);\n";
  pressure += "const int* p = relay(&value); unsafe { return *p - 7; } }";
  check("cache capacity does not exhaust a fresh demand", pressure,
        lang::RawPointerAuditMode::Strict, false);
  return failures == 0 ? 0 : 1;
}
