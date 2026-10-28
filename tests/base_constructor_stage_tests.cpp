#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

void checkSelectedGeneratedBaseSchedule() {
  const auto frontend =
      analyzeWithStandardLibrary("selected-generated-base.gti", R"(
class Base {
public:
  int state = 7;
public:
  Base() = default;
};
class ExplicitDerived : public Base {
public:
  ExplicitDerived() : Base() {}
};
class GeneratedDerived : public Base { int tag = 9;  public: GeneratedDerived() = default; };
int main() {
  ExplicitDerived explicit_value{};
  GeneratedDerived generated_value{};
  return explicit_value.state == 7 && generated_value.state == 7 ? 0 : 1;
}
)");
  expect(
      frontend.canGenerateCode(),
      "source and generated derived constructors select a generated base body");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "selected generated base schedules have complete backend authority");
  std::size_t selectedBases = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (std::size_t index = 0; index < constructor.initializers.size();
         ++index) {
      const auto &initializer = constructor.initializers[index];
      if (initializer.kind != lang::ConstructorInitializerTargetKind::Base ||
          !initializer.generatedDefault || !initializer.constructorTarget)
        continue;
      const auto *base = frontend.mir.findClassInstance(*initializer.base);
      const auto *target =
          frontend.mir.findConstructorInstance(*initializer.constructorTarget);
      expect(base != nullptr && base->generatedDefaultRequiresSchedule &&
                 target != nullptr && target->generatedDefault &&
                 target->owner == base->id && target->parameterTypes.empty(),
             "the selected base target preserves its exact generated schedule");
      const auto bindings = lang::cppMirBaseParameterInitializerBindings(
          frontend.mir, constructor);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().initializer == index &&
                 bindings->front().constructor == initializer.constructorTarget,
             "generated base targets use the verified selected-stage bindings");
      ++selectedBases;

      auto wrongKind = frontend.mir;
      auto *wrongConstructor = const_cast<lang::MirConstructorInstance *>(
          wrongKind.findConstructorInstance(constructor.id));
      wrongConstructor->initializers[index].generatedDefault = false;
      expect(!lang::verifyMirProgram(wrongKind).valid(),
             "a selected generated base must retain its generated-default "
             "initializer identity");

      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      changed->initializers[index].constructorTarget.reset();
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::InitializeBase &&
              instruction.constructorInitializer == index + 1)
            instruction.constructorTarget.reset();
      expect(!lang::verifyMirProgram(malformed).valid(),
             "erasing a selected generated base target cannot enable a "
             "targetless fallback");
      expect(!lang::cppMirBaseParameterInitializerBindings(malformed, *changed),
             "base bindings independently reject a missing required generated "
             "target");
      expect(!lang::CppMirBodyEmitter(malformed, map).analyzeProgram().ready(),
             "backend rejects erased generated-base authority");
    }
  }
  expect(selectedBases == 2, "both explicit and generated derived constructors "
                             "exercise selected base schedules");
}

void checkFieldArgumentOrder() {
  for (const std::string_view expression :
       {"observe()", "observe() == 7 ? 1 : 2"}) {
    const auto frontend = analyzeWithStandardLibrary(
        "base-before-field-argument.gti", std::string(R"gti(
mut int observed = 0;
int observe() { observed = 7; return observed; }
class Base { public: Base() { observed = 3; } };
class Derived : public Base {
  int field;
public:
  Derived() : Base(), field()gti") + std::string(expression) +
                                              R"gti() {}
};
int main() { Derived value{}; return 0; }
)gti");
    expect(frontend.canGenerateCode(),
           "ordinary and conditional field arguments follow base completion");
    if (!frontend.canGenerateCode())
      continue;
    bool exercised = false;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      for (const auto &block : constructor.body.blocks) {
        const auto base =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [](const auto &instruction) {
                           return instruction.kind ==
                                  lang::MirInstructionKind::InitializeBase;
                         });
        if (base == block.instructions.end())
          continue;
        const lang::MirBlock *callBlock = nullptr;
        const lang::MirInstruction *call = nullptr;
        for (const auto &candidateBlock : constructor.body.blocks)
          for (const auto &instruction : candidateBlock.instructions)
            if (instruction.kind == lang::MirInstructionKind::Call &&
                instruction.functionTarget &&
                instruction.definedFailure.empty()) {
              expect(call == nullptr,
                     "the fixture has exactly one field argument call");
              callBlock = &candidateBlock;
              call = &instruction;
            }
        if (call == nullptr || callBlock == nullptr)
          continue;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        // Preserve SSA identity, operands, final field publication and every
        // cleanup token; only move the observable argument evaluation. A
        // fallible base places the original call in its success block.
        auto &source = changed->body.blocks[callBlock->id - 1].instructions;
        const auto original = std::find_if(
            source.begin(), source.end(),
            [&](const auto &candidate) { return candidate.id == call->id; });
        auto lifted = *original;
        source.erase(original);
        auto &destination = changed->body.blocks[block.id - 1].instructions;
        const auto beforeBase = std::find_if(
            destination.begin(), destination.end(),
            [&](const auto &candidate) { return candidate.id == base->id; });
        destination.insert(beforeBase, std::move(lifted));
        expect(call->result.has_value(),
               "the lifted call produces the field argument");
        if (call->result)
          changed->body.values[*call->result - 1].definitionBlock = block.id;
        expect(lang::rebuildMirValueUses(changed->body),
               "the lifted call retains coherent independent SSA use metadata");
        const auto verified = lang::verifyMirProgram(malformed);
        expect(!verified.valid() &&
                   std::any_of(verified.errors.begin(), verified.errors.end(),
                               [&](const auto &error) {
                                 return error.bodyKind ==
                                            lang::MirBodyKind::Constructor &&
                                        error.owner == constructor.id &&
                                        error.message.find(
                                            "constructor initializer stages") !=
                                            std::string::npos;
                               }),
               "effectful field values and scalar-join conditions cannot "
               "precede the base");
        exercised = true;
      }
    }
    expect(exercised,
           "the ordering mutation moved an exact field argument call");
  }
  for (const std::string_view expression : {"7", "seed"}) {
    const auto frontend = analyzeWithStandardLibrary(
        "base-pure-field-preparation.gti", std::string(R"gti(
class Base { public: Base() {} };
class Derived : public Base {
  int field;
public:
  Derived(int seed) : Base(), field()gti") + std::string(expression) +
                                               R"gti() {}
};
int main() { Derived value{3}; return 0; }
)gti");
    expect(frontend.canGenerateCode(),
           "pure field preparation control is valid");
    if (!frontend.canGenerateCode())
      continue;
    bool exercised = false;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      for (const auto &block : constructor.body.blocks) {
        const auto base =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [](const auto &instruction) {
                           return instruction.kind ==
                                  lang::MirInstructionKind::InitializeBase;
                         });
        if (base == block.instructions.end())
          continue;
        const auto preparation = std::find_if(
            base + 1, block.instructions.end(), [](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::Load ||
                     (instruction.kind == lang::MirInstructionKind::Compute &&
                      instruction.operation == lang::MirOperation::Literal);
            });
        if (preparation == block.instructions.end())
          continue;
        auto reordered = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            reordered.findConstructorInstance(constructor.id));
        auto &instructions = changed->body.blocks[block.id - 1].instructions;
        const auto baseIndex = base - block.instructions.begin();
        const auto preparationIndex = preparation - block.instructions.begin();
        std::rotate(instructions.begin() + baseIndex,
                    instructions.begin() + preparationIndex,
                    instructions.begin() + preparationIndex + 1);
        expect(
            lang::verifyMirProgram(reordered).valid(),
            "constants and exact scalar parameter loads may precede the base");
        exercised = true;
      }
    }
    expect(exercised, "the pure preparation control moved an exact definition");
  }
  const auto baseArgument =
      analyzeWithStandardLibrary("effectful-base-argument.gti", R"(
mut int observed = 0;
int observe() { observed = 7; return observed; }
class Base { public: Base(int value) {}  public: Base() = default; };
class Derived : public Base {
  int field;
public:
  Derived() : Base(observe()), field(1) {}
};
int main() { Derived value{}; return 0; }
)");
  expect(
      baseArgument.canGenerateCode(),
      "the base's own argument still executes before its selected constructor");
}

void checkOwningEnumBaseArgumentAuthority() {
  const auto frontend =
      analyzeWithStandardLibrary("owning-enum-base-traits.gti", R"(
class Resource { public: Resource() {} ~Resource() {} };
enum class Parcel { empty, item(Resource value) };
class Base { public: Base(int value) {}  public: Base() = default; };
class Derived : public Base { public: Derived() : Base(7) {} };
int main() { Derived value{}; Parcel parcel = Parcel::empty; return 0; }
)");
  expect(frontend.canGenerateCode(),
         "owning enum trait mutation control is valid");
  if (!frontend.canGenerateCode())
    return;
  const auto enumeration = std::find_if(
      frontend.mir.payloadEnums().begin(), frontend.mir.payloadEnums().end(),
      [](const auto &candidate) {
        return candidate.traits.drop == lang::DropKind::Lexical;
      });
  expect(enumeration != frontend.mir.payloadEnums().end(),
         "the enum authority records its cleanup-owning payload");
  if (enumeration == frontend.mir.payloadEnums().end())
    return;
  bool exercised = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::InitializeBase ||
            !instruction.constructorTarget ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value)
          continue;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        auto *target = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(*instruction.constructorTarget));
        target->parameterTypes.front() = enumeration->type;
        auto &stage = *std::find_if(
            changed->body.blocks[block.id - 1].instructions.begin(),
            changed->body.blocks[block.id - 1].instructions.end(),
            [&](const auto &candidate) {
              return candidate.id == instruction.id;
            });
        stage.parameterTypes.front() = enumeration->type;
        stage.operands.front().type = enumeration->type;
        auto &value = changed->body.values[stage.operands.front().value - 1];
        value.info.type = enumeration->type;
        // Deliberately retain the forged scalar/trivial value traits. The
        // selected base signature must consult the independent enum catalog.
        for (auto &definitionBlock : changed->body.blocks)
          for (auto &definition : definitionBlock.instructions)
            if (definition.result == value.id)
              definition.info.type = enumeration->type;
        const auto verified = lang::verifyMirProgram(malformed);
        expect(
            !verified.valid() &&
                std::any_of(verified.errors.begin(), verified.errors.end(),
                            [&](const auto &error) {
                              return error.bodyKind ==
                                         lang::MirBodyKind::Constructor &&
                                     error.owner == constructor.id &&
                                     error.message.find(
                                         "constructor initializer stages") !=
                                         std::string::npos;
                            }),
            "owning payload enums cannot masquerade as trivial base arguments");
        exercised = true;
      }
    }
  }
  expect(exercised,
         "owning enum mutation exercised an exact selected base argument");
}

// A base argument computed through temporaries keeps each owned temporary in
// a preparation carrier slot. Its slot, borrow stage, drop obligation and the
// edges around the preparation are exact authority, never inferred.
void checkBaseInitializerTemporaries(const std::filesystem::path &fixtures) {
  const auto path = fixtures / "fixtures/base_initializer_temporaries.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "base initializer temporaries analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "base initializer temporaries have complete backend authority");
  bool exercised = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto preparation =
        lang::cppMirBasePreparation(frontend.mir, constructor);
    if (!preparation || preparation->slots.empty())
      continue;
    exercised = true;
    // The two probes own slots; the cleanup-free Plain values are read
    // through their fields inside the preparation and need none.
    expect(preparation->slots.size() == 2,
           "each owned base-initializer temporary has one carrier slot");
    const auto plan =
        emitter.buildPlan({lang::MirBodyKind::Constructor, constructor.id});
    expect(plan.supportsBasePreparationText() && plan.supportsFailureText(),
           "a carrier with temporary slots is admitted in the body plan");
    const auto &body = constructor.body;
    const auto *slot = body.findPlace(preparation->slots.front());
    const auto obligation = std::find_if(
        body.dropObligations.begin(), body.dropObligations.end(),
        [&](const auto &candidate) { return candidate.place == slot->id; });
    expect(obligation != body.dropObligations.end(),
           "a carrier slot keeps its temporary's value obligation");
    if (obligation == body.dropObligations.end())
      continue;
    const auto stageBlock = preparation->stageBlock;
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      bool applied = false;
      if (mutation == 0) {
        changed->body.dropObligations[obligation->id - 1].initiallyActive =
            true;
        applied = true;
      }
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          const bool inPreparation =
              std::find(preparation->instructions.begin(),
                        preparation->instructions.end(),
                        instruction.id) != preparation->instructions.end();
          if (mutation == 1 && !applied && !inPreparation &&
              instruction.kind == lang::MirInstructionKind::Drop &&
              instruction.destination == slot->id) {
            // After the base, a slot is ended only by its own obligation.
            instruction.lifecycle.front().source = obligation->id == 1 ? 2 : 1;
            applied = true;
          }
          if (mutation == 2 && !applied && inPreparation &&
              instruction.kind == lang::MirInstructionKind::CallInput &&
              (instruction.callInputKind ==
                   lang::HirCallInputKind::ReadTemporaryBorrow ||
               instruction.callInputKind ==
                   lang::HirCallInputKind::MutableTemporaryBorrow)) {
            instruction.callSite += 1000;
            applied = true;
          }
        }
      }
      if (mutation == 3) {
        // Re-entering the preparation after the base would skip its drops.
        changed->body.blocks[stageBlock - 1].terminator.target = body.entry;
        applied = true;
      }
      expect(applied, "base temporary mutation found its exact target");
      expect(!lang::cppMirBasePreparation(malformed, *changed),
             "preparation rejects forged temporary obligation, drop, borrow "
             "and re-entry authority");
      expect(!lang::CppMirBodyEmitter(malformed, map).analyzeProgram().ready(),
             "a forged base temporary never reaches native emission");
    }
  }
  expect(exercised, "the base initializer fixture prepared owned temporaries");
}

// Each concrete instance of a generic derived class prepares its generic
// base's arguments. The carrier and the base gate are members of the two
// classes' own concrete C++ definitions; a class without one, such as a
// retained native class template, is refused rather than mis-spelled.
void checkGenericBaseArguments(const std::filesystem::path &fixtures) {
  const auto path = fixtures / "fixtures/generic_base_arguments.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "generic base arguments analyze");
  if (!frontend.canGenerateCode())
    return;
  const auto rows = buildRows(frontend, frontend.mir);
  expect(
      lang::CppMirBodyEmitter(frontend.mir, lang::CppMirBodyEmissionMap(rows))
          .analyzeProgram()
          .ready(),
      "generic base arguments have complete backend authority");
  std::size_t prepared = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (!lang::cppMirBasePreparation(frontend.mir, constructor))
      continue;
    ++prepared;
    const lang::MirConstructorInstance *target = nullptr;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::InitializeBase &&
            instruction.constructorTarget)
          target = frontend.mir.findConstructorInstance(
              *instruction.constructorTarget);
    expect(target != nullptr, "a prepared generic base keeps its target");
    if (target == nullptr)
      continue;
    for (const auto owner : {constructor.owner, target->owner}) {
      const auto *instance = frontend.mir.findClassInstance(owner);
      auto forged = rows;
      bool marked = false;
      for (auto &row : forged.types)
        if (row.type == instance->type) {
          marked = row.concreteGenericClass;
          row.concreteGenericClass = false;
        }
      expect(marked, "a generic derived or base instance is a concrete class");
      expect(!lang::CppMirBodyEmitter(frontend.mir,
                                      lang::CppMirBodyEmissionMap(forged))
                  .analyzeProgram()
                  .ready(),
             "a generic class without its own C++ definition never receives "
             "a preparation carrier or base gate");
    }
  }
  expect(prepared == 2,
         "each concrete generic derived instance prepares its base");
}

// A class instantiated with a lambda type is a retained native class template.
// Its constructors emit under the closure overlay, including one that copies
// the closure into a field beside a literal base argument. A computed base
// argument would need a per-instance preparation carrier, which only a class
// with its own concrete C++ definition declares, so it stays refused.
void checkClosureGenericBaseArguments() {
  const auto frontend =
      analyzeWithStandardLibrary("closure-generic-base.gti", R"(
class Base {
public:
  int amount;
  Base(int amount) : amount(amount) {}
};
int third_of(int value) { return value / 3; }
class Fixed<F> : public Base {
public:
  F callback;
  Fixed(F f) : Base(20), callback(f) {}
};
class Computed<F> : public Base {
public:
  int extra;
  Computed(int extra) : Base(third_of(30)), extra(extra) {}
};
int fixed<F>(F f) {
  Fixed<F> value = Fixed<F>(f);
  return value.amount;
}
int computed<F>(F f) {
  Computed<F> value = Computed<F>(5);
  return value.amount + value.extra;
}
int main() {
  return fixed([]() -> int { return 1; }) +
         computed([]() -> int { return 2; }) - 35;
}
)");
  expect(frontend.canGenerateCode(), "closure-typed derived classes analyze");
  if (!frontend.canGenerateCode())
    return;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  std::size_t admitted = 0;
  std::size_t refused = 0;
  for (const auto &constructor : lowered.mir().constructorInstances()) {
    const auto *owner = lowered.mir().findClassInstance(constructor.owner);
    if (owner == nullptr || owner->type.arguments.size() != 1 ||
        owner->type.arguments.front().kind != lang::SemanticType::Lambda)
      continue;
    const auto *source = lowered.findConstructorInstance(constructor.id);
    const auto *declaration =
        source == nullptr
            ? nullptr
            : lowered.findConstructorDeclaration(source->declaration);
    auto rows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    const auto overlays =
        declaration == nullptr
            ? std::nullopt
            : lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                  rows, lowered, *declaration, constructor);
    expect(overlays && *overlays != 0,
           "a closure-typed owner's constructor spells F through its overlay");
    const auto row = std::find_if(
        rows.types.begin(), rows.types.end(),
        [&](const auto &candidate) { return candidate.type == owner->type; });
    expect(row != rows.types.end() && !row->concreteGenericClass,
           "a closure-typed generic class has no concrete C++ definition");
    const bool prepared =
        lang::cppMirBasePreparation(lowered.mir(), constructor).has_value();
    const lang::CppMirBodyEmissionMap map(std::move(rows));
    const lang::CppMirBodyEmitter emitter(lowered.mir(), map);
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    const bool text = emitter.supportsBodyText(address) ||
                      emitter.supportsFailureBodyText(address);
    expect(text != prepared,
           "a closure-typed class copies its closure beside a literal base "
           "but never receives a base preparation carrier");
    ++(text ? admitted : refused);
  }
  expect(admitted == 1 && refused == 1,
         "one literal and one computed closure-typed base were checked");
}

// An owned base argument, moved in or created in place, moves into one
// prepared parameter that the InitializeBase alone hands to the selected base
// constructor. The C++ backend does not represent this staging; it stays an
// LLVM-only shape.
void checkOwnedBaseArguments() {
  const auto frontend =
      analyzeWithStandardLibrary("owned-base-arguments.gti", R"(
class Resource {
  int id;
public:
  Resource(int id) : id(id) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() {}
  int value() { return this.id; }
};
enum class Parcel { empty, item(Resource value) };
Resource make(int id) { return Resource(id); }
class Base {
public:
  Resource first;
  Parcel parcel;
  int count;
  Base(Resource first, Parcel parcel, int count)
      : first(std::move(first)), parcel(std::move(parcel)), count(count) {}
};
class Moved : public Base {
public:
  Moved(Resource first, Parcel parcel)
      : Base(std::move(first), std::move(parcel), 1) {}
};
class Fresh : public Base {
public:
  Fresh(int id) : Base(make(id), Parcel::item(Resource(id + 1)), 2) {}
};
int main() {
  Moved moved{Resource(1), Parcel::empty};
  Fresh fresh{3};
  return moved.count + fresh.count - 3;
}
)");
  expect(frontend.canGenerateCode(), "owned base arguments verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(!lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the C++ backend does not represent staged owned base arguments");
  const auto definitionOf =
      [](const lang::MirBody &body,
         const lang::MirOperand &operand) -> const lang::MirInstruction * {
    const auto *value = operand.kind == lang::MirOperandKind::Value
                            ? body.findValue(operand.value)
                            : nullptr;
    if (value == nullptr)
      return nullptr;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == value->definition)
          return &instruction;
    return nullptr;
  };
  std::size_t stages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::InitializeBase ||
            instruction.operands.size() != 3)
          continue;
        ++stages;
        expect(lang::mirBodyRoutesFailureEdges(constructor.body),
               "a staged owned base argument is not an unarmed transfer");
        for (std::size_t index = 0; index < 2; ++index) {
          const auto *input =
              definitionOf(constructor.body, instruction.operands[index]);
          const auto *prepared = input && input->preparedParameterDrop
                                     ? constructor.body.findDropObligation(
                                           *input->preparedParameterDrop)
                                     : nullptr;
          expect(input != nullptr &&
                     input->kind == lang::MirInstructionKind::CallInput &&
                     input->callSite == 0 &&
                     input->constructorInitializer ==
                         instruction.constructorInitializer &&
                     input->callInputIndex == index &&
                     input->callInputKind == lang::HirCallInputKind::MoveValue,
                 "each owned base argument is an exact prepared move input");
          expect(
              prepared != nullptr &&
                  prepared->kind ==
                      lang::MirDropObligationKind::PreparedParameter &&
                  std::count_if(
                      instruction.lifecycle.begin(),
                      instruction.lifecycle.end(),
                      [&](const lang::MirLifecycleEvent &event) {
                        return event.kind ==
                                   lang::MirLifecycleEventKind::TransferOut &&
                               event.source == prepared->id;
                      }) == 1,
              "the base stage hands each prepared parameter over once");
        }
        expect(
            definitionOf(constructor.body, instruction.operands[2]) ==
                    nullptr ||
                definitionOf(constructor.body, instruction.operands[2])->kind !=
                    lang::MirInstructionKind::CallInput,
            "the scalar base argument is passed directly");
        for (int mutation = 0; mutation < 7; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto &instructions = changed->body.blocks[block.id - 1].instructions;
          auto &stage = *std::find_if(instructions.begin(), instructions.end(),
                                      [&](const auto &candidate) {
                                        return candidate.id == instruction.id;
                                      });
          const auto *original =
              definitionOf(constructor.body, instruction.operands[0]);
          lang::MirInstruction *input = nullptr;
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (original && candidate.id == original->id)
                input = &candidate;
          if (input == nullptr) {
            expect(false, "the owned base input is addressable");
            break;
          }
          if (mutation == 0) {
            // The callee never receives the prepared parameter.
            std::erase_if(stage.lifecycle, [](const auto &event) {
              return event.kind == lang::MirLifecycleEventKind::TransferOut;
            });
          } else if (mutation == 1) {
            // The same prepared parameter is handed over twice.
            stage.lifecycle.insert(stage.lifecycle.begin(),
                                   stage.lifecycle.front());
          } else if (mutation == 2) {
            input->callInputKind = lang::HirCallInputKind::Value;
          } else if (mutation == 3) {
            // Bypass the prepared input with the moved source value.
            stage.operands.front() = input->operands.front();
          } else if (mutation == 4) {
            input->constructorInitializer = 0;
          } else if (mutation == 5) {
            changed->body.dropObligations[*input->preparedParameterDrop - 1]
                .kind = lang::MirDropObligationKind::Value;
          } else {
            // A forged trivial value cannot replace the owning formal's
            // prepared parameter: the catalog decides that the formal owns.
            changed->body.values[stage.operands.front().value - 1]
                .info.traits.drop = lang::DropKind::Trivial;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "owned base argument staging is sealed (mutation " +
                     std::to_string(mutation) + ")");
        }
      }
    }
  }
  expect(stages == 2, "both derived constructors stage their owned base "
                      "arguments");
}

// A base reference formal may name a class or enum object that holds no
// borrowed state of its own. The derived constructor forwards its own
// reference formal: the stage's input borrows the exact incoming referent and
// the base cannot retain it. The C++ backend does not represent this family.
void checkClassReferenceBaseArguments() {
  const auto frontend =
      analyzeWithStandardLibrary("class-reference-base-arguments.gti", R"(
class Point {
public:
  int x;
  int y;
  Point(int x, int y) : x(x), y(y) {}
};
class Counter {
public:
  mut int count;
  Counter() : count(0) {}
};
class Base {
public:
  int sum;
  Base(Point& point, mut Counter& counter) : sum(point.x + point.y) {
    counter.count += 1;
  }
};
class Derived : public Base {
public:
  Derived(Point& point, mut Counter& counter) : Base(point, counter) {}
};
int main() {
  Point point{3, 4};
  mut Counter counter{};
  Derived value{point, counter};
  return value.sum + counter.count - 8;
}
)");
  expect(frontend.canGenerateCode(), "class reference base arguments verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(!lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the C++ backend does not represent class reference base arguments");
  const auto definitionOf =
      [](const lang::MirBody &body,
         const lang::MirOperand &operand) -> const lang::MirInstruction * {
    const auto *value = operand.kind == lang::MirOperandKind::Value
                            ? body.findValue(operand.value)
                            : nullptr;
    if (value == nullptr)
      return nullptr;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == value->definition)
          return &instruction;
    return nullptr;
  };
  std::size_t stages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::InitializeBase ||
            instruction.operands.size() != 2 || !instruction.constructorTarget)
          continue;
        ++stages;
        const auto expectedKinds = {lang::HirCallInputKind::ReadBorrow,
                                    lang::HirCallInputKind::MutableBorrow};
        std::size_t index = 0;
        for (const auto kind : expectedKinds) {
          const auto *input =
              definitionOf(constructor.body, instruction.operands[index]);
          expect(input != nullptr &&
                     input->kind == lang::MirInstructionKind::CallInput &&
                     input->callSite == 0 &&
                     input->constructorInitializer ==
                         instruction.constructorInitializer &&
                     input->callInputIndex == index &&
                     input->callInputKind == kind &&
                     !input->preparedParameterDrop,
                 "each class reference is an exact forwarded borrow input");
          ++index;
        }
        const auto *target = frontend.mir.findConstructorInstance(
            *instruction.constructorTarget);
        if (target == nullptr || target->parameterTypes.size() != 2) {
          expect(false, "the staged base keeps its selected constructor");
          continue;
        }
        const auto referent = target->parameterTypes.front().arguments.front();
        for (int mutation = 0; mutation < 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto &instructions = changed->body.blocks[block.id - 1].instructions;
          auto &stage = *std::find_if(instructions.begin(), instructions.end(),
                                      [&](const auto &candidate) {
                                        return candidate.id == instruction.id;
                                      });
          if (mutation == 0) {
            // The catalog records the referent as holding borrowed state, so
            // the forwarded formal no longer qualifies for the family.
            for (const auto &instance : malformed.classInstances())
              if (instance.type == referent)
                const_cast<lang::MirClassInstance &>(instance)
                    .traits.containsBorrowedState = true;
          } else if (mutation == 1) {
            // The selected formal's referent is not the forwarded place's.
            auto *selected = const_cast<lang::MirConstructorInstance *>(
                malformed.findConstructorInstance(
                    *instruction.constructorTarget));
            selected->parameterTypes.front().arguments.front() =
                lang::SemanticType::Int32;
            stage.parameterTypes.front().arguments.front() =
                lang::SemanticType::Int32;
            stage.operands.front().type.arguments.front() =
                lang::SemanticType::Int32;
          } else if (mutation == 2) {
            // A forwarded borrow is not an owning or copied value.
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.kind == lang::MirInstructionKind::CallInput &&
                    candidate.constructorInitializer ==
                        instruction.constructorInitializer &&
                    candidate.callInputIndex == 0)
                  candidate.callInputKind = lang::HirCallInputKind::Value;
          } else {
            // The two forwarded borrows trade formals.
            std::swap(stage.operands[0], stage.operands[1]);
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "class reference base forwarding is sealed (mutation " +
                     std::to_string(mutation) + ")");
        }
      }
    }
  }
  expect(stages == 1, "the derived constructor stages its class references");
}

int main() {
  checkSelectedGeneratedBaseSchedule();
  checkFieldArgumentOrder();
  checkOwningEnumBaseArgumentAuthority();
  const auto fixtures = std::filesystem::path(__FILE__).parent_path();
  checkBaseInitializerTemporaries(fixtures);
  checkGenericBaseArguments(fixtures);
  checkClosureGenericBaseArguments();
  checkOwnedBaseArguments();
  checkClassReferenceBaseArguments();
  for (const auto name :
       {"coverage/reproducers/a45-base-state-rollback-unsupported.gti",
        "coverage/reproducers/a45-base-plus-string-field-unsupported.gti",
        "coverage/reproducers/a45-literal-base-argument.gti",
        "coverage/reproducers/a45-derived-destructor-explicit-base.gti",
        "coverage/reproducers/a45-derived-destructor-generated-ctor.gti",
        "fixtures/base_constructor_stages.gti",
        "fixtures/checked_base_arguments.gti",
        "fixtures/effectful_base_arguments.gti",
        "fixtures/base_initializer_temporaries.gti",
        "fixtures/generic_base_arguments.gti"}) {
    const auto path = fixtures / name;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), name);
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(), name);
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      bool base = false;
      for (const auto &block : constructor.body.blocks)
        for (const auto &instruction : block.instructions)
          base |= instruction.kind == lang::MirInstructionKind::InitializeBase;
      if (!base)
        continue;
      if (std::string_view(name) == "fixtures/checked_base_arguments.gti" ||
          std::string_view(name) == "fixtures/effectful_base_arguments.gti" ||
          std::string_view(name) ==
              "fixtures/base_initializer_temporaries.gti" ||
          std::string_view(name) == "fixtures/generic_base_arguments.gti") {
        const auto preparation =
            lang::cppMirBasePreparation(frontend.mir, constructor);
        expect(
            preparation.has_value(),
            "computed base arguments retain an exact executable MIR partition");
        const auto plan =
            emitter.buildPlan({lang::MirBodyKind::Constructor, constructor.id});
        expect(plan.supportsBasePreparationText(),
               "preparation text is admitted in the immutable body plan");
        if (preparation) {
          for (int mutation = 0; mutation < 5; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirConstructorInstance *>(
                malformed.findConstructorInstance(constructor.id));
            if (mutation == 0)
              changed->parameterTypes.front() = lang::SemanticType::StringView;
            else if (mutation == 1)
              changed->body.places[preparation->places.front() - 1].root =
                  lang::MirPlaceRootKind::This;
            else if (mutation == 2)
              changed->body.values[preparation->values.front() - 1]
                  .info.traits.containsBorrowedState = true;
            else if (mutation == 3)
              changed->body.blocks[changed->body.entry - 1].terminator.target =
                  999999;
            else
              changed->initializers.front().base.reset();
            expect(!lang::cppMirBasePreparation(malformed, *changed),
                   "preparation rejects forged parameter, receiver, borrow and "
                   "CFG authority");
            expect(!lang::CppMirBodyEmitter(malformed, map)
                        .analyzeProgram()
                        .ready(),
                   "malformed preparation never reaches native emission");
          }
        }
      }
      for (const auto &block : constructor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::InitializeBase)
            continue;
          for (int mutation = 0; mutation < 6; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirConstructorInstance *>(
                malformed.findConstructorInstance(constructor.id));
            auto &instructions =
                changed->body.blocks[block.id - 1].instructions;
            auto &stage =
                *std::find_if(instructions.begin(), instructions.end(),
                              [&](const auto &candidate) {
                                return candidate.id == instruction.id;
                              });
            if (mutation == 0)
              stage.successResultDrop.reset();
            else if (mutation == 1)
              changed->body.dropObligations[*instruction.successResultDrop - 1]
                  .initiallyActive = true;
            else if (mutation == 2)
              changed->body.places[*instruction.destination - 1]
                  .projections.front()
                  .selection = constructor.owner;
            else if (mutation == 3)
              stage.constructorInitializer = 999999;
            else if (mutation == 5) {
              if (stage.operands.empty())
                stage.parameterTypes.push_back(lang::SemanticType::Int32);
              else {
                stage.operands.front().type = lang::SemanticType::Bool;
                stage.parameterTypes.front() = lang::SemanticType::Bool;
              }
            } else if (instruction.definedFailure.empty())
              stage.lifecycle.clear();
            else
              changed->body.blocks[block.id - 1]
                  .terminator.successLifecycle.clear();
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "base stage identity, activation and source selection are "
                   "sealed");
            expect(
                !lang::CppMirBodyEmitter(malformed, map)
                     .analyzeProgram()
                     .ready(),
                "native base emission rejects forged construction authority");
          }
        }
      }
      const bool supported = emitter.supportsFailureBodyText(
          {lang::MirBodyKind::Constructor, constructor.id});
      expect(supported, "base constructor must emit verified failure text");
      if (!supported) {
        std::cerr << "constructor " << constructor.id << " kind "
                  << int(constructor.definitionKind) << " failure "
                  << constructor.mayRaiseDefinedFailure << " base bindings "
                  << bool(lang::cppMirBaseParameterInitializerBindings(
                         frontend.mir, constructor))
                  << '\n';
      }
    }
  }
  const auto prematureField =
      analyzeWithStandardLibrary("premature-derived-field.gti", R"(
class Base { public: Base(int value) {}  public: Base() = default; };
class Derived : public Base {
  int field;
public:
  Derived() : Base(this.field), field(1) {}
};
int main() { Derived value = Derived(); return 0; }
)");
  expect(!prematureField.canGenerateCode(),
         "a base initializer cannot read an uninitialized derived field");
  return failures == 0 ? 0 : 1;
}
