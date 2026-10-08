#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/language_queries.h"

#include <type_traits>

using namespace gti_test::cpp_mir_body_emitter_fixture;

static_assert(
    !std::is_default_constructible_v<lang::SemanticDirectInitializationProof>);
static_assert(!std::is_aggregate_v<lang::SemanticDirectInitializationProof>);

std::optional<lang::SemanticDirectInitializationProof>
constructionProof(const lang::HirProgram &program) {
  for (const auto &function : program.functionInstances())
    for (const auto &alias : function.body.directInitializations)
      if (alias.constructionProof)
        return alias.constructionProof;
  return std::nullopt;
}

void eraseOperation(lang::HirProgram &program, lang::HirBody &body,
                    lang::HirValueId erased, lang::HirValueId replacement) {
  const auto redirect = [&](lang::HirValueId &id) {
    if (id == erased)
      id = replacement;
  };
  const auto optional = [&](std::optional<lang::HirValueId> &id) {
    if (id)
      redirect(*id);
  };
  for (auto &value : body.values) {
    if (value.source != nullptr) {
      auto &ids = const_cast<std::vector<lang::HirValueId> &>(
          program.valueIdsForSource(*value.source));
      for (auto &id : ids)
        redirect(id);
    }
    for (auto &id : value.operands)
      redirect(id);
    for (auto &id : value.conditionalInputs)
      redirect(id);
    for (auto &id : value.sequencedInputs)
      redirect(id);
    optional(value.receiver);
    optional(value.retainedReceiverOwner);
    if (value.callPlan) {
      if (value.callPlan->receiver)
        redirect(value.callPlan->receiver->value);
      for (auto &argument : value.callPlan->arguments)
        redirect(argument.value);
    }
  }
  for (auto &statement : body.statements) {
    optional(statement.value);
    optional(statement.condition);
    optional(statement.increment);
  }
  for (auto &expression : body.fullExpressions)
    for (auto &root : expression.roots)
      redirect(root);
  for (auto &drop : body.dropObligations)
    redirect(drop.value);
  for (auto &alias : body.directInitializations) {
    redirect(alias.value);
    auto &ids = const_cast<std::vector<lang::HirValueId> &>(
        program.valueIdsForSource(*alias.source));
    for (auto &id : ids)
      redirect(id);
  }
  std::erase_if(body.values,
                [&](const auto &value) { return value.id == erased; });
}

void checkFreshResults() {
  const auto frontend = analyzeWithStandardLibrary("fresh-results.gti", R"(
class Point {
public:
  int value;
  Point(int value) : value(value) {}
};
Point make() { return Point(4); }
Point relay() { return Point(Point((make()))); }
Point adjust(Point& source) { return Point(source.value + 1); }
int main() {
  Point a = Point(make());
  Point b{Point((relay()))};
  Point existing{7};
  Point copied = Point(Point(existing));
  Point moved = Point(Point((std::move(existing))));
  Point observed = adjust(Point(make()));
  return a.value + b.value + copied.value + moved.value == 22 &&
      observed.value == 5 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "fresh wrappers compose through HIR/MIR");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "fresh-result source aliases retain an exact semantic HIR witness");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the shared producer retains verified MIR ownership/cleanup");
  std::size_t aliases = 0;
  std::size_t copies = 0;
  std::size_t moves = 0;
  for (const auto &function : frontend.hir.functionInstances())
    aliases += function.body.directInitializations.size();
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        copies += instruction.kind == lang::MirInstructionKind::Construct &&
                  instruction.constructorKind == lang::ConstructorKind::Copy;
        moves += instruction.kind == lang::MirInstructionKind::Construct &&
                 instruction.constructorKind == lang::ConstructorKind::Move;
      }
  expect(aliases == 11, "typed fresh wrappers and parentheses share values");
  expect(copies == 1 && moves == 1,
         "existing-object copy and move remain real lifecycle operations");
  std::size_t selectedFresh = 0;
  for (const auto &occurrence : frontend.semantics.database().occurrences(
           frontend.sourceGraph.entryUnit())) {
    if (!occurrence.selectedConstruction ||
        !occurrence.selectedConstruction->directInitialization)
      continue;
    const auto hover = lang::LanguageQueries().hover(
        frontend, occurrence.sourceUnit, occurrence.span.start);
    expect(hover &&
               std::any_of(hover->notes.begin(), hover->notes.end(),
                           [](const auto &note) {
                             return note.find("no additional copy, move") !=
                                    std::string::npos;
                           }),
           "compiler-owned hover identifies direct initialization, not a move");
    ++selectedFresh;
  }
  expect(selectedFresh == 7,
         "typed fresh call sites retain selected occurrences");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
           "the complete production MIR vocabulary admits the original ICE");
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "fresh results use production emission, not an AST fallback");
  }

  auto corrupted = frontend.hir;
  for (const auto &function : corrupted.functionInstances()) {
    if (function.body.directInitializations.empty())
      continue;
    auto *body = const_cast<lang::HirBody *>(&function.body);
    body->directInitializations.front().type = lang::SemanticType::Int32;
    expect(!lang::verifyHirProgramPlans(frontend.semantics, corrupted).valid(),
           "a fresh-result witness cannot change its exact class type");
    break;
  }
  corrupted = frontend.hir;
  bool removedProof = false;
  for (const auto &function : corrupted.functionInstances()) {
    auto &body = const_cast<lang::HirBody &>(function.body);
    for (auto &alias : body.directInitializations) {
      if (!alias.constructionProof)
        continue;
      alias.constructionProof.reset();
      removedProof = true;
      break;
    }
    if (removedProof)
      break;
  }
  expect(
      removedProof &&
          !lang::verifyHirProgramPlans(frontend.semantics, corrupted).valid(),
      "a construction wrapper must retain concrete semantic construction "
      "provenance");
  corrupted = frontend.hir;
  for (const auto &function : corrupted.functionInstances()) {
    if (function.body.directInitializations.empty())
      continue;
    auto *body = const_cast<lang::HirBody *>(&function.body);
    body->directInitializations.front().producer = nullptr;
    expect(!lang::verifyHirProgramPlans(frontend.semantics, corrupted).valid(),
           "a fresh-result witness cannot lose its source producer");
    break;
  }

  for (const auto kind :
       {lang::ConstructorKind::Copy, lang::ConstructorKind::Move}) {
    auto forged = frontend.hir;
    bool mutated = false;
    for (const auto &function : forged.functionInstances()) {
      auto &body = const_cast<lang::HirBody &>(function.body);
      for (const auto &value : body.values) {
        const auto *selected =
            value.source == nullptr
                ? nullptr
                : frontend.semantics.findConstruction(*value.source);
        const auto *call = lang::exprAs<lang::Call>(value.source);
        if (selected == nullptr || selected->directInitialization ||
            selected->kind != kind || call == nullptr ||
            call->arguments().size() != 1)
          continue;
        auto *input = const_cast<lang::HirValue *>(
            body.valueForSource(*call->arguments().front()));
        expect(input != nullptr,
               "real lifecycle construction retains its source value");
        if (input == nullptr)
          continue;
        // Forge both category and source indexing, rather than relying on
        // an incidental lvalue/index mismatch to reject the missing lifecycle.
        input->info.category = lang::ValueCategory::Value;
        const auto proof = constructionProof(forged);
        body.directInitializations.push_back(
            {.source = value.source,
             .producer = call->arguments().front().get(),
             .value = input->id,
             .type = value.info.type,
             .constructionProof = proof});
        auto &ids = const_cast<std::vector<lang::HirValueId> &>(
            forged.valueIdsForSource(*value.source));
        ids.push_back(input->id);
        mutated = true;
        break;
      }
      if (mutated)
        break;
    }
    const auto checked =
        lang::verifyHirProgramPlans(frontend.semantics, forged);
    expect(mutated && std::any_of(checked.errors.begin(), checked.errors.end(),
                                  [](const auto &error) {
                                    return error.find("class result alias") !=
                                           std::string::npos;
                                  }),
           "an existing-object copy/move cannot be forged into a fresh-result "
           "alias");
  }

  auto forgedCall = frontend.hir;
  bool mutated = false;
  for (const auto &function : forgedCall.functionInstances()) {
    auto &body = const_cast<lang::HirBody &>(function.body);
    for (const auto &value : body.values) {
      const auto *call = lang::exprAs<lang::Call>(value.source);
      const auto *name =
          call == nullptr ? nullptr
                          : lang::exprAs<lang::Variable>(call->callee().get());
      if (name == nullptr || name->name().lexeme != "adjust" ||
          call->arguments().size() != 1)
        continue;
      const auto *input = body.valueForSource(*call->arguments().front());
      expect(input != nullptr,
             "an ordinary factory retains its fresh argument");
      if (input == nullptr)
        continue;
      const auto proof = constructionProof(forgedCall);
      const auto source = value.source;
      const auto erased = value.id;
      const auto replacement = input->id;
      body.directInitializations.push_back(
          {.source = value.source,
           .producer = call->arguments().front().get(),
           .value = input->id,
           .type = value.info.type,
           .constructionProof = proof});
      auto &ids = const_cast<std::vector<lang::HirValueId> &>(
          forgedCall.valueIdsForSource(*source));
      ids.assign(1, replacement);
      eraseOperation(forgedCall, body, erased, replacement);
      expect(body.findValue(erased) == nullptr,
             "the mutation actually removes the ordinary factory invocation");
      mutated = true;
      break;
    }
    if (mutated)
      break;
  }
  const auto checked =
      lang::verifyHirProgramPlans(frontend.semantics, forgedCall);
  expect(mutated && std::any_of(checked.errors.begin(), checked.errors.end(),
                                [](const auto &error) {
                                  return error.find("class result alias") !=
                                         std::string::npos;
                                }),
         "an observable ordinary factory cannot be erased as a construction "
         "wrapper");
}

void checkImmovableAndBorrowedResults() {
  const auto fixed = analyzeWithStandardLibrary("fixed-results.gti", R"(
class Fixed {
public:
  int value;
  Fixed(int value) : value(value) {}
  Fixed(Fixed&) = delete;
  Fixed(Fixed&&) = delete;
};
Fixed make() { return Fixed(4); }
Fixed relay() { return Fixed(make()); }
int main() {
  auto factory = []() -> Fixed { return Fixed(7); };
  Fixed result = Fixed((relay()));
  Fixed from_lambda = Fixed(factory());
  return result.value + from_lambda.value == 11 ? 0 : 1;
}
)");
  expect(fixed.canGenerateCode(),
         "initial creation and direct publication do not require movability");
  if (fixed.canGenerateCode())
    expect(!gti_test::emitCpp(fixed).contents.empty(),
           "an immovable factory result has a production representation");
  const auto borrow = analyzeWithStandardLibrary("borrowed-results.gti", R"(
class Lens {
  int& value;
public:
  Lens(int& value) : value(value) {}
  int read() { return this.value; }
};
Lens view(int& value) { return Lens(value); }
int main() {
  int owner = 4;
  Lens result = Lens(view(owner));
  return result.read() - 4;
}
)");
  expect(borrow.canGenerateCode(),
         "direct initialization retains a carrier's original external owner");

  for (
      const auto &source : {
          R"(class Fixed { public: Fixed() {} Fixed(Fixed&) = delete;
               Fixed(Fixed&&) = delete; };
               int main() { Fixed source{};
                 Fixed result = Fixed(std::move(source)); return 0; })",
          R"(class Fixed { public: Fixed() {} Fixed(Fixed&) = delete;
               Fixed(Fixed&&) = delete; };
               Fixed make() { return Fixed(); }
               void consume(Fixed value) {}
               int main() { consume(Fixed(make())); return 0; })",
          R"(class Lens { int& value; public: Lens(int& value) : value(value) {} };
               Lens bad() { int local = 4; return Lens(Lens(local)); }
               int main() { return 0; })"}) {
    const auto negative = lang::Frontend().analyze(
        "invalid-fresh-result.gti", source,
        {std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti"});
    expect(!negative.semanticValid &&
               std::any_of(negative.diagnostics.begin(),
                           negative.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2018" ||
                                    diagnostic.code == "GTI-S2045";
                           }),
           "fresh initialization does not bypass a deleted real move or escape "
           "loan");
  }
}

void checkConcreteInvocationErasure() {
  const auto frontend = analyze("concrete-alias-provenance.gti", R"(
class Point {
public:
  int value;
  Point(int value) : value(value) {}
};
Point make() { return Point(4); }
Point adjust(Point source) { return Point(source.value + 1); }
class Factory {
public:
  Factory() {}
  Point operator()(Point source) { return Point(source.value + 1); }
};
Point choose<uint64_t N>() {
  if constexpr (N == 1) { return adjust(make()); }
  else { return Point(make()); }
}
int main() {
  Point proof = Point(make());
  Factory factory{};
  Point called = factory(make());
  Point selected = choose<1>();
  Point direct = choose<2>();
  return proof.value == 4 && called.value == 5 && selected.value == 5 &&
      direct.value == 4 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "concrete provenance preserves nominal calls and dependent branches");
  if (!frontend.canGenerateCode())
    return;
  const auto proof = constructionProof(frontend.hir);
  expect(proof.has_value(),
         "a genuine construction supplies source-bound provenance");
  bool concreteProof = false;
  for (const auto &function : frontend.hir.functionInstances())
    for (const auto &alias : function.body.directInitializations)
      if (alias.constructionProof &&
          frontend.semantics.findConstruction(*alias.source) == nullptr) {
        concreteProof = true;
        expect(alias.constructionProof->source() == alias.source &&
                   alias.constructionProof->producer() == alias.producer &&
                   alias.constructionProof->type() == alias.type &&
                   alias.constructionProof->snapshot() ==
                       frontend.semantics.analysisSeal().programSnapshot,
               "a skipped primary arm retains its concrete construction proof");
      }
  expect(concreteProof,
         "the positive dependent arm needs concrete rather than primary proof");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "both concrete dependent arms and nominal calls remain representable");
  for (const std::string_view name : {"factory", "adjust"}) {
    auto forged = frontend.hir;
    bool mutated = false;
    for (const auto &function : forged.functionInstances()) {
      auto &body = const_cast<lang::HirBody &>(function.body);
      for (const auto &value : body.values) {
        const auto *call = lang::exprAs<lang::Call>(value.source);
        const auto *callee =
            call == nullptr
                ? nullptr
                : lang::exprAs<lang::Variable>(call->callee().get());
        if (callee == nullptr || callee->name().lexeme != name ||
            call->arguments().size() != 1)
          continue;
        if (name == "factory") {
          expect(frontend.semantics.findCall(*call) == nullptr &&
                     frontend.semantics.findOperator(*call) != nullptr,
                 "nominal invocation is not an ordinary-call record");
        } else {
          expect(frontend.semantics.findCall(*call) == nullptr &&
                     frontend.semantics.findConstruction(*call) == nullptr &&
                     frontend.semantics.directInitializationProof(*call) ==
                         std::nullopt,
                 "the deferred arm lacks a primary selection, not permission "
                 "to elide");
        }
        const auto *input = body.valueForSource(*call->arguments().front());
        expect(input != nullptr && input->info.type == value.info.type &&
                   input->info.category == lang::ValueCategory::Value,
               "the forgery keeps an exact fresh result child");
        if (input == nullptr)
          continue;
        const auto erased = value.id;
        const auto replacement = input->id;
        const auto source = value.source;
        const auto type = value.info.type;
        expect(
            proof && proof->source() != source && proof->type() == type &&
                proof->snapshot() ==
                    frontend.semantics.analysisSeal().programSnapshot,
            "same-type, same-snapshot proof still cannot name an invocation");
        body.directInitializations.push_back(
            {.source = source,
             .producer = call->arguments().front().get(),
             .value = replacement,
             .type = type,
             .constructionProof = proof});
        eraseOperation(forged, body, erased, replacement);
        auto &ids = const_cast<std::vector<lang::HirValueId> &>(
            forged.valueIdsForSource(*source));
        ids.assign(1, replacement);
        expect(body.findValue(erased) == nullptr &&
                   std::none_of(body.values.begin(), body.values.end(),
                                [&](const auto &remaining) {
                                  return remaining.source == source;
                                }),
               "the forgery removes the operation instead of leaving an "
               "executable source");
        mutated = true;
        break;
      }
      if (mutated)
        break;
    }
    const auto verified =
        lang::verifyHirProgramPlans(frontend.semantics, forged);
    expect(mutated &&
               std::any_of(verified.errors.begin(), verified.errors.end(),
                           [](const auto &error) {
                             return error.find("class result alias") !=
                                    std::string::npos;
                           }),
           "true nominal/dependent factory erasure requires actual concrete "
           "construction provenance");
  }
}

void checkConcreteAndConstantResults() {
  const auto frontend = analyzeWithStandardLibrary("concrete-results.gti", R"(
class Box<T> {
public:
  T value;
  Box(T value) : value(value) {}
  Box(Box<T>&) = delete;
  Box(Box<T>&&) = delete;
};
Box<T> make<T>(T value) { return Box<T>(value); }
Box<T> relay<T>(T value) { return Box<T>((make<T>(value))); }
class Number {
public:
  int value;
  constexpr Number(int value) : value(value) {}
  Number(Number&) = delete;
  Number(Number&&) = delete;
};
constexpr Number number() { return Number(Number(4)); }
constexpr Number constant = Number(number());
static_assert(constant.value == 4);
int main() {
  Box<int> result = Box<int>(relay<int>(7));
  Number runtime = Number(number());
  return result.value == 7 && runtime.value == constant.value ? 0 : 1;
}
)");
  expect(
      frontend.canGenerateCode(),
      "concrete generic reanalysis and constexpr share the fresh-result rule");
  if (frontend.canGenerateCode())
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "constant and concrete results retain production MIR execution");
}

void checkBorrowedResultComposition() {
  const std::string declarations = R"(
class Update {
  int& step;
  mut int& value;
public:
  Update(int& step, mut int& value) : step(step), value(value) {}
  void apply() mut { this.value += this.step; }
};
Update relay(int& step, mut int& value) {
  return Update(Update((view(step, value))));
}
Update view(int& step, mut int& value) { return Update(step, value); }
)";
  const auto positive = lang::Frontend().analyze("fresh-borrow-composition.gti",
                                                 declarations + R"(
int main() {
  int step = 4;
  mut int value = 3;
  mut Update result{Update((relay(step, value)))};
  result.apply();
  value += 1;
  return value == 8 ? 0 : 1;
}
)");
  expect(positive.canGenerateCode(),
         "nested direct initialization preserves mixed owners independent of "
         "factory declaration order");
  if (positive.canGenerateCode()) {
    std::size_t composed = 0;
    for (const auto &function : positive.hir.functionInstances()) {
      if (function.returnBorrowFields.size() != 2)
        continue;
      expect(function.returnBorrowFields[0].parameter == 0 &&
                 function.returnBorrowFields[0].access ==
                     lang::AccessMode::ReadOnly &&
                 function.returnBorrowFields[1].parameter == 1 &&
                 function.returnBorrowFields[1].access ==
                     lang::AccessMode::Mutable,
             "field-wise return facts preserve both exact owners and access");
      ++composed;
    }
    expect(composed == 2,
           "both factory and relay publish field-wise contracts");
    expect(lang::verifyMirProgram(positive.mir).valid(),
           "mixed result dependencies survive independent MIR verification");
  }

  struct Negative {
    std::string_view body;
    std::string_view code;
    std::string_view message;
    std::string_view anchor;
    bool closingParen = false;
  };
  for (const auto &test : std::array{
           Negative{R"(int main() { int step = 4; mut int value = 3;
                mut Update result = Update(relay(step, value));
                value += 1; result.apply(); return value; })",
                    "GTI-S2017",
                    "Cannot assign to storage while an overlapping borrow",
                    "value += 1"},
           Negative{R"(Update bad(int& step) { mut int local = 3;
                return Update(relay(step, local)); }
                int main() { return 0; })",
                    "GTI-S2045", "Every returned borrowed field must derive",
                    "Update(relay(step, local))", true},
           Negative{R"(int main() { mut int value = 3;
                mut Update result = Update(relay(value, value));
                result.apply(); return 0; })",
                    "GTI-S2017", "Call creates overlapping reference accesses",
                    "relay(value, value)", true}}) {
    const std::string source = declarations + std::string(test.body);
    const auto negative = lang::Frontend().analyze(
        "invalid-fresh-borrow-composition.gti", source);
    const auto *unit =
        negative.sourceGraph.findUnit(negative.sourceGraph.entryUnit());
    const auto start = source.find(test.anchor) +
                       (test.closingParen ? test.anchor.size() - 1 : 0);
    const auto length = test.closingParen ? 1u : 5u;
    const auto found = std::find_if(
        negative.diagnostics.begin(), negative.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == test.code &&
                 diagnostic.phase == lang::DiagnosticPhase::Semantics &&
                 diagnostic.severity == lang::DiagnosticSeverity::Error &&
                 diagnostic.message.find(test.message) != std::string::npos &&
                 unit != nullptr &&
                 diagnostic.primary.source == unit->path.string() &&
                 diagnostic.primary.start == start &&
                 diagnostic.primary.end == start + length;
        });
    expect(!negative.semanticValid && !negative.canGenerateCode() &&
               found != negative.diagnostics.end(),
           "fresh-result borrowing rejects the intended access/escape at its "
           "exact source span");
    expect(std::none_of(negative.diagnostics.begin(),
                        negative.diagnostics.end(),
                        [&](const auto &diagnostic) {
                          return diagnostic.severity ==
                                     lang::DiagnosticSeverity::Error &&
                                 diagnostic.code != test.code;
                        }),
           "an unrelated parser/backend rejection cannot satisfy a borrow "
           "regression");
    if (!test.closingParen && found != negative.diagnostics.end()) {
      expect(found->fixes.empty() && !found->hints.empty() &&
                 std::any_of(
                     found->related.begin(), found->related.end(),
                     [&](const auto &related) {
                       return related.message ==
                                  "The borrowed value is used later here." &&
                              related.span.source == found->primary.source &&
                              related.span.start ==
                                  source.find("result.apply()");
                     }),
             "conflicting mutation explains the later required use without a "
             "safety-changing fix");
    }
  }
}

int main() {
  checkFreshResults();
  checkImmovableAndBorrowedResults();
  checkConcreteInvocationErasure();
  checkConcreteAndConstantResults();
  checkBorrowedResultComposition();
  return failures == 0 ? 0 : 1;
}
