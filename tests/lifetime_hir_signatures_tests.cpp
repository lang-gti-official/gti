#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/mir_lowering.h"

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
void preparedReceiverRegion() {
  const auto standardLibrary = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "prepared-receiver-region.gti", R"gti(
struct Ticket {
  int value;
  Ticket(int initial) : value(initial) {}
  int take() && { return this.value; }
};
int consume(Ticket ticket) { return std::move(ticket).take(); }
int main() { return consume(Ticket(7)) - 7; }
)gti", {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "a moved receiver borrows its exact prepared destination until invocation");
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
        const auto origin = std::find_if(function.body.lifetimeBody.origins.begin(),
            function.body.lifetimeBody.origins.end(), [&](const auto &candidate) {
              return candidate.place == *stage.destination && candidate.source == stage.hirValue;
            });
        if (origin == function.body.lifetimeBody.origins.end()) continue;
        checked = true;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = result.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *changed = &owner->body.blocks[block.id - 1].instructions[
              &stage - block.instructions.data()];
          if (mutation == 0) changed->hirValue = 0;
          if (mutation == 1) changed->preparedParameterDrop = 0;
          if (mutation == 2) changed->destination = 0;
          if (mutation == 3) {
            const auto remove = [&](const LifetimeBound &bound) { return bound.longer == origin->region; };
            std::erase_if(owner->body.lifetimeBody.obligations, remove);
            for (auto &candidateBlock : owner->body.blocks)
              for (auto &instruction : candidateBlock.instructions)
                std::erase_if(instruction.lifetimeApplication.obligations, remove);
          }
          expect(!verifyMirProgram(forged).valid(),
                 "prepared receiver proof requires exact source, drop, destination and storage-region edge");
        }
      }
  expect(checked, "the consuming receiver fixture must materialize a checked local storage witness");
}

void contextualOperatorApplication() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "contextual-operator-lifetime-application.gti", R"gti(
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
    expect(false, "contextual operator calls preserve their checked receiver lifetime application");
  }
  bool checked = false;
  bool temporaryOrigin = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values) {
      if (value.kind != HirValueKind::Call || !value.source ||
          !result.semantics.findContextualConversion(*value.source))
        continue;
      const auto *target = value.functionTarget
          ? result.hir.findFunctionInstance(*value.functionTarget) : nullptr;
      expect(target && target->source && target->source->operatorName() &&
                 target->source->operatorName()->kind == OverloadedOperator::ContextualBool,
             "operator bool retains its distinct checked conversion identity");
      expect(value.lifetimeApplication.complete &&
                 !value.lifetimeApplication.bindings.empty(),
             "HIR contextual calls retain the independently checked receiver substitution");
      for (const auto &origin : value.lifetimeApplication.origins) {
        const auto *source = function.body.findValue(origin.source);
        if (origin.region.kind != Lifetime::Kind::Local || !source ||
            source->source != value.source || !value.receiver ||
            source->id != *value.receiver || source->id == value.id)
          continue;
        temporaryOrigin = true;
        const auto bodyOrigin = std::find(function.body.lifetimeBody.origins.begin(),
                                          function.body.lifetimeBody.origins.end(), origin);
        expect(bodyOrigin != function.body.lifetimeBody.origins.end(),
               "body facts retain the exact temporary receiver rather than its contextual result");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto stale = result.hir;
          auto *body = const_cast<HirBody *>(&stale.findFunctionInstance(function.id)->body);
          auto found = std::find(body->lifetimeBody.origins.begin(), body->lifetimeBody.origins.end(), origin);
          expect(found != body->lifetimeBody.origins.end(), "origin mutation selects exact checked storage");
          if (mutation == 0) {
            found->source = value.id;
          } else if (mutation == 1) {
            auto conflict = *found;
            conflict.source = value.id;
            body->lifetimeBody.origins.push_back(conflict);
          } else {
            auto *call = const_cast<HirValue *>(body->findValue(value.id));
            auto applied = std::find(call->lifetimeApplication.origins.begin(),
                                     call->lifetimeApplication.origins.end(), origin);
            expect(applied != call->lifetimeApplication.origins.end(), "application origin matches exact body witness");
            applied->source = value.id;
          }
          expect(!MirLowerer{}.lower(stale, result.failureMetadata).valid(),
                 "contextual receiver origins reject forged conversion results and conflicting witnesses");
        }
      }
      auto forged = result.mir;
      auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
      if (!changed) continue;
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call && instruction.hirValue == value.id) {
            instruction.lifetimeApplication.bindings.clear();
            checked = true;
          }
      if (checked)
        expect(!verifyMirProgram(forged).valid(),
               "a contextual call cannot lose its receiver contract application");
    }
  expect(checked, "the contextual operator fixture must exercise a concrete checked call");
  expect(temporaryOrigin, "unary contextual conversion must retain a temporary receiver origin");
}

void defaultFieldCallableUniverse() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "default-field-callable-universe.gti", R"gti(
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
    expect(false, "a selected field default has one exact stage in its constructor region universe");
  }
  bool checked = false;
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto *owner = result.mir.findClassInstance(constructor.owner);
    if (!owner || constructor.initializers.size() != 1 || owner->declaredFields.size() != 1)
      continue;
    const auto &initializer = constructor.initializers.front();
    if (initializer.targetType.kind != SemanticType::Enum ||
        initializer.targetType == owner->declaredFields.front().type)
      continue;
    checked = true;
    auto forged = result.mir;
    auto *changed = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
    changed->initializers.front().targetType = owner->declaredFields.front().type;
    expect(!verifyMirProgram(forged).valid(),
           "a layout-region field type cannot replace the selected constructor field type");
    forged = result.mir;
    changed = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
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
  expect(checked, "the default field fixture must distinguish layout and callable region identities");
}

void selectedBaseInputs() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "selected-base-input-lanes.gti", R"gti(
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
  if (!result.semanticValid || !result.mir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(result.mir).errors)
      std::cerr << error.message << '\n';
    expect(false, "selected Base constructors retain exact pack lanes, lifetime binders and defaults through MIR");
  }
  bool checked = false;
  for (const auto &constructor : result.hir.constructorInstances()) {
    if (!constructor.source || constructor.source->name().lexeme != "SumDerived")
      continue;
    expect(constructor.initializers.size() == 1 && constructor.initializers.front().arguments.size() == 2 &&
               constructor.initializers.front().explicitArgumentCount == 2,
           "a written pack expansion maps to two exact explicit Base inputs");
    for (std::size_t index = 0; index < 2; ++index) {
      const auto *value = constructor.body.findValue(constructor.initializers.front().arguments[index]);
      expect(value && value->kind == HirValueKind::PackElement && value->packElement == index,
             "Base input order is the exact selected source pack projection");
    }
    auto forged = result.mir;
    auto *instance = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
    bool mutated = false;
    for (auto &block : instance->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::InitializeBase && instruction.operands.size() == 2) {
          std::swap(instruction.operands[0], instruction.operands[1]);
          mutated = true;
        }
    expect(mutated && !verifyMirProgram(forged).valid(),
           "source-free Base proof rejects swapping equal-typed selected input lanes");
    checked = true;
  }
  expect(checked, "fixed-arity Base pack fixture must instantiate its selected owner");
}
void constructionLifetimeContexts() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
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
    expect(false, "mixed-kind callable and owner construction contexts preserve compact arguments");
  }
  expect(verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "construction context source proof accepts exact canonical lifetime substitutions");
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
  expect(firstCreator && secondCreator, "context mutation fixture has two exact lexical creators");
  // Make the same checked lambda body claim two parent scopes. Their compact
  // type/value arguments agree, but their declaration lifetime identities do not.
  secondCreator->lambdaTarget = firstCreator->lambdaTarget;
  secondCreator->source = firstCreator->source;
  secondCreator->info = firstCreator->info;
  const auto rejected = verifyHirProgramPlans(result.semantics, malformed);
  expect(std::find(rejected.errors.begin(), rejected.errors.end(),
                   "construction body has ambiguous instance arguments") != rejected.errors.end(),
         "a lambda construction body cannot inherit unequal lifetime contexts");
}
void projectedOnlyCallableOwners() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Mir})
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
  expect(result.canGenerateCode(),
         "projected-only owners preserve distinct layout and callable lifetime scopes");
  bool implicitReceiver = false;
  for (const auto &function : result.hir.functionInstances())
    if (function.owner)
      for (const auto &value : function.body.values)
        if (value.kind == HirValueKind::This && !value.source) {
          implicitReceiver = true;
          expect(value.info.type == function.ownerType,
                 "implicit method receivers use their canonical callable owner");
        }
  for (const auto &destructor : result.hir.destructorInstances())
    for (const auto &value : destructor.body.values)
      if (value.kind == HirValueKind::This && !value.source) {
        implicitReceiver = true;
        expect(value.info.type == destructor.ownerType,
               "implicit destructor receivers use their canonical callable owner");
      }
  expect(implicitReceiver, "projected-only fixture exercises implicit receiver lowering");
  const auto checkReceiverPlaces = [&](MirBodyAddress address, const SemanticType &ownerType,
                                       const MirClassInstance &layout) {
    const auto *body = findMirBody(result.mir, address);
    bool distinctReceiver = false;
    for (const auto &place : body->places) {
      if (place.root != MirPlaceRootKind::This || !place.projections.empty())
        continue;
      expect(place.type == ownerType,
             "abstract entry and evaluated receivers use the same exact callable owner universe");
      if (place.type == layout.type)
        continue;
      auto invalid = result.mir;
      auto *changed = findMirBody(invalid, address);
      changed->places[place.id - 1].type = layout.type;
      expect(!verifyMirProgram(invalid).valid(),
             "a receiver entry cannot import the class layout's independent lifetime binders");
      distinctReceiver = true;
    }
    return distinctReceiver;
  };
  bool methodReceiver = false, destructorReceiver = false;
  for (const auto &function : result.mir.functionInstances())
    if (function.owner)
      if (const auto *layout = result.mir.findClassInstance(*function.owner))
        methodReceiver |= checkReceiverPlaces({MirBodyKind::Function, function.id},
                                              function.ownerType, *layout);
  for (const auto &destructor : result.mir.destructorInstances())
    if (const auto *layout = result.mir.findClassInstance(destructor.owner))
      destructorReceiver |= checkReceiverPlaces({MirBodyKind::Destructor, destructor.id},
                                                destructor.ownerType, *layout);
  expect(methodReceiver && destructorReceiver,
         "method and destructor fixtures each distinguish callable and layout receiver scopes");
  bool fieldMutation = false;
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto *owner = result.mir.findClassInstance(constructor.owner);
    if (!owner)
      continue;
    for (std::size_t index = 0; index < constructor.initializers.size(); ++index) {
      const auto &initializer = constructor.initializers[index];
      const auto field = std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
          [&](const auto &candidate) { return candidate.symbol == initializer.field; });
      if (field == owner->declaredFields.end() || field->type == initializer.targetType)
        continue;
      auto invalid = result.mir;
      const_cast<MirConstructorInstance *>(invalid.findConstructorInstance(constructor.id))
          ->initializers[index].targetType = field->type;
      expect(!verifyMirProgram(invalid).valid(),
             "constructor field metadata cannot substitute layout regions for its callable owner");
      fieldMutation = true;
      break;
    }
  }
  expect(fieldMutation, "projected-only constructor has an independently scoped field type");
}

void nativeInvocationContexts() {
  const auto library = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto path = std::filesystem::weakly_canonical(library / "lifetime-native-hir.gti");
  const std::string source = R"gti(
using Count = (std::string_view) -> int32_t;
int32_t count<lifetime a>(std::string_view<a> value) { return 7; }
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = count; return invoke(callback, "text") - 7; }
)gti";
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir}).analyze(
      path, source, {library / "prelude.gti", path}, {{path.string(), source}}, {library});
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "native HIR keeps the carrier universal and instantiates its invocation");
  }
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values) {
      if (!value.nativeCalleeType)
        continue;
      found = true;
      expect(value.callPlan && value.lifetimeApplication.callableSignature &&
                 !value.lifetimeApplication.bindings.empty() && value.parameterTypes.size() == 1 &&
                 value.parameterTypes.front() != value.nativeCalleeType->nativeFunctionParameterTypes().front(),
             "native invocation fixture distinguishes carrier binders from invocation arguments");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = result.hir;
        auto *body = const_cast<HirBody *>(&malformed.findFunctionInstance(function.id)->body);
        auto *call = const_cast<HirValue *>(body->findValue(value.id));
        if (mutation == 0)
          call->lifetimeApplication.bindings.clear();
        else if (mutation == 1)
          call->parameterTypes.front() = call->nativeCalleeType->nativeFunctionParameterTypes().front();
        else if (mutation == 2)
          call->callPlan->arguments.front().parameterType =
              call->nativeCalleeType->nativeFunctionParameterTypes().front();
        else
          call->lifetimeApplication.callableSignature->signature.quantifiedLifetimes.clear();
        expect(!verifyHirProgramPlans(result.semantics, malformed).valid(),
               "native HIR cannot replace applied types or lose its exact carrier scope/application");
      }
    }
  expect(found, "native HIR fixture retains an indirect invocation");
}
} // namespace
int main() {
  preparedReceiverRegion();
  contextualOperatorApplication();
  defaultFieldCallableUniverse();
  projectedOnlyCallableOwners();
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
