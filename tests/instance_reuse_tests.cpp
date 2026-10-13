// Instance reuse: an editor snapshot may reuse a concrete instance analysis
// of a compiler-trusted declaration from an earlier snapshot (see
// InstanceAnalysisCache). Reuse must be invisible: every analysis that reuses
// instances must produce exactly the diagnostics, HIR and MIR of a fresh
// analysis of the same source.
//
// Three kinds of check. The cache container's keys, generations and bounds
// are unit tested directly. Fixtures exercise reuse across an unchanged
// snapshot, an edited entry body, a changed entry declaration set, a changed
// library unit, user-typed (ineligible) instances and complete analysis. The
// corpus gate analyses every example fresh, then twice through one cache, and
// requires identical results each time.
#include "gti/frontend.h"
#include "gti/instance_analysis_cache.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace {
int failures = 0;

void expect(bool valid, const std::string &message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Caches {
  std::shared_ptr<lang::FrontendCache> frontend =
      std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
          .maximumEntries = 1024,
          .maximumEstimatedBytes = 512U * 1024U * 1024U});
  std::shared_ptr<lang::InstanceAnalysisCache> instances =
      std::make_shared<lang::InstanceAnalysisCache>();
};

enum class Mode {
  // The LSP's snapshot configuration.
  Editor,
  // Reachable analysis through MIR, as an artifact build lowers it.
  Artifact,
};

struct Analysis {
  lang::FrontendResult result;
  lang::FrontendWorkMetrics metrics;
};

Analysis analyze(
    const std::filesystem::path &root, const std::filesystem::path &entry,
    Mode mode, Caches *caches = nullptr,
    std::optional<std::string> source = std::nullopt,
    const std::unordered_map<std::string, std::string> &overrides = {},
    lang::TrustedBodyAnalysis bodies = lang::TrustedBodyAnalysis::Reachable) {
  Analysis analysis;
  lang::FrontendOptions options;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = bodies;
  options.workMetrics = &analysis.metrics;
  if (mode == Mode::Editor) {
    options.stopAfter = lang::FrontendPhase::Hir;
    options.lowerEntryUnitOnly = true;
  } else {
    options.stopAfter = lang::FrontendPhase::Mir;
    options.lowerReachableOnly = true;
    options.lowerLibraryReachableOnly = true;
  }
  if (caches != nullptr) {
    options.cache = caches->frontend;
    options.instanceCache = caches->instances;
  }
  analysis.result = lang::Frontend(options).analyze(
      entry, std::move(source), {root / "stdlib/prelude.gti"}, overrides,
      {root / "stdlib"});
  return analysis;
}

std::string diagnosticKey(const lang::Diagnostic &diagnostic) {
  std::string result = diagnostic.code + '\t' +
                       std::to_string(static_cast<int>(diagnostic.severity)) +
                       '\t' + diagnostic.primary.source + ':' +
                       std::to_string(diagnostic.primary.start) + '-' +
                       std::to_string(diagnostic.primary.end) + '\t' +
                       diagnostic.message;
  for (const lang::RelatedDiagnostic &related : diagnostic.related)
    result += "\n  " + related.span.source + ':' +
              std::to_string(related.span.start) + '\t' + related.message;
  for (const std::string &hint : diagnostic.hints)
    result += "\n  hint " + hint;
  return result;
}

// Everything a snapshot exposes downstream of semantic analysis.
std::string outcome(const lang::FrontendResult &result) {
  std::ostringstream text;
  text << "semantic=" << result.semanticValid << " hir=" << result.hirValid
       << " mir=" << result.mirValid << '\n';
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    text << diagnosticKey(diagnostic) << '\n';
  text << "functions=" << result.hir.functionInstances().size()
       << " classes=" << result.hir.classInstances().size()
       << " constructors=" << result.hir.constructorInstances().size()
       << " instanceSymbols=" << result.hir.instanceSymbols().size() << '\n';
  // Concrete-only symbol identities are request-local: a reused delta keeps
  // the range it was first analysed with, so only the declarations (and, by
  // HIR verification, their distinctness) must agree.
  std::vector<std::string> symbols;
  for (const lang::SymbolRecord &symbol : result.hir.instanceSymbols())
    symbols.push_back(std::to_string(symbol.sourceUnit) + ':' +
                      std::to_string(symbol.nameSpan.start) + ' ' +
                      symbol.qualifiedName);
  std::sort(symbols.begin(), symbols.end());
  for (const std::string &symbol : symbols)
    text << "  symbol " << symbol << '\n';
  if (result.mirValid)
    lang::MirPrinter().print(result.mir, text);
  return text.str();
}

void compare(const std::string &label, const std::string &expected,
             const std::string &actual) {
  if (expected == actual)
    return;
  ++failures;
  std::cerr << "FAIL: " << label << " differs from a fresh analysis\n";
  std::istringstream left(expected);
  std::istringstream right(actual);
  std::string leftLine;
  std::string rightLine;
  for (std::size_t line = 1, shown = 0; shown < 6; ++line) {
    const bool more = static_cast<bool>(std::getline(left, leftLine));
    const bool other = static_cast<bool>(std::getline(right, rightLine));
    if (!more && !other)
      break;
    if (leftLine != rightLine || more != other) {
      std::cerr << "  line " << line << "\n    fresh:  " << leftLine
                << "\n    reused: " << rightLine << '\n';
      ++shown;
    }
  }
}

std::filesystem::path write(const std::filesystem::path &path,
                            const std::string &source) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << source;
  return path;
}

void checkContainer(const std::filesystem::path &root,
                    const std::filesystem::path &directory) {
  lang::InstanceAnalysisCache cache({.maximumEntries = 3,
                                     .maximumEstimatedBytes = 1U << 30U,
                                     .maximumGenerations = 2});
  const auto unit = [](std::string path, const lang::Stmt *declaration) {
    return lang::InstanceAnalysisGenerationKey::TrustedUnit{
        .id = 2, .path = std::move(path), .declarations = {declaration}};
  };
  auto syntax = std::make_shared<lang::EmptyStmt>(lang::Token{});
  const lang::InstanceAnalysisGenerationKey first{
      .context = "host",
      .units = {unit("std/a", syntax.get())},
      .registrationFingerprint = 7};
  const auto generation = cache.generation(first, {syntax});
  expect(generation != nullptr, "a generation is created");
  expect(cache.generation(first, {syntax}) == generation,
         "an equal generation key selects the same generation");
  auto renumbered = first;
  renumbered.registrationFingerprint = 8;
  const auto other = cache.generation(renumbered, {syntax});
  expect(other != generation,
         "a different registration fingerprint is another generation");

  const auto key = [](std::size_t declaration,
                      std::vector<lang::SemanticType> types = {}) {
    return lang::InstanceAnalysisKey{.kind =
                                         lang::InstanceAnalysisKind::Function,
                                     .declaration = declaration,
                                     .types = std::move(types)};
  };
  cache.store(other, key(1), {.estimatedBytes = 10});
  expect(cache.lookup(other, key(1)) != nullptr, "a stored entry is found");
  expect(cache.lookup(other, key(1, {lang::SemanticType::Int32})) == nullptr,
         "instance arguments are part of the key");
  expect(cache.lookup(other, key(2)) == nullptr,
         "the declaration is part of the key");
  expect(cache.lookup(generation, key(1)) == nullptr,
         "entries are scoped to their generation");
  cache.store(other, key(2), {.estimatedBytes = 10});
  cache.store(other, key(3), {.estimatedBytes = 10});
  (void)cache.lookup(other, key(1));
  cache.store(other, key(4), {.estimatedBytes = 10});
  expect(cache.stats().entries == 3, "the entry limit is enforced");
  expect(cache.lookup(other, key(2)) == nullptr &&
             cache.lookup(other, key(1)) != nullptr,
         "the least recently used entry is evicted");

  // Same unit paths with other syntax supersede the earlier generation.
  auto replacement = std::make_shared<lang::EmptyStmt>(lang::Token{});
  auto reparsed = renumbered;
  reparsed.units = {unit("std/a", replacement.get())};
  const auto superseding = cache.generation(reparsed, {replacement});
  expect(cache.lookup(other, key(1)) == nullptr &&
             cache.stats().generations <= 2,
         "reparsed library syntax supersedes the earlier generation");
  expect(superseding != nullptr, "the superseding generation is created");
  lang::InstanceAnalysisCache bounded({.maximumEntries = 100,
                                       .maximumEstimatedBytes = 25,
                                       .maximumGenerations = 1});
  const auto only = bounded.generation(first, {syntax});
  bounded.store(only, key(1), {.estimatedBytes = 10});
  bounded.store(only, key(2), {.estimatedBytes = 10});
  bounded.store(only, key(3), {.estimatedBytes = 10});
  expect(bounded.stats().estimatedBytes <= 25, "the byte limit is enforced");
  auto elsewhere = first;
  elsewhere.context = "other target";
  (void)bounded.generation(elsewhere, {syntax});
  expect(bounded.stats().generations == 1 && bounded.stats().entries == 0,
         "the generation limit evicts the oldest generation and its entries");

  // Implicit field-initializer anchors are shared per trusted field.
  const auto path = write(directory / "anchors.gti",
                          "class Holder {\n  int first;\n  int second;\n};\n");
  lang::FrontendOptions parse;
  parse.stopAfter = lang::FrontendPhase::Parsing;
  const lang::FrontendResult parsed =
      lang::Frontend(parse).analyze(path, std::nullopt, {}, {}, {});
  const lang::ClassDecl *holder = nullptr;
  for (const lang::StmtPtr &declaration : parsed.program.declarations())
    if (const auto *candidate =
            lang::stmtAs<lang::ClassDecl>(declaration.get()))
      holder = candidate;
  std::vector<const lang::VariableDecl *> fields;
  if (holder != nullptr)
    for (const lang::StmtPtr &member : holder->members())
      if (const auto *field = lang::stmtAs<lang::VariableDecl>(member.get()))
        fields.push_back(field);
  expect(fields.size() == 2, "the anchor fixture parses two fields");
  if (fields.size() == 2) {
    const auto anchor = cache.implicitFieldInitializer(*fields[0]);
    expect(anchor != nullptr && *anchor != nullptr &&
               cache.implicitFieldInitializer(*fields[0]) == anchor,
           "one field keeps one anchor across snapshots");
    expect(cache.implicitFieldInitializer(*fields[1]) != anchor,
           "distinct fields never share an anchor");
  }
  (void)root;
}

const char *const kLibraryUser = R"(#include <std/string>
#include <std/vector>
#include <std/set>
#include <std/format>

class Point {
  int x;
  int y;
public:
  Point(int x, int y) : x(x), y(y) {}
  int sum() { return this.x + this.y; }
};

int main() {
  mut std::vector<std::string> names{};
  names.push_back(std::string("ada"));
  names.push_back(std::string("grace"));
  mut std::set<std::string> seen{};
  for (auto& name : names) {
    [[discard]] seen.insert(name.clone());
  }
  mut std::vector<Point> points{};
  points.push_back(Point(1, 2));
  auto text = std::format("{} {}", names.size(), seen.size());
  return text.size() > std::size_t(0) && points[std::size_t(0)].sum() == 3 ? 0 : 1;
}
)";

std::string replaceOnce(std::string text, const std::string &from,
                        const std::string &to) {
  const auto found = text.find(from);
  if (found != std::string::npos)
    text.replace(found, from.size(), to);
  return text;
}

void checkFixtures(const std::filesystem::path &root,
                   const std::filesystem::path &directory) {
  const auto entry = write(directory / "library-user.gti", kLibraryUser);
  for (const Mode mode : {Mode::Editor, Mode::Artifact}) {
    const std::string label =
        mode == Mode::Editor ? "editor snapshot" : "reachable artifact";
    const Analysis fresh = analyze(root, entry, mode);
    expect(fresh.result.diagnostics.empty() && fresh.result.hirValid,
           label + ": the fixture is valid");
    Caches caches;
    const Analysis cold = analyze(root, entry, mode, &caches);
    compare(label + " (first analysis through the cache)",
            outcome(fresh.result), outcome(cold.result));
    expect(cold.metrics.semanticInstanceReuseHits == 0 &&
               cold.metrics.semanticInstanceReuseStores > 0,
           label + ": a first analysis stores reusable instances");
    expect(cold.metrics.semanticInstanceReuseIneligible > 0,
           label + ": an instance of a user-defined type is not reusable");
    const Analysis warm = analyze(root, entry, mode, &caches);
    compare(label + " (unchanged snapshot)", outcome(fresh.result),
            outcome(warm.result));
    expect(warm.metrics.semanticInstanceReuseHits ==
                   cold.metrics.semanticInstanceReuseStores &&
               warm.metrics.semanticInstanceReuseMisses == 0,
           label + ": an unchanged snapshot reuses every stored instance (" +
               std::to_string(warm.metrics.semanticInstanceReuseHits) + " of " +
               std::to_string(cold.metrics.semanticInstanceReuseStores) + ")");

    // An edited body: new locals and calls renumber body-local identities
    // the stored analyses read; each reuse is validated or re-analysed.
    const std::string edited = replaceOnce(
        kLibraryUser, "  mut std::vector<std::string> names{};",
        "  auto extra = std::string(\"x\");\n"
        "  mut std::vector<int> numbers{};\n"
        "  numbers.push_back(extra.size() > std::size_t(0) ? 1 : 2);\n"
        "  mut std::vector<std::string> names{};");
    const Analysis editedFresh = analyze(root, entry, mode, nullptr, edited);
    const Analysis editedWarm = analyze(root, entry, mode, &caches, edited);
    compare(label + " (edited entry body)", outcome(editedFresh.result),
            outcome(editedWarm.result));
    expect(editedWarm.metrics.semanticInstanceReuseHits > 0,
           label + ": an edited body still reuses instances");

    // A changed declaration set renumbers registration identities.
    const std::string declared = replaceOnce(
        kLibraryUser, "class Point {",
        "class Extra { int value; };\nint helper() { return 1; }\nclass "
        "Point {");
    const Analysis declaredFresh =
        analyze(root, entry, mode, nullptr, declared);
    const Analysis declaredWarm = analyze(root, entry, mode, &caches, declared);
    compare(label + " (changed entry declarations)",
            outcome(declaredFresh.result), outcome(declaredWarm.result));

    // A changed library unit is another generation: nothing is reused.
    const std::filesystem::path vectorUnit = root / "stdlib/std/vector.gti";
    std::ifstream input(vectorUnit, std::ios::binary);
    std::ostringstream library;
    library << input.rdbuf();
    const std::unordered_map<std::string, std::string> overrides{
        {vectorUnit.string(),
         library.str() + "\nnamespace std { int edited_unit() { return 1; } "
                         "}\n"}};
    const Analysis libraryFresh =
        analyze(root, entry, mode, nullptr, std::nullopt, overrides);
    const Analysis libraryWarm =
        analyze(root, entry, mode, &caches, std::nullopt, overrides);
    compare(label + " (edited library unit)", outcome(libraryFresh.result),
            outcome(libraryWarm.result));
    expect(libraryWarm.metrics.semanticInstanceReuseHits == 0 &&
               libraryWarm.metrics.semanticInstanceReuseStores > 0,
           label + ": editing a library unit invalidates every instance");
    const Analysis restored = analyze(root, entry, mode, &caches);
    compare(label + " (restored library unit)", outcome(fresh.result),
            outcome(restored.result));
  }

  // Complete analysis (`gti check`) never consults the cache.
  Caches caches;
  const Analysis complete =
      analyze(root, entry, Mode::Editor, &caches, std::nullopt, {},
              lang::TrustedBodyAnalysis::Complete);
  expect(complete.metrics.semanticInstanceReuseHits == 0 &&
             complete.metrics.semanticInstanceReuseStores == 0 &&
             caches.instances->stats().entries == 0,
         "complete analysis neither reuses nor stores instances");
}

std::size_t corpusHits = 0;

void checkCorpusEntry(const std::filesystem::path &root,
                      const std::filesystem::path &entry) {
  for (const Mode mode : {Mode::Editor, Mode::Artifact}) {
    const std::string label =
        entry.filename().string() +
        (mode == Mode::Editor ? " editor snapshot" : " reachable artifact");
    const std::string fresh = outcome(analyze(root, entry, mode).result);
    Caches caches;
    compare(label + " (first analysis through the cache)", fresh,
            outcome(analyze(root, entry, mode, &caches).result));
    const Analysis warm = analyze(root, entry, mode, &caches);
    compare(label + " (reused instances)", fresh, outcome(warm.result));
    corpusHits += warm.metrics.semanticInstanceReuseHits;
  }
}
} // namespace

int main(int argc, char *argv[]) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("gti-instance-reuse-" + std::to_string(::getpid()));
  std::filesystem::create_directories(scratch);
  checkContainer(root, scratch);
  checkFixtures(root, scratch);
  std::filesystem::remove_all(scratch);

  std::vector<std::filesystem::path> entries;
  for (const char *directory : {"examples", "examples/transit-planner/src"}) {
    const std::filesystem::path path = root / directory;
    if (!std::filesystem::is_directory(path))
      continue;
    for (const auto &item : std::filesystem::directory_iterator(path))
      if (item.is_regular_file() && item.path().extension() == ".gti")
        entries.push_back(item.path());
  }
  std::sort(entries.begin(), entries.end());
  // An optional second argument restricts the corpus to matching entries.
  if (argc > 2)
    std::erase_if(entries, [&](const std::filesystem::path &entry) {
      return entry.filename().string().find(argv[2]) == std::string::npos;
    });
  for (const std::filesystem::path &entry : entries)
    checkCorpusEntry(root, entry);
  expect(corpusHits > 0, "the corpus reuses instances");
  std::cout << "checked " << entries.size() << " corpus entries with "
            << corpusHits << " reused instances\n";
  return failures == 0 ? 0 : 1;
}
