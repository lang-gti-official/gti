// gti-corpus: one walk of the example corpus that checks, per entry, the
// whole-program analysis invariants that used to be separate corpus walks.
// It runs in the daily slow tier (see docs/plans/test-suite-redesign.md §7.3).
//
// Invariants (each a named sub-check sharing the entry's analyses):
//   order    Analysing trusted bodies lexically (Complete) or all deferred to
//            the end in reverse (Deferred) gives identical diagnostics and
//            validity. (Was body_analysis_order_[0-3].)
//   trusted  The editor's Reachable policy with entry-unit HIR reports
//            exactly the complete check's application diagnostics and a
//            subset of its library diagnostics, keeps valid HIR, and a trusted
//            prefix preamble built once and adopted twice changes neither.
//            (Was trusted_body_analysis_[0-3].)
//   reuse    In the editor and artifact configurations, an analysis through
//            an instance cache, first cold then warm, equals a fresh
//            analysis: diagnostics, HIR instance records and printed MIR.
//            (Was instance_reuse_[0-3].)
// Each invariant covers the entry set its old walk covered. A violation
// fails the run; the per-entry census (instance and body counts) is a report
// in the JSON output, never a failure.
//
// Usage: gti-corpus [--root DIR] [--jobs N] [--shard I/N] [--json FILE]
//                   [--only order,trusted,reuse] [entry-substring...]

#include "gti/frontend.h"
#include "gti/frontend_work.h"
#include "gti/instance_analysis_cache.h"
#include "gti/mir_printer.h"
#include "gti/semantic_preamble.h"
#include "gti/source_graph.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

// -- Shared helpers --------------------------------------------------------

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

std::vector<std::string> sortedKeys(const lang::FrontendResult &result) {
  std::vector<std::string> keys;
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    keys.push_back(diagnosticKey(diagnostic));
  std::sort(keys.begin(), keys.end());
  return keys;
}

// The findings of one invariant on one entry.
struct Check {
  std::vector<std::string> failures;
  void expect(bool valid, const std::string &message) {
    if (!valid)
      failures.push_back(message);
  }
  void difference(const std::string &label,
                  const std::vector<std::string> &expected,
                  const std::vector<std::string> &actual) {
    std::string text = label + " differs";
    std::vector<std::string> missing;
    std::set_difference(expected.begin(), expected.end(), actual.begin(),
                        actual.end(), std::back_inserter(missing));
    std::vector<std::string> extra;
    std::set_difference(actual.begin(), actual.end(), expected.begin(),
                        expected.end(), std::back_inserter(extra));
    for (const std::string &item : missing)
      text += "\n  missing: " + item;
    for (const std::string &item : extra)
      text += "\n  extra:   " + item;
    failures.push_back(text);
  }
};

lang::FrontendResult
analyzeHir(const fs::path &root, const fs::path &entry,
           lang::TrustedBodyAnalysis policy, bool entryUnitHir = false,
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

// -- order -----------------------------------------------------------------

Check checkOrder(const fs::path &root, const fs::path &entry,
                 const lang::FrontendResult &complete) {
  Check check;
  const lang::FrontendResult deferred =
      analyzeHir(root, entry, lang::TrustedBodyAnalysis::Deferred);
  const auto completeKeys = sortedKeys(complete);
  const auto deferredKeys = sortedKeys(deferred);
  if (completeKeys != deferredKeys)
    check.difference("deferred trusted-body order diagnostics", completeKeys,
                     deferredKeys);
  check.expect(complete.semanticValid == deferred.semanticValid &&
                   complete.hirValid == deferred.hirValid,
               "deferred trusted-body order changes validity");
  return check;
}

// -- trusted ---------------------------------------------------------------

bool applicationLocated(const lang::FrontendResult &result,
                        const lang::Diagnostic &diagnostic) {
  const lang::SourceUnitId unit =
      result.sourceGraph.sourceUnitForPath(diagnostic.primary.source);
  return unit == 0 || !result.sourceGraph.isCompilerTrusted(unit);
}

void split(const lang::FrontendResult &result,
           std::vector<std::string> &application,
           std::vector<std::string> &library) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    (applicationLocated(result, diagnostic) ? application : library)
        .push_back(diagnosticKey(diagnostic));
  std::sort(application.begin(), application.end());
  std::sort(library.begin(), library.end());
}

Check checkTrusted(const fs::path &root, const fs::path &entry,
                   const lang::FrontendResult &complete) {
  Check check;
  const lang::FrontendResult reachable =
      analyzeHir(root, entry, lang::TrustedBodyAnalysis::Reachable, true);
  check.expect(!complete.hirValid || reachable.hirValid,
               "reachable analysis loses valid HIR");
  std::vector<std::string> completeApplication, completeLibrary;
  split(complete, completeApplication, completeLibrary);
  std::vector<std::string> reachableApplication, reachableLibrary;
  split(reachable, reachableApplication, reachableLibrary);
  if (completeApplication != reachableApplication)
    check.difference("reachable application diagnostics", completeApplication,
                     reachableApplication);
  if (!std::includes(completeLibrary.begin(), completeLibrary.end(),
                     reachableLibrary.begin(), reachableLibrary.end()))
    check.difference("reachable library diagnostics (subset)", completeLibrary,
                     reachableLibrary);

  // The editor configuration over a trusted prefix preamble: built on the
  // first analysis, adopted by the second; the third round is complete
  // analysis over the preamble. Rounds share a parse cache, as the editor
  // does, because the preamble is keyed by the prefix's parsed syntax.
  auto preambles = std::make_shared<lang::SemanticPreambleCache>();
  auto parses = std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
      .maximumEntries = 1024, .maximumEstimatedBytes = 512U * 1024U * 1024U});
  for (int round = 0; round < 3; ++round) {
    lang::FrontendWorkMetrics work;
    const bool completeRound = round == 2;
    const lang::FrontendResult resumed =
        analyzeHir(root, entry,
                   completeRound ? lang::TrustedBodyAnalysis::Complete
                                 : lang::TrustedBodyAnalysis::Reachable,
                   !completeRound, preambles, &work, parses);
    const std::string label = "preamble round " + std::to_string(round);
    check.expect(work.semanticPreambleBuilds == (round == 0 ? 1U : 0U) &&
                     work.semanticPreambleAdoptions != 0,
                 label + " does not build the preamble once and adopt it");
    check.expect(!complete.hirValid || resumed.hirValid,
                 label + " loses valid HIR");
    std::vector<std::string> application, library;
    split(resumed, application, library);
    if (completeApplication != application)
      check.difference(label + " application diagnostics", completeApplication,
                       application);
    if (completeRound
            ? completeLibrary != library
            : !std::includes(completeLibrary.begin(), completeLibrary.end(),
                             library.begin(), library.end()))
      check.difference(label + " library diagnostics", completeLibrary,
                       library);
  }
  return check;
}

// -- reuse -----------------------------------------------------------------

struct Caches {
  std::shared_ptr<lang::FrontendCache> frontend =
      std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
          .maximumEntries = 1024,
          .maximumEstimatedBytes = 512U * 1024U * 1024U});
  std::shared_ptr<lang::InstanceAnalysisCache> instances =
      std::make_shared<lang::InstanceAnalysisCache>();
};

enum class Mode { Editor, Artifact };

lang::FrontendResult analyzeMode(const fs::path &root, const fs::path &entry,
                                 Mode mode, Caches *caches,
                                 lang::FrontendWorkMetrics *metrics) {
  lang::FrontendOptions options;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  options.workMetrics = metrics;
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
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

using ConcreteSymbols = std::unordered_map<lang::SymbolId, lang::SymbolId>;
using LocalBindings =
    std::unordered_map<lang::SymbolId, std::vector<lang::HirBindingId>>;

// Cached concrete-only locals retain their original disjoint identity ranges;
// reserving those ranges also shifts locals in later demanded bodies. Bind
// them to the program-wide HIR binding identity, never per MIR body or by
// source spelling (generic instances can share both).
ConcreteSymbols concreteSymbols(const lang::FrontendResult &result,
                                LocalBindings &localBindings, Check &check) {
  std::unordered_set<lang::SymbolId> concrete;
  for (const auto &symbol : result.hir.instanceSymbols())
    concrete.insert(symbol.id);
  ConcreteSymbols names;
  ConcreteSymbols owners;
  const auto bindings = [&](const lang::HirBody &body) {
    for (const auto &binding : body.bindings) {
      const auto *source =
          result.semantics.database().findSymbol(binding.info.symbol);
      const bool bodyLocal =
          source && (source->kind == lang::SymbolKind::LocalVariable ||
                     source->kind == lang::SymbolKind::Parameter);
      if (!concrete.contains(binding.info.symbol) && !bodyLocal)
        continue;
      const auto canonical =
          std::numeric_limits<lang::SymbolId>::max() - binding.id;
      const auto [found, inserted] =
          names.emplace(binding.info.symbol, canonical);
      if (concrete.contains(binding.info.symbol)) {
        check.expect(inserted || found->second == canonical,
                     "one concrete symbol has several binding identities");
      } else {
        found->second = std::min(found->second, canonical);
        localBindings[binding.info.symbol].push_back(binding.id);
      }
      const auto [owner, unique] =
          owners.emplace(canonical, binding.info.symbol);
      check.expect(unique || owner->second == binding.info.symbol,
                   "distinct concrete symbols share a canonical binding");
      check.expect(!concrete.contains(canonical) &&
                       !result.semantics.database().findSymbol(canonical),
                   "a canonical test identity collides with a source symbol");
    }
  };
  bindings(result.hir.module());
  for (const auto &owner : result.hir.classInstances()) {
    bindings(owner.fieldInitializers);
    bindings(owner.staticFieldInitializers);
  }
  for (const auto &function : result.hir.functionInstances())
    bindings(function.body);
  for (const auto &constructor : result.hir.constructorInstances())
    bindings(constructor.body);
  for (const auto &destructor : result.hir.destructorInstances())
    bindings(destructor.body);
  for (const auto &lambda : result.hir.lambdaInstances())
    bindings(lambda.body);
  for (auto &[_, ids] : localBindings) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }
  return names;
}

std::string localDeclarationKey(const lang::SymbolRecord &symbol) {
  std::ostringstream key;
  key << static_cast<int>(symbol.kind) << ':' << symbol.sourceUnit << ':'
      << symbol.expansion << ':' << symbol.nameSpan.start << ':'
      << symbol.nameSpan.end << ':' << symbol.declarationSpan.start << ':'
      << symbol.declarationSpan.end << ':' << symbol.name << ':'
      << symbol.qualifiedName << ':' << symbol.mutableBinding << ':'
      << static_cast<int>(symbol.access) << ':' << symbol.staticMember;
  return key.str();
}

void canonicalizeLocalSymbols(lang::MirBody &body,
                              const ConcreteSymbols &names) {
  const auto rename = [&](lang::SymbolId &symbol) {
    if (const auto found = names.find(symbol); found != names.end())
      symbol = found->second;
  };
  for (auto &place : body.places) {
    rename(place.symbol);
    if (place.key)
      rename(place.key->root);
  }
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.ownership)
        rename(instruction.ownership->place.root);
}

// Everything a snapshot exposes downstream of semantic analysis, modulo the
// request-local body-local symbol allocation ranges.
std::string outcome(const lang::FrontendResult &result, Check &check) {
  std::ostringstream text;
  text << "semantic=" << result.semanticValid << " hir=" << result.hirValid
       << " mir=" << result.mirValid << '\n';
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    text << diagnosticKey(diagnostic) << '\n';
  text << "functions=" << result.hir.functionInstances().size()
       << " classes=" << result.hir.classInstances().size()
       << " constructors=" << result.hir.constructorInstances().size()
       << " instanceSymbols=" << result.hir.instanceSymbols().size() << '\n';
  LocalBindings localBindings;
  const auto names = concreteSymbols(result, localBindings, check);
  std::vector<std::string> symbols;
  for (const auto &[id, canonical] : names) {
    if (const auto *symbol = result.semantics.database().findSymbol(id)) {
      std::string identity = "local " + std::to_string(canonical) + ':' +
                             localDeclarationKey(*symbol) + ":bindings=";
      for (const auto binding : localBindings[id])
        identity += std::to_string(binding) + ',';
      symbols.push_back(std::move(identity));
    }
  }
  for (const lang::SymbolRecord &symbol : result.hir.instanceSymbols()) {
    const auto found = names.find(symbol.id);
    symbols.push_back(
        (found == names.end() ? "unbound" : std::to_string(found->second)) +
        ':' + std::to_string(symbol.sourceUnit) + ':' +
        std::to_string(symbol.nameSpan.start) + ' ' + symbol.qualifiedName);
  }
  std::sort(symbols.begin(), symbols.end());
  for (const std::string &symbol : symbols)
    text << "  symbol " << symbol << '\n';
  if (result.mirValid) {
    auto canonical = result.mir;
    for (const auto address : lang::enumerateMirBodyAddresses(canonical))
      canonicalizeLocalSymbols(*lang::findMirBody(canonical, address), names);
    std::ostringstream mir;
    lang::MirPrinter().print(canonical, mir);
    text << mir.str();
  }
  return text.str();
}

void compareOutcome(Check &check, const std::string &label,
                    const std::string &expected, const std::string &actual) {
  if (expected == actual)
    return;
  std::string text = label + " differs from a fresh analysis";
  std::istringstream left(expected);
  std::istringstream right(actual);
  std::string leftLine, rightLine;
  for (std::size_t line = 1, shown = 0; shown < 6; ++line) {
    const bool more = static_cast<bool>(std::getline(left, leftLine));
    const bool other = static_cast<bool>(std::getline(right, rightLine));
    if (!more && !other)
      break;
    if (leftLine != rightLine || more != other) {
      text += "\n  line " + std::to_string(line) + "\n    fresh:  " + leftLine +
              "\n    reused: " + rightLine;
      ++shown;
    }
  }
  check.failures.push_back(text);
}

struct Census {
  std::size_t functions = 0;
  std::size_t classes = 0;
  std::size_t constructors = 0;
  std::size_t mirBodies = 0;
  bool recorded = false;
};

Check checkReuse(const fs::path &root, const fs::path &entry,
                 std::size_t &reuseHits, Census &census) {
  Check check;
  for (const Mode mode : {Mode::Editor, Mode::Artifact}) {
    const std::string label =
        mode == Mode::Editor ? "editor snapshot" : "reachable artifact";
    const lang::FrontendResult freshResult =
        analyzeMode(root, entry, mode, nullptr, nullptr);
    const std::string fresh = outcome(freshResult, check);
    if (mode == Mode::Artifact) {
      census.functions = freshResult.hir.functionInstances().size();
      census.classes = freshResult.hir.classInstances().size();
      census.constructors = freshResult.hir.constructorInstances().size();
      census.mirBodies =
          freshResult.mirValid
              ? lang::enumerateMirBodyAddresses(freshResult.mir).size()
              : 0;
      census.recorded = true;
    }
    Caches caches;
    compareOutcome(
        check, label + " (first analysis through the cache)", fresh,
        outcome(analyzeMode(root, entry, mode, &caches, nullptr), check));
    lang::FrontendWorkMetrics metrics;
    compareOutcome(
        check, label + " (reused instances)", fresh,
        outcome(analyzeMode(root, entry, mode, &caches, &metrics), check));
    reuseHits += metrics.semanticInstanceReuseHits;
  }
  return check;
}

// -- Driver ----------------------------------------------------------------

enum Invariant : unsigned { Order = 1, Trusted = 2, Reuse = 4 };
constexpr std::pair<Invariant, const char *> kInvariants[] = {
    {Order, "order"}, {Trusted, "trusted"}, {Reuse, "reuse"}};

struct Entry {
  fs::path path;
  unsigned invariants = 0;
};

// Each invariant keeps the entry set of the walk it replaced.
std::vector<Entry> discover(const fs::path &root) {
  const std::pair<const char *, unsigned> directories[] = {
      {"examples", Order | Trusted | Reuse},
      {"examples/transit-planner/src", Order | Reuse},
      {"stdlib/tests/src", Order},
  };
  std::map<fs::path, unsigned> found;
  for (const auto &[directory, invariants] : directories) {
    const fs::path path = root / directory;
    std::error_code error;
    if (!fs::is_directory(path, error))
      continue;
    for (const auto &item : fs::directory_iterator(path))
      if (item.is_regular_file() && item.path().extension() == ".gti")
        found[item.path()] |= invariants;
  }
  std::vector<Entry> entries;
  for (const auto &[path, invariants] : found)
    entries.push_back({path, invariants});
  return entries;
}

std::string jsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
        out += escaped;
      } else {
        out += c;
      }
    }
  }
  return out + '"';
}

double cpuSeconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
             1e6;
}

struct WorkerResult {
  std::vector<std::string> entries;
  std::size_t reuseHits = 0;
  std::size_t failed = 0;
  std::size_t checked[3] = {0, 0, 0};
  std::size_t violations[3] = {0, 0, 0};
  double cpu[3] = {0, 0, 0};
};

std::size_t slot(Invariant invariant) {
  return invariant == Order ? 0 : invariant == Trusted ? 1 : 2;
}

// One entry, all applicable invariants; returns a JSON object and prints
// one FAIL block per violated invariant.
std::string runEntry(const fs::path &root, const Entry &entry,
                     unsigned selected, WorkerResult &totals) {
  std::size_t &failed = totals.failed;
  const std::size_t hitsBefore = totals.reuseHits;
  std::size_t &reuseHits = totals.reuseHits;
  const std::string name = entry.path.lexically_relative(root).string();
  const unsigned invariants = entry.invariants & selected;
  std::ostringstream json;
  json << "{\"entry\": " << jsonString(name) << ", \"invariants\": {";
  // order and trusted share the complete analysis.
  std::optional<lang::FrontendResult> complete;
  if (invariants & (Order | Trusted)) {
    complete =
        analyzeHir(root, entry.path, lang::TrustedBodyAnalysis::Complete);
  }
  Census census;
  bool first = true;
  for (const auto &[invariant, label] : kInvariants) {
    if (!(invariants & invariant))
      continue;
    const double start = cpuSeconds();
    Check check;
    if (invariant == Order)
      check = checkOrder(root, entry.path, *complete);
    else if (invariant == Trusted)
      check = checkTrusted(root, entry.path, *complete);
    else
      check = checkReuse(root, entry.path, reuseHits, census);
    const double seconds = cpuSeconds() - start;
    ++totals.checked[slot(invariant)];
    totals.cpu[slot(invariant)] += seconds;
    if (!check.failures.empty())
      ++totals.violations[slot(invariant)];
    json << (first ? "" : ", ") << jsonString(label)
         << ": {\"ok\": " << (check.failures.empty() ? "true" : "false")
         << ", \"cpu\": " << seconds << ", \"failures\": [";
    first = false;
    for (std::size_t index = 0; index < check.failures.size(); ++index)
      json << (index ? ", " : "") << jsonString(check.failures[index]);
    json << "]}";
    if (!check.failures.empty()) {
      ++failed;
      std::cout << "FAIL " << label << ' ' << name << '\n';
      for (const std::string &failure : check.failures) {
        std::istringstream lines(failure);
        for (std::string line; std::getline(lines, line);)
          std::cout << "  " << line << '\n';
      }
      std::cout.flush();
    }
  }
  json << "}";
  if (invariants & Reuse)
    json << ", \"reused_instances\": " << (reuseHits - hitsBefore);
  if (census.recorded)
    json << ", \"census\": {\"functions\": " << census.functions
         << ", \"classes\": " << census.classes
         << ", \"constructors\": " << census.constructors
         << ", \"mir_bodies\": " << census.mirBodies << "}";
  json << "}";
  return json.str();
}

void usage(std::ostream &out) {
  out << "usage: gti-corpus [--root DIR] [--jobs N] [--shard I/N] "
         "[--json FILE]\n"
         "                  [--only order,trusted,reuse] "
         "[entry-substring...]\n";
}

} // namespace

int main(int argc, char **argv) {
  fs::path root = fs::current_path();
  unsigned jobs = 1;
  unsigned shardIndex = 0, shardCount = 1;
  std::optional<fs::path> jsonPath;
  unsigned selected = Order | Trusted | Reuse;
  std::vector<std::string> filters;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    const bool hasValue = index + 1 < argc;
    if (argument == "--root" && hasValue) {
      root = argv[++index];
    } else if (argument == "--jobs" && hasValue) {
      jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++index])));
    } else if (argument == "--json" && hasValue) {
      jsonPath = fs::path(argv[++index]);
    } else if (argument == "--shard" && hasValue) {
      const std::string spec = argv[++index];
      const auto slash = spec.find('/');
      if (slash == std::string::npos) {
        usage(std::cerr);
        return 2;
      }
      shardIndex = static_cast<unsigned>(std::stoul(spec.substr(0, slash)));
      shardCount = static_cast<unsigned>(std::stoul(spec.substr(slash + 1)));
      if (shardCount == 0 || shardIndex >= shardCount) {
        usage(std::cerr);
        return 2;
      }
    } else if (argument == "--only" && hasValue) {
      selected = 0;
      std::istringstream names{std::string(argv[++index])};
      for (std::string name; std::getline(names, name, ',');) {
        bool known = false;
        for (const auto &[invariant, label] : kInvariants)
          if (name == label) {
            selected |= invariant;
            known = true;
          }
        if (!known) {
          std::cerr << "gti-corpus: unknown invariant " << name << '\n';
          return 2;
        }
      }
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      return 0;
    } else if (!argument.empty() && argument[0] == '-') {
      usage(std::cerr);
      return 2;
    } else {
      filters.emplace_back(argument);
    }
  }
  root = fs::weakly_canonical(root);

  std::vector<Entry> entries;
  {
    const std::vector<Entry> all = discover(root);
    for (std::size_t index = 0; index < all.size(); ++index) {
      if (index % shardCount != shardIndex)
        continue;
      const std::string name =
          all[index].path.lexically_relative(root).string();
      if (!filters.empty() && std::none_of(filters.begin(), filters.end(),
                                           [&](const std::string &filter) {
                                             return name.find(filter) !=
                                                    std::string::npos;
                                           }))
        continue;
      if (all[index].invariants & selected)
        entries.push_back(all[index]);
    }
  }
  if (entries.empty()) {
    std::cerr << "gti-corpus: no entries under " << root << '\n';
    return 2;
  }

  const auto wallStart = std::chrono::steady_clock::now();
  jobs = std::min<unsigned>(jobs, static_cast<unsigned>(entries.size()));
  std::vector<WorkerResult> results(jobs);
  if (jobs == 1) {
    for (const Entry &entry : entries)
      results[0].entries.push_back(runEntry(root, entry, selected, results[0]));
  } else {
    // Workers take every jobs-th entry and hand back their JSON through a
    // temporary file; analysis is single-threaded, so fork is safe here.
    const fs::path scratch = fs::temp_directory_path() /
                             ("gti-corpus-" + std::to_string(::getpid()));
    fs::create_directories(scratch);
    std::cout.flush();
    std::vector<pid_t> children;
    for (unsigned worker = 0; worker < jobs; ++worker) {
      const pid_t child = fork();
      if (child < 0) {
        std::perror("gti-corpus: fork");
        return 2;
      }
      if (child == 0) {
        WorkerResult result;
        for (std::size_t index = worker; index < entries.size(); index += jobs)
          result.entries.push_back(
              runEntry(root, entries[index], selected, result));
        std::ofstream out(scratch / std::to_string(worker));
        out << result.reuseHits << ' ' << result.failed;
        for (std::size_t index = 0; index < 3; ++index)
          out << ' ' << result.checked[index] << ' ' << result.violations[index]
              << ' ' << result.cpu[index];
        out << '\n';
        for (const std::string &line : result.entries)
          out << line << '\n';
        out.close();
        std::cout.flush();
        std::_Exit(out ? 0 : 3);
      }
      children.push_back(child);
    }
    bool crashed = false;
    for (unsigned worker = 0; worker < jobs; ++worker) {
      int status = 0;
      waitpid(children[worker], &status, 0);
      std::ifstream in(scratch / std::to_string(worker));
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !in) {
        std::cout << "CRASH worker " << worker << " (status " << status
                  << ")\n";
        crashed = true;
        continue;
      }
      in >> results[worker].reuseHits >> results[worker].failed;
      for (std::size_t index = 0; index < 3; ++index)
        in >> results[worker].checked[index] >>
            results[worker].violations[index] >> results[worker].cpu[index];
      std::string line;
      std::getline(in, line);
      while (std::getline(in, line))
        results[worker].entries.push_back(line);
    }
    fs::remove_all(scratch);
    if (crashed)
      return 1;
  }

  std::size_t reuseHits = 0, failed = 0;
  std::size_t checked[3] = {0, 0, 0}, violations[3] = {0, 0, 0};
  double cpu[3] = {0, 0, 0};
  std::vector<std::string> lines;
  for (const WorkerResult &result : results) {
    reuseHits += result.reuseHits;
    failed += result.failed;
    for (std::size_t index = 0; index < 3; ++index) {
      checked[index] += result.checked[index];
      violations[index] += result.violations[index];
      cpu[index] += result.cpu[index];
    }
    lines.insert(lines.end(), result.entries.begin(), result.entries.end());
  }
  std::sort(lines.begin(), lines.end());
  // The corpus must actually exercise instance reuse, or the reuse
  // invariant proves nothing (unless a filter left no reuse-eligible entry).
  const bool reuseChecked =
      (selected & Reuse) &&
      std::any_of(entries.begin(), entries.end(),
                  [](const Entry &entry) { return entry.invariants & Reuse; });
  const bool reuseVacuous =
      reuseChecked && filters.empty() && shardCount == 1 && reuseHits == 0;
  if (reuseVacuous)
    std::cout << "FAIL reuse: the corpus reused no instances\n";
  const double wall = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - wallStart)
                          .count();
  if (jsonPath) {
    std::ofstream out(*jsonPath);
    out << "{\"schema\": 1, \"root\": " << jsonString(root.string())
        << ", \"reuse_hits\": " << reuseHits << ", \"failed\": " << failed
        << ", \"wall_seconds\": " << wall << ", \"invariants\": {";
    bool firstInvariant = true;
    for (const auto &[invariant, label] : kInvariants) {
      const std::size_t index = slot(invariant);
      out << (firstInvariant ? "" : ", ") << jsonString(label)
          << ": {\"entries\": " << checked[index]
          << ", \"violations\": " << violations[index]
          << ", \"cpu\": " << cpu[index] << "}";
      firstInvariant = false;
    }
    out << "}, \"entries\": [\n";
    for (std::size_t index = 0; index < lines.size(); ++index)
      out << "  " << lines[index] << (index + 1 < lines.size() ? ",\n" : "\n");
    out << "]}\n";
  }
  for (const auto &[invariant, label] : kInvariants) {
    const std::size_t index = slot(invariant);
    if (checked[index] == 0)
      continue;
    char line[160];
    std::snprintf(line, sizeof line,
                  "%-8s %s: %zu entries, %zu violations, %.1f cpu-s",
                  violations[index] == 0 ? "ok" : "FAIL", label, checked[index],
                  violations[index], cpu[index]);
    std::cout << line << '\n';
  }
  const bool ok = failed == 0 && !reuseVacuous;
  char seconds[32];
  std::snprintf(seconds, sizeof seconds, "%.1f", wall);
  std::cout << "RESULT " << (ok ? "ok" : "fail") << ": " << entries.size()
            << " entries, " << failed << " invariant violations, " << reuseHits
            << " reused instances; " << seconds << " wall-s, " << jobs
            << " jobs\n";
  return ok ? 0 : 1;
}
