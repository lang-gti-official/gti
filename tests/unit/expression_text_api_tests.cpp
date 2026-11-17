// Expression-text provenance API check with no source-level observable,
// moved unchanged from expression_text_tests.cpp (its GTI-source cases became
// tests/ui/language/expression_text files): semantic analysis of
// std::expression_text! fails closed when its owning source buffer is absent,
// which a Frontend snapshot always supplies.
#include "gti/frontend.h"
#include "gti/semantic_analyzer.h"
#include "unit_test.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void unavailableSourceFailsClosed() {
  const auto library = std::filesystem::path(__FILE__)
                           .parent_path()
                           .parent_path()
                           .parent_path() /
                       "stdlib";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Parsing;
  const auto parsed =
      lang::Frontend(options).analyze("unavailable-source.gti",
                                      "import std.source_location;\n"
                                      "int main() { std::string_view text = "
                                      "std::expression_text!(2); return 0; }",
                                      {library / "prelude.gti"}, {}, {library});
  expect(parsed.sourceValid && parsed.syntaxValid,
         "unavailable-source fixture should retain valid loaded syntax");
  lang::SemanticVisitor withoutSource(lang::TargetInfo::host(),
                                      &parsed.sourceGraph);
  expect(!withoutSource.check(parsed.program),
         "expression text must fail closed without its owning source buffer");
  bool unavailable = false;
  for (const auto &diagnostic : withoutSource.errors())
    unavailable |=
        diagnostic.code == "GTI-S2019" &&
        diagnostic.message.find("complete written source expression") !=
            std::string::npos;
  expect(unavailable, "missing source storage needs the provenance diagnostic");
}
} // namespace

GTI_UNIT_TEST(expression_text_api) {
  unavailableSourceFailsClosed();
  return failures == 0 ? 0 : 1;
}
