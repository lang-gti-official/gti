// Trusted-body analysis parity: the policy that lets editor snapshots skip
// standard-library bodies nothing reaches must not change what analysis
// concludes. `Reachable` (with entry-unit HIR, the editor
// configuration) must produce exactly the application-located diagnostics and
// no library-located diagnostic the complete pass did not.
#include "gti/frontend.h"
#include "gti/frontend_work.h"
#include "gti/semantic_analyzer.h"
#include "gti/semantic_preamble.h"
#include "gti/source_graph.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
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

  // Concrete callable lookup may discover a callee that the open generic body
  // cannot select. The closure rule is not specific to a standard-library type.
  const std::string genericLibrary =
      "int unused_broken() { return missing_unused; }\n"
      "int leaf() { return 42; }\n"
      "class Producer { public: Producer() = default; int operator()(int n) { "
      "return leaf() + n; } "
      "};\n"
      "int value_of<T>(T& value) { return value(0); }\n";
  const std::string consumer =
      "#include <std/cmath>\n"
      "int main() { Producer value{}; return value_of(value) - 42; }\n";
  lang::FrontendWorkMetrics metrics;
  lang::FrontendOptions options;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.workMetrics = &metrics;
  const auto generic = lang::Frontend(options).analyze(
      entry, consumer, {root / "stdlib/prelude.gti"},
      {{library.string(), genericLibrary}}, {root / "stdlib"});
  if (!generic.canGenerateCode())
    for (const auto &diagnostic : generic.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(generic.canGenerateCode(), "generic library demands reach valid MIR");
  expect(metrics.semanticDemandFallbacks == 0,
         "bounded generic demand does not require complete fallback");
  expect(metrics.semanticDemandRounds == 0 &&
             metrics.semanticLateDemandBodies != 0,
         "concrete discovery analyses the demanded body in the same "
         "analysis (late demand) instead of repeating it");

  // The editor configuration: entry-unit HIR over a reachable model. Late
  // demand must leave exactly the diagnostics a complete check reports, and
  // must fall back to a repeated analysis whenever completing the body late
  // could differ from analysing it in the pass.
  const auto editorCase = [&](const std::string &caseLabel,
                              const std::string &libraryText,
                              const std::string &source,
                              std::size_t expectedRounds,
                              bool expectedValid = true) {
    const auto run =
        [&](bool complete, lang::FrontendWorkMetrics &work,
            std::shared_ptr<lang::SemanticPreambleCache> preambles = nullptr) {
          lang::FrontendOptions editor;
          editor.stopAfter = lang::FrontendPhase::Hir;
          editor.analyzeRecoveredProgram = true;
          editor.lowerEntryUnitOnly = !complete;
          editor.lowerLibraryOnlyInstanceBodies = complete;
          editor.trustedBodies = complete
                                     ? lang::TrustedBodyAnalysis::Complete
                                     : lang::TrustedBodyAnalysis::Reachable;
          editor.workMetrics = &work;
          editor.preambleCache = std::move(preambles);
          return lang::Frontend(editor).analyze(
              entry, source, {root / "stdlib/prelude.gti"},
              {{library.string(), libraryText}}, {root / "stdlib"});
        };
    lang::FrontendWorkMetrics completeWork;
    const auto complete = run(true, completeWork);
    expect(complete.semanticValid == expectedValid &&
               complete.hirValid == expectedValid,
           caseLabel + ": complete check has the expected validity");
    // The same expectations hold over a trusted prefix preamble, whose
    // held-back body diagnostics late demand reaches instead of analysing.
    for (const bool preamble : {false, true}) {
      const std::string label = caseLabel + (preamble ? " [preamble]" : "");
      lang::FrontendWorkMetrics reachableWork;
      const auto reachable = run(
          false, reachableWork,
          preamble ? std::make_shared<lang::SemanticPreambleCache>() : nullptr);
      std::vector<std::string> completeKeys;
      std::vector<std::string> reachableKeys;
      for (const auto &diagnostic : complete.diagnostics)
        completeKeys.push_back(diagnostic.code + ' ' +
                               diagnostic.primary.source + ':' +
                               std::to_string(diagnostic.primary.start) + ' ' +
                               diagnostic.message);
      for (const auto &diagnostic : reachable.diagnostics)
        reachableKeys.push_back(diagnostic.code + ' ' +
                                diagnostic.primary.source + ':' +
                                std::to_string(diagnostic.primary.start) + ' ' +
                                diagnostic.message);
      std::sort(completeKeys.begin(), completeKeys.end());
      std::sort(reachableKeys.begin(), reachableKeys.end());
      if (completeKeys != reachableKeys) {
        std::cerr << "complete:\n";
        for (const auto &item : completeKeys)
          std::cerr << "  " << item << '\n';
        std::cerr << "reachable:\n";
        for (const auto &item : reachableKeys)
          std::cerr << "  " << item << '\n';
      }
      expect(completeKeys == reachableKeys,
             label + ": editor diagnostics equal the complete check");
      expect(complete.semanticValid == reachable.semanticValid &&
                 complete.hirValid == reachable.hirValid,
             label + ": editor validity equals the complete check");
      // Over a preamble the body's facts were complete before the plan was
      // built, so the initializer case needs no repeated analysis.
      expect(
          reachableWork.semanticDemandRounds == expectedRounds ||
              (preamble && reachableWork.semanticDemandRounds < expectedRounds),
          label + ": " + std::to_string(expectedRounds) +
              " repeated analysis round(s), got " +
              std::to_string(reachableWork.semanticDemandRounds));
      expect(reachableWork.semanticDemandFallbacks == 0,
             label + ": no complete fallback");
      // Every case reaches the body through concrete discovery first.
      expect(reachableWork.semanticLateDemandBodies != 0 ||
                 reachableWork.semanticPreambleReachedBodies != 0,
             label + ": concrete discovery attempts late demand");
    }
  };
  const std::string lateLibrary =
      "int leaf() { return 42; }\n"
      "class Producer { public: Producer() = default; int operator()(int n) { "
      "return leaf() + n; } "
      "};\n"
      "int value_of<T>(T& value) { return value(0); }\n";
  editorCase("late demand", lateLibrary,
             "#include <std/cmath>\n"
             "int main() { Producer value{}; return value_of(value) - 42; }\n",
             0);
  // The late body is invalid: invalid primary analysis never reaches HIR, so
  // the request falls back to analysing the body in a repeated pass.
  editorCase("late demand reporting an error",
             "int leaf() { return missing_late; }\n"
             "class Producer { public: Producer() = default; int "
             "operator()(int n) { return leaf() + "
             "n; } };\n"
             "int value_of<T>(T& value) { return value(0); }\n",
             "#include <std/cmath>\n"
             "int main() { Producer value{}; return value_of(value) - 42; }\n",
             1, false);
  // A global initializer's effect closure completes the consulted body before
  // validating the initialization plan. Later HIR discovery therefore needs
  // no repeated analysis to preserve those already-finalized effects.
  editorCase("late demand behind a global initializer", lateLibrary,
             "#include <std/cmath>\n"
             "Producer producer{};\n"
             "int first = value_of(producer);\n"
             "int main() { return first - 42; }\n",
             0);
  // Resolving that dependency early must still retain its source errors. An
  // invalid body requires the ordinary primary-analysis retry, just as it does
  // when reached from a function instead of a persistent initializer.
  editorCase("late demand reporting an error behind a global initializer",
             "int leaf() { return missing_late; }\n"
             "class Producer { public: Producer() = default; int "
             "operator()(int n) { return leaf() + "
             "n; } };\n"
             "int value_of<T>(T& value) { return value(0); }\n",
             "#include <std/cmath>\n"
             "Producer producer{};\n"
             "int first = value_of(producer);\n"
             "int main() { return first - 42; }\n",
             1, false);
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

lang::FrontendResult
analyze(const std::filesystem::path &root, const std::filesystem::path &entry,
        lang::TrustedBodyAnalysis policy, bool entryUnitHir,
        std::shared_ptr<lang::SemanticPreambleCache> preambles = nullptr,
        lang::FrontendWorkMetrics *metrics = nullptr,
        std::shared_ptr<lang::FrontendCache> parses = nullptr) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = policy;
  options.lowerEntryUnitOnly = entryUnitHir;
  // The editor also omits library-only instance bodies from HIR.
  options.lowerLibraryOnlyInstanceBodies = !entryUnitHir;
  options.preambleCache = std::move(preambles);
  options.cache = std::move(parses);
  options.workMetrics = metrics;
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

// Editor HIR omits the bodies of library-only instances (trusted
// declarations whose arguments name only trusted types). Each such instance
// is still analysed, so what its own analysis reports is kept; what only its
// body would request (callee and class instances) is not discovered and is
// left to the library's own check. An instance of the open file's own type is
// lowered completely and reports exactly what the complete check reports.
void checkLibraryOnlyOmission(const std::filesystem::path &root) {
  const auto entry = root / "tests/omission-entry.gti";
  const auto library = root / "stdlib/std/cmath.gti";
  // Probe<T> and checked<T> fail for every argument wider than a byte.
  const std::string declarations =
      "class Probe<T> { public: Probe() = default;\n"
      "  static_assert(sizeof(T) == uint64_t(1), \"needs a byte\");\n"
      "};\n"
      "int inner<T>(T& value) { Probe<T> probe{}; return 0; }\n"
      "int outer<T>(T& value) { return inner(value); }\n"
      "int direct<T>(T& value) { Probe<T> probe{}; return 0; }\n"
      "int checked<T>(T& value) {\n"
      "  static_assert(sizeof(T) == uint64_t(1), \"needs a byte\");\n"
      "  return 0;\n"
      "}\n"
      "int plain(int value) { return value + 1; }\n";
  const auto run = [&](const std::string &text, bool complete,
                       lang::FrontendWorkMetrics &metrics) {
    lang::FrontendOptions options;
    options.stopAfter = lang::FrontendPhase::Hir;
    options.analyzeRecoveredProgram = true;
    options.trustedBodies = complete ? lang::TrustedBodyAnalysis::Complete
                                     : lang::TrustedBodyAnalysis::Reachable;
    options.lowerEntryUnitOnly = !complete;
    options.lowerLibraryOnlyInstanceBodies = complete;
    options.workMetrics = &metrics;
    return lang::Frontend(options).analyze(
        entry, "#include <std/cmath>\n" + text, {root / "stdlib/prelude.gti"},
        {{library.string(), declarations}}, {root / "stdlib"});
  };
  const auto keys = [](const lang::FrontendResult &result) {
    std::vector<std::string> items;
    for (const lang::Diagnostic &diagnostic : result.diagnostics)
      items.push_back(key(diagnostic));
    std::sort(items.begin(), items.end());
    return items;
  };
  // Returns the complete check's diagnostics and the editor's.
  const auto compare = [&](const std::string &label, const std::string &text,
                           bool expectEqual) {
    lang::FrontendWorkMetrics completeWork;
    lang::FrontendWorkMetrics editorWork;
    const lang::FrontendResult complete = run(text, true, completeWork);
    const lang::FrontendResult editor = run(text, false, editorWork);
    expect(completeWork.hirOmittedInstanceBodies == 0,
           label + ": the complete check omits no body");
    expect(editorWork.hirOmittedInstanceBodies != 0,
           label + ": the editor omits a library-only body");
    expect(editorWork.hirFunctionInstances < completeWork.hirFunctionInstances,
           label + ": the editor discovers fewer instances");
    // Source errors found by concrete instance discovery are semantic
    // failures (src/compiler/frontend.cpp): each check is semantically valid
    // exactly when it reports no semantic error, instance diagnostics included.
    const auto semanticErrorFree = [](const lang::FrontendResult &result) {
      return std::none_of(
          result.diagnostics.begin(), result.diagnostics.end(),
          [](const lang::Diagnostic &diagnostic) {
            return diagnostic.phase == lang::DiagnosticPhase::Semantics &&
                   diagnostic.severity == lang::DiagnosticSeverity::Error;
          });
    };
    expect(editor.semanticValid == semanticErrorFree(editor) &&
               complete.semanticValid == semanticErrorFree(complete),
           label + ": semantic validity reflects exactly the reported "
                   "semantic errors");
    if (expectEqual) {
      if (keys(complete) != keys(editor))
        report(label, entry, keys(complete), keys(editor));
      expect(complete.hirValid == editor.hirValid,
             label + ": editor HIR validity equals the complete check");
    } else {
      expect(editor.hirValid,
             label + ": the editor's HIR is valid without the omitted bodies");
    }
    return std::make_pair(keys(complete), keys(editor));
  };
  // A library-only instance's own analysis still reports.
  const auto own =
      compare("library-only instance analysis",
              "int main() { int x = plain(0); return checked(x); }", true);
  expect(!own.first.empty(), "a checked library-only instance has an error");
  // A class instance that only an omitted body requests is not discovered,
  // whether the open file requests the enclosing instance directly or
  // through another library-only instance.
  for (const auto &[label, text] :
       {std::pair<std::string, std::string>{
            "class instance requested by an omitted body",
            "int main() { int x = plain(0); return direct(x); }"},
        {"class instance requested by a nested omitted body",
         "int main() { int x = 0; return outer(x); }"}}) {
    const auto result = compare(label, text, false);
    expect(!result.first.empty() && result.second.empty(),
           label + ": left to the library's own check");
  }
  // An instance of the file's own type is lowered completely and reaches
  // what the complete check reaches.
  const auto application =
      compare("instance of the file's own type",
              "class App { public: int32_t width = 0; };\n"
              "int main() { App app{}; return outer(app); }",
              true);
  expect(!application.first.empty(),
         "a nested instance of the file's own type has an error");
}

// Registration publishes each declaration's visibility per consuming unit,
// so a preamble is keyed by the whole include graph: two programs whose
// trusted prefix is the same prelude but whose later units differ must not
// share one (the second would not see the prelude's concepts from its own
// standard-library units).
void checkPreambleGraphs(const std::filesystem::path &root) {
  const auto library = root / "tests/preamble-graph-lib.gti";
  const auto first = root / "tests/preamble-graph-first.gti";
  const auto second = root / "tests/preamble-graph-second.gti";
  const std::string libraryText =
      "int helper(int value) { return value + 1; }\n";
  const std::unordered_map<std::string, std::string> overlays{
      {library.string(), libraryText},
      {first.string(), "#include \"preamble-graph-lib.gti\"\nint main() { "
                       "return helper(0) - 1; }\n"},
      {second.string(), "#include \"preamble-graph-lib.gti\"\n#include "
                        "<std/memory>\nint main() { std::unique_ptr<int> "
                        "owner = std::make_unique<int>(helper(1)); return "
                        "*owner - 2; }\n"}};
  auto preambles = std::make_shared<lang::SemanticPreambleCache>();
  auto parses = std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
      .maximumEntries = 1024, .maximumEstimatedBytes = 512U * 1024U * 1024U});
  const auto run = [&](const std::filesystem::path &entry,
                       lang::FrontendWorkMetrics &work) {
    lang::FrontendOptions options;
    options.stopAfter = lang::FrontendPhase::Hir;
    options.analyzeRecoveredProgram = true;
    options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
    options.lowerEntryUnitOnly = true;
    options.lowerLibraryOnlyInstanceBodies = false;
    options.preambleCache = preambles;
    options.cache = parses;
    options.workMetrics = &work;
    return lang::Frontend(options).analyze(entry, std::nullopt,
                                           {root / "stdlib/prelude.gti"},
                                           overlays, {root / "stdlib"});
  };
  lang::FrontendWorkMetrics firstWork;
  lang::FrontendWorkMetrics secondWork;
  const lang::FrontendResult firstResult = run(first, firstWork);
  const lang::FrontendResult secondResult = run(second, secondWork);
  expect(firstResult.semanticValid && firstResult.hirValid,
         "the first graph's program is valid");
  if (!secondResult.semanticValid)
    for (const auto &diagnostic : secondResult.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(secondResult.semanticValid && secondResult.hirValid,
         "a program with another include graph does not share the preamble");
  expect(firstWork.semanticPreambleBuilds == 1 &&
             secondWork.semanticPreambleBuilds == 1,
         "each include graph builds its own preamble");
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
  // The editor configuration over a trusted prefix preamble: built on the
  // first analysis, adopted by the second. Application diagnostics equal the
  // complete check's, library diagnostics are a subset, HIR stays valid.
  // Complete analysis over the preamble reports exactly the complete check.
  // Rounds share a parse cache large enough for the library, as the editor
  // does: the preamble is keyed by the prefix units' exact parsed syntax.
  auto preambles = std::make_shared<lang::SemanticPreambleCache>();
  auto parses = std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
      .maximumEntries = 1024, .maximumEstimatedBytes = 512U * 1024U * 1024U});
  for (int round = 0; round < 3; ++round) {
    lang::FrontendWorkMetrics work;
    const bool completeRound = round == 2;
    const lang::FrontendResult resumed =
        analyze(root, entry,
                completeRound ? lang::TrustedBodyAnalysis::Complete
                              : lang::TrustedBodyAnalysis::Reachable,
                !completeRound, preambles, &work, parses);
    const std::string label = "preamble round " + std::to_string(round);
    expect(work.semanticPreambleBuilds == (round == 0 ? 1U : 0U) &&
               work.semanticPreambleAdoptions != 0,
           label + " builds the preamble once and adopts it for " +
               entry.string());
    expect(!complete.hirValid || resumed.hirValid,
           label + " preserves valid HIR for " + entry.string());
    std::vector<std::string> application;
    std::vector<std::string> library;
    for (const lang::Diagnostic &diagnostic : resumed.diagnostics)
      (applicationLocated(resumed, diagnostic) ? application : library)
          .push_back(key(diagnostic));
    std::sort(application.begin(), application.end());
    std::sort(library.begin(), library.end());
    if (completeApplication != application)
      report(label + " application diagnostics", entry, completeApplication,
             application);
    if (completeRound) {
      if (completeLibrary != library)
        report(label + " library diagnostics", entry, completeLibrary, library);
    } else if (!std::includes(completeLibrary.begin(), completeLibrary.end(),
                              library.begin(), library.end())) {
      report(label + " library diagnostics", entry, completeLibrary, library);
    }
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
  checkLibraryOnlyOmission(root);
  checkPreambleGraphs(root);
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
