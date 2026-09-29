#include "frontend_snapshot_state.h"

#include "gti/frontend.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryProject final {
public:
  TemporaryProject() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("gti-frontend-cache-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
  }

  ~TemporaryProject() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  TemporaryProject(const TemporaryProject &) = delete;
  TemporaryProject &operator=(const TemporaryProject &) = delete;

  [[nodiscard]] std::filesystem::path path(std::string_view name) const {
    return root / name;
  }

  static void write(const std::filesystem::path &path,
                    std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
  }

private:
  std::filesystem::path root;
};

std::string canonicalKey(const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::path result =
      std::filesystem::weakly_canonical(path, error);
  return (error ? std::filesystem::absolute(path) : result).string();
}

bool contains(const std::vector<std::string> &values,
              std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

void appendSpan(std::ostringstream &output, const lang::SourceSpan &span) {
  output << span.source << ':' << span.start << ':' << span.end << ':'
         << span.line;
}

std::string resultFingerprint(const lang::FrontendResult &result) {
  std::ostringstream output;
  output << result.sourceValid << result.syntaxValid << result.semanticValid
         << result.semantics.functionCount() << ':'
         << result.semantics.lambdaCount() << ':'
         << result.semantics.resolvedCallCount() << ':'
         << result.semantics.classLifecycleCount() << ':'
         << result.semantics.resolvedConstructionCount() << '\n';

  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    output << diagnostic.code << ':' << static_cast<int>(diagnostic.phase)
           << ':' << static_cast<int>(diagnostic.severity) << ':'
           << diagnostic.group << ':';
    appendSpan(output, diagnostic.primary);
    output << ':' << diagnostic.message << '\n';
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      output << "related:";
      appendSpan(output, related.span);
      output << ':' << related.message << '\n';
    }
    for (const lang::FixIt &fix : diagnostic.fixes) {
      output << "fix:";
      appendSpan(output, fix.span);
      output << ':' << fix.replacement << ':' << fix.message << ':'
             << fix.preferred << '\n';
    }
    for (const std::string &hint : diagnostic.hints) {
      output << "hint:" << hint << '\n';
    }
    for (const lang::DiagnosticTag tag : diagnostic.tags) {
      output << "tag:" << static_cast<int>(tag) << '\n';
    }
  }

  for (const lang::SourceUnit &unit : result.sourceGraph.sourceUnits()) {
    output << "unit:" << unit.id << ':' << unit.path.string() << ':'
           << unit.sourceDigest << ':' << unit.syntaxDigest << ':'
           << unit.declarationStart << ':' << unit.declarationCount << ':'
           << static_cast<int>(unit.role) << ':' << unit.entry << ':'
           << unit.prelude << '\n';
  }
  for (const lang::SourceDependency &dependency :
       result.sourceGraph.dependencyEdges()) {
    output << "edge:" << dependency.source << ':' << dependency.target << ':'
           << static_cast<int>(dependency.kind) << ':'
           << dependency.includeSpelling << ':' << dependency.includeOccurrence
           << '\n';
  }

  const lang::SemanticTypePrinter types(result.semantics);
  const lang::SemanticDatabase &database = result.semantics.database();
  for (const lang::SymbolRecord &symbol : database.symbols()) {
    output << "symbol:" << symbol.id << ':' << static_cast<int>(symbol.kind)
           << ':' << symbol.name << ':' << symbol.qualifiedName << ':'
           << symbol.sourceUnit << ':';
    appendSpan(output, symbol.nameSpan);
    output << ':' << types.print(symbol.type) << ':'
           << static_cast<int>(symbol.access) << ':' << symbol.mutableBinding
           << ':' << symbol.defaultLibrary << ':' << symbol.staticMember << ':'
           << symbol.internalLinkage << ':' << symbol.generated << ':'
           << symbol.compilerPrivate << '\n';
  }
  for (const lang::SourceUnit &unit : result.sourceGraph.sourceUnits()) {
    for (const lang::SemanticOccurrence &occurrence :
         database.occurrences(unit.id)) {
      output << "occurrence:" << occurrence.sourceUnit << ':';
      appendSpan(output, occurrence.span);
      output << ':' << static_cast<int>(occurrence.kind) << ':'
             << occurrence.symbol << ':'
             << static_cast<std::uint32_t>(occurrence.roles) << ':'
             << occurrence.name << ':' << types.print(occurrence.type) << ':'
             << static_cast<int>(occurrence.access) << ':'
             << occurrence.mutableBinding << ':'
             << static_cast<int>(occurrence.bindingKind) << ':'
             << occurrence.staticMember << ':'
             << occurrence.selectedCall.has_value() << ':'
             << occurrence.selectedConstruction.has_value() << '\n';
    }
  }
  return output.str();
}

struct AnalysisRun {
  lang::FrontendWorkMetrics metrics;
  lang::FrontendResult result;
};

AnalysisRun
analyze(const std::filesystem::path &entry,
        std::optional<std::string> entrySource,
        const lang::lsp::FrontendSourceSnapshot &snapshot,
        const std::unordered_map<std::string, std::string> &overrides = {},
        lang::ConfigurationFlags flags = {}, bool useCache = true) {
  AnalysisRun run;
  lang::FrontendOptions options;
  options.configurationFlags = std::move(flags);
  options.analyzeRecoveredProgram = true;
  options.stopAfter = lang::FrontendPhase::Semantics;
  options.workMetrics = &run.metrics;
  options.cache = useCache ? snapshot.cache : nullptr;
  options.sourceGenerations = snapshot.generations;
  options.sourceDigests = snapshot.digests;
  run.result = lang::Frontend(std::move(options))
                   .analyze(entry, std::move(entrySource), {}, overrides);
  return run;
}

void publishIdentities(lang::lsp::FrontendSnapshotState &state,
                       const lang::lsp::FrontendSourceSnapshot &snapshot,
                       const lang::FrontendResult &result) {
  for (const lang::SourceUnit &unit : result.sourceGraph.sourceUnits()) {
    const std::string path = unit.path.string();
    const auto generation = snapshot.generations.find(path);
    expect(state.publish(path,
                         {.generation = generation == snapshot.generations.end()
                                            ? 0
                                            : generation->second,
                          .digest = unit.sourceDigest}),
           "a current completed analysis should publish its source identity");
  }
}

void testBoundedLruAndSnapshotIdentity() {
  lang::FrontendCache cache(
      {.maximumEntries = 2, .maximumEstimatedBytes = 1024 * 1024});
  const auto key = [](std::string path, std::uint64_t digest) {
    return lang::FrontendUnitCacheKey{.canonicalPath = std::move(path),
                                      .sourceGeneration = 1,
                                      .sourceDigest = digest,
                                      .context = "test"};
  };
  cache.storeLexed(key("a.gti", 1), {.source = "a", .sourceDigest = 1});
  cache.storeLexed(key("b.gti", 2), {.source = "b", .sourceDigest = 2});
  expect(cache.lookupLexed(key("a.gti", 1), "a").has_value(),
         "an exact overlay source should hit its cache row");
  cache.storeLexed(key("c.gti", 3), {.source = "c", .sourceDigest = 3});
  expect(!cache.lookupLexed(key("b.gti", 2)).has_value() &&
             cache.lookupLexed(key("a.gti", 1)).has_value() &&
             cache.lookupLexed(key("c.gti", 3)).has_value(),
         "the entry bound should evict the least-recently-used row");
  expect(!cache.lookupLexed(key("a.gti", 1), "collision").has_value(),
         "overlay hits should compare exact contents after digest lookup");
  cache.invalidate("a.gti");
  const lang::FrontendCacheStats stats = cache.stats();
  expect(!cache.lookupLexed(key("a.gti", 1)).has_value() &&
             stats.entries <= stats.limits.maximumEntries &&
             stats.estimatedBytes <= stats.limits.maximumEstimatedBytes,
         "explicit invalidation and both cache bounds should be enforced");

  lang::FrontendCache byteBounded(
      {.maximumEntries = 8, .maximumEstimatedBytes = 32});
  byteBounded.storeLexed(key("large.gti", 4),
                         {.source = std::string(128, 'x'), .sourceDigest = 4});
  expect(byteBounded.stats().entries == 0 &&
             byteBounded.stats().estimatedBytes == 0,
         "a row larger than the estimated-byte bound should evict itself");

  lang::lsp::FrontendSnapshotState state;
  expect(state.publish("source.gti", {.generation = 0, .digest = 11}) &&
             state.digest("source.gti") == 11,
         "generation-zero disk sources may publish a learned exact digest");
  expect(state.invalidate("source.gti") == 1 &&
             !state.digest("source.gti").has_value(),
         "invalidation should advance the generation and erase its digest");
  expect(!state.publish("source.gti", {.generation = 0, .digest = 12}) &&
             state.publish("source.gti", {.generation = 1, .digest = 13}) &&
             state.digest("source.gti") == 13,
         "stale generations should be rejected while the current one learns");
  const lang::lsp::FrontendSourceSnapshot captured = state.capture();
  expect(captured.cache == state.cache() &&
             captured.generations.at("source.gti") == 1 &&
             captured.digests.at("source.gti") == 13,
         "captured worker input should be an internally coherent snapshot");
}

void testFrontendReuseInvalidationAndEquivalence() {
  TemporaryProject project;
  const std::filesystem::path entry = project.path("main.gti");
  const std::filesystem::path dependency = project.path("dependency.gti");
  const std::filesystem::path unrelated = project.path("unrelated.gti");
  const std::string entryKey = canonicalKey(entry);
  const std::string dependencyKey = canonicalKey(dependency);
  const std::string unrelatedKey = canonicalKey(unrelated);
  const std::string dependencyDisk = "int dependency_value() { return 1; }\n";
  const std::string unrelatedSource = "int unrelated_value() { return 7; }\n";
  const std::string firstRoot = "#include \"dependency.gti\"\n"
                                "int main() { return dependency_value(); }\n";
  const std::string editedRoot =
      "#include \"dependency.gti\"\n"
      "int main() { return dependency_value() + 0; }\n";
  const std::string dirtyDependency = "int dependency_value() { return 2; }\n";
  TemporaryProject::write(entry, firstRoot);
  TemporaryProject::write(dependency, dependencyDisk);
  TemporaryProject::write(unrelated, unrelatedSource);

  lang::lsp::FrontendSnapshotState state(
      {.maximumEntries = 32, .maximumEstimatedBytes = 4U * 1024U * 1024U});
  state.invalidate(entryKey);
  state.invalidate(unrelatedKey);

  lang::lsp::FrontendSourceSnapshot snapshot = state.capture();
  AnalysisRun cold = analyze(entry, firstRoot, snapshot);
  expect(cold.result.semanticValid && cold.result.diagnostics.empty() &&
             contains(cold.metrics.lexedUnits, entryKey) &&
             contains(cold.metrics.lexedUnits, dependencyKey) &&
             contains(cold.metrics.parsedUnits, entryKey) &&
             contains(cold.metrics.parsedUnits, dependencyKey),
         "cold analysis should lex and parse the root and dependency");
  publishIdentities(state, snapshot, cold.result);

  snapshot = state.capture();
  AnalysisRun unrelatedCold = analyze(unrelated, unrelatedSource, snapshot);
  expect(unrelatedCold.result.semanticValid,
         "the unrelated document fixture should analyze successfully");
  publishIdentities(state, snapshot, unrelatedCold.result);

  state.invalidate(entryKey);
  snapshot = state.capture();
  AnalysisRun rootEdit = analyze(entry, editedRoot, snapshot);
  expect(rootEdit.result.semanticValid &&
             contains(rootEdit.metrics.lexedUnits, entryKey) &&
             !contains(rootEdit.metrics.lexedUnits, dependencyKey) &&
             contains(rootEdit.metrics.parsedUnits, entryKey) &&
             !contains(rootEdit.metrics.parsedUnits, dependencyKey) &&
             contains(rootEdit.metrics.lexCacheHits, dependencyKey) &&
             contains(rootEdit.metrics.parseCacheHits, dependencyKey),
         "a root-only edit should reuse unchanged dependency lexing and ASTs");
  publishIdentities(state, snapshot, rootEdit.result);

  const AnalysisRun uncachedRoot =
      analyze(entry, editedRoot, snapshot, {}, {}, false);
  expect(resultFingerprint(rootEdit.result) ==
             resultFingerprint(uncachedRoot.result),
         "cached and uncached root-edit analyses should be semantically and "
         "diagnostically equivalent");
  const std::size_t callOffset = editedRoot.find("dependency_value");
  const lang::SourceUnitId cachedEntryUnit =
      rootEdit.result.sourceGraph.sourceUnitForPath(entryKey);
  const lang::SourceUnitId uncachedEntryUnit =
      uncachedRoot.result.sourceGraph.sourceUnitForPath(entryKey);
  const std::optional<lang::HoverInfo> cachedHover =
      lang::LanguageQueries().hover(rootEdit.result, cachedEntryUnit,
                                    callOffset);
  const std::optional<lang::HoverInfo> uncachedHover =
      lang::LanguageQueries().hover(uncachedRoot.result, uncachedEntryUnit,
                                    callOffset);
  expect(cachedHover.has_value() && uncachedHover.has_value() &&
             cachedHover->range == uncachedHover->range &&
             cachedHover->signature == uncachedHover->signature &&
             cachedHover->documentationMarkdown ==
                 uncachedHover->documentationMarkdown &&
             cachedHover->notes == uncachedHover->notes,
         "cached ASTs should retain exact semantic hover behavior");

  state.invalidate(dependencyKey);
  snapshot = state.capture();
  AnalysisRun dependencyEdit =
      analyze(entry, editedRoot, snapshot, {{dependencyKey, dirtyDependency}});
  expect(dependencyEdit.result.semanticValid &&
             !contains(dependencyEdit.metrics.lexedUnits, entryKey) &&
             contains(dependencyEdit.metrics.lexedUnits, dependencyKey) &&
             contains(dependencyEdit.metrics.parsedUnits, entryKey) &&
             contains(dependencyEdit.metrics.parsedUnits, dependencyKey) &&
             dependencyEdit.result.sources.find(dependencyKey) != nullptr &&
             *dependencyEdit.result.sources.find(dependencyKey) ==
                 dirtyDependency,
         "a dirty dependency overlay should override disk, invalidate that "
         "unit, and invalidate its dependant parse identity");
  publishIdentities(state, snapshot, dependencyEdit.result);

  AnalysisRun unrelatedWarm =
      analyze(unrelated, unrelatedSource, state.capture());
  expect(unrelatedWarm.metrics.lexedUnits.empty() &&
             unrelatedWarm.metrics.parsedUnits.empty() &&
             contains(unrelatedWarm.metrics.lexCacheHits, unrelatedKey) &&
             contains(unrelatedWarm.metrics.parseCacheHits, unrelatedKey),
         "a dependency edit should leave unrelated document cache rows valid");

  state.invalidate(dependencyKey);
  snapshot = state.capture();
  AnalysisRun reopened = analyze(entry, editedRoot, snapshot);
  expect(reopened.result.sources.find(dependencyKey) != nullptr &&
             *reopened.result.sources.find(dependencyKey) == dependencyDisk &&
             contains(reopened.metrics.lexedUnits, dependencyKey) &&
             contains(reopened.metrics.parsedUnits, dependencyKey) &&
             contains(reopened.metrics.parsedUnits, entryKey),
         "closing a dirty overlay should force the next snapshot to reload "
         "the disk source and invalidate its dependant parse identity");
}

void testRecoveredAndConfigurationContextEquivalence() {
  TemporaryProject project;
  const std::filesystem::path recoveredPath = project.path("recovered.gti");
  const std::string recoveredKey = canonicalKey(recoveredPath);
  const std::string recoveredSource = "int main() { return 0 }\n";
  TemporaryProject::write(recoveredPath, recoveredSource);

  lang::lsp::FrontendSnapshotState recoveredState;
  recoveredState.invalidate(recoveredKey);
  lang::lsp::FrontendSourceSnapshot snapshot = recoveredState.capture();
  AnalysisRun first = analyze(recoveredPath, recoveredSource, snapshot);
  publishIdentities(recoveredState, snapshot, first.result);
  snapshot = recoveredState.capture();
  AnalysisRun cached = analyze(recoveredPath, recoveredSource, snapshot);
  AnalysisRun uncached =
      analyze(recoveredPath, recoveredSource, snapshot, {}, {}, false);
  expect(!cached.result.syntaxValid &&
             contains(cached.metrics.parseCacheHits, recoveredKey) &&
             resultFingerprint(cached.result) ==
                 resultFingerprint(uncached.result),
         "recovered parser diagnostics should be exact on cached and uncached "
         "paths");

  const std::filesystem::path configuredPath = project.path("configured.gti");
  const std::string configuredKey = canonicalKey(configuredPath);
  const std::string configuredSource = "#if defined(FEATURE)\n"
                                       "int selected() { return 1; }\n"
                                       "#else\n"
                                       "int selected() { return 2; }\n"
                                       "#endif\n"
                                       "int main() { return selected(); }\n";
  TemporaryProject::write(configuredPath, configuredSource);
  lang::lsp::FrontendSnapshotState configuredState;
  configuredState.invalidate(configuredKey);
  snapshot = configuredState.capture();
  AnalysisRun withoutFlag = analyze(configuredPath, configuredSource, snapshot);
  publishIdentities(configuredState, snapshot, withoutFlag.result);
  snapshot = configuredState.capture();
  AnalysisRun withFlag =
      analyze(configuredPath, configuredSource, snapshot, {}, {"FEATURE"});
  expect(withoutFlag.result.semanticValid && withFlag.result.semanticValid &&
             contains(withFlag.metrics.lexedUnits, configuredKey) &&
             contains(withFlag.metrics.parsedUnits, configuredKey),
         "frontend configuration should participate in lexical and parsed "
         "cache identity");
}

void testDependencyConfigurationChangesParsedIdentity() {
  TemporaryProject project;
  const std::filesystem::path entry = project.path("conditional-root.gti");
  const std::filesystem::path provider = project.path("flag-provider.gti");
  const std::string entryKey = canonicalKey(entry);
  const std::string providerKey = canonicalKey(provider);
  const std::string entrySource = "#include \"flag-provider.gti\"\n"
                                  "#if defined(FEATURE)\n"
                                  "int selected() { return 1; }\n"
                                  "#else\n"
                                  "int selected() { return 2; }\n"
                                  "#endif\n"
                                  "int main() { return selected(); }\n";
  const std::string enabledProvider = "#define FEATURE\n";
  const std::string disabledProvider = "#define FEATURE\n"
                                       "#undef FEATURE\n";
  TemporaryProject::write(entry, entrySource);
  TemporaryProject::write(provider, enabledProvider);

  lang::lsp::FrontendSnapshotState state;
  state.invalidate(entryKey);
  state.invalidate(providerKey);
  lang::lsp::FrontendSourceSnapshot snapshot = state.capture();
  AnalysisRun enabled = analyze(entry, entrySource, snapshot);
  expect(enabled.result.semanticValid,
         "the dependency-defined configuration fixture should be valid");
  publishIdentities(state, snapshot, enabled.result);

  state.invalidate(providerKey);
  snapshot = state.capture();
  AnalysisRun disabled =
      analyze(entry, entrySource, snapshot, {{providerKey, disabledProvider}});
  AnalysisRun disabledUncached =
      analyze(entry, entrySource, snapshot, {{providerKey, disabledProvider}},
              {}, false);
  const lang::SourceUnit *enabledRoot =
      std::as_const(enabled.result.sourceGraph)
          .findUnit(enabled.result.sourceGraph.sourceUnitForPath(entryKey));
  const lang::SourceUnit *disabledRoot =
      std::as_const(disabled.result.sourceGraph)
          .findUnit(disabled.result.sourceGraph.sourceUnitForPath(entryKey));
  expect(enabledRoot != nullptr && disabledRoot != nullptr &&
             enabledRoot->syntaxDigest != disabledRoot->syntaxDigest,
         "dependency-selected root token streams should have distinct "
         "syntax digests");
  expect(disabled.result.semanticValid,
         "the disabled dependency-provided configuration should remain valid");
  expect(!contains(disabled.metrics.lexedUnits, entryKey),
         "a dependency flag change should retain the unchanged root lex row");
  expect(contains(disabled.metrics.parsedUnits, entryKey),
         "a dependency flag change should invalidate an incompatible "
         "dependant parsed row");
  expect(contains(disabled.metrics.parsedUnits, providerKey),
         "a dirty flag provider should be reparsed");
  expect(resultFingerprint(disabled.result) ==
             resultFingerprint(disabledUncached.result),
         "dependency-conditioned cached and uncached results should be "
         "equivalent");
}

} // namespace

int main() {
  testBoundedLruAndSnapshotIdentity();
  testFrontendReuseInvalidationAndEquivalence();
  testRecoveredAndConfigurationContextEquivalence();
  testDependencyConfigurationChangesParsedIdentity();

  if (failures != 0) {
    std::cerr << failures << " frontend cache test(s) failed\n";
    return 1;
  }
  std::cout << "Frontend cache tests passed\n";
  return 0;
}
