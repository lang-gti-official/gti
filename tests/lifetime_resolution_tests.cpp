#include "gti/frontend.h"
#include "gti/lifetime_types.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

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
  for (const auto name : {"first", "recurse", "mutualFirst", "mutualSecond"}) {
    const auto *info = function(result, name);
    if (!info) {
      expect(false, "private helper metadata is available");
      continue;
    }
    const auto selected = std::string_view(name) == "first" ? 0U : 1U;
    const auto ignored = selected + 1;
    lang::LifetimeBounds bounds;
    for (const auto edge : info->lifetimeContract.bounds)
      (void)bounds.assume(edge.longer, edge.shorter);
    expect(info->lifetimeContract.origin == lang::LifetimeContractOrigin::BodyInferred &&
               info->lifetimeContract.valid() && info->parameterTypes.size() > ignored &&
               bounds.proves(info->parameterTypes[selected].referenceLifetime,
                             info->returnType.referenceLifetime) &&
               !bounds.proves(info->parameterTypes[ignored].referenceLifetime,
                              info->returnType.referenceLifetime) &&
               info->completeReturnBorrow &&
               info->completeReturnBorrow->dependencies.size() == 1 &&
               info->completeReturnBorrow->dependencies.front().origin ==
                   lang::BorrowOriginKind::Argument &&
               info->completeReturnBorrow->dependencies.front().parameter == selected,
           std::string("private recursive fixed point preserves only productive return suppliers: ") + name);
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
  testNominalFormation();
  testCStringRegions();
  testPrivateLifetimeInference();
  testInstantiatedReferenceIdentity();
  testLambdaLifetimeInference();
  testLambdaLifetimeEnvironments();
  return failures == 0 ? 0 : 1;
}
