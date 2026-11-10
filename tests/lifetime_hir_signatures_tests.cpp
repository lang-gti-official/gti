#include "gti/frontend.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void unprovedReferenceContents() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
                          .analyze("unproved-reference-contents.gti",
                                   "int main() { return 0; }");
  expect(result.semanticValid && result.hir.valid() &&
             !result.hir.functionInstances().empty(),
         "the reference contents mutation control must start from checked HIR");
  const auto &function = result.hir.functionInstances().front();
  expect(!function.body.values.empty(),
         "the contents control needs an evaluated value");
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto forged = result.hir;
    auto &body =
        const_cast<HirBody &>(forged.findFunctionInstance(function.id)->body);
    auto &value = body.values.front();
    const SemanticLoanDependency dependency{
        .referenceContents = SemanticReferenceContentsSource{
            .openingLoan = 991, .fields = {fieldProjection(992)}}};
    if (mutation == 0)
      value.referenceContentsLoan = 991;
    if (mutation == 1)
      value.borrowDependencies = SemanticBorrowDependencySet{{dependency}, {}};
    if (mutation == 2)
      value.captureBorrowDependencies = {dependency};
    if (mutation >= 3) {
      HirLoan loan{.semanticLoan = 991, .kind = SemanticLoanKind::StoredValue};
      if (mutation == 3)
        loan.referenceContents = dependency.referenceContents;
      if (mutation == 4)
        loan.contentsOpening = HirReferenceContentsOpening{};
      if (mutation == 5)
        loan.additionalDependencies = {dependency};
      body.loans.push_back(std::move(loan));
    }
    const auto proof = verifyHirProgramPlans(result.semantics, forged);
    expect(
        std::any_of(proof.errors.begin(), proof.errors.end(),
                    [](const auto &error) {
                      return error == "HIR reference contents require checked "
                                      "opening replay before publication";
                    }),
        "unproved contents facts cannot disappear at the HIR-to-MIR boundary");
  }
}

void preparedReceiverRegion() {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result =
      Frontend({.stopAfter = FrontendPhase::Mir})
          .analyze("prepared-receiver-region.gti", R"gti(
struct Ticket {
  int value;
  Ticket(int initial) : value(initial) {}
  int take() && { return this.value; }
};
int consume(Ticket ticket) { return std::move(ticket).take(); }
int main() { return consume(Ticket(7)) - 7; }
)gti",
                   {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "a moved receiver borrows its exact prepared destination "
                  "until invocation");
  }
  bool checked = false;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.kind != MirInstructionKind::CallInput ||
            stage.callInputRole != MirCallInputRole::Receiver ||
            stage.callInputKind != HirCallInputKind::MoveValue ||
            !stage.preparedParameterDrop || !stage.destination)
          continue;
        const auto origin =
            std::find_if(function.body.lifetimeBody.origins.begin(),
                         function.body.lifetimeBody.origins.end(),
                         [&](const auto &candidate) {
                           return candidate.place == *stage.destination &&
                                  candidate.source == stage.hirValue;
                         });
        if (origin == function.body.lifetimeBody.origins.end())
          continue;
        checked = true;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = result.mir;
          auto *owner = const_cast<MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          auto *changed =
              &owner->body.blocks[block.id - 1]
                   .instructions[&stage - block.instructions.data()];
          if (mutation == 0)
            changed->hirValue = 0;
          if (mutation == 1)
            changed->preparedParameterDrop = 0;
          if (mutation == 2)
            changed->destination = 0;
          if (mutation == 3) {
            const auto remove = [&](const LifetimeBound &bound) {
              return bound.longer == origin->region;
            };
            std::erase_if(owner->body.lifetimeBody.obligations, remove);
            for (auto &candidateBlock : owner->body.blocks)
              for (auto &instruction : candidateBlock.instructions)
                std::erase_if(instruction.lifetimeApplication.obligations,
                              remove);
          }
          expect(!verifyMirProgram(forged).valid(),
                 "prepared receiver proof requires exact source, drop, "
                 "destination and storage-region edge");
        }
      }
  expect(checked, "the consuming receiver fixture must materialize a checked "
                  "local storage witness");
}

void contextualOperatorApplication() {
  const auto result =
      Frontend({.stopAfter = FrontendPhase::Mir})
          .analyze("contextual-operator-lifetime-application.gti", R"gti(
struct Flag {
  int value;
  Flag(int initial) : value(initial) {}
  operator bool() { return this.value != 0; }
};
int choose(Flag& flag) { if (flag) { return 1; } return 0; }
Flag makeFlag(int value) { return Flag(value); }
int unary() { if (!makeFlag(1)) { return 1; } return 0; }
int main() { Flag flag{1}; return choose(flag) - 1 + unary(); }
)gti");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "contextual operator calls preserve their checked receiver "
                  "lifetime application");
  }
  bool checked = false;
  bool temporaryOrigin = false;
  bool checkedProducer = false;
  std::size_t contextualCalls = 0;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values) {
      if (value.kind != HirValueKind::Call || !value.source)
        continue;
      const auto *conversion =
          result.semantics.findContextualConversion(*value.source);
      if (!conversion)
        continue;
      const auto *target =
          value.functionTarget
              ? result.hir.findFunctionInstance(*value.functionTarget)
              : nullptr;
      expect(target != nullptr,
             "contextual source calls retain exact HIR targets");
      if (target->declaration != conversion->function) {
        // The ordinary makeFlag call evaluates the same source expression
        // before its contextual conversion. It must be that exact receiver,
        // not another call selected solely by shared syntax or result type.
        const bool isReceiver =
            std::any_of(function.body.values.begin(),
                        function.body.values.end(), [&](const auto &consumer) {
                          const auto *selected =
                              consumer.functionTarget
                                  ? result.hir.findFunctionInstance(
                                        *consumer.functionTarget)
                                  : nullptr;
                          return consumer.kind == HirValueKind::Call &&
                                 consumer.source == value.source &&
                                 consumer.receiver == value.id && selected &&
                                 selected->declaration == conversion->function;
                        });
        expect(isReceiver, "an ordinary contextual-source producer is the "
                           "exact conversion receiver");
        checkedProducer = true;
        continue;
      }
      ++contextualCalls;
      expect(target->source == conversion->declaration && target->source &&
                 target->source->operatorName() &&
                 target->source->operatorName()->kind ==
                     OverloadedOperator::ContextualBool &&
                 conversion->kind == OverloadedOperator::ContextualBool,
             "operator bool retains its distinct checked conversion identity");
      expect(value.lifetimeApplication.complete &&
                 !value.lifetimeApplication.bindings.empty(),
             "HIR contextual calls retain the independently checked receiver "
             "substitution");
      for (const auto &origin : value.lifetimeApplication.origins) {
        const auto *source = function.body.findValue(origin.source);
        if (origin.region.kind != Lifetime::Kind::Local || !source ||
            source->source != value.source || !value.receiver ||
            source->id != *value.receiver || source->id == value.id)
          continue;
        temporaryOrigin = true;
        const auto bodyOrigin =
            std::find(function.body.lifetimeBody.origins.begin(),
                      function.body.lifetimeBody.origins.end(), origin);
        expect(bodyOrigin != function.body.lifetimeBody.origins.end(),
               "body facts retain the exact temporary receiver rather than its "
               "contextual result");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto stale = result.hir;
          auto *body = const_cast<HirBody *>(
              &stale.findFunctionInstance(function.id)->body);
          auto found = std::find(body->lifetimeBody.origins.begin(),
                                 body->lifetimeBody.origins.end(), origin);
          expect(found != body->lifetimeBody.origins.end(),
                 "origin mutation selects exact checked storage");
          if (mutation == 0) {
            found->source = value.id;
          } else if (mutation == 1) {
            auto conflict = *found;
            conflict.source = value.id;
            body->lifetimeBody.origins.push_back(conflict);
          } else {
            auto *call = const_cast<HirValue *>(body->findValue(value.id));
            auto applied =
                std::find(call->lifetimeApplication.origins.begin(),
                          call->lifetimeApplication.origins.end(), origin);
            expect(applied != call->lifetimeApplication.origins.end(),
                   "application origin matches exact body witness");
            applied->source = value.id;
          }
          expect(!MirLowerer{}.lower(stale, result.failureMetadata).valid(),
                 "contextual receiver origins reject forged conversion results "
                 "and conflicting witnesses");
        }
      }
      auto forged = result.mir;
      auto *changed = const_cast<MirFunctionInstance *>(
          forged.findFunctionInstance(function.id));
      if (!changed)
        continue;
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.hirValue == value.id) {
            instruction.lifetimeApplication.bindings.clear();
            checked = true;
          }
      if (checked)
        expect(
            !verifyMirProgram(forged).valid(),
            "a contextual call cannot lose its receiver contract application");
    }
  expect(contextualCalls == 2 && checkedProducer,
         "both contextual calls and their shared-syntax ordinary producer are "
         "checked");
  expect(
      checked,
      "the contextual operator fixture must exercise a concrete checked call");
  expect(temporaryOrigin,
         "unary contextual conversion must retain a temporary receiver origin");
}

void syntheticAssignmentReceiverOrigins() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir})
                          .analyze("synthetic-assignment-receiver.gti", R"gti(
struct Counter {
  mut int value;
  Counter(int initial) : value(initial) {}
  void operator=(int replacement) mut { this.value = replacement; }
  void operator+=(int amount) mut { this.value += amount; }
  int get() { return this.value; }
};
int main() {
  mut Counter first{1};
  mut Counter second{9};
  first = 3;
  second += 4;
  return first.get() + second.get() - 16;
}
)gti");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "assignment operators retain their exact checked physical "
                  "receiver origin");
  }
  std::size_t checked = 0;
  for (const auto &function : result.hir.functionInstances()) {
    std::vector<const HirValue *> assignments;
    for (const auto &value : function.body.values)
      if (value.kind == HirValueKind::Call &&
          dynamic_cast<const Assign *>(value.source) && value.receiver)
        assignments.push_back(&value);
    if (assignments.empty())
      continue;
    expect(
        assignments.size() == 2,
        "the assignment control needs two distinct same-type receiver objects");
    for (std::size_t index = 0; index != assignments.size(); ++index) {
      const auto &call = *assignments[index];
      const auto *receiver = function.body.findValue(*call.receiver);
      const auto *other =
          function.body.findValue(*assignments[1 - index]->receiver);
      expect(receiver && receiver->place && other && other->place &&
                 receiver->info.type == other->info.type &&
                 receiver->place != other->place,
             "synthetic assignment receivers retain distinct qualified "
             "physical places");
      expect(call.lifetimeApplication.complete &&
                 call.lifetimeApplication.origins.size() == 1,
             "assignment calls retain their checked receiver application");
      const auto &origin = call.lifetimeApplication.origins.front();
      expect(origin.source == 0 &&
                 origin.region.kind == Lifetime::Kind::Local &&
                 origin.place == *receiver->place &&
                 std::find(function.body.lifetimeBody.origins.begin(),
                           function.body.lifetimeBody.origins.end(),
                           origin) != function.body.lifetimeBody.origins.end(),
             "a syntax-only receiver contributes its checked place, never a "
             "temporary AST witness");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = result.hir;
        auto *body = const_cast<HirBody *>(
            &forged.findFunctionInstance(function.id)->body);
        auto *changed = const_cast<HirValue *>(body->findValue(call.id));
        auto applicationOrigin =
            std::find(changed->lifetimeApplication.origins.begin(),
                      changed->lifetimeApplication.origins.end(), origin);
        auto bodyOrigin = std::find(body->lifetimeBody.origins.begin(),
                                    body->lifetimeBody.origins.end(), origin);
        expect(applicationOrigin !=
                       changed->lifetimeApplication.origins.end() &&
                   bodyOrigin != body->lifetimeBody.origins.end(),
               "the coordinated mutation selects the exact application and "
               "body witness");
        for (auto *witness : {&*applicationOrigin, &*bodyOrigin}) {
          if (mutation == 0)
            witness->place = {};
          if (mutation == 1) {
            witness->place = {};
            witness->source = call.id;
          }
          if (mutation == 2)
            witness->place = *other->place;
        }
        expect(!MirLowerer{}.lower(forged, result.failureMetadata).valid(),
               "assignment receiver proof rejects missing, result-valued and "
               "unrelated same-type origins");
      }
      ++checked;
    }
  }
  expect(checked == 2,
         "both simple and compound assignment receiver origins are checked");
}

void defaultFieldCallableUniverse() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir})
                          .analyze("default-field-callable-universe.gti", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int count) {}
};
int main() { Default<int&<static>> holder{0}; return 0; }
)gti");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "a selected field default has one exact stage in its "
                  "constructor region universe");
  }
  bool checked = false;
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto *owner = result.mir.findClassInstance(constructor.owner);
    if (!owner || constructor.initializers.size() != 1 ||
        owner->declaredFields.size() != 1)
      continue;
    const auto &initializer = constructor.initializers.front();
    if (initializer.targetType.kind != SemanticType::Enum ||
        initializer.targetType == owner->declaredFields.front().type)
      continue;
    checked = true;
    auto forged = result.mir;
    auto *changed = const_cast<MirConstructorInstance *>(
        forged.findConstructorInstance(constructor.id));
    changed->initializers.front().targetType =
        owner->declaredFields.front().type;
    expect(!verifyMirProgram(forged).valid(),
           "a layout-region field type cannot replace the selected constructor "
           "field type");
    forged = result.mir;
    changed = const_cast<MirConstructorInstance *>(
        forged.findConstructorInstance(constructor.id));
    bool removed = false;
    for (auto &block : changed->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Initialize &&
            instruction.constructorInitializer == 1) {
          instruction.constructorInitializer = 0;
          removed = true;
        }
    expect(removed && !verifyMirProgram(forged).valid(),
           "a cleanup-free default still needs its exact initializer stage");
    break;
  }
  expect(checked, "the default field fixture must distinguish layout and "
                  "callable region identities");
}

void selectedBaseInputs() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir})
                          .analyze("selected-base-input-lanes.gti", R"gti(
struct SumBase {
  int value;
  SumBase(int first, int second) : value(first * 10 + second) {}
};
struct SumDerived<Args...> : public SumBase {
  SumDerived(Args... values) : SumBase(values...) {}
};
struct RegionBase<lifetime a> {
  int&<a> value;
  int marker;
  RegionBase<lifetime input>(int&<input> source, int selected = 7)
      requires outlives<input, a> : value(source), marker(selected) {}
};
struct RegionDerived<lifetime a> : public RegionBase<a> {
  RegionDerived(int&<a> source) : RegionBase<a>(source) {}
};
int main() {
  int number = 5;
  SumDerived<int, int> sum{3, 8};
  RegionDerived view{number};
  return sum.value + view.value + view.marker - 50;
}
)gti");
  if (!result.semanticValid || !result.hirValid || !result.mirValid ||
      !result.mir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(result.mir).errors)
      std::cerr << error.message << '\n';
    expect(false, "selected Base constructors retain exact pack lanes, "
                  "lifetime binders and defaults through MIR");
  }
  bool checked = false;
  for (const auto &constructor : result.hir.constructorInstances()) {
    if (!constructor.source ||
        constructor.source->name().lexeme != "SumDerived")
      continue;
    expect(constructor.initializers.size() == 1 &&
               constructor.initializers.front().arguments.size() == 2 &&
               constructor.initializers.front().explicitArgumentCount == 2,
           "a written pack expansion maps to two exact explicit Base inputs");
    for (std::size_t index = 0; index < 2; ++index) {
      const auto *value = constructor.body.findValue(
          constructor.initializers.front().arguments[index]);
      expect(value && value->kind == HirValueKind::PackElement &&
                 value->packElement == index,
             "Base input order is the exact selected source pack projection");
    }
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto changed = result.hir;
      auto *owner = const_cast<HirConstructorInstance *>(
          changed.findConstructorInstance(constructor.id));
      auto *lane = const_cast<HirValue *>(
          owner->body.findValue(owner->initializers.front().arguments.front()));
      expect(lane && lane->operands.size() == 1,
             "Base pack control selects a real projected input");
      if (mutation == 0)
        lane->packElement = 1;
      if (mutation == 1)
        ++lane->symbol;
      expect(!verifyHirProgramPlans(result.semantics, changed).valid(),
             "HIR rejects equal-typed wrong Base pack index or foreign grouped "
             "binding");
    }
    auto forged = result.mir;
    auto *instance = const_cast<MirConstructorInstance *>(
        forged.findConstructorInstance(constructor.id));
    bool mutated = false;
    for (auto &block : instance->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::InitializeBase &&
            instruction.operands.size() == 2) {
          std::swap(instruction.operands[0], instruction.operands[1]);
          mutated = true;
        }
    expect(mutated && !verifyMirProgram(forged).valid(),
           "source-free Base proof rejects swapping equal-typed selected input "
           "lanes");
    checked = true;
  }
  expect(checked,
         "fixed-arity Base pack fixture must instantiate its selected owner");
}
void constructionLifetimeContexts() {
  const auto result =
      Frontend({.stopAfter = FrontendPhase::Hir})
          .analyze("lifetime-hir-construction-contexts.gti", R"gti(
struct Token<T> { Token() = default; };
struct Builder<lifetime a, T> {
  Builder() { Token<T> token{}; }
  int make() { Token<T> token{}; return 1; }
};
struct Projected<T&<borrowed>> {
  Projected() = default;
  int make() { Token<T> token{}; return 1; }
};
namespace family { struct Exact<T, lifetime a> { Exact() = default; }; }
struct family::Exact<int> {
  Exact() = default;
  int make() { Token<int> token{}; return 1; }
};
int build<lifetime a, T>(T&<a> input) { Token<T> token{}; return 1; }
int first<lifetime a>(int&<a> input) {
  auto invoke = []() -> int { return 1; };
  return invoke();
}
int second<lifetime b>(int&<b> input) {
  auto invoke = []() -> int { return 1; };
  return invoke();
}
int main() {
  int value = 0;
  Builder<static, int> builder{};
  Projected<int&<static>> projected{};
  family::Exact<int, static> exact{};
  return build(value) + builder.make() + projected.make() + exact.make() + first(value) + second(value) - 6;
}
)gti");
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "mixed-kind callable and owner construction contexts "
                  "preserve compact arguments");
  }
  expect(verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "construction context source proof accepts exact canonical lifetime "
         "substitutions");
  auto malformed = result.hir;
  HirValue *firstCreator = nullptr;
  HirValue *secondCreator = nullptr;
  for (const auto &function : malformed.functionInstances()) {
    if (!function.source)
      continue;
    const auto &name = function.source->name().lexeme;
    if (name != "first" && name != "second")
      continue;
    for (const auto &value : function.body.values)
      if (value.kind == HirValueKind::Lambda) {
        auto *creator = const_cast<HirValue *>(&value);
        (name == "first" ? firstCreator : secondCreator) = creator;
      }
  }
  expect(firstCreator && secondCreator,
         "context mutation fixture has two exact lexical creators");
  // Make the same checked lambda body claim two parent scopes. Their compact
  // type/value arguments agree, but their declaration lifetime identities do
  // not.
  secondCreator->lambdaTarget = firstCreator->lambdaTarget;
  secondCreator->source = firstCreator->source;
  secondCreator->info = firstCreator->info;
  const auto staleFormation =
      verifyHirProgramPlans(result.semantics, malformed);
  expect(std::find(staleFormation.errors.begin(), staleFormation.errors.end(),
                   "construction context has no exact lambda creator") !=
             staleFormation.errors.end(),
         "retargeting a closure cannot retain another creator's "
         "fixed-environment application");
  // Keep the forged creator internally coherent to reach the separate
  // unequal-parent-context gate rather than failing formation first.
  secondCreator->lifetimeApplication = firstCreator->lifetimeApplication;
  const auto rejected = verifyHirProgramPlans(result.semantics, malformed);
  expect(std::find(rejected.errors.begin(), rejected.errors.end(),
                   "construction body has ambiguous instance arguments") !=
             rejected.errors.end(),
         "a lambda construction body cannot inherit unequal lifetime contexts");
}
void scopedClassAliases() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir})
                           .analyze("scoped-class-aliases.gti", R"gti(
class Mutable<R(Args...) mut> {
public:
  using Signature = R(Args...) mut;
  Mutable() = default;
  R invoke(Args... args) mut;
};
class Consuming<R&<result>(Args...) &&> {
public:
  using Signature = R&<result>(Args...) &&;
  Consuming() = default;
  R&<result> invoke(Args... args) &&;
};
using MutableSelection = for<lifetime a, lifetime b> int&<a>(int&<a>, int&<b>) mut;
using ConsumingSelection = for<lifetime a, lifetime b> int&<a>(int&<a>, int&<b>) &&;
Mutable<MutableSelection> mutableCallback();
Consuming<ConsumingSelection> consumingCallback();
int main() { return 0; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(
      checked.canGenerateCode(),
      "qualified callable aliases reconstruct the exact closed owner recipe");
  std::size_t examined = 0;
  for (const auto &owner : checked.hir.classInstances())
    for (std::size_t index = 0; index < owner.typeAliases.size(); ++index) {
      const auto &alias = owner.typeAliases[index];
      if (alias.type.kind != SemanticType::FunctionSignature ||
          alias.type.signatureReceiver == ReceiverMutability::ReadOnly)
        continue;
      ++examined;
      expect(alias.type.quantifiedLifetimes.size() == 2,
             "alias reconstruction preserves every invocation binder");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto forged = checked.hir;
        auto &type = const_cast<HirClassInstance &>(
                         forged.classInstances()[owner.id - 1])
                         .typeAliases[index]
                         .type;
        if (mutation == 0)
          type.signatureReceiver = ReceiverMutability::ReadOnly;
        if (mutation == 1)
          type.quantifiedLifetimes.clear();
        if (mutation == 2)
          type.arguments.front().referenceLifetime = Lifetime::staticLifetime();
        if (mutation == 3)
          type.arguments.back().referenceLifetime =
              type.arguments[1].referenceLifetime;
        expect(!verifyHirProgramPlans(checked.semantics, forged).valid(),
               "alias verification rejects changed receiver kind, binder "
               "scope, fixed region and correlation");
      }
    }
  expect(examined == 2, "alias fixture verifies both qualified owner recipes");
}
void projectedOnlyCallableOwners() {
  const auto result =
      Frontend({.stopAfter = FrontendPhase::Mir})
          .analyze("projected-only-callable-owner-scopes.gti", R"gti(
struct Fixed<int&<source>> {
  int&<source> value;
  Fixed(int&<source> input) : value(input) {}
  int&<source> get() { return value; }
  ~Fixed() { int observed = value; }
};
int read<lifetime a>(int&<a> input) {
  Fixed<int&<a>> holder{input};
  return holder.get();
}
int main() { int value = 7; return read(value) - 7; }
)gti");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), "projected-only owners preserve distinct "
                                   "layout and callable lifetime scopes");
  bool implicitReceiver = false;
  for (const auto &function : result.hir.functionInstances())
    if (function.owner)
      for (const auto &value : function.body.values)
        if (value.kind == HirValueKind::This && !value.source) {
          implicitReceiver = true;
          expect(
              value.info.type == function.ownerType,
              "implicit method receivers use their canonical callable owner");
        }
  for (const auto &destructor : result.hir.destructorInstances())
    for (const auto &value : destructor.body.values)
      if (value.kind == HirValueKind::This && !value.source) {
        implicitReceiver = true;
        expect(
            value.info.type == destructor.ownerType,
            "implicit destructor receivers use their canonical callable owner");
      }
  expect(implicitReceiver,
         "projected-only fixture exercises implicit receiver lowering");
  const auto checkReceiverPlaces = [&](MirBodyAddress address,
                                       const SemanticType &ownerType,
                                       const MirClassInstance &layout) {
    const auto *body = findMirBody(result.mir, address);
    bool distinctReceiver = false;
    for (const auto &place : body->places) {
      if (place.root != MirPlaceRootKind::This || !place.projections.empty())
        continue;
      expect(place.type == ownerType,
             "abstract entry and evaluated receivers use the same exact "
             "callable owner universe");
      if (place.type == layout.type)
        continue;
      auto invalid = result.mir;
      auto *changed = findMirBody(invalid, address);
      changed->places[place.id - 1].type = layout.type;
      expect(!verifyMirProgram(invalid).valid(),
             "a receiver entry cannot import the class layout's independent "
             "lifetime binders");
      distinctReceiver = true;
    }
    return distinctReceiver;
  };
  bool methodReceiver = false, destructorReceiver = false;
  for (const auto &function : result.mir.functionInstances())
    if (function.owner)
      if (const auto *layout = result.mir.findClassInstance(*function.owner))
        methodReceiver |= checkReceiverPlaces(
            {MirBodyKind::Function, function.id}, function.ownerType, *layout);
  for (const auto &destructor : result.mir.destructorInstances())
    if (const auto *layout = result.mir.findClassInstance(destructor.owner))
      destructorReceiver |=
          checkReceiverPlaces({MirBodyKind::Destructor, destructor.id},
                              destructor.ownerType, *layout);
  expect(methodReceiver && destructorReceiver,
         "method and destructor fixtures each distinguish callable and layout "
         "receiver scopes");
  bool fieldMutation = false;
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto *owner = result.mir.findClassInstance(constructor.owner);
    if (!owner)
      continue;
    for (std::size_t index = 0; index < constructor.initializers.size();
         ++index) {
      const auto &initializer = constructor.initializers[index];
      const auto field =
          std::find_if(owner->declaredFields.begin(),
                       owner->declaredFields.end(), [&](const auto &candidate) {
                         return candidate.symbol == initializer.field;
                       });
      if (field == owner->declaredFields.end() ||
          field->type == initializer.targetType)
        continue;
      auto invalid = result.mir;
      const_cast<MirConstructorInstance *>(
          invalid.findConstructorInstance(constructor.id))
          ->initializers[index]
          .targetType = field->type;
      expect(!verifyMirProgram(invalid).valid(),
             "constructor field metadata cannot substitute layout regions for "
             "its callable owner");
      fieldMutation = true;
      break;
    }
  }
  expect(fieldMutation,
         "projected-only constructor has an independently scoped field type");
}

void nativeInvocationContexts() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto path =
      std::filesystem::weakly_canonical(library / "lifetime-native-hir.gti");
  const std::string source = R"gti(
using Count = (std::string_view) -> int32_t;
int32_t count<lifetime a>(std::string_view<a> value) { return 7; }
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = count; return invoke(callback, "text") - 7; }
)gti";
  const auto result =
      Frontend({.stopAfter = FrontendPhase::Hir})
          .analyze(path, source, {library / "prelude.gti", path},
                   {{path.string(), source}}, {library});
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "native HIR keeps the carrier universal and instantiates its "
                  "invocation");
  }
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values) {
      if (!value.nativeCalleeType)
        continue;
      found = true;
      expect(value.callPlan && value.lifetimeApplication.callableSignature &&
                 !value.lifetimeApplication.bindings.empty() &&
                 value.parameterTypes.size() == 1 &&
                 value.parameterTypes.front() !=
                     value.nativeCalleeType->nativeFunctionParameterTypes()
                         .front(),
             "native invocation fixture distinguishes carrier binders from "
             "invocation arguments");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = result.hir;
        auto *body = const_cast<HirBody *>(
            &malformed.findFunctionInstance(function.id)->body);
        auto *call = const_cast<HirValue *>(body->findValue(value.id));
        if (mutation == 0)
          call->lifetimeApplication.bindings.clear();
        else if (mutation == 1)
          call->parameterTypes.front() =
              call->nativeCalleeType->nativeFunctionParameterTypes().front();
        else if (mutation == 2)
          call->callPlan->arguments.front().parameterType =
              call->nativeCalleeType->nativeFunctionParameterTypes().front();
        else
          call->lifetimeApplication.callableSignature->signature
              .quantifiedLifetimes.clear();
        expect(!verifyHirProgramPlans(result.semantics, malformed).valid(),
               "native HIR cannot replace applied types or lose its exact "
               "carrier scope/application");
      }
    }
  expect(found, "native HIR fixture retains an indirect invocation");
}
void structuralConstructorPolicies() {
  auto result = Frontend({.stopAfter = FrontendPhase::Hir})
                    .analyze("structural-constructor-policy-hir.gti", R"gti(
struct Cell<lifetime a> {
  int&<a> value;
  Cell(int&<a> input) : value(input) {}
  Cell(Cell<a>& source) = default;
  Cell(Cell<a>&& source) = default;
};
struct Plain {
  int value = 7;
  Plain() = default;
  Plain(Plain& source) = default;
  Plain(Plain&& source) = default;
};
int main() {
  int input = 7;
  Cell cell{input};
  Plain first{};
  Plain second{first};
  return second.value + cell.value - 14;
}
)gti");
  if (!result.semanticValid || !result.hir.valid())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.semanticValid && result.hir.valid(),
         "defaulted structural policies retain checked HIR declarations and "
         "targetless copies");
  expect(verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "independent HIR policy replay accepts exact owner and shell scopes");
  bool foundPolicy = false, foundCopy = false;
  for (const auto &instance : result.hir.classInstances()) {
    if (!instance.source || instance.source->name().lexeme != "Cell")
      continue;
    auto *lifecycle = const_cast<ClassLifecycleInfo *>(
        result.semantics.findClassLifecycle(*instance.source));
    expect(lifecycle && lifecycle->declaredCopyConstructor,
           "the HIR fixture retains its declared borrowed-owner policy");
    foundPolicy = true;
    const auto original = *lifecycle->declaredCopyConstructor;
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto &policy = *lifecycle->declaredCopyConstructor;
      policy = original;
      if (mutation == 0)
        policy.parameterTypes.front().referenceLifetime =
            Lifetime::staticLifetime();
      if (mutation == 1) {
        policy.lifetimeContract.typeBounds.clear();
        policy.lifetimeContract.requirements = {};
      }
      if (mutation == 2)
        policy.parameterTypes.front()
            .arguments.front()
            .lifetimeArguments.front() = Lifetime::staticLifetime();
      if (mutation == 3)
        policy.parameterTypes.front().referenceAccess = AccessMode::Mutable;
      if (mutation == 4)
        policy.requirements.push_back(
            {.lifetimeBound = LifetimeBound{{}, Lifetime::staticLifetime()},
             .outlivesType = policy.parameterTypes.front().arguments.front()});
      const auto proof = verifyHirProgramPlans(result.semantics, result.hir);
      expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error ==
                                  "HIR defaulted constructor policy changes "
                                  "its structural lifetime contract";
                         }),
             "HIR policies reject static/tied owner, missing requirements and "
             "changed access");
    }
    lifecycle->declaredCopyConstructor = original;
    auto forged = result.hir;
    auto &changed = const_cast<HirClassInstance &>(
        forged.classInstances()[instance.id - 1]);
    changed.copyConstructor = SpecialMemberStatus::Deleted;
    const auto proof = verifyHirProgramPlans(result.semantics, forged);
    expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                       [](const auto &error) {
                         return error == "HIR class changes its selected "
                                         "structural constructor policy";
                       }),
           "HIR cannot change a declared structural operation's availability");
  }
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values) {
      if (value.constructorKind != ConstructorKind::Copy || !value.source)
        continue;
      foundCopy = true;
      expect(
          !value.constructorTarget &&
              value.lifetimeApplication == HirLifetimeApplication{} &&
              value.lifetimeArguments.empty(),
          "generated copies have no discarded policy invocation application");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = result.hir;
        auto &body = const_cast<HirBody &>(
            forged.findFunctionInstance(function.id)->body);
        auto *changed = const_cast<HirValue *>(body.findValue(value.id));
        if (mutation == 0)
          changed->lifetimeApplication.complete = true;
        if (mutation == 1)
          changed->lifetimeArguments.push_back(Lifetime::staticLifetime());
        if (mutation == 2)
          changed->constructorKind = ConstructorKind::Move;
        const auto proof = verifyHirProgramPlans(result.semantics, forged);
        expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                           [](const auto &error) {
                             return error ==
                                    "HIR structural transfer retains an "
                                    "arbitrary constructor invocation";
                           }),
               "HIR generated transfer rejects arbitrary policy applications, "
               "arguments and modes");
      }
    }
  expect(
      foundPolicy && foundCopy,
      "the HIR policy fixture exercises both declaration and transfer proof");
}

} // namespace
int main() {
  structuralConstructorPolicies();
  unprovedReferenceContents();
  preparedReceiverRegion();
  contextualOperatorApplication();
  syntheticAssignmentReceiverOrigins();
  defaultFieldCallableUniverse();
  projectedOnlyCallableOwners();
  scopedClassAliases();
  nativeInvocationContexts();
  constructionLifetimeContexts();
  selectedBaseInputs();
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
                          .analyze("lifetime-hir-signatures.gti", R"gti(
T&<a> identity<lifetime a, T>(T&<a> value) { return value; }
int&<a_first> first<lifetime a_first>(int&<a_first> value) {
  return identity<a_first, int>(value);
}
int&<a_second> second<lifetime a_second>(int&<a_second> value) {
  return identity<a_second, int>(value);
}
struct Box<lifetime owner, T> { T value; Box(T initial) : value(initial) {} };
int main() {
  Box<static, int> box{0};
  int value = 17;
  return first(value) + second(value) - 34;
}
)gti");
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "universal-signature fixture must lower through HIR");
  }
  const FunctionInfo *declaration = nullptr;
  for (const auto &statement : result.program.declarations()) {
    const auto *function = dynamic_cast<const FunctionDecl *>(statement.get());
    if (function && function->name().lexeme == "identity")
      declaration = result.semantics.findFunction(*function);
  }
  expect(declaration && declaration->lifetimeContract.valid(),
         "generic identity must have a complete declaration lifetime contract");
  const HirFunctionInstance *instance = nullptr;
  std::size_t count = 0;
  for (const auto &candidate : result.hir.functionInstances())
    if (candidate.declaration == declaration->id) {
      instance = &candidate;
      ++count;
    }
  expect(count == 1 && instance, "different caller regions must share one "
                                 "executable declaration instance");
  expect(instance->lifetimeContract.parameters ==
             declaration->lifetimeContract.parameters,
         "executable contract must retain declaration universal identities");
  expect(instance->returnType.referenceLifetime ==
                 declaration->returnType.referenceLifetime &&
             instance->parameterTypes.size() == 1 &&
             instance->parameterTypes[0].referenceLifetime ==
                 declaration->parameterTypes[0].referenceLifetime,
         "first caller regions must not replace universal return and input "
         "regions");
  expect(instance->returnType.arguments ==
                 std::vector<SemanticType>{SemanticType::Int32} &&
             instance->typeArguments ==
                 std::vector<SemanticType>{SemanticType::Int32},
         "lifetime binders must not consume compact executable type arguments");
  expect(
      !instance->lifetimeContract.typeBounds.empty(),
      "the exact implied referent-outlives obligation must remain available");
  for (const auto &bound : instance->lifetimeContract.typeBounds)
    expect(bound.type == SemanticType::Int32,
           "concrete HIR contracts must substitute exact type-outlives bound "
           "types");
  bool foundBox = false;
  for (const auto &candidate : result.hir.classInstances())
    if (candidate.source && candidate.source->name().lexeme == "Box") {
      foundBox = true;
      expect(candidate.typeArguments ==
                     std::vector<SemanticType>{SemanticType::Int32} &&
                 candidate.fields.size() == 1 &&
                 candidate.fields[0].info.type == SemanticType::Int32,
             "class lifetime binders must not shift concrete field type "
             "substitutions");
    }
  expect(foundBox, "the mixed-kind user-defined class must be instantiated");
  std::cout << "universal HIR lifetime signatures passed\n";
}
