#include "gti/frontend.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
struct Probe {
  const char *name;
  const char *api;
  const char *body;
  bool valid;
};
void testPreambleGlobalDeclarationParity() {
  using namespace lang;
  const auto root = std::filesystem::current_path() / "global-cache-fixture";
  const auto api = root / "api.gti", body = root / "body.gti";
  const Probe probes[]{
      {"function-before-variable", "export int value(){return 2;}",
       "int value=1;", false},
      {"external-function", "export int value=1;",
       "int value(){return 2;} int use(){return value();}", false},
      {"private-function", "int value=1;",
       "int value(){return 2;} int use(){return value();}", false},
      {"external-static-function", "export int value=1;",
       "static int value(){return 2;}", true},
      {"static-file-separation", "static int value=1;",
       "int value(){return 2;} int use(){return value();}", false},
      {"external-class", "export int value=1;", "struct value {};", false},
      {"external-alias", "export int value=1;", "using value = int;", false},
      {"external-enum", "export int value=1;", "enum value { one };", false},
      {"external-namespace", "export int value=1;",
       "namespace value { int get(){return 0;} }", false},
      {"overload", "export int pick(int value){return value;}",
       "int pick(bool value){return value?1:0;}", true},
      {"read-static-field", "export struct Holder { static int value=1; };",
       "int read(){return Holder::value;}", true},
      {"write-global", "export mut int value=1;",
       "int read(){value=2; return value;}", true},
  };

  for (const auto &probe : probes) {
    SourceOwnerDescriptor owner{.identity = "owner",
                                .name = "owner",
                                .packageRoot = root,
                                .sourceRoot = root,
                                .modules = {{"api", api}},
                                .moduleImplementations = {{"api", {body}}}};
    std::unordered_map<std::string, std::string> sources{
        {api.string(), std::string("export module api; ") + probe.api},
        {body.string(),
         std::string("module api; ") + probe.body + " int main(){return 0;}"}};
    FrontendOptions cached;
    cached.suggestIncludes = false;
    cached.cache = std::make_shared<FrontendCache>();
    cached.preambleCache = std::make_shared<SemanticPreambleCache>();
    std::vector<std::string> baseline;
    for (int mode = 0; mode < 3; ++mode) {
      FrontendWorkMetrics work;
      cached.workMetrics = &work;
      FrontendOptions fresh;
      fresh.suggestIncludes = false;
      auto result = Frontend(mode ? cached : fresh)
                        .analyze(body, sources.at(body.string()), {api},
                                 sources, {}, {owner});
      std::vector<std::string> diagnostics;
      for (const auto &diagnostic : result.diagnostics)
        diagnostics.push_back(diagnostic.code + ":" + diagnostic.message + ":" +
                              diagnostic.primary.source + ":" +
                              std::to_string(diagnostic.primary.start) + ":" +
                              std::to_string(diagnostic.primary.end));
      std::sort(diagnostics.begin(), diagnostics.end());
      if (mode == 0)
        baseline = diagnostics;
      if (result.semanticValid != probe.valid || diagnostics != baseline)
        std::cerr << "global declaration parity: " << probe.name
                  << " mode=" << mode << '\n';
      expect(result.semanticValid == probe.valid && diagnostics == baseline,
             "fresh and resumed namespace/static declarations use identical "
             "collision rules");
      if (mode != 0)
        expect(work.semanticPreambleAdoptions == 1 &&
                   work.semanticPreambleBuilds == (mode == 1 ? 1U : 0U),
               "valid prefix state is reused even when resumed declarations "
               "collide");
    }
    cached.workMetrics = nullptr;
    auto repaired = sources;
    repaired[body.string()] = "module api; int main(){return 0;}";
    const auto restored = Frontend(cached).analyze(
        body, repaired.at(body.string()), {api}, repaired, {}, {owner});
    expect(
        restored.canGenerateCode(),
        "discarding one invalid adoption does not poison later valid requests");
  }
}

void testPreambleGlobalOwnerParity() {
  using namespace lang;
  const auto root =
      std::filesystem::current_path() / "global-owner-cache-fixture";
  const auto a = root / "a.gti", b = root / "b.gti", entry = root / "main.gti";
  std::unordered_map<std::string, std::string> sources{
      {a.string(),
       "export module a; namespace shared { int secret=1; static int local=3; "
       "export int value=7; export int readA(){return secret+local;} }"},
      {b.string(),
       "export module b; namespace shared { int secret=2; static int local=4; "
       "export int value=8; export int readB(){return secret+local;} }"}};
  FrontendOptions cache;
  cache.suggestIncludes = false;
  cache.cache = std::make_shared<FrontendCache>();
  cache.preambleCache = std::make_shared<SemanticPreambleCache>();
  const char *bodies[] = {"return shared::readA();", "return shared::secret;",
                          "return shared::readA()+shared::readB();",
                          "return shared::value;", "return shared::readA();"};
  for (int i = 0; i < 5; i++) {
    std::string text = "import \"a.gti\"; ";
    if (i == 2 || i == 3)
      text += "import \"b.gti\"; ";
    text += "int main(){";
    text += bodies[i];
    text += "}";
    std::vector<std::string> baseline;
    for (int cached = 0; cached < 2; cached++) {
      FrontendOptions fresh;
      fresh.suggestIncludes = false;
      auto r =
          Frontend(cached ? cache : fresh).analyze(entry, text, {a}, sources);
      std::vector<std::string> diagnostics;
      for (const auto &diagnostic : r.diagnostics)
        diagnostics.push_back(diagnostic.code + ":" + diagnostic.message + ":" +
                              diagnostic.primary.source + ":" +
                              std::to_string(diagnostic.primary.start) + ":" +
                              std::to_string(diagnostic.primary.end));
      std::sort(diagnostics.begin(), diagnostics.end());
      if (!cached)
        baseline = diagnostics;
      expect(r.semanticValid == (i != 1 && i != 3) && diagnostics == baseline,
             "resumed globals retain private owner identity and all visible "
             "ambiguity candidates");
    }
  }
}

void testPreambleLifetimeConformanceParity() {
  using namespace lang;
  const auto root = std::filesystem::current_path() / "lifetime-preamble-fixture";
  const auto prefix = root / "prefix.gti", api = root / "measure.gti";
  const auto entry = root / "main.gti";
  const std::unordered_map<std::string, std::string> sources{
      {prefix.string(), "export module prefix; export struct Seed {};"},
      {api.string(), R"GTI(export module measure;
export concept measurable<T> {
  int measure(T& item);
  bool is_empty(T& item);
};
export concept selecting<T> {
  T&<a> select<lifetime a, lifetime b>(T&<a> left, T&<b> right);
};
export concept inspecting<T> {
  int inspect<lifetime a, lifetime b>(T&<a> left, T&<b> right);
};
)GTI"}};
  const Probe probes[]{
      {"elided-reference-operations", "", R"GTI(
for concept measurable<Rope> {
  int measure(Rope& item) { return 1; }
  bool is_empty(Rope& item) { return false; }
};
)GTI", true},
      {"alpha-renamed-operation", "", R"GTI(
for concept selecting<Rope> {
  Rope&<chosen> select<lifetime chosen, lifetime other>(
      Rope&<chosen> left, Rope&<other> right) { return left; }
};
)GTI", true},
      {"wrong-result-relationship", "", R"GTI(
for concept selecting<Rope> {
  Rope&<other> select<lifetime chosen, lifetime other>(
      Rope&<chosen> left, Rope&<other> right) { return right; }
};
)GTI", false},
      {"stronger-operation-requirement", "", R"GTI(
for concept inspecting<Rope> {
  int inspect<lifetime a, lifetime b>(Rope&<a> left, Rope&<b> right)
      requires outlives<a, b> { return 0; }
};
)GTI", false},
      {"static-input-strengthening", "", R"GTI(
for concept measurable<Rope> {
  int measure(Rope&<static> item) { return 1; }
  bool is_empty(Rope& item) { return false; }
};
)GTI", false},
  };
  FrontendOptions cached;
  cached.stopAfter = FrontendPhase::Semantics;
  cached.suggestIncludes = false;
  cached.cache = std::make_shared<FrontendCache>();
  cached.preambleCache = std::make_shared<SemanticPreambleCache>();
  bool builtPrefix = false;
  for (const auto &probe : probes) {
    const std::string source = "import \"measure.gti\"; struct Rope {}; " +
                               std::string(probe.body);
    std::vector<std::string> baseline;
    for (int mode = 0; mode != 3; ++mode) {
      FrontendOptions options = cached;
      FrontendWorkMetrics work;
      options.workMetrics = &work;
      if (mode == 0)
        options.preambleCache.reset();
      const auto result = Frontend(options).analyze(entry, source, {prefix}, sources);
      std::vector<std::string> diagnostics;
      for (const auto &diagnostic : result.diagnostics)
        diagnostics.push_back(diagnostic.code + ":" + diagnostic.message);
      std::sort(diagnostics.begin(), diagnostics.end());
      if (mode == 0)
        baseline = diagnostics;
      if (result.semanticValid != probe.valid || diagnostics != baseline)
        std::cerr << "lifetime conformance parity: " << probe.name
                  << " mode=" << mode << '\n';
      expect(result.semanticValid == probe.valid && diagnostics == baseline,
             "new nominal operation contracts agree under cold and resumed registration");
      if (!probe.valid)
        expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                 return diagnostic.code == "GTI-S2123";
               }),
               "preamble reuse preserves incompatible lifetime operation rejection");
      if (mode != 0) {
        expect(work.semanticPreambleAdoptions == 1 &&
                   work.semanticPreambleBuilds == (builtPrefix ? 0U : 1U),
               "lifetime conformance checks exercise real immutable prefix reuse");
        builtPrefix = true;
      }
    }
  }
}

void testCancelledPreambleDoesNotPublish() {
  using namespace lang;
  const auto root =
      std::filesystem::current_path() / "cancelled-preamble-fixture";
  const auto api = root / "api.gti", entry = root / "main.gti";
  const std::string text = "int main() { return library_value(); }";
  const std::unordered_map<std::string, std::string> sources{
      {api.string(), "export module api; "
                     "export int library_value() { return 42; }"}};
  std::atomic<bool> cancellation{true};
  FrontendWorkMetrics metrics;
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  options.suggestIncludes = false;
  options.requireEntryPoint = true;
  options.cache = std::make_shared<FrontendCache>();
  options.preambleCache = std::make_shared<SemanticPreambleCache>();
  options.cancellation = &cancellation;
  options.workMetrics = &metrics;
  const auto analyze = [&] {
    return Frontend(options).analyze(entry, text, {api}, sources);
  };

  const auto cancelled = analyze();
  expect(cancelled.cancelled && options.preambleCache->stats().entries == 0 &&
             options.preambleCache->stats().stores == 0 &&
             metrics.semanticPreambleBuilds == 0,
         "a cancelled cold prefix must stop without publishing a preamble");

  cancellation = false;
  const auto recovered = analyze();
  expect(!recovered.cancelled && recovered.semanticValid &&
             recovered.diagnostics.empty() &&
             options.preambleCache->stats().entries == 1 &&
             options.preambleCache->stats().stores == 1 &&
             metrics.semanticPreambleBuilds == 1,
         "a fresh analysis after cancellation builds a complete valid prefix");

  cancellation = true;
  const auto warmCancelled = analyze();
  expect(warmCancelled.cancelled &&
             options.preambleCache->stats().entries == 1 &&
             options.preambleCache->stats().stores == 1 &&
             metrics.semanticPreambleBuilds == 0,
         "cancelling a resumed analysis preserves its complete cached prefix");

  cancellation = false;
  const auto resumed = analyze();
  expect(!resumed.cancelled && resumed.semanticValid &&
             resumed.diagnostics.empty() &&
             metrics.semanticPreambleBuilds == 0 &&
             metrics.semanticPreambleAdoptions == 1,
         "later requests still reuse the complete prefix after cancellation");

  auto prefix =
      Frontend({.stopAfter = FrontendPhase::Parsing})
          .analyze("cancelled-prefix.gti", "int value() { return 1; }");
  SemanticVisitor builder(TargetInfo::host());
  cancellation = true;
  builder.setCancellation(&cancellation);
  const auto incomplete = builder.buildPreamble(std::move(prefix.program));
  expect(incomplete == nullptr && builder.cancelled(),
         "direct preamble building catches cancellation and returns no state");
}

} // namespace

int main() {
  testCancelledPreambleDoesNotPublish();
  testPreambleGlobalDeclarationParity();
  testPreambleGlobalOwnerParity();
  testPreambleLifetimeConformanceParity();
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto root = std::filesystem::current_path() / "module-preamble-fixture";
  const auto api = root / "api.gti", body = root / "body.gti";
  const auto extra = root / "extra.gti";
  lang::SourceOwnerDescriptor owner{.identity = "fixture",
                                    .name = "fixture",
                                    .packageRoot = root,
                                    .sourceRoot = root,
                                    .modules = {{"api", api}, {"extra", extra}},
                                    .moduleImplementations = {{"api", {body}}}};
  const std::unordered_map<std::string, std::string> sources{
      {api.string(), R"GTI(export module api;
export int answer();
export constexpr int choose(int value) { return value; }
export struct Box<T> { T value; T read(); };
export T identity<T>(T value);
static constexpr int private_seed() { return 5; }
export constexpr int seed() { return private_seed(); }
export int collision() { return 0; }
)GTI"},
      {extra.string(), "export module extra; export int collision = 1;"}};
  auto syntax = std::make_shared<lang::FrontendCache>();
  auto reused = std::make_shared<lang::SemanticPreambleCache>();
  struct Case {
    std::string definition;
    std::string expectedError;
  };
  const Case cases[]{
      {"int answer(){return 41;}", ""},
      {"int answer(){return 42;}", ""},
      {"", "GTI-S2106"},
      {"bool answer(){return true;}", "GTI-S2011"},
      {"int answer(){return 41;}", "GTI-S2006"},
      {"int answer(){return 41;}", ""},
  };
  for (std::size_t round = 0; round < std::size(cases); ++round) {
    const auto &test = cases[round];
    const std::string entry =
        "module api; import extra; " + test.definition + R"GTI(
constexpr int choose(bool value) { return value ? 7 : 0; }
U Box<U>::read() { return value; }
V identity<V>(V value) { return value; }
static_assert(choose(true) == 7);
static_assert(seed() == 5);
)GTI" +
        (test.expectedError == "GTI-S2006" ? "int main() { return collision; }"
                                           : R"GTI(
int main() { Box<int> box{.value=42}; return answer() + box.read() + identity(1); }
)GTI");
    for (int mode = 0; mode != 3; ++mode) {
      lang::FrontendWorkMetrics work;
      lang::FrontendOptions options;
      options.cache = syntax;
      options.preambleCache =
          mode == 0   ? nullptr
          : mode == 1 ? std::make_shared<lang::SemanticPreambleCache>()
                      : reused;
      options.workMetrics = &work;
      options.requireFunctionDefinitions = true;
      options.requireEntryPoint = true;
      options.suggestIncludes = false;
      const auto result = lang::Frontend(options).analyze(
          body, entry, {stdlib / "prelude.gti", api}, sources, {stdlib},
          {owner});
      if (test.expectedError.empty() && !result.canGenerateCode())
        std::cerr << "case " << round << ", cache mode " << mode << '\n';
      if (test.expectedError.empty() && !result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      if (test.expectedError.empty()) {
        expect(result.canGenerateCode(),
               "free/member/generic split bodies and new overloads survive "
               "preamble copying");
        for (const auto &statement : result.program.declarations()) {
          const auto *function =
              dynamic_cast<const lang::FunctionDecl *>(statement.get());
          if (function && function->name().lexeme == "answer" &&
              function->body()) {
            const auto *resolved = result.semantics.findFunction(*function);
            expect(resolved && resolved->bodyDeclaration() == function,
                   "each valid overlay selects its current body syntax, not a "
                   "cached body");
          }
        }
      } else
        expect(!result.semanticValid &&
                   std::any_of(result.diagnostics.begin(),
                               result.diagnostics.end(),
                               [&](const auto &diagnostic) {
                                 return diagnostic.code == test.expectedError;
                               }),
               "a previous implementation body must not leak into the "
               "immutable preamble");
      if (mode == 2 && round != 0)
        expect(work.semanticPreambleBuilds == 0 &&
                   work.semanticPreambleAdoptions == 1,
               "alternate valid/invalid implementation overlays reuse the "
               "immutable preamble");
    }
  }
  // Prefix-only constexpr validation needs the later definition. Declining
  // this cache entry must preserve the full program's otherwise valid meaning.
  auto incomplete = sources;
  incomplete[api.string()] =
      "export module api; export constexpr int answer();";
  for (int value : {41, 42}) {
    lang::FrontendWorkMetrics work;
    lang::FrontendOptions options;
    options.cache = syntax;
    options.preambleCache = reused;
    options.workMetrics = &work;
    options.requireFunctionDefinitions = true;
    options.requireEntryPoint = true;
    options.suggestIncludes = false;
    const std::string entry =
        "module api; constexpr int answer(){return " + std::to_string(value) +
        ";} static_assert(answer()==" + std::to_string(value) +
        "); int main(){return answer();}";
    const auto result = lang::Frontend(options).analyze(
        body, entry, {stdlib / "prelude.gti", api}, incomplete, {stdlib},
        {owner});
    expect(result.canGenerateCode() && work.semanticPreambleAdoptions == 0,
           "an incomplete constexpr prefix falls back to complete program "
           "analysis");
  }
  // A prefix global forces an enum-return query and retains a reusable worker
  // catalog. The resumed body adds declarations and asks for a different key;
  // it must never reuse the prefix's catalog or mutate its query journal.
  const std::unordered_map<std::string, std::string> borrowing{
      {api.string(), R"GTI(export module api;
export enum class Choice { one };
export Choice pick<T>(T value) { return Choice::one; }
Choice early = pick(1);
)GTI"}};
  auto borrowCache = std::make_shared<lang::SemanticPreambleCache>();
  for (int round = 0; round != 3; ++round) {
    const bool invalid = round == 1;
    for (int mode = 0; mode != 3; ++mode) {
      lang::FrontendWorkMetrics work;
      lang::FrontendOptions options;
      options.cache = syntax;
      options.preambleCache =
          mode == 0   ? nullptr
          : mode == 1 ? std::make_shared<lang::SemanticPreambleCache>()
                      : borrowCache;
      options.workMetrics = &work;
      options.requireFunctionDefinitions = true;
      options.requireEntryPoint = true;
      options.suggestIncludes = false;
      const std::string entry =
          "module api; int main() { auto later = pick(true); return " +
          std::string(invalid ? "missing" : "0") + "; }";
      const auto result = lang::Frontend(options).analyze(
          body, entry, {api}, borrowing, {}, {owner});
      expect(invalid ? !result.semanticValid &&
                           std::any_of(result.diagnostics.begin(),
                                       result.diagnostics.end(),
                                       [](const auto &diagnostic) {
                                         return diagnostic.code == "GTI-S2001";
                                       })
                     : result.canGenerateCode(),
             "complete-result query workers belong to each resumed request, "
             "including independent valid analysis after an invalid overlay");
      if (mode != 0)
        expect(work.semanticPreambleAdoptions == 1,
               "the borrow-query regression must adopt its reusable prefix");
      if (mode == 2 && round != 0)
        expect(work.semanticPreambleBuilds == 0,
               "valid and invalid consumers reuse the immutable query prefix");
    }
  }
}
