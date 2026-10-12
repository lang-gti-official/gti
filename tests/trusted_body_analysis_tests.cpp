// Trusted-body analysis parity: the policy that lets editor snapshots skip
// standard-library bodies nothing reaches must not change what analysis
// concludes. `Reachable` (with entry-unit HIR, the editor
// configuration) must produce exactly the application-located diagnostics and
// no library-located diagnostic the complete pass did not.
#include "gti/frontend.h"
#include "gti/frontend_work.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_graph.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void checkFixtures(const std::filesystem::path &root) {
  const auto entry = root / "tests/demand-entry.gti";
  // Overlay an existing trusted path; nonexistent overlays are deliberately
  // not granted compiler trust by the source loader.
  const auto library = root / "stdlib/std/cmath.gti";
  const std::string declarations =
      "int unused_broken() { return missing_unused; }\n"
      "int leaf() { return missing_reached; }\n"
      "int relay() { return leaf(); }\n";
  const auto analyzeSource = [&](std::string text, bool complete = false,
                                 bool libraryEntry = false,
                                 std::string extra = "") {
    lang::FrontendWorkMetrics metrics;
    lang::FrontendOptions options;
    options.stopAfter = lang::FrontendPhase::Semantics;
    options.workMetrics = &metrics;
    options.trustedBodies = complete ? lang::TrustedBodyAnalysis::Complete
                                     : lang::TrustedBodyAnalysis::Reachable;
    auto result = lang::Frontend(options).analyze(
        libraryEntry ? library : entry,
        libraryEntry ? declarations + extra : "#include <std/cmath>\n" + text,
        {root / "stdlib/prelude.gti"},
        {{library.string(), declarations + extra}}, {root / "stdlib"});
    if (!complete && !libraryEntry && text == "int main() { return 0; }")
      expect(metrics.semanticDeferredBodies >
                 metrics.semanticAnalyzedDeferredBodies,
             "unused trusted bodies are actually skipped");
    return result;
  };
  const auto unused = analyzeSource("int main() { return 0; }");
  if (!unused.semanticValid)
    for (const auto &diagnostic : unused.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(unused.semanticValid,
         "unused trusted errors are left to a full library check");
  lang::SemanticVisitor reused(lang::TargetInfo::host(), &unused.sourceGraph,
                               false, nullptr, &unused.sources,
                               lang::RawPointerAuditMode::Off, false,
                               lang::TrustedBodyAnalysis::Reachable);
  expect(reused.check(unused.program) && reused.check(unused.program),
         "reused visitors discard body-demand and lexical state");
  expect(!analyzeSource("int main() { return 0; }", true).semanticValid,
         "complete checks still reject unused trusted errors");
  expect(!analyzeSource("", false, true).semanticValid,
         "a trusted unit opened as the entry is checked completely");
  for (const auto &source :
       {"int main() { return relay(); }",
        "int main() { auto callback = relay; return callback(); }",
        "int unused_application() { return missing_application; } int main() { "
        "return 0; }"})
    expect(!analyzeSource(source).semanticValid,
           "transitive calls, function items and unused application bodies are "
           "checked");
  expect(!analyzeSource(
              "int main() { return 0; }", false, false,
              "constexpr int broken_constant() { return missing_constant; }")
              .semanticValid,
         "constant definitions remain checked");
  expect(!analyzeSource("int main() { return 0; }", false, false,
                        "class Virtual { public: virtual int f() { return "
                        "missing_virtual; } };")
              .semanticValid,
         "virtual dispatch bodies remain checked");
}

std::string key(const lang::Diagnostic &diagnostic) {
  std::string result = diagnostic.code + '\t' +
                       std::to_string(static_cast<int>(diagnostic.severity)) +
                       '\t' + diagnostic.primary.source + ':' +
                       std::to_string(diagnostic.primary.start) + '-' +
                       std::to_string(diagnostic.primary.end) + '\t' +
                       diagnostic.message;
  for (const lang::RelatedDiagnostic &related : diagnostic.related) {
    result += "\n  " + related.span.source + ':' +
              std::to_string(related.span.start) + '\t' + related.message;
  }
  for (const std::string &hint : diagnostic.hints) {
    result += "\n  hint " + hint;
  }
  return result;
}

lang::FrontendResult analyze(const std::filesystem::path &root,
                             const std::filesystem::path &entry,
                             lang::TrustedBodyAnalysis policy,
                             bool entryUnitHir) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = policy;
  options.lowerEntryUnitOnly = entryUnitHir;
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

bool applicationLocated(const lang::FrontendResult &result,
                        const lang::Diagnostic &diagnostic) {
  const lang::SourceUnitId unit =
      result.sourceGraph.sourceUnitForPath(diagnostic.primary.source);
  return unit == 0 || !result.sourceGraph.isCompilerTrusted(unit);
}

void report(const std::string &label, const std::filesystem::path &entry,
            const std::vector<std::string> &expected,
            const std::vector<std::string> &actual) {
  std::cerr << "FAIL: " << label << " differs for " << entry << '\n';
  std::vector<std::string> missing;
  std::set_difference(expected.begin(), expected.end(), actual.begin(),
                      actual.end(), std::back_inserter(missing));
  std::vector<std::string> extra;
  std::set_difference(actual.begin(), actual.end(), expected.begin(),
                      expected.end(), std::back_inserter(extra));
  for (const std::string &item : missing)
    std::cerr << "  missing: " << item << '\n';
  for (const std::string &item : extra)
    std::cerr << "  extra:   " << item << '\n';
  ++failures;
}

void checkEntry(const std::filesystem::path &root,
                const std::filesystem::path &entry) {
  const lang::FrontendResult complete =
      analyze(root, entry, lang::TrustedBodyAnalysis::Complete, false);
  std::vector<std::string> completeKeys;
  for (const lang::Diagnostic &diagnostic : complete.diagnostics)
    completeKeys.push_back(key(diagnostic));
  std::sort(completeKeys.begin(), completeKeys.end());

  const lang::FrontendResult reachable =
      analyze(root, entry, lang::TrustedBodyAnalysis::Reachable, true);
  expect(!complete.hirValid || reachable.hirValid,
         "reachable analysis preserves valid HIR for " + entry.string());
  std::vector<std::string> completeApplication;
  std::vector<std::string> completeLibrary;
  for (const lang::Diagnostic &diagnostic : complete.diagnostics)
    (applicationLocated(complete, diagnostic) ? completeApplication
                                              : completeLibrary)
        .push_back(key(diagnostic));
  std::vector<std::string> reachableApplication;
  std::vector<std::string> reachableLibrary;
  for (const lang::Diagnostic &diagnostic : reachable.diagnostics)
    (applicationLocated(reachable, diagnostic) ? reachableApplication
                                               : reachableLibrary)
        .push_back(key(diagnostic));
  std::sort(completeApplication.begin(), completeApplication.end());
  std::sort(reachableApplication.begin(), reachableApplication.end());
  std::sort(completeLibrary.begin(), completeLibrary.end());
  std::sort(reachableLibrary.begin(), reachableLibrary.end());
  if (completeApplication != reachableApplication) {
    report("reachable trusted-body application diagnostics", entry,
           completeApplication, reachableApplication);
  }
  if (!std::includes(completeLibrary.begin(), completeLibrary.end(),
                     reachableLibrary.begin(), reachableLibrary.end())) {
    report("reachable trusted-body library diagnostics (subset)", entry,
           completeLibrary, reachableLibrary);
  }
}

void measureEntry(const std::filesystem::path &root,
                  const std::filesystem::path &entry) {
  for (auto policy : {lang::TrustedBodyAnalysis::Complete,
                      lang::TrustedBodyAnalysis::Reachable}) {
    lang::FrontendWorkMetrics metrics;
    lang::FrontendOptions options;
    options.trustedBodies = policy;
    options.lowerReachableOnly = true;
    options.lowerLibraryReachableOnly = true;
    options.workMetrics = &metrics;
    const auto result = lang::Frontend(options).analyze(
        entry, std::nullopt, {root / "stdlib/prelude.gti"}, {},
        {root / "stdlib"});
    expect(result.canGenerateCode(), "measured artifact profile remains valid");
    std::cout << entry.filename().string() << " "
              << (policy == lang::TrustedBodyAnalysis::Complete ? "complete"
                                                                : "reachable")
              << " semantic_us=" << metrics.semanticMicroseconds
              << " hir_us=" << metrics.hirMicroseconds
              << " deferred=" << metrics.semanticDeferredBodies
              << " checked=" << metrics.semanticAnalyzedDeferredBodies
              << " rounds=" << metrics.semanticDemandRounds
              << " fallback=" << metrics.semanticDemandFallbacks << '\n';
  }
}
} // namespace

int main(int argc, char *argv[]) {
  const std::filesystem::path root = std::filesystem::weakly_canonical(
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path(__FILE__).parent_path().parent_path());
  checkFixtures(root);
  std::vector<std::filesystem::path> entries;
  bool measure = false;
  if (argc > 2) {
    for (int index = 2; index < argc; ++index)
      if (std::string(argv[index]) == "--measure")
        measure = true;
      else if (std::string(argv[index]) != "--fixtures")
        entries.push_back(root / argv[index]);
  } else {
    for (const char *directory : {"examples", "stdlib/tests"}) {
      const std::filesystem::path path = root / directory;
      if (!std::filesystem::is_directory(path))
        continue;
      for (const auto &item : std::filesystem::directory_iterator(path)) {
        if (item.is_regular_file() && item.path().extension() == ".gti")
          entries.push_back(item.path());
      }
    }
  }
  std::sort(entries.begin(), entries.end());
  for (const std::filesystem::path &entry : entries) {
    expect(std::filesystem::is_regular_file(entry),
           "parity entry exists: " + entry.string());
    checkEntry(root, entry);
    if (measure)
      measureEntry(root, entry);
  }
  std::cout << "checked " << entries.size()
            << " entries for trusted-body analysis parity\n";
  if (failures != 0) {
    std::cerr << failures << " parity failure(s)\n";
    return 1;
  }
  return 0;
}
