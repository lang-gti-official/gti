// A body-only edit reuses closed dependency modules. A callee edit with
// the same signature invalidates caller diagnostics, including on undo.
#include "gti/frontend.h"
#include "unit_test.h"

#include <filesystem>
#include <iostream>

GTI_UNIT_TEST(incremental_module_bodies) {
  using namespace lang;
  const auto root = std::filesystem::current_path() / "module-body-fixture";
  const auto api = root / "api.gti";
  const auto main = root / "main.gti";
  SourceOwnerDescriptor owner{.identity = "test",
                              .name = "test",
                              .packageRoot = root,
                              .sourceRoot = root,
                              .modules = {{"api", api}}};
  FrontendOptions options;
  options.lowerEntryUnitOnly = true;
  options.trustedBodies = TrustedBodyAnalysis::Reachable;
  options.suggestIncludes = false;
  options.cache = std::make_shared<FrontendCache>();
  options.preambleCache = std::make_shared<SemanticPreambleCache>();
  struct Edit {
    bool enabled;
    bool localError;
    bool expectBuild;
  };
  for (const Edit edit : {Edit{true, false, true}, Edit{true, true, false},
                          Edit{false, false, true}, Edit{true, false, true}}) {
    const std::string dependency =
        std::string(
            "export module api; export consteval bool enabled() { return ") +
        (edit.enabled ? "true;" : "false;") + " }";
    const std::string entry =
        std::string("import api; int caller() { static_assert(enabled()); ") +
        (edit.localError ? "bool bad = \"text\"; " : "") + "return 0; }";
    FrontendWorkMetrics work;
    options.workMetrics = &work;
    const auto analyze = [&](const FrontendOptions &config) {
      return Frontend(config).analyze(
          main, entry, {}, {{api.string(), dependency}}, {}, {owner});
    };
    const auto warm = analyze(options);
    auto freshOptions = options;
    freshOptions.cache.reset();
    freshOptions.preambleCache.reset();
    freshOptions.workMetrics = nullptr;
    const auto fresh = analyze(freshOptions);
    if (work.semanticPreambleAdoptions != 1 ||
        work.semanticPreambleBuilds !=
            static_cast<unsigned>(edit.expectBuild) ||
        warm.semanticValid != (!edit.localError && edit.enabled) ||
        warm.semanticValid != fresh.semanticValid ||
        warm.diagnostics != fresh.diagnostics) {
      std::cerr << "unexpected semantic result: "
                << work.semanticPreambleAdoptions << " adopted, "
                << work.semanticPreambleBuilds << " built\n";
      for (const auto &d : warm.diagnostics)
        std::cerr << d.message << '\n';
      return 1;
    }
    if (!warm.semanticValid &&
        (warm.diagnostics.front().code !=
             (edit.localError ? "GTI-S2003" : "GTI-S2079") ||
         warm.diagnostics.front().primary.source != main.string()))
      return 1;
  }

  // A dependency error must survive adoption of a shorter, trusted-only
  // fallback, and a repaired dependency must displace that fallback.
  const auto prelude = root / "prelude.gti";
  for (const bool broken : {true, true, false}) {
    FrontendWorkMetrics work;
    options.workMetrics = &work;
    const std::unordered_map<std::string, std::string> sources{
        {prelude.string(), "int good() { return 1; }"},
        {api.string(),
         std::string("export module api; export int use() { return ") +
             (broken ? "missing;" : "good();") + " }"}};
    const auto warm = Frontend(options).analyze(
        main, "import api; int main() { return use(); }", {prelude}, sources,
        {}, {owner});
    auto fresh = options;
    fresh.cache.reset();
    fresh.preambleCache.reset();
    fresh.workMetrics = nullptr;
    const auto expected = Frontend(fresh).analyze(
        main, "import api; int main() { return use(); }", {prelude}, sources,
        {}, {owner});
    if (work.semanticPreambleAdoptions != 1 || warm.semanticValid == broken ||
        warm.diagnostics != expected.diagnostics)
      return 1;
  }
  // An unchecked trusted body is demanded by an application function even
  // when nothing in the entry calls that function.
  const std::unordered_map<std::string, std::string> libraryError{
      {prelude.string(), "int broken() { return missing; }"},
      {api.string(),
       "export module api; export int uses_broken() { return broken(); }"}};
  FrontendWorkMetrics work;
  options.workMetrics = &work;
  const std::string importer = "import api; int main() { return 0; }";
  auto fresh = options;
  fresh.cache.reset();
  fresh.preambleCache.reset();
  fresh.workMetrics = nullptr;
  const auto expected = Frontend(fresh).analyze(main, importer, {prelude},
                                                libraryError, {}, {owner});
  const auto resumed = Frontend(options).analyze(main, importer, {prelude},
                                                 libraryError, {}, {owner});
  if (expected.semanticValid || expected.diagnostics.empty() ||
      resumed.diagnostics != expected.diagnostics) {
    std::cerr << "dependency's transitive library diagnostics differ\n";
    return 1;
  }

  // An implementation can complete a declaration visible to an earlier
  // body. Keep the full check for such graphs.
  owner.moduleImplementations = {{"api", {main}}};
  const std::unordered_map<std::string, std::string> implementation{
      {api.string(), "export module api; export int read();"}};
  const auto sharedModule =
      Frontend(options).analyze(main, "module api; int read() { return 1; }",
                                {}, implementation, {}, {owner});
  if (!sharedModule.semanticValid || work.semanticPreambleAdoptions != 0)
    return 1;
  return 0;
}
