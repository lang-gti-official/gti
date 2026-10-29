#include "gti/frontend.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "module-editor-fixture";
  const auto entry = root / "main.gti";
  const auto available = root / "available.gti";
  const auto missing = root / "missing.gti";
  const std::string source = "/* 😀 */ import \"missing.gti\";\n"
                             "import \"available.gti\";\n"
                             "int main() { return available::answer(); }\n";
  std::unordered_map<std::string, std::string> overlays{
      {available.string(),
       "export module available;\nexport namespace available { "
       "int answer() { return 42; } }\n"}};
  const auto closed = lang::Frontend().analyze(entry, source, {}, overlays);
  expect(!closed.sourceValid && closed.program.declarations().empty() &&
             !closed.canGenerateCode(),
         "ordinary compilation must stop when an import cannot load");

  lang::FrontendOptions options;
  options.analyzeRecoveredProgram = true;
  options.analyzeRecoveredSourceGraph = true;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.suggestIncludes = false;
  options.cache = std::make_shared<lang::FrontendCache>();
  for (int pass = 0; pass != 2; ++pass) {
    const auto partial =
        lang::Frontend(options).analyze(entry, source, {}, overlays);
    expect(
        !partial.sourceValid && partial.syntaxValid && !partial.hirValid &&
            !partial.canGenerateCode() && !partial.diagnostics.empty(),
        "partial editor snapshots retain load errors and cannot lower or emit");
    const auto unit = partial.sourceGraph.entryUnit();
    const auto definition = lang::LanguageQueries().definition(
        partial, unit, source.find("answer"));
    expect(definition && definition->target.source == available.string(),
           "an unresolved import must not hide a loaded module's exact "
           "definition");
  }
  const auto broken = lang::Frontend(options).analyze(
      entry, source + "int broken = ;\n", {}, overlays);
  expect(
      !broken.sourceValid && !broken.syntaxValid &&
          std::any_of(broken.diagnostics.begin(), broken.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.phase ==
                               lang::DiagnosticPhase::Parsing;
                      }),
      "source-loading failure must not suppress an independent syntax error");

  overlays[missing.string()] =
      "export module missing;\nexport int extra() { return 1; }\n";
  const auto restored =
      lang::Frontend(options).analyze(entry, source, {}, overlays);
  expect(restored.sourceValid && restored.syntaxValid &&
             restored.semanticValid && restored.hirValid &&
             restored.diagnostics.empty(),
         "an unsaved missing interface restores full analysis using the same "
         "cache");
  overlays.erase(missing.string());
  const auto removed =
      lang::Frontend(options).analyze(entry, source, {}, overlays);
  expect(!removed.sourceValid && !removed.hirValid &&
             !removed.canGenerateCode(),
         "closing the unsaved interface restores the load failure without "
         "stale HIR");
  std::cout << "Partial module editor snapshot checks passed\n";
}
