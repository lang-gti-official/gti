#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

int main() {
  const auto frontend =
      analyzeWithStandardLibrary("expected-constructor-fields.gti", R"gti(
mut int observed = 0;
class Payload {
public:
  int id;
  Payload(int value) : id(value) { int checked = 10 / value; }
  ~Payload() { observed += this.id; }
};
Payload create(int id) { return Payload(id); }
struct Success {
  expected<Payload, int> first = create(1);
  Payload later = create(2);
};
struct Error {
  expected<int, Payload> first = unexpected(create(1));
  Payload later = create(2);
};
struct Passive {
  expected<int, Payload> first = 1;
  Payload later = create(2);
};
int main() {
  Success success{};
  Error error{};
  Passive passive{};
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "typed success, error and passive alternatives initialize fields");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "field publication and rollback have independently valid MIR");
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  std::size_t checked = 0, mutations = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const lang::MirInstruction *publication = nullptr;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? constructor.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            destination && destination->root == lang::MirPlaceRootKind::This &&
            destination->type.kind == lang::SemanticType::Expected &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().type != destination->type)
          publication = &instruction;
      }
    if (!publication)
      continue;
    ++checked;
    expect(
        emitter.supportsFailureBodyText(
            {.kind = lang::MirBodyKind::Constructor, .owner = constructor.id}),
        "all three field forms use the exact transformed constructor");
    const auto reject = [&](auto mutate, std::string_view reason) {
      auto broken = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          broken.findConstructorInstance(constructor.id));
      lang::MirInstruction *initialize = nullptr;
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == publication->id)
            initialize = &instruction;
      expect(initialize != nullptr, "mutation retains exact publication ID");
      if (!initialize)
        return;
      mutate(*changed, *initialize);
      (void)lang::rebuildMirValueUses(changed->body);
      expect(
          !lang::CppMirBodyEmitter(broken, rows)
               .supportsFailureBodyText({.kind = lang::MirBodyKind::Constructor,
                                         .owner = constructor.id}),
          reason);
      ++mutations;
    };
    reject(
        [](auto &ctor, auto &initialize) {
          ctor.initializers[initialize.constructorInitializer - 1].field =
              ctor.initializers.back().field;
        },
        "field publication rejects a different selected declaration");
    reject(
        [](auto &, auto &initialize) { ++initialize.constructorInitializer; },
        "field publication rejects a forged initializer index");
    reject([](auto &,
              auto &initialize) { initialize.lifecycle.back().target = 0; },
           "field publication requires exact rollback activation");
    reject(
        [](auto &ctor, auto &initialize) {
          auto *drop = const_cast<lang::MirDropObligation *>(
              ctor.body.findDropObligation(initialize.lifecycle.back().target));
          drop->kind = lang::MirDropObligationKind::Binding;
        },
        "field publication cannot invent a local-binding lifetime");
    reject(
        [](auto &ctor, auto &initialize) {
          auto *field = const_cast<lang::MirPlace *>(
              ctor.body.findPlace(*initialize.destination));
          field->initiallyAvailable = true;
        },
        "field publication cannot replace an already live field");
    reject(
        [](auto &ctor, auto &initialize) {
          for (auto &expression : ctor.body.fullExpressions)
            if (expression.constructorInitializer ==
                initialize.constructorInitializer)
              expression.roots.clear();
        },
        "field publication retains its exact source expression boundary");
    reject(
        [](auto &ctor, auto &initialize) {
          for (auto &block : ctor.body.blocks) {
            const auto at = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.id == initialize.id;
                });
            if (at == block.instructions.end())
              continue;
            // Even a metadata boundary cannot be moved across this exact
            // publication. The writer must not delay ownership transfer.
            lang::MirInstruction barrier;
            barrier.id = 100000;
            barrier.kind = lang::MirInstructionKind::Lifecycle;
            block.instructions.insert(at, barrier);
            return;
          }
        },
        "field publication cannot bypass an intervening staging boundary");
    if (publication->lifecycle.size() == 2)
      reject([](auto &,
                auto &initialize) { initialize.lifecycle.front().source = 0; },
             "owning publication cannot lose its source transfer");
  }
  expect(checked == 3, "all three unrelated field alternatives were checked");
  expect(mutations == 23, "every bounded field shape has negative proofs");
  const bool ready = emitter.analyzeProgram().ready();
  expect(ready, "the entire source program has complete backend coverage");
  if (ready) {
    const auto optimized = lang::OptimizationPipeline().run(
        frontend.hir, lang::OptimizationLevel::O0);
    const auto artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimized);
    std::size_t count = 0, offset = 0;
    const std::string marker = "// GTI MIR Expected field publication";
    while ((offset = artifact.contents.find(marker, offset)) !=
           std::string::npos) {
      ++count;
      offset += marker.size();
    }
    expect(count == 3,
           "native field publication never uses a fictitious local slot");
  }
  std::size_t plainProducerControls = 0, plainProducerMutations = 0;
  for (const std::string_view payload :
       {"class Payload<T> { public: int marker = 1; ~Payload() {} };",
        "class Payload<T> { public: int marker; "
        "Payload() : marker(7) {} ~Payload() {} };"}) {
    const auto control = analyzeWithStandardLibrary(
        "generic-expected-fields.gti", std::string(payload) + R"gti(
class Wrapped<T> {
  expected<Payload<T>, int> success = Payload<T>();
  expected<int, Payload<T>> error = unexpected(Payload<T>());
  expected<int, Payload<T>> passive = 1;
};
int main() { Wrapped<int> value{}; return 0; }
)gti");
    expect(control.canGenerateCode(),
           "generic generated and explicit payload field controls analyze");
    if (!control.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap controlRows(
        buildRows(control, control.mir));
    expect(lang::CppMirBodyEmitter(control.mir, controlRows)
               .analyzeProgram()
               .ready(),
           "generated and explicit zero-argument payloads retain execution");
    for (const auto &ctor : control.mir.constructorInstances()) {
      for (const auto &block : ctor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Construct ||
              !instruction.definedFailure.empty() ||
              instruction.lifecycle.size() != 1 || !instruction.result)
            continue;
          const auto uses = ctor.body.usesOf(*instruction.result);
          const bool fieldPayload =
              std::any_of(uses.begin(), uses.end(), [&](const auto &use) {
                if (use.kind != lang::MirValueUseKind::InstructionOperand)
                  return false;
                for (const auto &consumerBlock : ctor.body.blocks)
                  for (const auto &consumer : consumerBlock.instructions)
                    if (consumer.id == use.instruction &&
                        consumer.kind == lang::MirInstructionKind::Initialize &&
                        consumer.info.type.kind == lang::SemanticType::Expected)
                      return true;
                return false;
              });
          if (!fieldPayload)
            continue;
          ++plainProducerControls;
          for (bool wrongTarget : {false, true}) {
            auto broken = control.mir;
            auto *changed = const_cast<lang::MirConstructorInstance *>(
                broken.findConstructorInstance(ctor.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &producer : changedBlock.instructions)
                if (producer.id == instruction.id) {
                  if (wrongTarget)
                    producer.constructorTarget = ctor.id;
                  else
                    producer.lifecycle.clear();
                }
            expect(!lang::CppMirBodyEmitter(broken, controlRows)
                        .supportsFailureBodyText(
                            {.kind = lang::MirBodyKind::Constructor,
                             .owner = ctor.id}),
                   wrongTarget
                       ? "plain payload retains its selected constructor"
                       : "plain payload retains its local activation");
            ++plainProducerMutations;
          }
        }
      }
    }
  }
  expect(plainProducerControls == 1 && plainProducerMutations == 2,
         "the explicit nonraising success payload exercises both mutations");
  return failures ? 1 : 0;
}
