#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

int main() {
  const auto fixtures = std::filesystem::path(__FILE__).parent_path();
  for (const auto name :
       {"coverage/reproducers/a45-base-state-rollback-unsupported.gti",
        "coverage/reproducers/a45-base-plus-string-field-unsupported.gti",
        "coverage/reproducers/a45-literal-base-argument.gti",
        "coverage/reproducers/a45-derived-destructor-explicit-base.gti",
        "coverage/reproducers/a45-derived-destructor-generated-ctor.gti",
        "fixtures/base_constructor_stages.gti"}) {
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
