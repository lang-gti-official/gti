#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

// expected<T, E> is the prelude's ordinary source class.
bool isExpected(const lang::SemanticModel &semantics,
                const lang::SemanticType &type) {
  const auto *info = type.kind == lang::SemanticType::Class
                         ? semantics.findClassType(type.classId)
                         : nullptr;
  return info != nullptr && info->declaration != nullptr &&
         info->declaration->name().lexeme == "expected";
}

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
public:
  Success() = default;
};
struct Error {
  expected<int, Payload> first = unexpected(create(1));
  Payload later = create(2);
public:
  Error() = default;
};
struct Passive {
  expected<int, Payload> first = 1;
  Payload later = create(2);
public:
  Passive() = default;
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
        // The field's implicit expected constructor converts the payload;
        // its owned class result is then published into the field.
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            destination && destination->root == lang::MirPlaceRootKind::This &&
            isExpected(frontend.semantics, destination->type) &&
            instruction.operands.size() == 1)
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
    {
      // The field is constructed in place only when its Initialize is the
      // first step on the constructor's success edge. A valid intervening
      // staging step cannot be bypassed: publication then moves the sealed
      // result slot after that step instead of writing the field early.
      auto delayed = frontend.mir;
      auto &body = const_cast<lang::MirConstructorInstance *>(
                       delayed.findConstructorInstance(constructor.id))
                       ->body;
      lang::MirInstructionId maximum = 0;
      const lang::MirInstruction *literal = nullptr;
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) {
          maximum = std::max(maximum, instruction.id);
          if (literal == nullptr &&
              instruction.kind == lang::MirInstructionKind::Compute &&
              instruction.operation == lang::MirOperation::Literal &&
              instruction.result)
            literal = &instruction;
        }
      bool inserted = false;
      for (auto &block : body.blocks) {
        const auto at =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const auto &instruction) {
                           return instruction.id == publication->id;
                         });
        if (literal == nullptr || at == block.instructions.end())
          continue;
        lang::MirInstruction step = *literal;
        lang::MirValue value = *body.findValue(*literal->result);
        step.id = maximum + 1;
        value.id = body.values.size() + 1;
        value.definitionBlock = block.id;
        value.definition = step.id;
        step.result = value.id;
        block.instructions.insert(at, step);
        body.values.push_back(value);
        inserted = true;
        break;
      }
      (void)lang::rebuildMirValueUses(body);
      const std::string field = std::to_string(*publication->destination);
      const std::string text =
          lang::CppMirBodyEmitter(delayed, rows)
              .emitFailureBodyText({.kind = lang::MirBodyKind::Constructor,
                                    .owner = constructor.id},
                                   "delayed-field", 1)
              .text;
      expect(inserted && lang::verifyMirProgram(delayed).valid() &&
                 text.find("in-place constructor field reparent into p" +
                           field + "\n") == std::string::npos &&
                 text.find("__gti_mir_p_" + field + " = std::move(") !=
                     std::string::npos,
             "field publication cannot bypass an intervening staging "
             "boundary");
      ++mutations;
    }
    if (publication->lifecycle.front().source != 0)
      reject([](auto &,
                auto &initialize) { initialize.lifecycle.front().source = 0; },
             "owning publication cannot lose its source transfer");
  }
  expect(checked == 3, "all three unrelated field alternatives were checked");
  // Every alternative, passive included, now publishes an owned class value.
  expect(mutations == 24, "every bounded field shape has negative proofs");
  const bool ready = emitter.analyzeProgram().ready();
  expect(ready, "the entire source program has complete backend coverage");
  if (ready) {
    const auto optimized = lang::OptimizationPipeline().run(
        frontend.hir, lang::OptimizationLevel::O0);
    const auto artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimized);
    std::size_t count = 0, offset = 0;
    const std::string marker =
        "// GTI MIR in-place constructor field reparent into p";
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
       {"class Payload<T> { public: int marker = 1; ~Payload() {}  public: Payload() = default; };",
        "class Payload<T> { public: int marker; "
        "Payload() : marker(7) {} ~Payload() {} };"}) {
    const auto control = analyzeWithStandardLibrary(
        "generic-expected-fields.gti", std::string(payload) + R"gti(
class Wrapped<T> {
  expected<Payload<T>, int> success = Payload<T>();
  expected<int, Payload<T>> error = unexpected(Payload<T>());
  expected<int, Payload<T>> passive = 1;
public:
  Wrapped() = default;
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
          // The payload is staged as the argument of the field's implicit
          // expected constructor.
          const auto consumers = [&](lang::MirValueId value) {
            std::vector<const lang::MirInstruction *> result;
            for (const auto &use : ctor.body.usesOf(value)) {
              if (use.kind != lang::MirValueUseKind::InstructionOperand)
                continue;
              for (const auto &consumerBlock : ctor.body.blocks)
                for (const auto &consumer : consumerBlock.instructions)
                  if (consumer.id == use.instruction)
                    result.push_back(&consumer);
            }
            return result;
          };
          bool fieldPayload = false;
          for (const auto *input : consumers(*instruction.result))
            if (input->kind == lang::MirInstructionKind::CallInput &&
                input->result)
              for (const auto *construct : consumers(*input->result))
                fieldPayload =
                    fieldPayload ||
                    (construct->kind == lang::MirInstructionKind::Construct &&
                     isExpected(control.semantics, construct->info.type));
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
            // The payload reaches the field as the prepared argument of the
            // implicit expected constructor; MIR verification inside the
            // emitter's analysis checks its exact constructor owner.
            const lang::CppMirBodyEmitter brokenEmitter(broken, controlRows);
            const lang::MirBodyAddress address{
                .kind = lang::MirBodyKind::Constructor, .owner = ctor.id};
            expect(!brokenEmitter.supportsFailureBodyText(address) ||
                       !brokenEmitter.analyze(address).ready(),
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
