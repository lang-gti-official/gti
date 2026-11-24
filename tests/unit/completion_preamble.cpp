// Completion reuses the editor's exact project preamble and invalidates changed
// imports.
#include "gti/language_queries.h"
#include "unit_test.h"

#include <algorithm>
#include <iostream>

GTI_UNIT_TEST(completion_preamble) {
  using namespace lang;
  const auto root =
      std::filesystem::current_path() / "completion-preamble-fixture";
  const auto api = root / "api.gti";
  const auto main = root / "main.gti";
  SourceOwnerDescriptor owner{.identity = "test",
                              .name = "test",
                              .packageRoot = root,
                              .sourceRoot = root,
                              .modules = {{"api", api}}};
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.synthesizeTestEntry = false;
  options.lowerEntryUnitOnly = true;
  options.trustedBodies = TrustedBodyAnalysis::Reachable;
  options.suggestIncludes = false;
  options.cache = std::make_shared<FrontendCache>();
  options.preambleCache = std::make_shared<SemanticPreambleCache>();
  const std::string original = "export module api; export struct Stream { "
                               "Stream() {} bool is_open() { return true; } };";
  const std::string changed = "export module api; export struct Stream { "
                              "Stream() {} bool is_ready() { return true; } };";
  const std::string source =
      "import api; int main() { Stream stream{}; if (stream.is) {} return 0; }";
  const std::size_t offset =
      source.find("stream.is") + std::string("stream.is").size();
  for (const auto &dependency : {original, changed}) {
    auto seed = Frontend(options).analyze(
        main, "import api; int main() { Stream stream{}; return 0; }", {},
        {{api.string(), dependency}}, {}, {owner});
    if (!seed.semanticValid) {
      for (const auto &d : seed.diagnostics)
        std::cerr << d.message << '\n';
      return 1;
    }
    FrontendWorkMetrics metrics;
    const auto result = LanguageQueries().complete(
        {.entryPath = main,
         .source = source,
         .byteOffset = offset,
         .trustedBodies = TrustedBodyAnalysis::Reachable,
         .sourceOverrides = {{api.string(), dependency}},
         .packageSourceRoots = {owner},
         .workMetrics = &metrics,
         .cache = options.cache,
         .preambleCache = options.preambleCache,
         .deferPreambleBuild = true,
         .requireEntryPoint = true});
    const auto contains = [&](const char *name) {
      return std::any_of(
          result.candidates.begin(), result.candidates.end(),
          [&](const auto &candidate) { return candidate.label == name; });
    };
    if (metrics.semanticPreambleAdoptions != 1 ||
        metrics.semanticPreambleBuilds != 0 ||
        !contains(dependency == original ? "is_open" : "is_ready") ||
        contains(dependency == original ? "is_ready" : "is_open")) {
      std::cerr << "completion adopted " << metrics.semanticPreambleAdoptions
                << " preambles, built " << metrics.semanticPreambleBuilds
                << '\n';
      return 1;
    }
  }
  return 0;
}
