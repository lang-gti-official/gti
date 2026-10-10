// Body analysis order: what semantic analysis concludes about a program must
// not depend on the order in which function bodies are analysed, beyond the
// language's own lexical rules (a namespace global or constexpr definition is
// usable only after its declaration).
//
// Two kinds of check. Fixtures written here exercise the rules directly:
// caller-before-callee against callee-before-caller for a body-derived fact,
// global visibility across an on-demand analysis, constexpr availability, and
// recursive demand. The corpus gate then analyses every example twice,
// lexically (`Complete`) and with every trusted body deferred to the end in
// reverse order (`Deferred`), and requires identical diagnostics.
#include "gti/frontend.h"
#include "gti/source_graph.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

std::vector<std::string> keys(const lang::FrontendResult &result) {
  std::vector<std::string> keys;
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    keys.push_back(key(diagnostic));
  std::sort(keys.begin(), keys.end());
  return keys;
}

lang::FrontendResult analyze(
    const std::filesystem::path &root, const std::filesystem::path &entry,
    lang::TrustedBodyAnalysis policy = lang::TrustedBodyAnalysis::Complete) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = policy;
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
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

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &d) { return d.code == code; });
}

// Writes `source` under `directory` and analyses it as a standalone root.
lang::FrontendResult analyzeSource(const std::filesystem::path &root,
                                   const std::filesystem::path &directory,
                                   const std::string &name,
                                   const std::string &source) {
  const std::filesystem::path path = directory / name;
  std::ofstream(path) << source;
  return analyze(root, path);
}

const char *const kGenericFind =
    R"(Iterator my_find<Iterator, Sentinel, T>(mut Iterator first, Sentinel last,
                                        T& value)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::dereferences_to<Iterator, T> &&
           std::equality_comparable<T> {
  while (first != last) {
    if (*first == value) {
      break;
    }
    ++first;
  }
  return std::move(first);
}
)";

const char *const kFindCaller = R"(int main() {
  std::list<int> priorities = {2, 4, 4, 9};
  int urgent = 9;
  auto found = my_find(priorities.begin(), priorities.end(), urgent);
  return found != priorities.end() ? 0 : 1;
}
)";

void checkFixtures(const std::filesystem::path &root,
                   const std::filesystem::path &directory) {
  const std::string includes =
      "#include <std/iterator>\n#include <std/list>\n\n";

  // The moved-return fact of a generic callee: both orders accept the program
  // and conclude the same thing.
  const auto calleeFirst =
      analyzeSource(root, directory, "callee-first.gti",
                    includes + kGenericFind + "\n" + kFindCaller);
  const auto callerFirst =
      analyzeSource(root, directory, "caller-first.gti",
                    includes + kFindCaller + "\n" + kGenericFind);
  expect(calleeFirst.diagnostics.empty() && calleeFirst.hirValid,
         "callee-first generic moved return is accepted");
  expect(callerFirst.diagnostics.empty() && callerFirst.hirValid,
         "caller-first generic moved return is accepted");

  // A later callee analysed on demand sees a global declared before it, even
  // though the global follows the caller.
  const auto globalBetween = analyzeSource(
      root, directory, "global-between.gti",
      includes + kFindCaller + "constexpr int limit = 9;\n" +
          std::string(kGenericFind)
              .replace(
                  std::string(kGenericFind).find("return std::move(first);"),
                  std::string("return std::move(first);").size(),
                  "if (limit > 0) { return std::move(first); }\n"
                  "  return std::move(first);"));
  expect(globalBetween.diagnostics.empty(),
         "a later callee sees a global declared before it");

  // ... but never one declared after its own position, and the caller that
  // precedes the global still cannot see it either.
  const auto globalAfter = analyzeSource(
      root, directory, "global-after.gti",
      includes + kFindCaller + "\n" +
          std::string(kGenericFind)
              .replace(
                  std::string(kGenericFind).find("return std::move(first);"),
                  std::string("return std::move(first);").size(),
                  "if (limit > 0) { return std::move(first); }\n"
                  "  return std::move(first);") +
          "constexpr int limit = 9;\n");
  expect(hasCode(globalAfter, "GTI-S2001"),
         "a callee cannot see a global declared after it");
  const auto callerBeforeGlobal = analyzeSource(
      root, directory, "caller-before-global.gti",
      includes +
          "int main() { return limit - 9; }\nconstexpr int limit = 9;\n");
  expect(hasCode(callerBeforeGlobal, "GTI-S2001"),
         "a body cannot see a global declared after it");

  // Constexpr availability keeps its lexical rule even when the definition
  // has been analysed early by a demand.
  const auto constexprLater =
      analyzeSource(root, directory, "constexpr-later.gti",
                    includes + "static_assert(answer() == 42, \"later\");\n"
                               "constexpr int answer() { return 42; }\n"
                               "int main() { return 0; }\n");
  expect(hasCode(constexprLater, "GTI-S2057"),
         "a constexpr definition declared later is still unavailable");
  const auto constexprEarlier =
      analyzeSource(root, directory, "constexpr-earlier.gti",
                    includes + "constexpr int answer() { return 42; }\n"
                               "static_assert(answer() == 42, \"earlier\");\n"
                               "int main() { return 0; }\n");
  expect(constexprEarlier.diagnostics.empty(),
         "a constexpr definition declared earlier is available");

  // Diagnostics read in source order even when a demand analysed the callee
  // in the middle of the caller.
  const auto orderedDiagnostics = analyzeSource(
      root, directory, "diagnostic-order.gti",
      includes +
          std::string(kFindCaller)
              .replace(std::string(kFindCaller).find("? 0 : 1"),
                       std::string("? 0 : 1").size(),
                       "? 0 : missing_in_caller") +
          "\n" +
          std::string(kGenericFind)
              .replace(
                  std::string(kGenericFind).find("return std::move(first);"),
                  std::string("return std::move(first);").size(),
                  "int oops = missing_in_callee;\n  return std::move(first);"));
  expect(orderedDiagnostics.diagnostics.size() == 2 &&
             orderedDiagnostics.diagnostics[0].primary.line <
                 orderedDiagnostics.diagnostics[1].primary.line,
         "demanded callee diagnostics follow the caller's");

  // Mutual recursion between two carrier-returning generics terminates and
  // is accepted in either order.
  const char *const ping =
      R"(Iterator ping<Iterator, Sentinel, T>(mut Iterator first, Sentinel last, T& value, int depth)
  requires std::input_iterator<Iterator> && std::sentinel_for<Sentinel, Iterator> &&
           std::dereferences_to<Iterator, T> && std::equality_comparable<T> {
  if (depth > 0) { return pong(std::move(first), last, value, depth - 1); }
  return std::move(first);
}
)";
  const char *const pong =
      R"(Iterator pong<Iterator, Sentinel, T>(mut Iterator first, Sentinel last, T& value, int depth)
  requires std::input_iterator<Iterator> && std::sentinel_for<Sentinel, Iterator> &&
           std::dereferences_to<Iterator, T> && std::equality_comparable<T> {
  if (depth > 0) { return ping(std::move(first), last, value, depth - 1); }
  return std::move(first);
}
)";
  const char *const recursiveCaller = R"(int main() {
  std::list<int> values = {1, 2};
  int wanted = 2;
  auto cursor = ping(values.begin(), values.end(), wanted, 3);
  return cursor != values.end() ? 0 : 1;
}
)";
  const auto recursiveAfter =
      analyzeSource(root, directory, "recursive-after.gti",
                    includes + recursiveCaller + ping + pong);
  const auto recursiveBefore =
      analyzeSource(root, directory, "recursive-before.gti",
                    includes + ping + pong + recursiveCaller);
  expect(keys(recursiveAfter).size() == keys(recursiveBefore).size(),
         "mutually recursive demand terminates with the same outcome");
}

void checkCorpusEntry(const std::filesystem::path &root,
                      const std::filesystem::path &entry) {
  const lang::FrontendResult complete =
      analyze(root, entry, lang::TrustedBodyAnalysis::Complete);
  const lang::FrontendResult deferred =
      analyze(root, entry, lang::TrustedBodyAnalysis::Deferred);
  const auto completeKeys = keys(complete);
  const auto deferredKeys = keys(deferred);
  if (completeKeys != deferredKeys ||
      complete.semanticValid != deferred.semanticValid ||
      complete.hirValid != deferred.hirValid) {
    report("deferred trusted-body order", entry, completeKeys, deferredKeys);
  }
}
} // namespace

int main(int argc, char *argv[]) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("gti-body-order-" + std::to_string(::getpid()));
  std::filesystem::create_directories(scratch);
  checkFixtures(root, scratch);
  std::filesystem::remove_all(scratch);

  std::vector<std::filesystem::path> entries;
  for (const char *directory :
       {"examples", "examples/transit-planner/src", "stdlib/tests/src"}) {
    const std::filesystem::path path = root / directory;
    if (!std::filesystem::is_directory(path))
      continue;
    for (const auto &item : std::filesystem::directory_iterator(path)) {
      if (item.is_regular_file() && item.path().extension() == ".gti")
        entries.push_back(item.path());
    }
  }
  std::sort(entries.begin(), entries.end());
  for (const std::filesystem::path &entry : entries) {
    checkCorpusEntry(root, entry);
  }
  std::cout << "checked " << entries.size()
            << " corpus entries for body-analysis order parity\n";
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
