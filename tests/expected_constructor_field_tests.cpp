#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

namespace {

// The shipped backend accepts the verified MIR program.
void expectBackendAccepts(
    const lang::FrontendResult &frontend, const std::string &message,
    const lang::TargetInfo &target = lang::TargetInfo::host()) {
  std::string error;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         message + " (MIR verifies)");
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0, target,
                             &error),
         message + (error.empty() ? "" : ": " + error));
}

} // namespace

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
  expectBackendAccepts(frontend, "typed success, error and passive field "
                                 "alternatives compile");
  std::size_t checked = 0;
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
    // The publication is the field's exact selected initializer, writes a
    // field that is not yet live inside its own full-expression boundary
    // and activates the field's rollback obligation, not a local binding.
    const auto *field = constructor.body.findPlace(*publication->destination);
    const lang::MirDropObligation *rollback =
        publication->lifecycle.empty()
            ? nullptr
            : constructor.body.findDropObligation(
                  publication->lifecycle.back().target);
    expect(publication->constructorInitializer != 0 &&
               publication->constructorInitializer <=
                   constructor.initializers.size(),
           "field publication names its selected initializer");
    expect(field != nullptr && !field->initiallyAvailable,
           "field publication cannot replace an already live field");
    expect(rollback != nullptr &&
               rollback->kind != lang::MirDropObligationKind::Binding,
           "field publication activates exact field rollback, not a "
           "local-binding lifetime");
    expect(std::any_of(constructor.body.fullExpressions.begin(),
                       constructor.body.fullExpressions.end(),
                       [&](const auto &expression) {
                         return expression.constructorInitializer ==
                                    publication->constructorInitializer &&
                                !expression.roots.empty();
                       }),
           "field publication retains its exact source expression boundary");
  }
  expect(checked == 3, "all three unrelated field alternatives were checked");
  std::size_t plainProducerControls = 0, plainProducerMutations = 0;
  for (const std::string_view payload :
       {"class Payload<T> { public: int marker = 1; ~Payload() {}  public: "
        "Payload() = default; };",
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
    expectBackendAccepts(
        control,
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
            // implicit expected constructor; MIR verification checks its
            // exact constructor owner and local activation.
            expect(!lang::verifyMirProgram(broken).valid(),
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
