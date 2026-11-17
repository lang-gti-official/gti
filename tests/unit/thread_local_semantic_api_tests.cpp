// Semantic instance-provider API checks with no source-level observable,
// moved unchanged from thread_local_semantic_tests.cpp (its GTI-source cases
// became tests/ui/concurrency/thread_local/semantics files): a fault-injected
// empty-pack destructor outcome gates HIR admission, and a retained
// effect-budget failure gates only its exact value key on direct provider
// requests.
#include "gti/frontend.h"
#include "gti/semantic_analyzer.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

lang::FrontendResult analyze(std::string source) {
  lang::FrontendOptions options;
  options.target = lang::TargetInfo::host();
  options.target.executionProfile = lang::ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "stdlib";
  return lang::Frontend(options).analyze(
      "thread-local-semantics.gti", std::move(source),
      {library / "prelude.gti"}, {}, {library});
}

void effectQueryBudgetKeys() {
  // The effect budget is an uncertified exact query, not an empty-effects
  // proof. Direct provider calls do not run recursive HIR discovery, so this
  // exercises the retained failure when that ordinary body would be valid.
  const std::string chain = R"(
void chain<uint64_t N>() {
  if constexpr (N != 0) { chain<N - 1>(); }
}
int main() { chain<65>(); return 0; }
)";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto parsed =
      lang::Frontend(options).analyze("query-budget.gti", chain);
  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &parsed.sourceGraph,
                                 false, nullptr, &parsed.sources);
  expect(semantic.check(parsed.program),
         "effect-query budget exhaustion defers diagnostic ownership to "
         "ordinary concrete discovery");
  lang::FunctionId function = 0;
  for (const auto &statement : parsed.program.declarations())
    if (const auto *declaration =
            lang::stmtAs<lang::FunctionDecl>(statement.get());
        declaration && declaration->name().lexeme == "chain")
      if (const auto *info = semantic.model().findFunction(*declaration))
        function = info->id;
  expect(function != 0, "the budget fixture resolves its exact generic owner");
  if (function == 0)
    return;
  for (const bool reverse : {false, true}) {
    const auto check = [&](std::uint64_t value) {
      return semantic.analyzeFunctionInstance(
          function, {}, {}, {}, {lang::CompileTimeValue::uint64(value)});
    };
    const auto first = check(reverse ? 1 : 0);
    const auto second = check(reverse ? 0 : 1);
    const auto &good = reverse ? second : first;
    const auto &bad = reverse ? first : second;
    expect(good.valid() && !bad.valid() &&
               std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "GTI-S2105" &&
                                    d.message.find(
                                        "initializer-effect-depth") !=
                                        std::string::npos;
                           }),
           "a retained effect-budget failure gates its exact value key on "
           "fresh/repeated provider requests without poisoning other keys");
  }
}

void emptyPackDestructorProvider() {
  const auto source = analyze(R"(
class Empty<Args...> { public: ~Empty() {}  public: Empty() = default; };
int main() { Empty<> value{}; return 0; }
)");
  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &source.sourceGraph,
                                 false, nullptr, &source.sources);
  expect(semantic.check(source.program),
         "the empty-pack destructor provider fixture has valid primary "
         "semantic facts");
  struct Provider final : lang::SemanticInstanceProvider {
    lang::SemanticVisitor &delegate;
    std::size_t emptyDestructors = 0;
    bool reject = false;
    explicit Provider(lang::SemanticVisitor &delegate) : delegate(delegate) {}
    lang::SemanticTypeTraits
    traitsFor(const lang::SemanticType &type) const override {
      return delegate.traitsFor(type);
    }
    bool
    requiresActiveCleanupFor(const lang::SemanticType &type) const override {
      return delegate.requiresActiveCleanupFor(type);
    }
    lang::SemanticInstanceAnalysis analyzeFunctionInstance(
        lang::FunctionId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b,
        const std::vector<lang::SemanticType> &c,
        const std::vector<lang::CompileTimeValue> &d) override {
      return delegate.analyzeFunctionInstance(id, a, b, c, d);
    }
    lang::SemanticInstanceAnalysis analyzeConstructorInstance(
        lang::ConstructorId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b,
        const std::vector<lang::SemanticType> &c,
        const std::vector<lang::CompileTimeValue> &d) override {
      return delegate.analyzeConstructorInstance(id, a, b, c, d);
    }
    lang::SemanticInstanceAnalysis analyzeDestructorInstance(
        lang::ClassId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b) override {
      auto result = delegate.analyzeDestructorInstance(id, a, b);
      if (a.empty() && b.empty()) {
        ++emptyDestructors;
        // Public-provider fault injection: a selected invalid outcome must
        // not disappear merely because its concrete argument pack is empty.
        if (reject)
          result.diagnostics.push_back(lang::makeDiagnostic(
              "GTI-S2079", lang::DiagnosticPhase::Semantics,
              {"empty-provider.gti", 0, 1, 1},
              "Rejected concrete destructor provider outcome."));
      }
      return result;
    }
    lang::SemanticInstanceAnalysis analyzeClassFieldInitializers(
        lang::ClassId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b) override {
      return delegate.analyzeClassFieldInitializers(id, a, b);
    }
  } provider(semantic);
  const auto accepted =
      lang::HirLowerer().lower(source.program, semantic.model(), provider);
  expect(accepted.valid() && provider.emptyDestructors == 1,
         "an empty concrete class pack still requires one ordinary destructor "
         "provider analysis");
  provider.reject = true;
  provider.emptyDestructors = 0;
  const auto rejected =
      lang::HirLowerer().lower(source.program, semantic.model(), provider);
  expect(!rejected.valid() && provider.emptyDestructors == 1 &&
             std::any_of(rejected.diagnostics.begin(),
                         rejected.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2079";
                         }),
         "an ordinary invalid empty-pack destructor outcome gates executable "
         "HIR admission instead of bypassing its provider");
}
} // namespace

GTI_UNIT_TEST(thread_local_semantic_api) {
  effectQueryBudgetKeys();
  emptyPackDestructorProvider();
  return failures == 0 ? 0 : 1;
}
