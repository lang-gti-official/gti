// Completion reuses the editor's exact project preamble and invalidates changed
// imports.
#include "gti/language_queries.h"
#include "unit_test.h"

#include <algorithm>
#include <iostream>

namespace {

// A generic body can revisit one marker in several concrete instances. Keep
// the usual semantic pass and its completion facts when the owner is generic.
int checkGenericMemberCompletion() {
  using namespace lang;
  const std::string source = R"(
struct Left {};
struct Right {};
struct Stream {
  /// Checks the stream.
  bool is_open() { return true; }
};
int probe<T>(T& argument) {
  Stream stream{};
  if (stream.is) {}
  return 0;
}
int main() {
  Left left{};
  Right right{};
  return probe(left) + probe(right);
}
)";
  FrontendWorkMetrics work;
  const auto result = LanguageQueries().complete(
      {.entryPath = std::filesystem::current_path() / "generic-member.gti",
       .source = source,
       .byteOffset = source.find("stream.is") + std::string("stream.is").size(),
       .workMetrics = &work});
  const auto found = std::find_if(
      result.candidates.begin(), result.candidates.end(), [](const auto &item) {
        return item.label == "is_open" &&
               item.documentationMarkdown == "Checks the stream.";
      });
  if (found == result.candidates.end() ||
      !work.semanticFinalizationMicroseconds) {
    std::cerr << "generic completion must retain the full semantic pass\n";
    return 1;
  }
  return 0;
}

} // namespace

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
                               "Stream() {}\n/// Checks the stream.\n"
                               "bool is_open() { return true; } };";
  const std::string changed = "export module api; export struct Stream { "
                              "Stream() {}\n/// Checks the stream.\n"
                              "bool is_ready() { return true; } };";
  const std::string source =
      "import api; int main() { Stream stream{}; if (stream.is) {} return 0; }";
  const std::size_t offset =
      source.find("stream.is") + std::string("stream.is").size();
  for (const bool testSuite : {false, true}) {
    options.program.testSuite = testSuite;
    for (const auto &dependency : {original, changed}) {
      auto seed = Frontend(options).analyze(
          main, "import api; int main() { Stream stream{}; return 0; }", {},
          {{api.string(), dependency}}, {}, {owner});
      if (!seed.semanticValid) {
        for (const auto &d : seed.diagnostics)
          std::cerr << d.message << '\n';
        return 1;
      }
      for (const bool usePreamble : {false, true}) {
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
             .preambleCache = usePreamble ? options.preambleCache : nullptr,
             .deferPreambleBuild = true,
             .program = options.program,
             .requireEntryPoint = true});
        const auto contains = [&](const char *name) {
          return std::any_of(
              result.candidates.begin(), result.candidates.end(),
              [&](const auto &candidate) { return candidate.label == name; });
        };
        const auto documented = std::find_if(
            result.candidates.begin(), result.candidates.end(),
            [&](const auto &candidate) {
              return candidate.label ==
                         (dependency == original ? "is_open" : "is_ready") &&
                     candidate.documentationMarkdown == "Checks the stream." &&
                     candidate.detail.find("bool") != std::string::npos;
            });
        if (documented == result.candidates.end()) {
          std::cerr << "member signature or documentation missing\n";
          return 1;
        }
        if (metrics.semanticFinalizationMicroseconds != 0) {
          std::cerr
              << "completed an unrelated whole-program finalization for a "
                 "captured member list\n";
          return 1;
        }
        if (metrics.semanticPreambleAdoptions != (usePreamble ? 1u : 0u) ||
            metrics.semanticPreambleBuilds != 0 ||
            contains(dependency == original ? "is_ready" : "is_open")) {
          std::cerr << "completion adopted "
                    << metrics.semanticPreambleAdoptions << " preambles, built "
                    << metrics.semanticPreambleBuilds << '\n';
          return 1;
        }
      }
    }
  }
  return checkGenericMemberCompletion();
}
