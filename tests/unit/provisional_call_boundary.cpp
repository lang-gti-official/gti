// HIR must reject provisional requirement facts returned by a concrete instance
// provider.
#include "gti/frontend.h"
#include "gti/semantic_analyzer.h"
#include "unit_test.h"
#include <iostream>

namespace lang {
struct SemanticModelTestAccess {
  static void record(SemanticModel &model, const Call &call,
                     ResolvedCallInfo info) {
    model.record(call, std::move(info));
  }
};
} // namespace lang

GTI_UNIT_TEST(provisional_call_boundary) {
  using namespace lang;
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  const auto source = Frontend(options).analyze(
      "provisional.gti", "int value() { return 7; } "
                         "int run<T>(T ignored) { return value(); } "
                         "int main() { return run(7); }");
  SemanticVisitor semantic(TargetInfo::host(), &source.sourceGraph, false,
                           nullptr, &source.sources);
  if (!semantic.check(source.program)) {
    std::cerr << "Primary semantic analysis failed.\n";
    return 1;
  }
  const Call *call = nullptr;
  for (const auto &statement : source.program.declarations()) {
    const auto *function = stmtAs<FunctionDecl>(statement.get());
    if (!function || function->name().lexeme != "run" || !function->body())
      continue;
    const auto *returned =
        stmtAs<ReturnStmt>(function->body()->statements().back().get());
    if (returned)
      call = exprAs<Call>(returned->value().get());
  }
  if (!call) {
    std::cerr << "Fixture call was not found.\n";
    return 1;
  }
  struct Provider final : SemanticInstanceProvider {
    SemanticVisitor &delegate;
    const Call &call;
    bool forge = false;
    bool changed = false;
    Provider(SemanticVisitor &delegate, const Call &call)
        : delegate(delegate), call(call) {}
    SemanticTypeTraits traitsFor(const SemanticType &type) const override {
      return delegate.traitsFor(type);
    }
    bool requiresActiveCleanupFor(const SemanticType &type) const override {
      return delegate.requiresActiveCleanupFor(type);
    }
    SemanticInstanceAnalysis
    analyzeFunctionInstance(FunctionId id, const std::vector<SemanticType> &a,
                            const std::vector<CompileTimeValue> &b,
                            const std::vector<SemanticType> &c,
                            const std::vector<CompileTimeValue> &d) override {
      auto result = delegate.analyzeFunctionInstance(id, a, b, c, d);
      if (forge) {
        if (const auto *resolved = result.model.findCall(call)) {
          auto provisional = *resolved;
          // Even an otherwise valid target must not hide an unvalidated
          // open-body requirement left in the concrete provider's result.
          provisional.provisionalRequirement = true;
          SemanticModelTestAccess::record(result.model, call,
                                          std::move(provisional));
          changed = true;
        }
      }
      return result;
    }
    SemanticInstanceAnalysis analyzeConstructorInstance(
        ConstructorId id, const std::vector<SemanticType> &a,
        const std::vector<CompileTimeValue> &b,
        const std::vector<SemanticType> &c,
        const std::vector<CompileTimeValue> &d) override {
      return delegate.analyzeConstructorInstance(id, a, b, c, d);
    }
    SemanticInstanceAnalysis
    analyzeDestructorInstance(ClassId id, const std::vector<SemanticType> &a,
                              const std::vector<CompileTimeValue> &b) override {
      return delegate.analyzeDestructorInstance(id, a, b);
    }
    SemanticInstanceAnalysis analyzeClassFieldInitializers(
        ClassId id, const std::vector<SemanticType> &a,
        const std::vector<CompileTimeValue> &b) override {
      return delegate.analyzeClassFieldInitializers(id, a, b);
    }
  } provider(semantic, *call);
  if (!HirLowerer().lower(source.program, semantic.model(), provider).valid()) {
    std::cerr << "Unmodified provider failed HIR lowering.\n";
    return 1;
  }
  provider.forge = true;
  const auto rejected =
      HirLowerer().lower(source.program, semantic.model(), provider);
  if (!provider.changed || rejected.valid()) {
    std::cerr << "Expected a rejected provisional call: injected="
              << provider.changed << ", valid=" << rejected.valid() << '\n';
    return 1;
  }
  return 0;
}
