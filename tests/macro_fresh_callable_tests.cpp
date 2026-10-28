#include "compiler_test_fixture.h"

namespace {
lang::FrontendResult analyzeMacroCallable(const std::string &source) {
  return lang::Frontend().analyze("macro-fresh-callables.gti", source,
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

void valid(const std::string &source, const std::string &message,
           std::optional<std::size_t> expectedLambdas = std::nullopt) {
  const auto result = analyzeMacroCallable(source);
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), message);
  if (!result.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(result.mir).valid(),
         message + " preserves verified closure operations");
  if (expectedLambdas)
    expect(result.hir.lambdaInstances().size() == *expectedLambdas,
           message + " preserves the expected lexical/concrete identities");
}

void rejected(const std::string &source, std::string_view diagnostic,
              const std::string &message) {
  const auto result = analyzeMacroCallable(source);
  expect(!result.semanticValid, message);
  expect(
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [&](const auto &found) { return found.code == diagnostic; }),
      message + " uses the ordinary ownership/loan diagnostic");
}

void discoveryBoundary() {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto result =
      lang::Frontend(options).analyze("macro-discovery-boundary.gti", R"(
#macro ignore<F>(F callback) {}
int main() {
  int value = 1;
  ignore([value]() -> void { int local = value; });
  return 0;
}
)");
  expect(result.semanticValid, "unused lambda discovery has valid semantics");
  for (const auto &declaration : result.program.declarations()) {
    const auto *function = lang::stmtAs<lang::FunctionDecl>(declaration.get());
    if (!function || function->name().lexeme != "main" || !function->body())
      continue;
    const auto *block = lang::stmtAs<lang::BlockStmt>(function->body().get());
    if (!block || block->statements().size() < 2)
      continue;
    const auto *statement =
        lang::stmtAs<lang::ExpressionStmt>(block->statements()[1].get());
    const auto *call =
        statement ? lang::exprAs<lang::Call>(statement->expression().get())
                  : nullptr;
    const auto *lambda =
        call && call->arguments().size() == 1
            ? lang::exprAs<lang::Lambda>(call->arguments()[0].get())
            : nullptr;
    const auto *callee =
        call ? lang::exprAs<lang::Variable>(call->callee().get()) : nullptr;
    const auto *occurrence =
        callee ? result.semantics.database().findOccurrence(
                     result.sourceGraph.entryUnit(), callee->name().position)
               : nullptr;
    const auto *selected = occurrence && occurrence->selectedCall
                               ? &*occurrence->selectedCall
                               : nullptr;
    expect(lambda && selected && selected->typeArguments.size() == 1,
           "unused lambda retains its selected callable type");
    if (!lambda || !selected || selected->typeArguments.size() != 1)
      return;
    const auto *shape =
        result.semantics.findLambda(selected->typeArguments[0].lambdaId);
    expect(shape && !result.semantics.findLambda(*lambda),
           "type discovery never advertises an analyzed executable lambda AST");
    expect(
        shape && std::all_of(shape->captures.begin(), shape->captures.end(),
                             [](const auto &capture) {
                               return !capture.initializer &&
                                      !capture.sourceSymbol &&
                                      !capture.bindingSymbol;
                             }),
        "discovery shape retains no speculative capture pointers or bindings");
    return;
  }
  expect(false, "unused callable discovery fixture has its written invocation");
}

void tests() {
  valid(R"(
mut int total = 0;
#macro twice<F>(F callback) { callback(); callback(); }
#macro invoke<F>(F callback) { callback(); }
#macro nested<F>(F callback) { invoke(callback); }
int main() {
  int value = 7;
  twice([value]() -> void { total += value; });
  nested([]() -> void { total += 1; });
  auto named = []() -> void { total += 2; };
  invoke(named);
  return total == 17 ? 0 : 1;
}
)",
        "repeated and nested forcing shares its written lambda, while another "
        "lambda and named callable remain distinct",
        3);

  valid(R"(
#macro invoke<F>(F callback) { callback(); }
void observe<T>(T value) {}
void generic<T>(T value) {
  invoke([value]() -> void { observe(value); });
}
int main() { generic(1); generic(true); return 0; }
)",
        "one lexical lambda retains distinct enclosing concrete generic types",
        2);

  valid(R"(
mut int total = 0;
#macro invoke<F>(F callback) { callback(); }
int main() {
  invoke([]() -> void {
    invoke([]() -> void { total += 1; });
  });
  return total == 1 ? 0 : 1;
}
)",
        "nested lambda execution bodies have independent macro result regions",
        2);

  valid(R"(
#include <std/memory>
#macro ignore<F>(F callback) {}
#macro when<F>(bool enabled, F callback) { if (enabled) { callback(); } }
int main() {
  auto owner = std::make_unique<int>(7);
  ignore([owned = std::move(owner)]() -> void { int value = *owned; });
  when(false, [owned = std::move(owner)]() -> void { int value = *owned; });
  return *owner == 7 ? 0 : 1;
}
)",
        "unused and disabled captures establish no caller moves or cleanup");

  valid(R"(
#macro when_mut<F>(bool enabled, F action) {
  if (enabled) { mut F callback = action; callback(); }
}
int main() {
  mut int value = 1;
  when_mut(false, [mut &value]() -> void { value += 100; });
  value += 1;
  when_mut(true, [mut &value]() -> void { value += 1; });
  return value == 3 ? 0 : 1;
}
)",
        "mutable borrowed closures use ordinary explicit mutable local "
        "invocation");

  rejected(R"(
#macro invoke<F>(F action) { action(); }
int main() {
  mut int value = 1;
  invoke([mut &value]() -> void { value += 1; });
  return 0;
}
)",
           "GTI-S2046",
           "direct fresh closure invocation does not acquire mutable access");

  rejected(R"(
#include <std/memory>
#macro twice<F>(F callback) { callback(); callback(); }
int main() {
  auto owner = std::make_unique<int>(7);
  twice([owned = std::move(owner)]() -> void { int value = *owned; });
  return 0;
}
)",
           "GTI-S2018",
           "repeated closure construction cannot consume one owner twice");

  rejected(R"(
#include <std/memory>
#macro invoke<F>(F callback) { callback(); }
int main() {
  auto owner = std::make_unique<int>(7);
  auto& held = owner;
  invoke([owned = std::move(owner)]() -> void { int value = *owned; });
  return *held;
}
)",
           "GTI-S2017", "a forced move capture respects a live checked borrow");

  rejected(R"(
#macro ignore<F>(F callback) {}
int main() {
  ignore([]() -> int { return missing; });
  return 0;
}
)",
           "GTI-S2001", "unused fresh lambdas still type-check their bodies");

  rejected(
      R"(
#include <std/memory>
#macro F repeated<F>(F expression) { F first = expression; return expression; }
int main() {
  auto owner = std::make_unique<int>(7);
  auto callback = repeated([owned = std::move(owner)]() -> int { return *owned; });
  return 0;
}
)",
      "GTI-S2018",
      "returning a closure does not permit forcing one move capture twice");
  rejected(R"(
#macro F keep<F>(F expression) { return expression; }
int main() {
  int value = 7;
  auto callback = keep([&value]() -> int { return value; });
  return callback();
}
)",
           "GTI-S2124",
           "callable results do not export checked capture borrows");
}

void constrainedMacroQueryCycle() {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const std::string source = R"(
concept recursive_query<T> = requires(T value) {
  { constrained(value) } -> int;
};
#macro int constrained<T>(T value) requires recursive_query<T> { return 1; }
int main() { return constrained(1); }
)";
  const auto result =
      lang::Frontend(options).analyze("macro-query-cycle.gti", source);
  expect(!result.semanticValid,
         "a constrained macro query cycle must terminate before expansion");
  const auto application =
      source.find("recursive_query<T>", source.find("#macro"));
  const auto query = source.find("requires(T value)");
  const auto expectedSource =
      std::filesystem::weakly_canonical(
          std::filesystem::absolute("macro-query-cycle.gti"))
          .string();
  const bool anchored = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) {
        const auto relatedSpan = [&](std::size_t start, std::size_t length) {
          return std::any_of(diagnostic.related.begin(),
                             diagnostic.related.end(), [&](const auto &note) {
                               return note.span.source ==
                                          diagnostic.primary.source &&
                                      note.span.start == start &&
                                      note.span.end == start + length;
                             });
        };
        return diagnostic.code == "GTI-S2029" &&
               diagnostic.phase == lang::DiagnosticPhase::Semantics &&
               diagnostic.primary.source == expectedSource &&
               diagnostic.primary.start == source.rfind(");") &&
               diagnostic.primary.end == diagnostic.primary.start + 1 &&
               diagnostic.message.find(
                   "Generic arguments do not satisfy concept") !=
                   std::string::npos &&
               relatedSpan(application, 15) && relatedSpan(query, 8) &&
               diagnostic.fixes.empty();
      });
  if (!anchored) {
    std::cerr << "cycle expected source=" << expectedSource << '\n';
    for (std::size_t index = 0;
         index < std::min<std::size_t>(result.diagnostics.size(), 3); ++index) {
      const auto &diagnostic = result.diagnostics[index];
      std::cerr << diagnostic.code
                << " phase=" << static_cast<int>(diagnostic.phase)
                << " primary=" << diagnostic.primary.source << ":"
                << diagnostic.primary.start << ".." << diagnostic.primary.end
                << " fixes=" << diagnostic.fixes.size() << ": "
                << diagnostic.message.substr(0, 240) << '\n';
    }
  }
  expect(anchored,
         "macro signature probes reject an identical in-flight query at the "
         "written caller through the ordinary constraint diagnostic");
  const auto finite =
      lang::Frontend(options).analyze("macro-finite-query.gti", R"(
struct Leaf { public: Leaf() = default; };
struct Wrap<T> { public: Wrap() = default; };
concept available<T> = requires(T value) { { step(value) } -> int; };
int step(Leaf value) { return 1; }
int step<T>(Wrap<T> value) requires available<T> { return 1; }
#macro int constrained<T>(T value) requires available<T> { return 1; }
int main() {
  Wrap<Wrap<Leaf>> value{};
  return constrained(value) - 1;
}
)");
  if (!finite.semanticValid)
    for (const auto &diagnostic : finite.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(finite.semanticValid,
         "changing concrete generic query instances may terminate within "
         "the existing depth budget");
}
} // namespace

int main() {
  discoveryBoundary();
  tests();
  constrainedMacroQueryCycle();
  return failures == 0 ? 0 : 1;
}
