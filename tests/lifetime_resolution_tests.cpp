#include "gti/frontend.h"
#include "gti/lifetime_types.h"
#include "gti/semantic_analyzer.h"
#include "../src/compiler/semantic_analyzer_impl.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace lang {
// Exercise the real staging key and lookup without exposing cache internals as
// a product API or relying on a timing-sensitive number of source evaluations.
struct SemanticLifetimeTestAccess {
  struct CacheChecks {
    bool repeatedHit = false;
    bool distinctLifetimeMiss = false;
    bool distinctGenerationMiss = false;
    bool distinctBoundaryMiss = false;
    bool workerRefreshed = false;
    bool boundaryWorkerRefreshed = false;
  };
  static CacheChecks stagedLifetimeKeys() {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    using Cache = SemanticVisitor::Impl::StagedConstantResults;
    auto cache = std::make_shared<Cache>();
    impl.requestSession.stagedConstants = cache;
    FunctionInfo function{.id = 1};
    GenericSubstitution substitution;
    substitution.lifetimes.emplace(1, Lifetime::staticLifetime());
    const auto key = [&] {
      return impl.stagedConstantKey(static_cast<const FunctionDecl *>(nullptr),
                                    &function, {}, substitution, std::nullopt,
                                    {}, {});
    };
    const auto original = key();
    if (!original)
      return {};
    cache->record({.key = *original, .value = ConstantValue{true}});
    CacheChecks checks;
    const auto repeated = key();
    checks.repeatedHit = repeated && cache->find(*repeated);
    substitution.lifetimes[1] = Lifetime::parameter(2);
    const auto different = key();
    checks.distinctLifetimeMiss = different && !cache->find(*different);
    substitution.lifetimes[1] = Lifetime::staticLifetime();
    ++impl.requestSession.lifetimeContractRevision;
    const auto revised = key();
    checks.distinctGenerationMiss = revised && !cache->find(*revised);
    EmptyStmt earlier(Token{}), later(Token{});
    impl.requestSession.privateConstexprPrerequisites =
        std::make_shared<SemanticVisitor::Impl::RequestSessionContext::PrivateConstexprPrerequisites>();
    impl.requestSession.privateConstexprBoundary = &earlier;
    const auto earlierKey = key();
    if (earlierKey)
      cache->record({.key = *earlierKey, .value = ConstantValue{true}});
    impl.requestSession.privateConstexprBoundary = &later;
    const auto laterKey = key();
    checks.distinctBoundaryMiss = earlierKey && laterKey && !cache->find(*laterKey);
    impl.catalog.seal();
    auto work = std::make_shared<SemanticVisitor::Impl::ConstexprWorkBudget>();
    (void)impl.stagedWorkerFor(impl.semanticModel, work);
    impl.macroLookupBase = &impl;
    ++impl.requestSession.lifetimeContractRevision;
    auto &worker = impl.stagedWorkerFor(impl.semanticModel, work);
    checks.workerRefreshed = worker.macroLookupBase == &impl &&
        worker.requestSession.lifetimeContractRevision == impl.requestSession.lifetimeContractRevision;
    impl.requestSession.privateConstexprBoundary = &earlier;
    auto &earlierWorker = impl.stagedWorkerFor(impl.semanticModel, work);
    checks.boundaryWorkerRefreshed =
        earlierWorker.requestSession.privateConstexprBoundary == &earlier &&
        impl.stagedWorker.privateConstexprBoundary == &earlier;
    return checks;
  }
};
} // namespace lang

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string_view source) {
  return lang::Frontend().analyze("lifetime-resolution.gti", std::string(source));
}

void expectValid(const lang::FrontendResult &result, std::string_view message) {
  if (!result.semanticValid)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << " at line " << diagnostic.primary.line
                << ": " << diagnostic.message << '\n';
  expect(result.semanticValid, message);
}

const lang::FunctionInfo *function(const lang::FrontendResult &result,
                                    std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::FunctionDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findFunction(*syntax);
  return nullptr;
}

const lang::TypeAliasInfo *alias(const lang::FrontendResult &result,
                                 std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::TypeAliasDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findTypeAlias(*syntax);
  return nullptr;
}

const lang::ClassTypeInfo *record(const lang::FrontendResult &result,
                                 std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::ClassDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findClassType(*syntax);
  return nullptr;
}

void testAnonymousNominalRegions() {
  const auto result = analyze(R"(
struct PairView {
  int& first; int& second;
  PairView(int& left, int& right) : first(left), second(right) {}
};
using PairAlias = PairView;
struct Nested {
  PairAlias left; PairAlias right;
  Nested(PairAlias first, PairAlias second) : left(first), right(second) {}
};
struct Derived : public PairView {
  int& third;
  Derived(int& first, int& second, int& last) : PairView(first, second), third(last) {}
};
struct Packed<Ts...> { int& anchor; Packed(int& value) : anchor(value) {} };
enum Event { first(int& value), second(int& value) };
void inspect(PairView pair, Nested nested, Derived derived, Packed<int, bool> packed, Event event);
int main() { return 0; }
)");
  expectValid(result, "record, alias, inherited, packed, and enum regions normalize before use");
  for (const auto &[name, count] :
       {std::pair{"PairView", 2u}, {"Nested", 4u}, {"Derived", 3u}, {"Packed", 1u}}) {
    const auto *info = record(result, name);
    expect(info && std::size_t(std::count_if(info->genericParameters.begin(), info->genericParameters.end(),
                                 [](const auto &parameter) {
                                   return parameter.lifetime && parameter.implicit;
                                 })) == count,
           "each omitted stored occurrence has an independent anonymous declaration slot");
  }
  const auto *info = function(result, "inspect");
  expect(info && info->parameterTypes.size() == 5,
         "all normalized nominal signatures remain available");
  if (info && info->parameterTypes.size() == 5) {
    const std::vector<std::size_t> counts{2, 4, 3, 1, 2};
    for (std::size_t index = 0; index < counts.size(); ++index)
      expect(info->parameterTypes[index].lifetimeArguments.size() == counts[index],
             "normalized signatures carry every anonymous nominal region");
    expect(info->parameterTypes[3].arguments.size() == 2,
           "anonymous lifetime slots do not consume explicit type pack arguments");
  }
  const auto *pair = record(result, "PairView");
  expect(pair && pair->fields.size() == 2 &&
             pair->fields[0].type.referenceLifetime != pair->fields[1].type.referenceLifetime &&
             pair->fields[0].type.referenceLifetime.declarationRegion() &&
             pair->fields[1].type.referenceLifetime.declarationRegion(),
         "field metadata contains declaration binders rather than inference holes");
}

void testNominalVariance() {
  const auto result = analyze(R"(
struct Shared<T, lifetime a> {
  T&<a> value; Shared(T&<a> source) : value(source) {}
};
struct Exclusive<T, lifetime a> {
  mut T&<a> value; Exclusive(mut T&<a> source) : value(source) {}
};
struct Raw<T> { T* pointer = nullptr; };
struct Owned<T> { T value; Owned(T initial) : value(initial) {} void replace(T other) mut; };
struct Unused<T, lifetime a> {};
struct Node<lifetime a> {
  Node<a>* next = nullptr; int&<a> value;
  Node(int&<a> source) : value(source) {}
};
int main() { return 0; }
)");
  expectValid(result, "explicit recursive types and verified nominal variance resolve");
  const auto verify = [&](std::string_view name, lang::TypeVariance type,
                          std::optional<lang::TypeVariance> lifetime) {
    const auto *info = record(result, name);
    expect(info && info->variance.complete && info->variance.types == std::vector{type} &&
               info->variance.lifetimes == (lifetime ? std::vector{*lifetime}
                                                     : std::vector<lang::TypeVariance>{}),
           "variance follows verified owned/shared/mutable/raw storage positions");
  };
  verify("Shared", lang::TypeVariance::Covariant, lang::TypeVariance::Covariant);
  verify("Exclusive", lang::TypeVariance::Invariant, lang::TypeVariance::Covariant);
  verify("Raw", lang::TypeVariance::Invariant, std::nullopt);
  verify("Owned", lang::TypeVariance::Covariant, std::nullopt);
  verify("Unused", lang::TypeVariance::Bivariant, lang::TypeVariance::Bivariant);
  if (const auto *shared = record(result, "Shared");
      shared && shared->genericParameters.size() == 2) {
    const auto expected = lang::LifetimeTypeBound{
        lang::SemanticType::typeParameter(shared->genericParameters[0].id),
        lang::Lifetime::parameter(shared->genericParameters[1].id)};
    expect(shared->lifetimeContract.valid() &&
               std::find(shared->lifetimeContract.typeBounds.begin(),
                         shared->lifetimeContract.typeBounds.end(), expected) !=
                   shared->lifetimeContract.typeBounds.end(),
           "nominal metadata retains the exact referent well-formedness obligation");
  }

  for (const std::string &source : {
           std::string("struct Recursive { Recursive* next = nullptr; int& value; Recursive(int& source) : value(source) {} }; int main() { return 0; }"),
           std::string("using Cycle = Cycle; int main() { return 0; }")}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "recursive anonymous region expansion and alias cycles are rejected finitely");
  }
}

void testSpecializationLifetimeShape() {
  const auto result = analyze(R"(
namespace family {
  struct View<T, lifetime a> { T&<a> value; View(T&<a> source) : value(source) {} };
}
struct family::View<int> { int&<a> value; View(int&<a> source) : value(source) {} };
void inspect<lifetime selected>(family::View<int, selected> value);
int main() { return 0; }
)");
  expectValid(result, "exact specialization retains the primary's universal region shape");
  const auto *inspect = function(result, "inspect");
  if (inspect && inspect->parameterTypes.size() == 1 &&
      inspect->genericParameters.size() == 1) {
    const auto &type = inspect->parameterTypes.front();
    const auto *selected = result.semantics.findClassType(type.classId);
    expect(selected && selected->exactSpecializationPrimary != 0 &&
               selected->lifetimeContract.valid() &&
               type.arguments == std::vector{lang::SemanticType(lang::SemanticType::Int32)} &&
               type.lifetimeArguments == std::vector{
                   lang::Lifetime::parameter(inspect->genericParameters.front().id)},
           "selected specialization metadata preserves the actual region argument");
  } else {
    expect(false, "specialized lifetime-bearing parameter metadata is available");
  }

  const auto nested = analyze(R"(
struct Token<lifetime storage> {};
namespace family {
  struct Holder<T, lifetime owner> { T&<owner> value; Holder(T&<owner> source) : value(source) {} };
}
struct family::Holder<Token<_>> {};
void inspectNested<lifetime nested, lifetime selected>(
    family::Holder<Token<nested>, selected> value) requires outlives<nested, selected>;
int main() { return 0; }
)");
  expectValid(nested, "exact specialization preserves lifetimes nested in public type arguments");
  const auto *inspectNested = function(nested, "inspectNested");
  if (inspectNested && inspectNested->parameterTypes.size() == 1 &&
      inspectNested->genericParameters.size() == 2) {
    const auto &type = inspectNested->parameterTypes.front();
    const auto *selected = nested.semantics.findClassType(type.classId);
    const auto *primary = nested.semantics.findPublicClassType(type.classId);
    const auto expected = lang::Lifetime::parameter(inspectNested->genericParameters[0].id);
    expect(selected && primary && selected->id != primary->id &&
               type.arguments.size() == 1 &&
               type.arguments.front().lifetimeArguments == std::vector{expected} &&
               nested.semantics.resolveExactClassSpecialization(type) == type,
           "selected identity and primary argument projection retain the actual nested region");
    expect(primary && selected && !primary->genericParameters.empty() &&
               std::any_of(selected->lifetimeContract.typeBounds.begin(),
                           selected->lifetimeContract.typeBounds.end(), [&](const auto &bound) {
                 return bound.type.kind == lang::SemanticType::TypeParameter &&
                        bound.type.genericParameterId == primary->genericParameters.front().id;
               }),
           "specialized formation retains the public type obligation until application");
  } else {
    expect(false, "nested specialization lifetime metadata is available");
  }
  for (const std::string &definition : {
           std::string("struct family::View<int> { int&<a> value; int& extra; View(int&<a> first, int& second) : value(first), extra(second) {} };"),
           std::string("struct family::View<int, static> { int&<static> value; View(int&<static> source) : value(source) {} };")}) {
    const auto invalid = analyze(
        "namespace family { struct View<T, lifetime a> { T&<a> value; View(T&<a> source) : value(source) {} }; } " +
        definition + " int main() { return 0; }");
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.message.find("cannot add anonymous lifetime") != std::string::npos ||
                                    diagnostic.message.find("cannot select a lifetime") != std::string::npos;
                           }),
           "specializations cannot add hidden region arity or select on a lifetime");
  }
}

void testNominalArguments() {
  const auto result = analyze(R"(
struct Marker<T, lifetime a> { T value; };
enum Message<T, lifetime a> { item(T value) };
using Label<lifetime a> = Marker<int, a>;
void inspect<lifetime a>(Label<a> first, Message<int, a> second);
int main() { return 0; }
)");
  expectValid(result, "nominal lifetime arguments and generic aliases resolve");
  const auto *info = function(result, "inspect");
  expect(info && info->genericParameters.size() == 1 &&
             info->genericParameters.front().lifetime &&
             info->parameterTypes.size() == 2,
         "lifetime binders retain a dedicated semantic kind");
  if (!info || info->genericParameters.empty() || info->parameterTypes.size() != 2)
    return;
  const auto region = lang::Lifetime::parameter(info->genericParameters[0].id);
  for (const auto &type : info->parameterTypes)
    expect(type.arguments.size() == 1 &&
               type.arguments[0] == lang::SemanticType::Int32 &&
               type.lifetimeArguments == std::vector{region},
           "regions do not occupy runtime type argument slots");
}

void testTransparentAliases() {
  const auto result = analyze(R"(
using Ref<lifetime a> = int&<a>;
using Elided = int&;
void inspect<lifetime a>(Ref<a> first, Elided second, Elided third);
int main() { return 0; }
)");
  expectValid(result, "explicit and elided reference aliases are transparent");
  const auto *elided = alias(result, "Elided");
  expect(elided && elided->genericParameters.size() == 1 &&
             elided->genericParameters[0].lifetime &&
             elided->type.referenceLifetime.kind == lang::Lifetime::Kind::Parameter,
         "an elided alias stores one finite anonymous declaration region");
  const auto *info = function(result, "inspect");
  if (!info || info->parameterTypes.size() != 3) {
    expect(false, "reference alias parameter metadata is available");
    return;
  }
  const auto &parameters = info->parameterTypes;
  expect(parameters[0].kind == lang::SemanticType::Reference &&
             parameters[1].kind == lang::SemanticType::Reference &&
             parameters[2].kind == lang::SemanticType::Reference &&
             parameters[1].referenceLifetime != parameters[2].referenceLifetime &&
             parameters[1].referenceLifetime.declarationRegion() &&
             parameters[2].referenceLifetime.declarationRegion(),
         "separate alias uses receive independent normalized lifetime slots");
}

void testHigherRankedAlias() {
  const auto result = analyze(R"(
using Callback = for<lifetime a> int&<a>(int&<a>);
int main() { return 0; }
)");
  expectValid(result, "higher-ranked aliases keep their local binder scope");
  const auto *info = alias(result, "Callback");
  expect(info && info->type.kind == lang::SemanticType::FunctionSignature &&
             info->type.quantifiedLifetimes.size() == 1 &&
             info->type.arguments.size() == 2,
         "higher-ranked callable metadata retains its quantified region");
  if (info && info->type.quantifiedLifetimes.size() == 1 &&
      info->type.arguments.size() == 2) {
    const auto region = lang::Lifetime::parameter(info->type.quantifiedLifetimes[0]);
    expect(info->type.arguments[0].referenceLifetime == region &&
               info->type.arguments[1].referenceLifetime == region &&
               lang::freeTypeLifetimes(info->type).empty(),
           "the result and input share a scoped region that does not escape");
  }
}

void testScopedNominalCallableFormation() {
  const std::string declarations = R"(
struct Holder<T, lifetime a> {
  T&<a> value;
  Holder(T&<a> initial) : value(initial) {}
};
struct Carrier<F> {};
using Scoped<T, lifetime outer> = for<lifetime inner> void(Holder<T, inner>, int&<outer>);
void preserve<T, lifetime outer>(Carrier<Scoped<T, outer>> callback) {}
)";
  const auto result = analyze(declarations + "int main() { return 0; }");
  expectValid(result, "higher-ranked nominal premises remain inside the callable's invocation");
  const auto *preserve = function(result, "preserve");
  expect(preserve && preserve->lifetimeContract.valid() &&
             preserve->lifetimeContract.parameters.size() == 1 &&
             preserve->lifetimeContract.bounds.empty() &&
             preserve->lifetimeContract.typeBounds.empty() &&
             preserve->lifetimeContract.requirements.nodes.empty(),
         "receiving a phantom callable carrier cannot assume its nested invocation premises");
  const auto escaping = analyze(declarations + R"(
int&<static> escape<lifetime outer>(Carrier<Scoped<int, outer>> callback, int&<outer> value) {
  return value;
}
int main() { return 0; }
)");
  expect(!escaping.semanticValid && std::any_of(escaping.diagnostics.begin(), escaping.diagnostics.end(),
         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "a nested callable's lifetime premises cannot justify an outer reference escape");
  const auto exactSelector = analyze(R"(
namespace exact { struct Fixed<T> {}; }
struct exact::Fixed<int&<static>> {};
using Preserved = for<lifetime a> void(exact::Fixed<int&<a>>);
int main() { return 0; }
)");
  expectValid(exactSelector, "exact specialization selection preserves a callable's actual region");
  const auto *preserved = alias(exactSelector, "Preserved");
  expect(preserved && preserved->type.quantifiedLifetimes.size() == 1 &&
             preserved->type.arguments.size() == 2 &&
             preserved->type.arguments[1].arguments.size() == 1 &&
             preserved->type.arguments[1].arguments.front().referenceLifetime ==
                 lang::Lifetime::parameter(preserved->type.quantifiedLifetimes.front()),
         "an exact selector's static spelling never replaces the actual quantified region");
  const auto fixedPattern = analyze(R"(
class Fixed<T&<static>> {};
using Invalid = for<lifetime a> void(Fixed<int&<a>>);
int main() { return 0; }
)");
  expect(!fixedPattern.semanticValid &&
             std::any_of(fixedPattern.diagnostics.begin(), fixedPattern.diagnostics.end(),
                         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "an inner invocation must satisfy fixed nominal argument-pattern regions exactly");
}

void testAssociatedLifetimeFamily() {
  const auto result = analyze(R"(
concept BorrowType<T, lifetime owner> { typename Borrow<lifetime a>; };
for<lifetime owner> concept BorrowType<int, owner> {
  using Borrow<lifetime a> = int&<a>;
};
using Result<lifetime a> = BorrowType<int, a>::Borrow<a>;
int main() { return 0; }
)");
  expectValid(result, "associated families bind and substitute lifetime arguments");
  const auto *info = alias(result, "Result");
  expect(info && info->genericParameters.size() == 1 &&
             info->type.kind == lang::SemanticType::Reference &&
             info->type.referenceLifetime ==
                 lang::Lifetime::parameter(info->genericParameters[0].id),
         "associated type reduction preserves the selected family region");
}

void testBinderDiagnostics() {
  for (const std::string source : {
           "void invalid(int&<missing> value); int main() { return 0; }",
           "void invalid(int&<this> value); int main() { return 0; }",
           "void invalid<lifetime a, lifetime a>(); int main() { return 0; }",
           "struct Owner<T> { using a = int; }; struct Tag<lifetime a> {}; void invalid<lifetime a>(Tag<Owner<int>::a> value); int main() { return 0; }",
           "struct Pair<T, lifetime a> {}; using Bad = Pair<static, int>; int main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2126";
                           }),
           "invalid lifetime names and parameter kinds produce binder diagnostics");
    expect(function(result, "main") != nullptr,
           "binder diagnostics preserve following declaration analysis");
  }
}

void testProtocolOperationBinders() {
  const auto result = analyze(R"(
concept Shared<T> {
  T&<a> borrow<lifetime a>(T&<a> value);
  T&<a> relay<lifetime a>(T&<a> value);
};
for concept Shared<int> {
  int&<b> borrow<lifetime b>(int&<b> value) { return value; }
  int& relay(int& value) { return value; }
};
T&<a> forward<T, lifetime a>(T&<a> value) requires Shared<T> {
  return Shared<T>::relay<a>(value);
}
int main() {
  int value = 7;
  int& selected = Shared<int>::borrow<_>(value);
  int& relayed = Shared<int>::relay<_>(selected);
  return forward(relayed) - 7;
}
)");
  expectValid(result, "explicit protocol lifetimes select named and elided implementation binders");
}

void testExplicitFunctionKinds() {
  const auto result = analyze(R"(
void mixed<T, lifetime a, uint64_t N, lifetime b>(T&<a> first, int&<b> second) {}
void collect<lifetime a, T, Args...>(T&<a> first, Args&<a>... rest) {}
void exercise<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  mixed<int, a, 3, b>(first, second);
  collect<a, int, int, int>(first, first, first);
}
int main() { int value = 7; exercise(value, value); return 0; }
)");
  expectValid(result, "explicit lifetime arguments retain mixed generic order and type packs");
  const auto *exercise = function(result, "exercise");
  if (!exercise || !exercise->declaration || !exercise->declaration->body()) {
    expect(false, "mixed-kind call metadata remains available");
    return;
  }
  std::size_t checked = 0;
  for (const auto &statement : exercise->declaration->body()->statements()) {
    const auto *expression = lang::stmtAs<lang::ExpressionStmt>(statement.get());
    const auto *call = expression
        ? lang::exprAs<lang::Call>(expression->expression().get()) : nullptr;
    const auto *selected = call ? result.semantics.findCall(*call) : nullptr;
    if (!selected)
      continue;
    expect(selected->lifetimeApplication.complete &&
               !selected->lifetimeApplication.bindings.empty(),
           "selected generic calls retain a checked lifetime application");
    if (checked == 0)
      expect(selected->typeArguments.size() == 1 &&
                 selected->valueArguments.size() == 1 &&
                 selected->lifetimeArguments.size() == 2,
             "mixed kinds use separate compact runtime and lifetime vectors");
    else
      expect(selected->typeArguments.size() == 3 &&
                 selected->valueArguments.empty() &&
                 selected->lifetimeArguments.size() == 1 &&
                 selected->parameterTypes.size() == 3,
             "a leading lifetime does not count toward fixed types or pack elements");
    ++checked;
  }
  expect(checked == 2, "both explicit generic calls publish semantic metadata");

  for (const std::string source : {
           "void use<T, lifetime a>(T&<a> value) {} int main() { int x = 0; use<static, int>(x); return 0; }",
           "void use<lifetime a>(int&<a> value) {} int main() { int x = 0; use<missing>(x); return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2126";
                           }),
           "explicit call arguments reject wrong kinds and unknown lifetime names");
  }
}
void testExecutableLifetimeIdentity() {
  const auto parsed = analyze(R"(
struct Phantom<lifetime left, lifetime right> {};
void keep<T>(T value) {}
void nested<T>(T value) { keep(value); }
int main() { return 0; }
)");
  expectValid(parsed, "executable lifetime identity fixture resolves");
  lang::SemanticVisitor visitor;
  expect(visitor.check(parsed.program), "canonical instance provider accepts source declarations");
  const auto *keepSyntax = lang::stmtAs<lang::FunctionDecl>(parsed.program.declarations()[1].get());
  const auto *nestedSyntax = lang::stmtAs<lang::FunctionDecl>(parsed.program.declarations()[2].get());
  const auto *phantomSyntax = lang::stmtAs<lang::ClassDecl>(parsed.program.declarations()[0].get());
  if (!keepSyntax || !nestedSyntax || !phantomSyntax)
    return;
  const auto *keep = visitor.model().findFunction(*keepSyntax);
  const auto *nested = visitor.model().findFunction(*nestedSyntax);
  const auto *phantom = visitor.model().findClassType(*phantomSyntax);
  if (!keep || !nested || !phantom)
    return;
  auto same = lang::SemanticType::classType(phantom->id);
  same.lifetimeArguments = {lang::Lifetime::local(41), lang::Lifetime::local(41)};
  auto different = same;
  different.lifetimeArguments.back() = lang::Lifetime::local(42);
  const auto first = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {same}});
  const auto second = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {different}});
  expect(first && second && first->types == second->types &&
             first->lifetimeContract == second->lifetimeContract,
         "caller region identities and incidental equality share one universal instance");
  if (!first || !second)
    return;
  expect(first->types[0].lifetimeArguments.size() == 2 &&
             first->types[0].lifetimeArguments[0] != first->types[0].lifetimeArguments[1] &&
             first->lifetimeApplication.bindings.size() == 2 &&
             first->lifetimeApplication.bindings[0].second == first->lifetimeApplication.bindings[1].second &&
             second->lifetimeApplication.bindings[0].second != second->lifetimeApplication.bindings[1].second,
         "separate universal occurrences retain per-call equal or independent choices");
  const auto nestedAnalysis = visitor.analyzeFunctionInstance(nested->id, {}, {}, {same}, {});
  expect(nestedAnalysis.valid(), "nested concrete body analysis accepts universal lifted arguments");
  const auto replay = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {same}});
  expect(replay && replay->types == first->types,
         "nested instance analysis never rewinds or reuses another lifted identity");
  auto callback = lang::SemanticType::functionSignature(
      lang::SemanticType::referenceTo(lang::SemanticType::Int32, lang::AccessMode::ReadOnly,
                                     lang::Lifetime::parameter(8001)),
      {lang::SemanticType::referenceTo(lang::SemanticType::Int32, lang::AccessMode::ReadOnly,
                                      lang::Lifetime::parameter(8001))});
  callback.quantifiedLifetimes = {8001};
  const auto higherRanked = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {callback}});
  callback.quantifiedLifetimes = {9001};
  callback.arguments[0].referenceLifetime = lang::Lifetime::parameter(9001);
  callback.arguments[1].referenceLifetime = lang::Lifetime::parameter(9001);
  const auto alpha = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {callback}});
  expect(higherRanked && alpha && higherRanked->types == alpha->types &&
             higherRanked->lifetimeApplication.bindings.empty() &&
             higherRanked->types[0].arguments[0].referenceLifetime ==
                 higherRanked->types[0].arguments[1].referenceLifetime,
         "higher-ranked binder scopes and repeated uses survive alpha-canonicalization");
}

void testInferredCarrierRegions() {
  constexpr std::string_view declarations = R"(
struct View<lifetime owner> {
  int&<owner> value;
  View(int&<owner> source) : value(source) {}
};
void consume<T>(T value) {}
void combine<T>(T first, T second) {}
)";
  const auto result = lang::Frontend({.stopAfter = lang::FrontendPhase::Hir}).analyze(
      "lifetime-instance-reuse.gti", std::string(declarations) + R"(
void exercise<lifetime left, lifetime right>(View<left> first, View<right> second) {
  consume(first); consume(second); combine(first, second);
}
int main() {
  int leftValue = 3; int rightValue = 5;
  auto first = View(leftValue); auto second = View(rightValue);
  exercise(first, second);
  return 0;
}
)");
  expectValid(result, "same-shape borrowed carriers infer common call regions");
  expect(result.hir.valid(), "inferred carrier calls lower with universal instance signatures");
  if (const auto *target = function(result, "consume")) {
    std::size_t instances = 0;
    for (const auto &instance : result.hir.functionInstances())
      if (instance.declaration == target->id) {
        ++instances;
        expect(!instance.liftedLifetimes.empty() && instance.lifetimeContract.valid(),
               "native body carries checked universal lifted binder descriptors");
      }
    if (instances != 1)
      std::cerr << "consume executable instances: " << instances << '\n';
    expect(instances == 1, "different source lifetimes do not multiply native function instances");
  }
  const auto fixed = analyze(std::string(declarations) + R"(
void rejected<lifetime left, lifetime right>(View<left> first, View<right> second) {
  combine<View<left>>(first, second);
}
int main() { return 0; }
)");
  expect(!fixed.semanticValid && std::any_of(fixed.diagnostics.begin(), fixed.diagnostics.end(),
             [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "explicit generic type choices retain their fixed lifetime obligations");
}

void testInstantiatedReferenceIdentity() {
  const auto result = analyze(R"(
int&<region> identity<lifetime region>(int&<region> value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
)");
  expectValid(result, "a generic lifetime instance rebuilds reference dependencies in its own body identity domain");
}

void testPrivateLifetimeInference() {
  const auto result = analyze(R"(
static int& first(int& selected, int& ignored) { return selected; }
static int& relay(int& selected, int& ignored) { return first(selected, ignored); }
static int& recurse(bool done, int& selected, int& ignored) {
  if (done) { return selected; }
  return recurse(true, selected, ignored);
}

static int& both(bool chooseFirst, int& firstValue, int& secondValue) {
  if (chooseFirst) { return firstValue; }
  return secondValue;
}
static int& mutualFirst(bool done, int& selected, int& ignored) {
  if (done) { return selected; }
  return mutualSecond(true, selected, ignored);
}
static int& mutualSecond(bool done, int& selected, int& ignored) {
  return mutualFirst(done, selected, ignored);
}
int&<a> explicitRelay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return first(selected, ignored);
}
int&<a> explicitMutualRelay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return mutualSecond(true, selected, ignored);
}
int main() { int firstValue = 1; int secondValue = 2; return first(firstValue, secondValue) + recurse(false, firstValue, secondValue); }
)");
  expectValid(result, "private omitted lifetime contracts infer selected and recursive suppliers");
  for (const auto name : {"first", "relay", "recurse", "mutualFirst", "mutualSecond"}) {
    const auto *info = function(result, name);
    if (!info) {
      expect(false, "private helper metadata is available");
      continue;
    }
    const auto selected = std::string_view(name) == "first" ||
                                  std::string_view(name) == "relay" ? 0U : 1U;
    const auto ignored = selected + 1;
    lang::LifetimeBounds bounds;
    for (const auto edge : info->lifetimeContract.bounds)
      (void)bounds.assume(edge.longer, edge.shorter);
    const bool validSummary = info->lifetimeContract.origin == lang::LifetimeContractOrigin::BodyInferred &&
               info->lifetimeContract.valid() && info->parameterTypes.size() > ignored &&
               bounds.proves(info->parameterTypes[selected].referenceLifetime,
                             info->returnType.referenceLifetime) &&
               !bounds.proves(info->parameterTypes[ignored].referenceLifetime,
                              info->returnType.referenceLifetime) &&
               info->completeReturnBorrow &&
               info->completeReturnBorrow->dependencies.size() == 1 &&
               info->completeReturnBorrow->dependencies.front().origin ==
                   lang::BorrowOriginKind::Argument &&
               info->completeReturnBorrow->dependencies.front().parameter == selected;
    expect(validSummary,
           std::string("private recursive fixed point preserves only productive return suppliers: ") + name);
    if (!validSummary) {
      const auto region = [](lang::Lifetime value) {
        std::cerr << unsigned(value.kind) << ':' << value.id;
      };
      const auto edges = [&](std::span<const lang::LifetimeBound> values) {
        for (std::size_t index = 0; index < std::min<std::size_t>(values.size(), 16); ++index) {
          std::cerr << ' '; region(values[index].longer);
          std::cerr << '>'; region(values[index].shorter);
        }
        std::cerr << " (" << values.size() << ")\n";
      };
      std::cerr << name << " result="; region(info->returnType.referenceLifetime);
      std::cerr << " contract:"; edges(info->lifetimeContract.bounds);
      std::cerr << name << " binders:";
      for (const auto binder : info->lifetimeContract.parameters) {
        std::cerr << ' '; region(binder);
      }
      std::cerr << '\n';
      if (info->declaration && info->declaration->body())
        if (const auto *body = result.semantics.findLifetimeBody(*info->declaration->body())) {
          std::cerr << name << " body complete=" << body->complete << " obligations:";
          edges(body->obligations);
          std::cerr << name << " type obligations:";
          for (std::size_t index = 0; index < std::min<std::size_t>(body->typeObligations.size(), 16); ++index) {
            const auto &bound = body->typeObligations[index];
            std::cerr << " kind=" << unsigned(bound.type.kind) << ':';
            region(bound.region);
            std::cerr << " free=";
            for (const auto free : lang::freeTypeLifetimes(bound.type)) {
              region(free); std::cerr << ',';
            }
          }
          std::cerr << '\n';
          for (const auto &statement : info->declaration->body()->statements()) {
            const auto *returned = lang::stmtAs<lang::ReturnStmt>(statement.get());
            const auto *call = returned ? lang::exprAs<lang::Call>(returned->value().get()) : nullptr;
            if (!call)
              continue;
            const auto *resolved = result.semantics.findCall(*call);
            std::cerr << name << " return call=" << (resolved ? resolved->function : 0) << " region=";
            if (resolved) region(resolved->returnType.referenceLifetime);
            if (const auto *complete = result.semantics.findBorrowDependencySet(*call)) {
              std::cerr << " roots=";
              for (const auto &dependency : complete->dependencies)
                std::cerr << dependency.place.root << '/' << dependency.parent << ',';
            }
            std::cerr << " parameter roots=";
            for (const auto &parameter : info->declaration->parameters())
              if (const auto *binding = result.semantics.findBinding(parameter))
                std::cerr << binding->symbol << ',';
            std::cerr << '\n';
          }
        }
    }
  }
  const auto *both = function(result, "both");
  lang::LifetimeBounds bothBounds;
  if (both)
    for (const auto edge : both->lifetimeContract.bounds)
      (void)bothBounds.assume(edge.longer, edge.shorter);
  expect(both && both->parameterTypes.size() == 3 &&
             bothBounds.proves(both->parameterTypes[1].referenceLifetime,
                               both->returnType.referenceLifetime) &&
             bothBounds.proves(both->parameterTypes[2].referenceLifetime,
                               both->returnType.referenceLifetime) &&
             both->completeReturnBorrow && both->completeReturnBorrow->dependencies.size() == 2,
         "distinct return paths infer every permitted supplier");
  for (const std::string source : {
           "int& publicBoundary(int& first, int& second) { return first; } int main() { return 0; }",
           "static int&<a> explicitBoundary<lifetime a, lifetime b>(int&<a> first, int&<b> second) { return second; } int main() { return 0; }",
           "static int& dangling(int& first, int& second) { int local = 0; return local; } int main() { return 0; }",
           "static int& both(bool chooseFirst, int& first, int& second) { if (chooseFirst) { return first; } return second; } int&<a> escape<lifetime a, lifetime b>(int&<a> first, int&<b> second) { return both(false, first, second); } int main() { return 0; }",
           "static int& unproductive(int& first, int& second) { return unproductive(first, second); } int main() { return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "private inference cannot change a public/explicit contract or export a local region");
  }
}

void testPrivateLifetimeWorklist() {
  const auto cacheKeys = lang::SemanticLifetimeTestAccess::stagedLifetimeKeys();
  expect(cacheKeys.repeatedHit, "an unchanged staging request can reuse its result");
  expect(cacheKeys.distinctLifetimeMiss,
         "staged results distinguish explicit lifetime substitutions");
  expect(cacheKeys.distinctGenerationMiss,
         "an inferred contract revision invalidates the previous staging environment");
  expect(cacheKeys.distinctBoundaryMiss,
         "early staged results preserve the original required-constant source boundary");
  expect(cacheKeys.workerRefreshed,
         "a changed lifetime generation rebuilds the staged worker's semantic snapshot");
  expect(cacheKeys.boundaryWorkerRefreshed,
         "a staged query worker retains the exact source availability boundary");
  constexpr std::size_t chainLength = 16;
  constexpr std::size_t independentCount = 16;
  const auto checkOrder = [&](bool leafFirst) {
    std::string source;
    for (std::size_t order = 0; order < chainLength; ++order) {
      const auto index = leafFirst ? chainLength - order - 1 : order;
      source += "static int& chain" + std::to_string(index) +
                "(int& selected, int& ignored) { return ";
      source += index + 1 == chainLength
                    ? "selected; }\n"
                    : "chain" + std::to_string(index + 1) + "(selected, ignored); }\n";
    }
    for (std::size_t index = 0; index < independentCount; ++index)
      source += "static int& independent" + std::to_string(index) +
                "(int& selected, int& ignored) { return selected; }\n";
    source += R"(
int&<a> relay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return chain0(selected, ignored);
}
int main() { int first = 3; int second = 5; return relay(first, second); }
)";
    lang::FrontendWorkMetrics metrics;
    const auto result = lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                                        .workMetrics = &metrics})
                            .analyze("private-lifetime-worklist.gti", source);
    expectValid(result, "private contract propagation is independent of declaration order");
    for (std::size_t index = 0; index < chainLength; ++index) {
      const auto *info = function(result, "chain" + std::to_string(index));
      expect(info && info->completeReturnBorrow &&
                 info->completeReturnBorrow->dependencies.size() == 1 &&
                 info->completeReturnBorrow->dependencies.front().origin == lang::BorrowOriginKind::Argument &&
                 info->completeReturnBorrow->dependencies.front().parameter == 0,
             "every relay retains the same exact productive supplier");
    }
    const auto maximumProbes = 4 * (chainLength + independentCount);
    if (metrics.semanticPrivateLifetimeProbes > maximumProbes)
      std::cerr << "Private lifetime work: candidates=" << metrics.semanticPrivateLifetimeCandidates
                << " probes=" << metrics.semanticPrivateLifetimeProbes
                << " updates=" << metrics.semanticPrivateLifetimeUpdates
                << " edges=" << metrics.semanticPrivateLifetimeDependencyEdges
                << " opaque=" << metrics.semanticPrivateLifetimeOpaqueProbes << '\n';
    expect(metrics.semanticPrivateLifetimeCandidates == chainLength + independentCount &&
               metrics.semanticPrivateLifetimeProbes <= maximumProbes,
           "a relay update revisits its dependents without rechecking unrelated private bodies");
  };
  checkOrder(false);
  checkOrder(true);

  const std::string precondition = R"(
void requireStatic(int&<static> value);
static void relay(int& value) { leaf(value); }
static void leaf(int& value) { requireStatic(value); }
)";
  const auto accepted = analyze(precondition + R"(
void valid(int&<static> value) { relay(value); }
int main() { return 0; }
)");
  expectValid(accepted, "private inference also propagates a void helper's input preconditions");
  const auto *relay = function(accepted, "relay");
  expect(relay && !relay->parameterTypes.empty() &&
             std::find(relay->lifetimeContract.bounds.begin(), relay->lifetimeContract.bounds.end(),
                       lang::LifetimeBound{relay->parameterTypes.front().referenceLifetime,
                                           lang::Lifetime::staticLifetime()}) !=
                 relay->lifetimeContract.bounds.end(),
         "a previously unconstrained caller is revisited when its callee requires static storage");
  const auto rejected = analyze(precondition + R"(
void invalid(int& value) { relay(value); }
int main() { return 0; }
)");
  expect(!rejected.semanticValid &&
             std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "a strengthened private precondition rejects a caller without the required relationship");

  const std::string cached = R"(
static constexpr bool leaf() { return true; }
static constexpr bool enabled() { return leaf(); }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) {
    if constexpr (enabled()) { return selected; }
    else { return ignored; }
  } else {
    return ignored;
  }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)";
  for (const bool reuse : {false, true}) {
    lang::FrontendWorkMetrics metrics;
    const auto result = lang::Frontend({.reuseConstantEvaluations = reuse,
                                        .stopAfter = lang::FrontendPhase::Semantics,
                                        .workMetrics = &metrics})
                            .analyze("private-lifetime-cache.gti", cached);
    expectValid(result, "private lifetime inference agrees with fresh and reused constant proofs");
    const auto *selected = function(result, "choose");
    expect(selected && selected->completeReturnBorrow &&
               selected->completeReturnBorrow->dependencies.size() == 1 &&
               selected->completeReturnBorrow->dependencies.front().parameter == 0,
           "cached branch proofs preserve the exact inferred return supplier");
    if (reuse)
      expect(metrics.semanticPrivateLifetimeOpaqueProbes != 0,
             "an unjournaled cached branch proof retains conservative contract dependencies");
  }

  const auto construction = lang::Frontend().analyze("private-lifetime-constructor.gti", R"(
struct Choice {
  bool value;
  constexpr Choice(bool selected) : value(selected) {}
  constexpr bool enabled() { return this.value; }
};
static constexpr bool enabled() { Choice choice{true}; return choice.enabled(); }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)");
  expectValid(construction,
              "private inference can demand checked constexpr constructor and method prerequisites");

  const auto dependent = lang::Frontend().analyze("private-lifetime-dependent.gti", R"(
static constexpr bool leaf() { return true; }
static constexpr bool enabled<uint64_t N>() {
  if constexpr (N == 0) { return leaf(); }
  else { return false; }
}
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled<0>()) { return selected; }
  else { return ignored; }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)");
  expectValid(dependent,
              "a constexpr callee discovered by concrete dependent-branch staging is checked in source order");
  const auto *dependentChoice = function(dependent, "choose");
  expect(dependentChoice && dependentChoice->completeReturnBorrow &&
             dependentChoice->completeReturnBorrow->dependencies.size() == 1 &&
             dependentChoice->completeReturnBorrow->dependencies.front().parameter == 0,
         "dependent staging preserves the inferred private supplier");

  const auto later = analyze(R"(
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
static constexpr bool enabled() { return true; }
int main() { return 0; }
)");
  expect(!later.semanticValid,
         "early private inference does not make a later constexpr definition source-visible");
  const auto invalid = analyze(R"(
static constexpr bool enabled() { return 1; }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int main() { return 0; }
)");
  expect(!invalid.semanticValid,
         "a failed constexpr prerequisite cannot authorize an inferred branch contract");
  const auto cycle = analyze(R"(
static constexpr bool enabled() {
  if constexpr (enabled()) { return true; }
  else { return false; }
}
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int main() { return 0; }
)");
  expect(!cycle.semanticValid,
         "a recursive constant prerequisite remains unavailable instead of publishing a provisional proof");
}

void testProjectedPatternLifetimes() {
  const auto result = analyze(R"(
enum Outcome<T> { ready(T value), failed(int code) };
class Result<T&> {
  Outcome<T&> state;
public:
  Result(int error) : state(Outcome<T&>::failed(error)) {}
  Result(T& value) : state(Outcome<T&>::ready(value)) {}
};
void inspect<lifetime a>(Result<int&<a>> value);
int main() { return 0; }
)");
  expectValid(result, "profile regions are projected from public arguments even in a failure-only constructor");
  const auto *owner = record(result, "Result");
  expect(owner && owner->genericArgumentPatternLifetimes.size() == 1 &&
             owner->genericArgumentPatternLifetimes.front().lifetime &&
             owner->genericArgumentPatternLifetimes.front().implicit &&
             owner->genericArgumentPatterns.size() == 1 &&
             owner->genericArgumentPatterns.front().referenceLifetime ==
                 lang::Lifetime::parameter(owner->genericArgumentPatternLifetimes.front().id),
         "a normalized argument pattern owns a separate projected region binder");
  if (owner && !owner->genericArgumentPatternLifetimes.empty()) {
    const auto projected = lang::Lifetime::parameter(owner->genericArgumentPatternLifetimes.front().id);
    expect(owner->lifetimeContract.valid() &&
               std::find(owner->lifetimeContract.parameters.begin(),
                         owner->lifetimeContract.parameters.end(), projected) !=
                   owner->lifetimeContract.parameters.end() &&
               std::none_of(owner->genericParameters.begin(), owner->genericParameters.end(),
                            [&](const auto &parameter) { return parameter.id == projected.id; }),
           "projected binders participate in formation without changing public generic arity");
    const auto *inspect = function(result, "inspect");
    expect(inspect && inspect->parameterTypes.size() == 1 &&
               inspect->parameterTypes.front().arguments.size() == 1 &&
               inspect->parameterTypes.front().arguments.front().kind == lang::SemanticType::Reference &&
               inspect->parameterTypes.front().arguments.front().referenceLifetime != projected,
           "public profile arguments retain their actual region independently of the declaration recipe");
  }
  const auto fixedMismatch = analyze(R"(
class Fixed<T&<static>> { int value = 0; public: Fixed() {} };
void invalid<lifetime a>(Fixed<int&<a>> value);
int main() { return 0; }
)");
  expect(!fixedMismatch.semanticValid &&
             std::any_of(fixedMismatch.diagnostics.begin(), fixedMismatch.diagnostics.end(),
                         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "a structural profile match cannot replace a fixed static lifetime with a caller region");
}

void testEnumInitializerLifetimeComposition() {
  const auto result = analyze(R"(
enum Choice { first(int& value), second(int& value), missing(int code) };
enum Wrapped<T> { present(T value), missing(int code) };
struct Selected {
  Choice state;
  Selected(int& value) : state(Choice::first(value)) {}
};
struct Nested {
  Wrapped<Choice> state;
  Nested(int& value) : state(Wrapped<Choice>::present(Choice::second(value))) {}
  Nested(int code) : state(Wrapped<Choice>::missing(code)) {}
};
struct Copied {
  Choice state;
  Copied(Choice value) : state(value) {}
};
struct ThroughConstructor {
  Copied state;
  ThroughConstructor(int& value) : state(Copied(Choice::second(value))) {}
  ThroughConstructor(int code) : state(Copied(Choice::missing(code))) {}
};
enum Named<lifetime a> { present(int&<a> value), missing(int code) };
struct Explicit<lifetime a> {
  Named<a> state;
  Explicit(int&<a> value) : state(Named<a>::present(value)) {}
};
int main() { return 0; }
)");
  expectValid(result, "explicit enum initializer trees compose active payload dependencies and exact nested guards");
  for (const auto name : {"Selected", "Nested", "ThroughConstructor"}) {
    const auto *owner = record(result, name);
    const auto *lifecycle = owner && owner->declaration
                                ? result.semantics.findClassLifecycle(*owner->declaration)
                                : nullptr;
    bool connected = false;
    if (lifecycle)
      for (const auto &constructor : lifecycle->constructors) {
        if (constructor.parameterTypes.size() != 1 ||
            constructor.parameterTypes.front().kind != lang::SemanticType::Reference)
          continue;
        const auto input = constructor.parameterTypes.front().referenceLifetime;
        connected |= std::any_of(constructor.lifetimeContract.bounds.begin(),
                                 constructor.lifetimeContract.bounds.end(),
                                 [&](const auto &bound) {
          return bound.longer == input &&
                 std::any_of(owner->genericParameters.begin(), owner->genericParameters.end(),
                             [&](const auto &parameter) {
            return parameter.lifetime && parameter.implicit &&
                   bound.shorter == lang::Lifetime::parameter(parameter.id);
          });
        });
      }
    if (!connected) {
      std::cerr << "Enum initializer " << name << " owner slots:";
      if (owner)
        for (const auto &parameter : owner->genericParameters)
          if (parameter.lifetime)
            std::cerr << ' ' << parameter.id << (parameter.implicit ? "i" : "e");
      std::cerr << '\n';
      if (lifecycle)
        for (const auto &constructor : lifecycle->constructors) {
          std::cerr << "  ctor " << constructor.id << " inputs:";
          for (const auto &type : constructor.parameterTypes)
            for (const auto region : lang::freeTypeLifetimes(type))
              std::cerr << ' ' << static_cast<unsigned>(region.kind) << ':' << region.id;
          std::cerr << " bounds:";
          for (std::size_t index = 0;
               index < std::min<std::size_t>(8, constructor.lifetimeContract.bounds.size()); ++index) {
            const auto bound = constructor.lifetimeContract.bounds[index];
            std::cerr << ' ' << static_cast<unsigned>(bound.longer.kind) << ':' << bound.longer.id
                      << "->" << static_cast<unsigned>(bound.shorter.kind) << ':' << bound.shorter.id;
          }
          std::cerr << '\n';
        }
    }
    expect(connected, std::string("active enum input-to-anonymous-storage relation for ") + name);
  }
  const auto named = analyze(R"(
enum Named<lifetime a> { present(int&<a> value), missing(int code) };
struct Fixed<lifetime a> {
  Named<a> state;
  Fixed(int& value) : state(Named<a>::present(value)) {}
};
int main() { return 0; }
)");
  expect(!named.semanticValid,
         "enum initializer summaries do not invent relationships into named owner lifetimes");
  const auto wrong = analyze(R"(
enum Left { present(int& value) };
enum Right { present(int& value) };
struct Holder { Left state; Holder(int& value) : state(Right::present(value)) {} };
int main() { return 0; }
)");
  expect(!wrong.semanticValid,
         "matching payload spelling does not substitute a different enum declaration");

  const auto pair = analyze(R"(
enum Reading<lifetime a> { ready(int&<a> value), missing(int code) };
struct Pair<lifetime a, lifetime b> {
  Reading<a> first;
  Reading<b> second;
  Pair(Reading<a> left, Reading<b> right) : first(left), second(right) {}
  Pair(int code) : first(Reading<a>::missing(code)), second(Reading<b>::missing(code)) {}
};
void repeat<lifetime a>(Reading<a> source) {
  Pair<a, a> pair{source, source};
}
int main() { Pair<static, static> empty{0}; return 0; }
)");
  expectValid(pair, "independent conditional fields preserve complete constructor domains");
  const auto *pairOwner = record(pair, "Pair");
  const auto *pairLifecycle = pairOwner && pairOwner->declaration
      ? pair.semantics.findClassLifecycle(*pairOwner->declaration) : nullptr;
  bool completePair = false;
  bool completeEmpty = false;
  if (pairLifecycle)
    for (const auto &constructor : pairLifecycle->constructors) {
      if (!constructor.completeBorrow)
        continue;
      const auto &summary = *constructor.completeBorrow;
      if (constructor.parameterTypes.size() == 2)
        completePair |= summary.dependencies.size() == 2 &&
            summary.possibleAlternatives.size() == 4 &&
            summary.dependencies[0].resultFields.size() == 2 &&
            summary.dependencies[1].resultFields.size() == 2 &&
            summary.dependencies[0].resultFields.front() !=
                summary.dependencies[1].resultFields.front();
      if (constructor.parameterTypes.size() == 1)
        completeEmpty |= summary.dependencies.empty() &&
            summary.possibleAlternatives.size() == 2;
    }
  expect(completePair, "constructor publication retains both independent guarded field paths");
  expect(completeEmpty, "two selected empty fields publish two domains and no borrowed origin");

  const auto exclusive = analyze(R"(
enum Exclusive<lifetime a> { ready(mut int&<a> value), missing(int code) };
struct Duplicated<lifetime a> {
  Exclusive<a> first;
  Exclusive<a> second;
  Duplicated(mut int&<a> value)
      : first(Exclusive<a>::ready(value)), second(Exclusive<a>::ready(value)) {}
};
int main() { return 0; }
)");
  expect(!exclusive.semanticValid &&
             std::any_of(exclusive.diagnostics.begin(), exclusive.diagnostics.end(),
                         [](const auto &diagnostic) {
               return diagnostic.code == "GTI-S2045" &&
                   diagnostic.message.find("duplicate an exclusive") != std::string::npos;
             }),
         "independent conditional fields cannot duplicate one exclusive source");
}

void testNamedProjectedPatternLifetimes() {
  const std::string declarations = R"(
class Borrowed<T&<source>> {
  T&<source> stored;
public:
  using Reference = T&<source>;
  Borrowed(T&<source> value) : stored(value) {}
  T&<source> get() { return this.stored; }
};
int&<a> extract<lifetime a>(Borrowed<int&<a>> value) { return value.get(); }
)";
  const auto result = analyze(declarations + R"(
int main() { int value = 7; Borrowed<int&> view{value}; return extract(view); }
)");
  expectValid(result, "a named projected region preserves the exact reference shell through fields and methods");
  const auto *owner = record(result, "Borrowed");
  expect(owner && owner->genericParameters.size() == 1 &&
             !owner->genericParameters.front().lifetime &&
             owner->genericArgumentPatternLifetimes.size() == 1 &&
             owner->genericArgumentPatternLifetimes.front().lifetime &&
             !owner->genericArgumentPatternLifetimes.front().implicit &&
             owner->genericArgumentPatternLifetimes.front().name.lexeme == "source",
         "named projected binders do not become separately supplied lifetime arguments");
  if (owner && owner->genericArgumentPatternLifetimes.size() == 1)
    expect(owner->fields.size() == 1 &&
               owner->fields.front().type.referenceLifetime ==
                   lang::Lifetime::parameter(owner->genericArgumentPatternLifetimes.front().id),
           "an explicitly related stored reference uses the projected binder");
  const auto escaping = analyze(declarations + R"(
int&<static> invalid<lifetime a>(Borrowed<int&<a>> value) { return value.get(); }
int main() { return 0; }
)");
  expect(!escaping.semanticValid && std::any_of(escaping.diagnostics.begin(), escaping.diagnostics.end(),
             [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "a named projected lifetime does not widen the selected external region");
  const auto kindConflict = analyze("class Invalid<T&<T>> {}; int main() { return 0; }");
  expect(!kindConflict.semanticValid &&
             std::any_of(kindConflict.diagnostics.begin(), kindConflict.diagnostics.end(),
                         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2126"; }),
         "a projected lifetime name cannot also bind a type");
  for (const char *source : {
           "class Duplicate<T&<a>, U&<a>> {}; class Duplicate<T&<a>, U&<b>> {}; int main() { return 0; }",
           "class Duplicate<R&<a>(T&<a>)> {}; class Duplicate<R&<b>(T&<c>)> {}; int main() { return 0; }"}) {
    const auto duplicate = analyze(source);
    expect(!duplicate.semanticValid &&
               std::any_of(duplicate.diagnostics.begin(), duplicate.diagnostics.end(),
                           [](const auto &diagnostic) { return diagnostic.code == "GTI-S2006"; }),
           "declaration families cannot dispatch on projected lifetime relationships");
  }
}

void testNominalFormation() {
  const std::string declarations = R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
struct Holder<T, lifetime a> { T&<a> value; Holder(T&<a> input) : value(input) {} };
struct Envelope<T, lifetime a> { Holder<T, a> value; Envelope(Holder<T, a> input) : value(input) {} };
)";
  const auto result = analyze(declarations + R"(
void inspect<lifetime a, lifetime b>(Envelope<View<b>, a> value);
int main() { return 0; }
)");
  expectValid(result, "nested nominal formation premises are normalized before signature publication");
  const auto *inspect = function(result, "inspect");
  expect(inspect && inspect->lifetimeContract.valid() &&
             std::any_of(inspect->lifetimeContract.typeBounds.begin(),
                         inspect->lifetimeContract.typeBounds.end(), [](const auto &bound) {
                           return bound.type.kind == lang::SemanticType::Class &&
                                  !bound.type.lifetimeArguments.empty();
                         }),
         "a signature receiving nested storage retains its substituted type-outlives premise");
  const auto invalid = analyze(declarations + R"(
void invalid<lifetime a>(View<a>& input) {
  Holder<View<a>, static>* impossible = nullptr;
}
int main() { return 0; }
)");
  expect(!invalid.semanticValid &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) { return diagnostic.code == "GTI-S2127"; }),
         "even an uninitialized pointer target must satisfy its nominal lifetime formation requirements");
}

void testNativeInvocationLifetimes() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const std::string declarations = R"(
namespace gti_internal { class text_view<lifetime storage> {}; }
namespace std { using string_view<lifetime storage> = gti_internal::text_view<storage>; }
)";
  const auto native = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics}).analyze(
        "native-invocation-lifetimes.gti", std::move(source), {prelude},
        {{prelude.string(), declarations}}, {root});
  };
  const auto result = native(R"(
using Count = (std::string_view) -> int32_t;
using Pair = (std::string_view, std::string_view) -> int32_t;
int32_t length(std::string_view value) { return 0; }
int32_t named<lifetime a>(std::string_view<a> value) { return 0; }
Count selected() { return length; }
Count namedTarget() { return named; }
struct Holder { Count callback = nullptr; Holder(Count value) : callback(value) {} };
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = length; unsafe { return callback("text"); } }
)");
  expectValid(result, "native callback invocation lifetimes normalize and apply without becoming pointer storage");
  const auto *count = alias(result, "Count");
  const auto *pair = alias(result, "Pair");
  const auto *owner = record(result, "Holder");
  expect(count && count->type.kind == lang::SemanticType::NativeFunction &&
             count->type.quantifiedLifetimes.size() == 1 &&
             lang::freeTypeLifetimes(count->type).empty(),
         "one elided native parameter introduces one invocation universal");
  expect(pair && pair->type.quantifiedLifetimes.size() == 2 &&
             pair->type.arguments[1].lifetimeArguments != pair->type.arguments[2].lifetimeArguments,
         "independent native input annotations remain distinct invocation slots");
  expect(owner && owner->genericParameters.empty(),
         "a stored native pointer does not add anonymous owner regions for its future inputs");
  for (const auto source : {
           R"(using Count = (std::string_view) -> int32_t;
int32_t fixed(std::string_view<static> value) { return 0; }
Count invalid() { return fixed; } int main() { return 0; })",
           R"(using Count = (std::string_view<static>) -> int32_t;
int32_t invalid<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
} int main() { return 0; })"}) {
    const auto rejected = native(source);
    expect(!rejected.semanticValid,
           "native target adaptation and invocation preserve fixed static storage requirements");
  }
}

void testCStringRegions() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  // This owning capability test isolates the trusted declaration mechanism.
  // The complete standard library has separate source/runtime acceptance.
  const std::string capabilityPrelude = R"(
namespace gti_internal { class c_string<lifetime storage> {}; }
using c_string<lifetime storage> = gti_internal::c_string<storage>;
)";
  const auto analyzeCString = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics}).analyze(
        "cstring-lifetimes.gti", std::move(source), {prelude},
        {{prelude.string(), capabilityPrelude}}, {root});
  };
  const auto result = analyzeCString(R"(
c_string<region> relayCString<lifetime region>(c_string<region> value) { return value; }
void inspectCString<lifetime named>(c_string<named> explicitValue, c_string elidedValue);
c_string<static> staticCString();
int main() { return 0; }
)");
  expectValid(result, "C-string borrowed storage has named, elided, and static lifetime slots");
  const auto *relay = function(result, "relayCString");
  expect(relay && relay->returnType.kind == lang::SemanticType::CString &&
             relay->returnType.lifetimeArguments.size() == 1 &&
             relay->returnType.lifetimeArguments == relay->parameterTypes.front().lifetimeArguments,
         "C-string aliases preserve their declared input/result region relationship");
  const auto *inspect = function(result, "inspectCString");
  expect(inspect && inspect->parameterTypes.size() == 2 &&
             inspect->parameterTypes[0].lifetimeArguments.size() == 1 &&
             inspect->parameterTypes[1].lifetimeArguments.size() == 1 &&
             inspect->parameterTypes[0].lifetimeArguments != inspect->parameterTypes[1].lifetimeArguments,
         "unrelated omitted C-string input regions normalize independently");
  const auto *staticResult = function(result, "staticCString");
  expect(staticResult && staticResult->returnType.lifetimeArguments ==
                            std::vector{lang::Lifetime::staticLifetime()},
         "C-string declarations retain an explicit static storage contract");
  const auto invalid = analyzeCString("void invalid(c_string<int> text); int main() { return 0; }");
  expect(!invalid.semanticValid && std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
             [](const auto &diagnostic) { return diagnostic.code == "GTI-S2126"; }),
         "C-string lifetime slots reject ordinary runtime type arguments");
}

void testLambdaLifetimeInference() {
  const auto result = analyze(R"(
int&<a> relay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  auto first = [](int& left, int& right) -> int& { return left; };
  return first(selected, ignored);
}
int main() {
  auto both = [](bool chooseFirst, int& left, int& right) -> int& {
    if (chooseFirst) { return left; }
    return right;
  };
  int left = 1;
  int right = 2;
  return both(true, left, right);
}
)");
  expectValid(result, "local lambdas infer complete parameter lifetime suppliers");
  const auto *main = function(result, "main");
  const auto *binding = main && main->declaration && main->declaration->body()
      ? lang::stmtAs<lang::VariableDecl>(main->declaration->body()->statements().front().get()) : nullptr;
  const auto *syntax = binding ? lang::exprAs<lang::Lambda>(binding->initializer().get()) : nullptr;
  const auto *lambda = syntax ? result.semantics.findLambda(*syntax) : nullptr;
  expect(lambda && lambda->lifetimeContract.valid() &&
             lambda->lifetimeContract.origin == lang::LifetimeContractOrigin::BodyInferred &&
             lambda->completeReturnBorrow && lambda->completeReturnBorrow->dependencies.size() == 2,
         "lambda metadata records both productive suppliers in the shared dependency summary");

  for (const std::string source : {
           "int&<a> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> second) { auto both = [](bool choice, int& left, int& right) -> int& { if (choice) { return left; } return right; }; return both(false, first, second); } int main() { return 0; }",
           "int main() { auto dangling = []() -> int& { int local = 0; return local; }; return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "lambda inference preserves every supplier and cannot return body-local storage");
  }
}

void testLambdaLifetimeEnvironments() {
  const auto result = analyze(R"(
int&<a> ambient<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  auto fixed = [](int&<a> value) -> int&<a> { return value; };
  return fixed(selected);
}
int&<a> externalCapture<lifetime a>(int&<a> source) {
  auto get = [&source]() -> int& { return source; };
  return get();
}
void acceptStatic<F>(F& callback) requires outlives<F, static> {}
int invoke<F>(F& callback) { return callback(); }
int main() {
  auto identity = [](int& value) -> int& { return value; };
  acceptStatic(identity);
  int left = 1;
  int right = 2;
  auto choose = [&left, &right](bool first) -> int& {
    if (first) { return left; }
    return right;
  };
  auto owned = [left]() -> int& { return left; };
  auto external = [&left]() -> int& { return left; };
  return choose(true) + owned() + invoke(external);
}
)");
  expectValid(result, "closure environments preserve ambient and captured lifetime relationships");
  const auto *ambient = function(result, "ambient");
  const auto *binding = ambient && ambient->declaration && ambient->declaration->body()
      ? lang::stmtAs<lang::VariableDecl>(ambient->declaration->body()->statements().front().get())
      : nullptr;
  const auto *syntax = binding ? lang::exprAs<lang::Lambda>(binding->initializer().get()) : nullptr;
  const auto *lambda = syntax ? result.semantics.findLambda(*syntax) : nullptr;
  const auto actual = syntax ? result.semantics.typeOf(*syntax) : lang::SemanticType::Unknown;
  expect(lambda && lambda->type.hasLambdaShape() &&
             !lambda->environmentLifetimes.empty() &&
             lambda->type.lifetimeArguments == lambda->environmentLifetimes &&
             actual.lifetimeArguments.size() == lambda->environmentLifetimes.size() &&
             actual.lifetimeArguments != lambda->environmentLifetimes,
         "canonical lambda environment slots are distinct from each closure value's fixed choices");
  for (const std::string source : {
           "int&<b> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> second) { auto fixed = [](int&<a> value) -> int&<a> { return value; }; return fixed(second); } int main() { return 0; }",
           "int&<a> invalid<lifetime a>(int&<a> source) { int local = 1; auto get = [&local]() -> int& { return local; }; return get(); } int main() { return 0; }",
           "int&<a> invalid<lifetime a>(int&<a> source) { int local = 1; auto get = [local]() -> int& { return local; }; return get(); } int main() { return 0; }",
           "void acceptStatic<F>(F& callback) requires outlives<F, static> {} int main() { int local = 1; auto get = [&local]() -> int& { return local; }; acceptStatic(get); return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "fixed closure environments reject incompatible input, local escape and static storage promises");
  }
}
} // namespace

int main() {
  testNominalArguments();
  testTransparentAliases();
  testHigherRankedAlias();
  testScopedNominalCallableFormation();
  testAssociatedLifetimeFamily();
  testBinderDiagnostics();
  testProtocolOperationBinders();
  testExplicitFunctionKinds();
  testAnonymousNominalRegions();
  testNominalVariance();
  testSpecializationLifetimeShape();
  testExecutableLifetimeIdentity();
  testInferredCarrierRegions();
  testProjectedPatternLifetimes();
  testEnumInitializerLifetimeComposition();
  testNamedProjectedPatternLifetimes();
  testNominalFormation();
  testCStringRegions();
  testNativeInvocationLifetimes();
  testPrivateLifetimeInference();
  testPrivateLifetimeWorklist();
  testInstantiatedReferenceIdentity();
  testLambdaLifetimeInference();
  testLambdaLifetimeEnvironments();
  return failures == 0 ? 0 : 1;
}
