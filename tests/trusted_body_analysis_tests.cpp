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
      "export module cmath;\n"
      "int unused_broken() { return missing_unused; }\n"
      "int leaf() { return missing_reached; }\n"
      "export int relay() { return leaf(); }\n";
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
        libraryEntry ? declarations + extra : "import std.cmath;\n" + text,
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
      "export module cmath;\n"
      "int unused_broken() { return missing_unused; }\n"
      "int leaf() { return 42; }\n"
      "export class Producer { public: Producer() = default; int "
      "operator()(int n) { "
      "return leaf() + n; } "
      "};\n"
      "export int value_of<T>(T& value) { return value(0); }\n";
  const std::string consumer =
      "import std.cmath;\n"
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
              {{library.string(), "export module cmath;\n" + libraryText}},
              {root / "stdlib"});
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
      "export class Producer { public: Producer() = default; int "
      "operator()(int n) { "
      "return leaf() + n; } "
      "};\n"
      "export int value_of<T>(T& value) { return value(0); }\n";
  editorCase("late demand", lateLibrary,
             "import std.cmath;\n"
             "int main() { Producer value{}; return value_of(value) - 42; }\n",
             0);
  // The late body is invalid: invalid primary analysis never reaches HIR, so
  // the request falls back to analysing the body in a repeated pass.
  editorCase("late demand reporting an error",
             "int leaf() { return missing_late; }\n"
             "export class Producer { public: Producer() = default; int "
             "operator()(int n) { return leaf() + "
             "n; } };\n"
             "export int value_of<T>(T& value) { return value(0); }\n",
             "import std.cmath;\n"
             "int main() { Producer value{}; return value_of(value) - 42; }\n",
             1, false);
  // A global initializer's effect closure completes the consulted body before
  // validating the initialization plan. Later HIR discovery therefore needs
  // no repeated analysis to preserve those already-finalized effects.
  editorCase("late demand behind a global initializer", lateLibrary,
             "import std.cmath;\n"
             "Producer producer{};\n"
             "int first = value_of(producer);\n"
             "int main() { return first - 42; }\n",
             0);
  // Resolving that dependency early must still retain its source errors. An
  // invalid body requires the ordinary primary-analysis retry, just as it does
  // when reached from a function instead of a persistent initializer.
  editorCase("late demand reporting an error behind a global initializer",
             "int leaf() { return missing_late; }\n"
             "export class Producer { public: Producer() = default; int "
             "operator()(int n) { return leaf() + "
             "n; } };\n"
             "export int value_of<T>(T& value) { return value(0); }\n",
             "import std.cmath;\n"
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

void report(const std::string &label, const std::filesystem::path &entry,
            const std::vector<std::string> &expected,
            const std::vector<std::string> &actual);

// Constant-evaluation demand parity (#398). Each case evaluates a trusted
// constexpr definition at compile time through a different entry point, or
// reaches one from a context that cannot complete it in place. The reachable
// policy must report exactly the complete check's diagnostics and validity,
// without repeated analysis rounds, with and without a preamble; the
// `Deferred` test order must report the same. Every declaration is the
// test's own (Non-Negotiable 6): the library is an overlay of a trusted path,
// and no case depends on a standard-library type except where noted.
// These pass with constexpr bodies analysed eagerly and must keep passing
// once unused trusted constexpr bodies are deferred.
void checkConstantDemandParity(const std::filesystem::path &root) {
  const auto entry = root / "tests/demand-entry.gti";
  const auto library = root / "stdlib/std/cmath.gti";
  const auto run =
      [&](const std::string &libraryText, const std::string &source,
          lang::TrustedBodyAnalysis policy, lang::FrontendWorkMetrics &work,
          std::shared_ptr<lang::SemanticPreambleCache> preambles = nullptr,
          std::shared_ptr<lang::FrontendCache> parses = nullptr) {
        lang::FrontendOptions options;
        options.stopAfter = lang::FrontendPhase::Hir;
        options.analyzeRecoveredProgram = true;
        options.trustedBodies = policy;
        options.workMetrics = &work;
        options.preambleCache = std::move(preambles);
        options.cache = std::move(parses);
        return lang::Frontend(options).analyze(
            entry, "import std.cmath;\n" + source,
            {root / "stdlib/prelude.gti"},
            {{library.string(), "export module cmath;\n" + libraryText}},
            {root / "stdlib"});
      };
  const auto keys = [](const lang::FrontendResult &result) {
    std::vector<std::string> items;
    for (const auto &diagnostic : result.diagnostics)
      items.push_back(key(diagnostic));
    std::sort(items.begin(), items.end());
    return items;
  };
  // The complete check of one program, with its expected validity.
  const auto completeCheck = [&](const std::string &caseLabel,
                                 const std::string &libraryText,
                                 const std::string &source, bool valid) {
    lang::FrontendWorkMetrics completeWork;
    auto complete = run(libraryText, source,
                        lang::TrustedBodyAnalysis::Complete, completeWork);
    if ((complete.semanticValid && complete.hirValid) != valid)
      for (const auto &diagnostic : complete.diagnostics)
        std::cerr << caseLabel << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect((complete.semanticValid && complete.hirValid) == valid,
           caseLabel + (valid ? ": the complete check accepts the program"
                              : ": the complete check rejects the program"));
    if (!valid)
      expect(!complete.diagnostics.empty(),
             caseLabel + ": the complete check reports why");
    return complete;
  };
  // One analysis compared with the complete check of the same program.
  const auto compare = [&](const std::string &label,
                           const lang::FrontendResult &complete,
                           const lang::FrontendResult &other,
                           const lang::FrontendWorkMetrics *work) {
    const auto completeKeys = keys(complete);
    const auto otherKeys = keys(other);
    if (completeKeys != otherKeys)
      report(label + ": diagnostics", entry, completeKeys, otherKeys);
    expect(complete.semanticValid == other.semanticValid &&
               complete.hirValid == other.hirValid,
           label + ": validity equals the complete check");
    if (work != nullptr)
      expect(
          work->semanticDemandRounds == 0 && work->semanticDemandFallbacks == 0,
          label + ": no repeated analysis round, got " +
              std::to_string(work->semanticDemandRounds) + " round(s) and " +
              std::to_string(work->semanticDemandFallbacks) + " fallback(s)");
  };
  const auto parityCase = [&](const std::string &caseLabel,
                              const std::string &libraryText,
                              const std::string &source, bool valid = true) {
    const auto complete = completeCheck(caseLabel, libraryText, source, valid);
    for (const bool preamble : {false, true}) {
      const std::string label = caseLabel + (preamble ? " [preamble]" : "");
      lang::FrontendWorkMetrics reachableWork;
      const auto reachable = run(
          libraryText, source, lang::TrustedBodyAnalysis::Reachable,
          reachableWork,
          preamble ? std::make_shared<lang::SemanticPreambleCache>() : nullptr);
      compare(label, complete, reachable, &reachableWork);
    }
    // Every trusted body analysed last, in reverse order: a body is first
    // needed where it cannot be completed, and only later where it can.
    lang::FrontendWorkMetrics deferredWork;
    const auto deferred = run(
        libraryText, source, lang::TrustedBodyAnalysis::Deferred, deferredWork);
    compare(caseLabel + " [deferred order]", complete, deferred, nullptr);
  };

  // A chain of trusted constexpr callees reached only from compile time.
  const std::string chain =
      "constexpr int chain_leaf(int n) { return n * 2; }\n"
      "constexpr int chain_middle(int n) { return chain_leaf(n) + 1; }\n"
      "export constexpr int chain_root(int n) { return chain_middle(n); }\n";
  parityCase("namespace-scope static_assert through a callee chain", chain,
             "static_assert(chain_root(20) == 41);\n"
             "int main() { return 0; }\n");
  parityCase("if constexpr through a callee chain", chain,
             "int main() { if constexpr (chain_root(1) == 3) { return 0; } "
             "return 1; }\n");
  // A generic library constexpr function first instantiated, and its callee
  // first reached, from a concrete instance of a user function.
  parityCase("constexpr callee first resolved in a concrete instance",
             chain + "export constexpr int scaled<T>(int n) { "
                     "return chain_leaf(n); }\n",
             "int instance<T>() { static_assert(scaled<T>(3) == 6); "
             "return 0; }\n"
             "int main() { return instance<int32_t>(); }\n");
  // A concept predicate whose wrapper calls a trusted constexpr function.
  parityCase("concept predicate calling a trusted constexpr function",
             "constexpr bool lib_accepts() { return true; }\n"
             "export constexpr bool accepts<T>() { return lib_accepts(); }\n",
             "concept accepted<T> = accepts<T>();\n"
             "int lift<accepted T>() { return 0; }\n"
             "int main() { return lift<int32_t>(); }\n");
  // `?` in a constant evaluation selects trusted conformance operations of
  // the test's own protocol conformance, which nothing else calls.
  const std::string checkedLibrary =
      "export class checked_rejection {\n"
      "public:\n"
      "  int code;\n"
      "  constexpr checked_rejection(int code) : code(code) {}\n"
      "};\n"
      "export class checked_result {\n"
      "public:\n"
      "  int code;\n"
      "  int value;\n"
      "  constexpr checked_result(int code, int value) : code(code), "
      "value(value) {}\n"
      "};\n"
      "export for concept std::tryable<checked_result> {\n"
      "  using value_type = int;\n"
      "  using residual_type = checked_rejection;\n"
      "  constexpr bool has_value(checked_result& result) { "
      "return result.code == 0; }\n"
      "  constexpr int take_value(checked_result result) { "
      "return result.value; }\n"
      "  constexpr checked_rejection take_residual(checked_result result) { "
      "return checked_rejection(result.code); }\n"
      "};\n"
      "export constexpr checked_result validated(int v) { "
      "if (v > 0) { return checked_result(0, v); } "
      "return checked_result(v - 1, 0); }\n";
  const std::string checkedSource =
      "class status {\n"
      "public:\n"
      "  int code;\n"
      "  int value;\n"
      "  constexpr status(int value) : code(0), value(value) {}\n"
      "  implicit constexpr status(checked_rejection rejected) "
      ": code(rejected.code), value(0) {}\n"
      "};\n"
      "constexpr status doubled(int v) { "
      "int checked = ?validated(v); return status(checked * 2); }\n";
  parityCase("? propagation in a constant evaluation", checkedLibrary,
             checkedSource + "static_assert(doubled(21).value == 42);\n"
                             "static_assert(doubled(0).code == -1);\n"
                             "int main() { return 0; }\n");
  parityCase("? propagation in a failing constant evaluation", checkedLibrary,
             checkedSource + "static_assert(doubled(0).value == 42);\n"
                             "int main() { return 0; }\n",
             false);
  // A class-scope static_assert over a trusted constexpr function.
  parityCase("class-scope static_assert",
             "export constexpr int lib_size() { return 4; }\n",
             "class Fixed { public: Fixed() = default; "
             "static_assert(lib_size() == 4); int value = 0; };\n"
             "int main() { Fixed fixed{}; return fixed.value; }\n");
  // A procedural macro whose trusted helper nothing else uses.
  parityCase("procedural macro with an otherwise unused trusted helper",
             "export constexpr int lib_nine() { return 9; }\n",
             "#macro int nine() { return lib_nine(); }\n"
             "int main() { return nine!() - 9; }\n");

  // D1: trusted constexpr callees first reached at run time from a concrete
  // instance that only the program-effect closure analyses: a member of a
  // generic class calls a constrained generic constexpr function, whose
  // constraint check (a requirement attempt inside that instance) looks up
  // constexpr members of a non-generic class that nothing else reaches. The
  // effect worker completes such bodies in a body context of its own.
  const auto effectLibrary = [](const std::string &leafBody) {
    return "constexpr int effect_leaf(int n) { " + leafBody +
           " }\n"
           "export class effect_cell {\n"
           "  mut int value = 0;\n"
           "public:\n"
           "  effect_cell() = default;\n"
           "  constexpr void attach(int n) mut { this.value = effect_leaf(n); "
           "}\n"
           "  constexpr int read() { return this.value; }\n"
           "};\n"
           "export concept attachable<T> = requires(mut T& cell, int n) {\n"
           "  { cell.attach(n) } -> void;\n"
           "  { cell.read() } -> int;\n"
           "};\n"
           "export constexpr int effect_place<T>(mut T& target, int n) "
           "requires attachable<T> { target.attach(n); "
           "return target.read(); }\n"
           "export class effect_box<T> {\n"
           "  mut T inner;\n"
           "public:\n"
           "  effect_box(T inner) : inner(inner) {}\n"
           "  int fill(int n) mut { if constexpr (attachable<T>) { "
           "return effect_place(this.inner, n); } return 0; }\n"
           "};\n";
  };
  parityCase("constexpr callees first reached inside an effect-closure "
             "instance",
             effectLibrary("return n + 1;"),
             "int main() { mut effect_box<effect_cell> box{effect_cell()}; "
             "return box.fill(4) - 5; }\n");
  parityCase("constexpr callees first reached from a global initializer's "
             "effect closure",
             effectLibrary("return n + 1;"),
             "int filled() { mut effect_box<effect_cell> box{effect_cell()}; "
             "return box.fill(6); }\n"
             "int seeded = filled();\n"
             "int main() { return seeded - 7; }\n");
  parityCase("a broken constexpr callee first reached inside an "
             "effect-closure instance",
             effectLibrary("return missing_effect_leaf;"),
             "int main() { mut effect_box<effect_cell> box{effect_cell()}; "
             "return box.fill(4) - 5; }\n",
             false);

  // D3: one constexpr body needed first where it cannot be completed (the
  // concrete instance analysis of a constrained call, and with `Deferred`
  // the end-of-pass trusted bodies) and then where it can (the body phase of
  // a later application function). The second need must not be hidden by
  // the first: a member of a generic class behind a constexpr predicate,
  // the shape of a reflection query behind a concept.
  const std::string probeLibrary =
      "export class kind_probe<T> {\n"
      "public:\n"
      "  constexpr kind_probe() {}\n"
      "  constexpr bool narrow() { return sizeof(T) <= uint64_t(4); }\n"
      "};\n"
      "export constexpr bool narrow_kind<T>() { "
      "return kind_probe<T>().narrow(); }\n"
      "export concept narrow_value<T> = narrow_kind<T>();\n"
      "export int narrowed<narrow_value T>(T value) { return 1; }\n";
  const std::string probeSource =
      "int in_instance<T>(T value) { static_assert(narrow_kind<T>()); "
      "if constexpr (narrow_value<T>) { return narrowed(value); } "
      "return 0; }\n"
      "int first() { return in_instance(int32_t(3)); }\n"
      "int later() { static_assert(narrow_kind<int32_t>()); "
      "static_assert(!narrow_value<int64_t>); return 1; }\n"
      "int main() { return first() + later() - 2; }\n";
  parityCase("a constexpr body needed first by an instance, then by the "
             "body phase",
             probeLibrary, probeSource);
  parityCase("an unsatisfied constraint needed first by an instance, then "
             "by the body phase",
             probeLibrary,
             "int in_instance<T>(T value) { static_assert(narrow_kind<T>()); "
             "if constexpr (narrow_value<T>) { return narrowed(value); } "
             "return 0; }\n"
             "int first() { return in_instance(int64_t(3)); }\n"
             "int later() { static_assert(narrow_kind<int32_t>()); "
             "return 1; }\n"
             "int main() { return first() + later() - 2; }\n",
             false);

  // D4: a held-back preamble body (a constexpr predicate with `if constexpr`
  // over its callee) needed by a staged constant evaluation that a concrete
  // instance's constraint check runs, first during the program-effect
  // closure or HIR discovery rather than in the body phase.
  const std::string stagedLibrary =
      "constexpr bool staged_leaf<T>() { return sizeof(T) >= uint64_t(4); }\n"
      "constexpr bool staged_wide<T>() {\n"
      "  if constexpr (staged_leaf<T>()) { return true; } else { return false; "
      "}\n"
      "}\n"
      "export concept staged_value<T> = staged_wide<T>();\n"
      "export int staged_measure<T>(T value) requires staged_value<T> "
      "{ return 1; }\n"
      "export class staged_relay<T> {\n"
      "  T value;\n"
      "public:\n"
      "  staged_relay(T value) : value(value) {}\n"
      "  int measure() { if constexpr (staged_value<T>) { "
      "return staged_measure(this.value); } return 0; }\n"
      "};\n";
  parityCase("a held-back predicate needed by a staged evaluation in a "
             "concrete instance",
             stagedLibrary,
             "int relayed<T>(T value) { staged_relay<T> relay{value}; "
             "return relay.measure(); }\n"
             "int main() { return relayed(int32_t(5)) - 1; }\n");
  parityCase("a held-back predicate needed by a staged evaluation behind a "
             "global initializer",
             stagedLibrary,
             "int relayed<T>(T value) { staged_relay<T> relay{value}; "
             "return relay.measure(); }\n"
             "int measured = relayed(int64_t(5));\n"
             "int main() { return measured - 1; }\n");
  parityCase("a held-back predicate rejecting a concrete instance",
             stagedLibrary,
             "int relayed<T>(T value) { staged_relay<T> relay{value}; "
             "static_assert(staged_value<T>, \"too narrow\"); "
             "return relay.measure(); }\n"
             "int main() { return relayed(int8_t(5)) - 1; }\n",
             false);

  // A constant evaluation that fails inside a trusted constexpr callee:
  // every attempt that is discarded before the body is available must leave
  // no diagnostic behind, and the final attempt reports exactly the
  // complete check's failure.
  const std::string failingLibrary =
      "constexpr int failing_leaf(int n) { return 100 / n; }\n"
      "export constexpr int failing_root(int n) { "
      "return failing_leaf(n) + 1; }\n"
      "export constexpr bool failing_kind<T>() { "
      "return failing_root(int(sizeof(T)) - 4) > 0; }\n"
      "export concept failing_value<T> = failing_kind<T>();\n";
  parityCase("a failing constant evaluation at namespace scope", failingLibrary,
             "static_assert(failing_root(0) == 1);\n"
             "int main() { return 0; }\n",
             false);
  parityCase("a failing constant evaluation in an instance's constraint",
             failingLibrary,
             "int guarded<failing_value T>(T value) { return 0; }\n"
             "int main() { return guarded(int32_t(1)); }\n",
             false);
  parityCase("a failed assertion over a trusted constant", failingLibrary,
             "static_assert(failing_root(50) == 4, \"wrong constant\");\n"
             "int main() { return 0; }\n",
             false);

  // Cross-snapshot reuse (plan risk 1): a predicate over a preamble type is
  // evaluated first inside a concrete instance of one snapshot, then at
  // namespace scope in a second snapshot that adopts the same preamble. A
  // result recorded by the first (a shared concept proof, an instance) must
  // be exactly what the second computes on its own.
  const auto crossSnapshot = [&](const std::string &caseLabel,
                                 const std::string &firstSource,
                                 const std::string &secondSource,
                                 bool secondValid) {
    const auto firstComplete =
        completeCheck(caseLabel + " (first)", probeLibrary, firstSource, true);
    const auto secondComplete = completeCheck(
        caseLabel + " (second)", probeLibrary, secondSource, secondValid);
    // The snapshots share a parse cache, as the editor does: a preamble is
    // keyed by its prefix units' exact parsed syntax.
    auto preambles = std::make_shared<lang::SemanticPreambleCache>();
    auto parses =
        std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
            .maximumEntries = 1024,
            .maximumEstimatedBytes = 512U * 1024U * 1024U});
    lang::FrontendWorkMetrics firstWork;
    const auto first =
        run(probeLibrary, firstSource, lang::TrustedBodyAnalysis::Reachable,
            firstWork, preambles, parses);
    compare(caseLabel + " [first snapshot]", firstComplete, first, &firstWork);
    lang::FrontendWorkMetrics secondWork;
    const auto second =
        run(probeLibrary, secondSource, lang::TrustedBodyAnalysis::Reachable,
            secondWork, preambles, parses);
    expect(secondWork.semanticPreambleBuilds == 0 &&
               secondWork.semanticPreambleAdoptions != 0,
           caseLabel + ": the second snapshot adopts the first's preamble");
    compare(caseLabel + " [second snapshot]", secondComplete, second,
            &secondWork);
  };
  const std::string instanceSnapshot =
      "int in_instance<T>(T value) { static_assert(narrow_kind<T>()); "
      "if constexpr (narrow_value<T>) { return narrowed(value); } "
      "return 0; }\n"
      "int main() { return in_instance(int32_t(3)) - 1; }\n";
  crossSnapshot("a predicate proved in an instance, then asserted at "
                "namespace scope",
                instanceSnapshot,
                "static_assert(narrow_value<int32_t>);\n"
                "static_assert(narrow_kind<int16_t>());\n"
                "int main() { return 0; }\n",
                true);
  crossSnapshot("a predicate proved in an instance, then refuted at "
                "namespace scope",
                instanceSnapshot,
                "static_assert(narrow_value<int64_t>, \"not narrow\");\n"
                "int main() { return 0; }\n",
                false);
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
      "export module cmath;\n"
      "class Probe<T> { public: Probe() = default;\n"
      "  static_assert(sizeof(T) == uint64_t(1), \"needs a byte\");\n"
      "};\n"
      "int inner<T>(T& value) { Probe<T> probe{}; return 0; }\n"
      "export int outer<T>(T& value) { return inner(value); }\n"
      "export int direct<T>(T& value) { Probe<T> probe{}; return 0; }\n"
      "export int checked<T>(T& value) {\n"
      "  static_assert(sizeof(T) == uint64_t(1), \"needs a byte\");\n"
      "  return 0;\n"
      "}\n"
      "export int plain(int value) { return value + 1; }\n";
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
        entry, "import std.cmath;\n" + text, {root / "stdlib/prelude.gti"},
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
              "class App { public: App() = default; int32_t width = 0; };\n"
              "int main() { App app{}; return outer(app); }",
              true);
  expect(!application.first.empty(),
         "a nested instance of the file's own type has an error");
}

// Registration publishes each declaration's visibility per consuming unit,
// so a preamble is keyed by the whole import graph: two programs whose
// trusted prefix is the same prelude but whose later units differ must not
// share one (the second would not see the prelude's concepts from its own
// standard-library units).
void checkPreambleGraphs(const std::filesystem::path &root) {
  const auto library = root / "tests/preamble-graph-lib.gti";
  const auto first = root / "tests/preamble-graph-first.gti";
  const auto second = root / "tests/preamble-graph-second.gti";
  const std::string libraryText =
      "export module preamble_graph_lib;\nexport int helper(int value) { "
      "return value + 1; }\n";
  const std::unordered_map<std::string, std::string> overlays{
      {library.string(), libraryText},
      {first.string(), R"GTIMODULE(export module preamble_graph_first;
import "preamble-graph-lib.gti";
export int main() { return helper(0) - 1; }
)GTIMODULE"},
      {second.string(), R"GTIMODULE(export module preamble_graph_second;
import "preamble-graph-lib.gti";
import std.memory;
export int main() { std::unique_ptr<int> owner = std::make_unique<int>(helper(1)); return *owner - 2; }
)GTIMODULE"}};
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
         "a program with another import graph does not share the preamble");
  expect(firstWork.semanticPreambleBuilds == 1 &&
             secondWork.semanticPreambleBuilds == 1,
         "each import graph builds its own preamble");
}

void checkEntry(const std::filesystem::path &root,
                const std::filesystem::path &entry) {
  const lang::FrontendResult complete =
      analyze(root, entry, lang::TrustedBodyAnalysis::Complete, false);
  std::vector<std::string> completeKeys;
  for (const lang::Diagnostic &diagnostic : complete.diagnostics)
    completeKeys.push_back(key(diagnostic));
  std::sort(completeKeys.begin(), completeKeys.end());

  // A reachable analysis completes every body it needs in the same pass: no
  // repeated round and no complete fallback, with or without a preamble.
  const auto expectNoRounds = [&](const std::string &label,
                                  const lang::FrontendWorkMetrics &work) {
    expect(work.semanticDemandRounds == 0 && work.semanticDemandFallbacks == 0,
           label + " needs no repeated analysis round for " + entry.string() +
               ", got " + std::to_string(work.semanticDemandRounds) +
               " round(s) and " + std::to_string(work.semanticDemandFallbacks) +
               " fallback(s)");
  };
  lang::FrontendWorkMetrics reachableWork;
  const lang::FrontendResult reachable =
      analyze(root, entry, lang::TrustedBodyAnalysis::Reachable, true, nullptr,
              &reachableWork);
  expectNoRounds("reachable analysis", reachableWork);
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
    expectNoRounds(label, work);
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
  checkConstantDemandParity(root);
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
