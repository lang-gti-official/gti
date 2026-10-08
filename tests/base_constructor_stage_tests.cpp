#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

void checkSelectedGeneratedBaseSchedule() {
  const auto frontend =
      analyzeWithStandardLibrary("selected-generated-base.gti", R"(
class Base {
public:
  int state = 7;
};
class ExplicitDerived : public Base {
public:
  ExplicitDerived() : Base() {}
};
class GeneratedDerived : public Base { int tag = 9; };
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
class Base { public: Base(int value) {} };
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
class Base { public: Base(int value) {} };
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

int main() {
  checkSelectedGeneratedBaseSchedule();
  checkFieldArgumentOrder();
  checkOwningEnumBaseArgumentAuthority();
  const auto fixtures = std::filesystem::path(__FILE__).parent_path();
  for (const auto name :
       {"coverage/reproducers/a45-base-state-rollback-unsupported.gti",
        "coverage/reproducers/a45-base-plus-string-field-unsupported.gti",
        "coverage/reproducers/a45-literal-base-argument.gti",
        "coverage/reproducers/a45-derived-destructor-explicit-base.gti",
        "coverage/reproducers/a45-derived-destructor-generated-ctor.gti",
        "fixtures/base_constructor_stages.gti",
        "fixtures/checked_base_arguments.gti",
        "fixtures/effectful_base_arguments.gti"}) {
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
          std::string_view(name) == "fixtures/effectful_base_arguments.gti") {
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
class Base { public: Base(int value) {} };
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
