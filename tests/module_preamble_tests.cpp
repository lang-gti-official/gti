#include "gti/frontend.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
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
}
