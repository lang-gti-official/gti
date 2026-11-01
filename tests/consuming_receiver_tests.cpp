#include "../src/compiler/cpp_mir_body_emitter.h"
#include "compiler_test_fixture.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"

#include <unordered_map>
#include <unordered_set>

void checkConsumingLambda(const std::string &name, bool pack) {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    ("fixtures/" + name + ".gti");
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend =
      analyzeTrustedPreludeFixture("consuming-lambda.gti", source);
  expect(frontend.canGenerateCode(),
         "owned lambda receivers reach verified MIR");
  if (!frontend.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.lambdaTarget ||
            !call.receiver ||
            call.receiver->kind != lang::MirOperandKind::Value)
          continue;
        const auto *value = function.body.findValue(call.receiver->value);
        if (value && value->info.traits.drop != lang::DropKind::Lexical)
          continue;
        // Fixed-arity and pack invocations both stage the moved owner.
        if (value) {
          const lang::MirInstruction *stage = nullptr;
          for (const auto &candidate : function.body.blocks)
            for (const auto &instruction : candidate.instructions)
              if (instruction.id == value->definition)
                stage = &instruction;
          expect(stage && stage->kind == lang::MirInstructionKind::CallInput &&
                     stage->callInputKind ==
                         lang::HirCallInputKind::MoveValue &&
                     !stage->preparedParameterDrop && stage->lifecycle.empty(),
                 "receiver stage retains the moved environment lifetime");
          if (!stage || stage->operands.size() != 1)
            continue;
          value = function.body.findValue(stage->operands.front().value);
        }
        const lang::MirDropObligation *lifetime = nullptr;
        for (const auto &drop : function.body.dropObligations)
          if (drop.kind == lang::MirDropObligationKind::Value && value &&
              drop.value == value->sourceValue)
            lifetime = &drop;
        expect(lifetime && lifetime->fullExpression != 0 &&
                   !lifetime->initiallyActive,
               "moved lambda has one retained full-expression obligation");
        if (!lifetime)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          if (mutation == 0) {
            for (auto &drop : body->dropObligations)
              if (drop.id == lifetime->id)
                drop.fullExpression = 0;
          } else {
            for (auto &candidate : body->blocks)
              for (auto &instruction : candidate.instructions) {
                if (mutation == 1 && instruction.id == value->definition)
                  instruction.lifecycle.clear();
                if (mutation == 2 && instruction.id == call.id)
                  instruction.lifecycle.push_back(
                      {.kind = lang::MirLifecycleEventKind::TransferOut,
                       .source = lifetime->id});
              }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "MIR rejects consumed lambda lifetime mutation " +
                     std::to_string(mutation));
        }
        checked = true;
      }
    }
  }
  expect(checked, "lambda fixture exercises a retained receiver");
  try {
    expect(!gti_test::emitCppText(frontend).empty(),
           "owned lambda receiver has a complete MIR representation");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (pack)
    return;
  auto reused = source;
  const auto insertion = reused.find("  std::println(5);");
  expect(insertion != std::string::npos, "lambda fixture has reuse insertion");
  if (insertion != std::string::npos) {
    reused.insert(insertion, "  int again = callback(1);\n");
    const auto invalid =
        analyzeTrustedPreludeFixture("consuming-lambda-reuse.gti", reused);
    expect(!invalid.canGenerateCode(),
           "explicitly consumed lambda cannot be reused");
    expect(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.message.find("moved") !=
                                std::string::npos;
                       }),
           "lambda reuse is rejected by move-state analysis");
  }
}

// A lambda Call operand prepared by an argument CallInput that borrows a
// derived class place for an exact base-reference parameter.
struct LambdaBaseReferenceArgument {
  lang::HirFunctionInstanceId function = 0;
  lang::MirInstructionId call = 0;
  lang::MirInstructionId input = 0;
  std::size_t index = 0;
  lang::MirOperand borrow;
};

std::vector<LambdaBaseReferenceArgument>
lambdaBaseReferenceArguments(const lang::MirProgram &program) {
  std::vector<LambdaBaseReferenceArgument> result;
  for (const auto &function : program.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.lambdaTarget)
          continue;
        for (std::size_t index = 0;
             index < call.operands.size() && index < call.parameterTypes.size();
             ++index) {
          const auto &operand = call.operands[index];
          const auto &parameter = call.parameterTypes[index];
          const auto *value = operand.kind == lang::MirOperandKind::Value
                                  ? body.findValue(operand.value)
                                  : nullptr;
          const lang::MirInstruction *input = nullptr;
          for (const auto &candidate : body.blocks)
            for (const auto &instruction : candidate.instructions)
              if (value && instruction.id == value->definition)
                input = &instruction;
          if (input == nullptr ||
              input->kind != lang::MirInstructionKind::CallInput ||
              input->callInputRole != lang::MirCallInputRole::Argument ||
              input->operands.size() != 1 ||
              parameter.kind != lang::SemanticType::Reference ||
              parameter.arguments.size() != 1)
            continue;
          const auto &borrow = input->operands.front();
          const auto *place = body.findPlace(borrow.place);
          if (place != nullptr && operand.type == parameter &&
              borrow.type == parameter &&
              place->type.kind == lang::SemanticType::Class &&
              place->type != parameter.arguments.front())
            result.push_back({.function = function.id,
                              .call = call.id,
                              .input = input->id,
                              .index = index,
                              .borrow = borrow});
        }
      }
    }
  }
  return result;
}

void checkLambdaBaseReferenceArguments() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/lambda_base_reference_arguments.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend =
      analyzeTrustedPreludeFixture("lambda-base-reference.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "a lambda accepts a derived class object for an exact base-reference "
         "parameter");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "HIR plans verify for derived lambda reference arguments");

  // Every fixed-arity invocation is planned; only the final pack expansion
  // keeps MIR's shared pack schedule.
  std::size_t unplanned = 0, readPlans = 0, mutablePlans = 0;
  std::size_t placeReceivers = 0, valueReceivers = 0, movedReceivers = 0;
  for (const auto &instance : frontend.hir.functionInstances()) {
    for (const auto &value : instance.body.values) {
      if (value.kind != lang::HirValueKind::Call || !value.lambdaTarget)
        continue;
      const bool pack = std::any_of(
          value.operands.begin(), value.operands.end(), [&](auto id) {
            const auto *operand = instance.body.findValue(id);
            return operand &&
                   operand->kind == lang::HirValueKind::PackExpansion;
          });
      if (pack)
        continue;
      if (!value.callPlan || !value.callPlan->receiver) {
        ++unplanned;
        continue;
      }
      switch (value.callPlan->receiver->kind) {
      case lang::HirCallInputKind::ReadBorrow:
        ++placeReceivers;
        break;
      case lang::HirCallInputKind::Value:
        ++valueReceivers;
        break;
      case lang::HirCallInputKind::MoveValue:
        ++movedReceivers;
        break;
      default:
        break;
      }
      for (const auto &argument : value.callPlan->arguments) {
        const auto *operand = instance.body.findValue(argument.value);
        if (operand == nullptr ||
            argument.parameterType.kind != lang::SemanticType::Reference ||
            argument.parameterType.arguments.size() != 1 ||
            argument.parameterType.arguments.front() == operand->info.type)
          continue;
        readPlans += argument.kind == lang::HirCallInputKind::ReadBorrow;
        mutablePlans += argument.kind == lang::HirCallInputKind::MutableBorrow;
      }
    }
  }
  expect(unplanned == 0 && readPlans == 7 && mutablePlans == 2,
         "fixed-arity lambda invocations plan each derived argument as an "
         "ordered borrow of its exact base-reference parameter");
  // Closure places are borrowed; a literal and a cleanup-free moved
  // environment keep value stages; a moved owning environment is MoveValue.
  expect(placeReceivers == 6 && valueReceivers == 2 && movedReceivers == 1,
         "lambda call plans stage each closure receiver by its ownership");

  const auto verified = lang::verifyMirProgram(frontend.mir);
  for (const auto &error : verified.errors)
    std::cerr << "MIR: " << error.message << '\n';
  expect(verified.valid(),
         "verified MIR accepts staged derived-to-base lambda arguments");
  const auto staged = lambdaBaseReferenceArguments(frontend.mir);
  const auto stagedWith = [&](lang::MirOperandKind kind) {
    return std::count_if(staged.begin(), staged.end(), [&](const auto &entry) {
      return entry.borrow.kind == kind;
    });
  };
  expect(stagedWith(lang::MirOperandKind::BorrowRead) == 8 &&
             stagedWith(lang::MirOperandKind::BorrowWrite) == 2,
         "each lambda invocation, including the pack prefix, receives a "
         "prepared exact base reference borrowed from the derived place");
  if (!staged.empty()) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(
        forged, {lang::MirBodyKind::Function, staged.front().function});
    bool changed = false;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == staged.front().call) {
          instruction.operands[staged.front().index] = staged.front().borrow;
          changed = true;
        }
    expect(changed && lang::rebuildMirValueUses(*body),
           "the unstaged lambda mutation changes its exact argument and "
           "retains a coherent use index");
    const auto direct = lang::verifyMirProgram(forged);
    expect(!direct.valid() &&
               std::any_of(
                   direct.errors.begin(), direct.errors.end(),
                   [&](const lang::MirVerificationError &error) {
                     return error.bodyKind == lang::MirBodyKind::Function &&
                            error.owner == staged.front().function &&
                            error.instruction == staged.front().input &&
                            error.message ==
                                "call input must have exactly one executable "
                                "use or be abandoned by a terminal argument";
                   }),
           "MIR rejects replacing a lambda's ordered base-reference input "
           "with a bare derived place despite a valid public-base conversion");
  }

  // A once invocation of a cleanup-free moved parameter has an ordinary value
  // receiver stage. Its proof still requires the exact ownership Move.
  bool checkedOnce = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    const auto definition = [&](lang::MirValueId id) {
      const auto *value = body.findValue(id);
      const lang::MirInstruction *found = nullptr;
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          if (value && instruction.id == value->definition)
            found = &instruction;
      return found;
    };
    for (const auto &block : body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.lambdaTarget ||
            !call.receiver ||
            call.receiver->kind != lang::MirOperandKind::Value)
          continue;
        const auto *stage = definition(call.receiver->value);
        const auto *move = stage && stage->operands.size() == 1
                               ? definition(stage->operands.front().value)
                               : nullptr;
        if (stage == nullptr ||
            stage->callInputRole != lang::MirCallInputRole::Receiver ||
            stage->callInputKind != lang::HirCallInputKind::Value ||
            move == nullptr || move->kind != lang::MirInstructionKind::Move)
          continue;
        checkedOnce = true;
        auto forged = frontend.mir;
        auto *mutated = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        for (auto &candidate : mutated->blocks)
          for (auto &instruction : candidate.instructions)
            if (instruction.id == move->id)
              instruction.ownership.reset();
        expect(hasMirVerificationError(
                   lang::verifyMirProgram(forged),
                   "once-callable invocation is not rooted in an exact "
                   "ownership move"),
               "a once invocation's value receiver stage requires its exact "
               "ownership Move");
      }
    }
  }
  expect(checkedOnce, "the fixture invokes a moved cleanup-free closure once");

  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
    const auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(optimized.valid(), "derived lambda reference arguments survive "
                              "verified optimization");
    if (optimized.valid()) {
      const auto lowered = gti_test::lowerProgram(
          frontend, optimized.sourceMir, optimized.mir,
          lang::TargetInfo::host(), optimized.report.transformations);
      expect(lang::verifyLoweredProgram(lowered).empty(),
             "derived lambda reference arguments lower to one closed program");
    }
  }

  const std::string declarations = R"(
interface Operation { int run(int value); };
class Increment : public Operation {
public:
  Increment() {}
  int run(int value) override { return value + 1; }
};
class Unrelated { public: Unrelated() {} };
)";
  for (const std::string invocation :
       {"int main() { auto read = [](Operation& s, int v) -> int { "
        "return s.run(v); }; Unrelated other{}; return read(other, 40); }",
        "int main() { auto write = [](mut Operation& s, int v) -> int { "
        "return s.run(v); }; Increment concrete{}; "
        "return write(concrete, 40); }"}) {
    const auto invalid = analyzeTrustedPreludeFixture(
        "invalid-lambda-base-reference.gti", declarations + invocation);
    expect(!invalid.canGenerateCode() &&
               countDiagnosticCode(invalid.diagnostics, "GTI-B0001") == 0 &&
               !invalid.diagnostics.empty(),
           "an unrelated class or read-only derived object for a lambda's "
           "base reference fails in semantics, not in MIR");
  }
}

void checkVirtualConsumingReceiver() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/consuming_virtual_receiver.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend =
      analyzeTrustedPreludeFixture("consuming-virtual.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "consuming interface requirements and exact overrides reach MIR");
  if (!frontend.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            call.dispatch != lang::CallDispatch::Virtual ||
            call.callableInvocation !=
                lang::CallableInvocationCapability::Once ||
            !call.functionTarget)
          continue;
        const auto *target =
            frontend.mir.findFunctionInstance(*call.functionTarget);
        expect(
            target && target->virtualMethod &&
                target->receiverMutability ==
                    lang::ReceiverMutability::Consuming,
            "virtual invocation preserves the selected consuming capability");
        auto forged = frontend.mir;
        auto *body = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        for (auto &candidate : body->blocks)
          for (auto &instruction : candidate.instructions)
            if (instruction.id == call.id)
              instruction.callableInvocation =
                  lang::CallableInvocationCapability::Read;
        expect(!lang::verifyMirProgram(forged).valid(),
               "virtual consuming call cannot forge a reusable capability");
        checked = true;
      }
    }
  }
  expect(checked, "fixture actually invokes a virtual consuming operator");
  try {
    expect(!gti_test::emitCppText(frontend).empty(),
           "virtual consuming dispatch has a complete MIR representation");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }

  for (const auto &qualifier : {std::string{}, std::string{"mut"}}) {
    auto mismatch = source;
    const auto offset = mismatch.find("&& override");
    expect(offset != std::string::npos, "virtual fixture has an override");
    if (offset == std::string::npos)
      continue;
    mismatch.replace(offset, 2, qualifier);
    const auto rejected =
        analyzeTrustedPreludeFixture("bad-override.gti", mismatch);
    expect(!rejected.canGenerateCode() &&
               std::any_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2042";
                           }),
           "read/mutable override cannot replace a consuming interface "
           "requirement");
  }
  const auto borrowed =
      analyzeTrustedPreludeFixture("borrowed-consumption.gti", R"(
interface Operation { int operator()() &&; };
int invoke(mut Operation& operation) { return std::move(operation)(); }
int main() { return 0; }
)");
  expect(!borrowed.canGenerateCode() &&
             std::any_of(borrowed.diagnostics.begin(),
                         borrowed.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2018";
                         }),
         "virtual consumption does not permit moving an ordinary borrowed "
         "reference");
}

void checkContextualCallableArguments() {
  const std::string prefix = R"(
int combine(int left, int right) { return left + right; }
class Sink { public: Sink(int value) {}  public: Sink() = default; };
class Invoke { public: int operator()(int value) { return value; }  public: Invoke() = default; };
int exact<T>(T value) { return 0; }
int choose(int selector, int value) { return value; }
int choose(bool selector, bool value) { return 0; }
class Owner<T> {
  T callback;
public:
  Owner(T callback) : callback(std::move(callback)) {}
  int call() { return combine(this.callback(3), 2); }
};
Owner<T> own<T>(T callback) { return Owner<T>(std::move(callback)); }
)";
  bool checkedReceiverStage = false;
  bool checkedClosureIdentity = false;
  for (const auto &body :
       {"return combine(callback(3), 2);",
        "return combine((callback(3)), callback(2));",
        "return combine(std::move(callback)(3), 2);",
        "Sink sink = Sink(callback(3)); return 0;",
        "Sink sink{std::move(callback)(3)}; return 0;",
        "auto sink = [](int value) -> int { return value; }; "
        "return sink(callback(3));",
        "auto sink = combine; return sink(callback(3), 2);",
        "Invoke sink{}; return sink(callback(3));",
        "return exact<int>(callback(3));", "return choose(1, callback(3));",
        "return combine(1, callback(3));",
        "auto owner = own(std::move(callback)); return owner.call();"}) {
    const auto source =
        prefix + "int invoke<T>(T callback) { " + body +
        " } int main() { auto callback = [](int value) -> int "
        "{ return value; }; return invoke(std::move(callback)); }";
    const auto result =
        analyzeTrustedPreludeFixture("contextual-callback.gti", source);
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(result.canGenerateCode(),
           std::string("selected parameter contextualizes callback: ") + body);
    if (result.canGenerateCode()) {
      try {
        expect(!gti_test::emitCppText(result).empty(),
               "contextual callback arguments reach MIR-backed emission");
      } catch (const std::exception &error) {
        expect(false, std::string(body) + ": " + error.what());
      }
      for (const auto &function : result.mir.functionInstances()) {
        if (function.typeArguments.empty())
          continue;
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            const bool stage =
                std::string_view(body).starts_with("Invoke sink") &&
                instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.callInputRole == lang::MirCallInputRole::Receiver;
            const bool closure =
                std::string_view(body).starts_with("auto sink = []") &&
                instruction.operation == lang::MirOperation::Closure;
            if ((!stage || checkedReceiverStage) &&
                (!closure || checkedClosureIdentity))
              continue;
            auto forged = result.mir;
            auto *mutated = lang::findMirBody(
                forged, {lang::MirBodyKind::Function, function.id});
            for (auto &candidate : mutated->blocks)
              for (auto &member : candidate.instructions)
                if (member.id == instruction.id) {
                  if (stage)
                    member.callSite = 0;
                  else
                    member.closureCaptureModes.push_back(
                        lang::LambdaCaptureMode::Move);
                }
            expect(!lang::verifyMirProgram(forged).valid(),
                   "nested callable emission still requires exact stage and "
                   "closure identity");
            checkedReceiverStage = checkedReceiverStage || stage;
            checkedClosureIdentity = checkedClosureIdentity || closure;
          }
        }
      }
    }
  }
  expect(
      checkedReceiverStage && checkedClosureIdentity,
      "contextual fixtures exercise both nested receiver and closure proofs");
  for (const auto &body :
       {"return combine(std::move(callback)(3), callback(2));",
        "return ambiguous(callback(3));"}) {
    const auto source =
        prefix +
        "int ambiguous(int value) { return value; } "
        "int ambiguous(bool value) { return 0; } "
        "int invoke<T>(T callback) { " +
        body +
        " } int main() { auto callback = [](int value) -> int "
        "{ return value; }; return invoke(std::move(callback)); }";
    const auto result =
        analyzeTrustedPreludeFixture("invalid-contextual-callback.gti", source);
    expect(
        !result.canGenerateCode(),
        "contextual result does not bypass move state or choose an overload");
    expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto &diagnostic) {
                         return diagnostic.message.find(
                                    std::string(body).find("ambiguous") !=
                                            std::string::npos
                                        ? "ambiguous"
                                        : "moved") != std::string::npos;
                       }),
           "invalid contextual invocation reports its actual semantic cause");
  }
  const auto mismatch = analyzeTrustedPreludeFixture(
      "contextual-callback-result-mismatch.gti",
      prefix + "int invoke<T>(T callback) { return combine(callback(3), 2); } "
               "int main() { auto callback = [](int value) -> bool { return "
               "true; }; return invoke(std::move(callback)); }");
  expect(!mismatch.canGenerateCode() &&
             hasDiagnosticCode(mismatch.diagnostics, "GTI-S2046"),
         "concrete callable must satisfy the inferred exact result contract");
}

void checkConstructorArgumentStaging() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/constructor_argument_staging.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeTrustedPreludeFixture(path.string(), source);
  expect(frontend.canGenerateCode(),
         "constructor argument staging must verify");
  if (!frontend.canGenerateCode())
    return;
  std::size_t checkedStages = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    for (const auto &block : entry.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.preparedParameterDrop)
          continue;
        ++checkedStages;
        const auto *drop =
            entry.body.findDropObligation(*stage.preparedParameterDrop);
        expect(
            drop &&
                drop->kind == lang::MirDropObligationKind::PreparedParameter &&
                !drop->initiallyActive && drop->fullExpression != 0 &&
                stage.destination == drop->place &&
                stage.lifecycle.size() == 1 &&
                stage.lifecycle.front().kind ==
                    lang::MirLifecycleEventKind::Reparent,
            "fresh constructor arguments retain prepared-parameter rollback");
        const lang::MirInstruction *consumer = nullptr;
        std::size_t failureDrops = 0;
        for (const auto &candidate : entry.body.blocks)
          for (const auto &instruction : candidate.instructions)
            for (const auto &event : instruction.lifecycle) {
              if (event.source != *stage.preparedParameterDrop)
                continue;
              if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                  instruction.kind == lang::MirInstructionKind::Construct)
                consumer = &instruction;
              if (event.kind == lang::MirLifecycleEventKind::Drop &&
                  event.failureCleanup)
                ++failureDrops;
            }
        expect(consumer && consumer->callSite == stage.callSite &&
                   failureDrops != 0,
               "invocation transfers the parameter; earlier failure cleans it");
        if (!consumer)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, entry.id});
          for (auto &candidate : body->blocks)
            for (auto &instruction : candidate.instructions) {
              if (instruction.id == stage.id && mutation == 0)
                instruction.preparedParameterDrop.reset();
              if (instruction.id == stage.id && mutation == 1) {
                instruction.lifecycle.front().kind =
                    lang::MirLifecycleEventKind::TransferOut;
                instruction.lifecycle.front().target = 0;
              }
              if (instruction.id == consumer->id && mutation == 2)
                std::erase_if(instruction.lifecycle, [&](const auto &event) {
                  return event.source == *stage.preparedParameterDrop;
                });
            }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "MIR rejects missing, premature, or absent "
                 "constructor-argument handoff");
        }
      }
    }
  }
  expect(checkedStages == 2, "both fresh owning arguments must be staged");
  try {
    expect(!gti_test::emitCppText(frontend).empty(),
           "prepared constructor arguments must use production MIR emission");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
}

void checkScalarPrefixBeforeOwnedFieldMove() {
  const auto frontend =
      analyzeTrustedPreludeFixture("scalar-prefix-owned-field.gti", R"(
class Payload { public: Payload() {} ~Payload() {} };
class Box {
  int identity;
  mut Payload payload;
public:
  Box(int identity, Payload payload)
      : identity(identity), payload(std::move(payload)) {}
};
struct Other { int foreign = 0;  public: Other() = default; };
int main() {
  Payload payload{};
  Box box{8, std::move(payload)};
  Other other{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "scalar parameter followed by an owning field is valid source");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances())
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes.front() == lang::SemanticType::Int32 &&
        candidate.parameterTypes.back().kind == lang::SemanticType::Class)
      constructor = &candidate;
  expect(constructor != nullptr,
         "scalar-prefix fixture selects one mixed constructor");
  if (!constructor)
    return;
  const auto canonical =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor);
  expect(canonical && canonical->size() == 1 &&
             canonical->front().initializer == 1,
         "the exact scalar prefix admits the following owned field move");
  if (!canonical || canonical->size() != 1)
    return;
  lang::SymbolId foreignField = 0;
  for (const auto &owner : frontend.mir.classInstances())
    if (owner.id != constructor->owner)
      for (const auto &field : owner.declaredFields)
        if (field.type == lang::SemanticType::Int32)
          foreignField = field.symbol;
  expect(foreignField != 0, "fixture has an unrelated scalar field identity");
  if (!foreignField)
    return;

  for (int mutation = 0; mutation != 9; ++mutation) {
    auto forged = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        forged.findConstructorInstance(constructor->id));
    auto entry = std::find_if(
        changed->body.blocks.begin(), changed->body.blocks.end(),
        [&](const auto &block) { return block.id == changed->body.entry; });
    expect(
        entry != changed->body.blocks.end() && entry->instructions.size() >= 4,
        "prefix mutations require Load, Initialize, boundary, and owned Move");
    if (entry == changed->body.blocks.end() || entry->instructions.size() < 4)
      continue;
    auto &load = entry->instructions[0];
    auto &store = entry->instructions[1];
    auto &boundary = entry->instructions[2];
    expect(load.kind == lang::MirInstructionKind::Load &&
               store.kind == lang::MirInstructionKind::Initialize &&
               boundary.kind == lang::MirInstructionKind::Lifecycle &&
               entry->instructions[3].id == canonical->front().moveInstruction,
           "each direct-helper mutation starts from the newly admitted prefix");
    switch (mutation) {
    case 0:
      load.definedFailure.propagation =
          lang::FailurePropagationKind::DirectCall;
      break;
    case 1:
      load.kind = lang::MirInstructionKind::Compute;
      load.operation = lang::MirOperation::Add;
      break;
    case 2:
      load.info.type = lang::SemanticType::RawPointer;
      load.info.type.arguments = {lang::SemanticType::Int32};
      break;
    case 3:
      load.info.traits.containsBorrowedState = true;
      break;
    case 4:
      load.operands.front().kind = lang::MirOperandKind::BorrowRead;
      break;
    case 5: {
      auto field = std::find_if(
          changed->body.places.begin(), changed->body.places.end(),
          [&](const auto &place) { return store.destination == place.id; });
      expect(field != changed->body.places.end() && !field->projections.empty(),
             "scalar store retains one destination field projection");
      if (field != changed->body.places.end() && !field->projections.empty())
        field->projections.front().field = foreignField;
      break;
    }
    case 6:
      boundary.fullExpressionEnd = 0;
      for (const auto &expression : changed->body.fullExpressions)
        if (expression.constructorInitializer == 2)
          boundary.fullExpressionEnd = expression.id;
      expect(
          boundary.fullExpressionEnd != 0,
          "mutation retargets the boundary to an existing other initializer");
      break;
    case 7:
      changed->initializers.front().arguments =
          changed->initializers.back().arguments;
      break;
    case 8:
      store.definedFailure.propagation =
          lang::FailurePropagationKind::DirectCall;
      break;
    }
    // Deliberately call the local proof directly: rejection must not be an
    // incidental effect of whole-program verifier failure elsewhere.
    expect(!lang::cppMirOwnedParameterFieldBindings(forged, *changed),
           "owned-field scalar-prefix proof rejects mutation " +
               std::to_string(mutation));
  }
}

void checkRetainedConsumingFieldReceiver() {
  const std::string source = R"(
class Piece {
public:
  Piece() {}
  ~Piece() {}
  int operator()() && { return 1; }
  int read() { return 1; }
};
class Owner {
  mut Piece first;
  mut Piece second;
public:
  Owner(Piece first, Piece second)
      : first(std::move(first)), second(std::move(second)) {}
  int operator()() && {
    int value = std::move(this.first)();
    return std::move(this.second)();
  }
};
class Extract {
  mut Piece field;
public:
  Extract(Piece field) : field(std::move(field)) {}
  Piece operator()() && { return std::move(this.field); }
};
int main() {
  Piece first{};
  Piece second{};
  Piece third{};
  Owner owner{std::move(first), std::move(second)};
  Extract extract{std::move(third)};
  Piece extracted = std::move(extract)();
  return std::move(owner)();
}
)";
  const auto frontend =
      analyzeTrustedPreludeFixture("retained-consuming-field.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "owned consuming fields and explicit extraction reach verified MIR");
  if (!frontend.canGenerateCode())
    return;

  std::size_t retainedHir = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (!value.retainedReceiverProjection)
        continue;
      ++retainedHir;
      expect(value.kind == lang::HirValueKind::Move &&
                 value.operands.size() == 1 && value.source != nullptr &&
                 value.source->exprKind() == lang::ExprKind::Call,
             "retained HIR is the exact resolved explicit field-consumption "
             "expression");
      if (value.source && value.source->exprKind() == lang::ExprKind::Call) {
        const auto *resolved = frontend.semantics.findCall(
            static_cast<const lang::Call &>(*value.source));
        expect(resolved && resolved->retainedReceiverProjection &&
                   resolved->intrinsic == lang::IntrinsicKind::Move,
               "semantics explicitly authorizes retained receiver projection");
      }
    }
  }
  expect(retainedHir == 2,
         "only the two immediate field invocations retain receiver storage");

  bool checkedStages = false;
  bool checkedExtraction = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    std::vector<const lang::MirInstruction *> stages;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Move &&
            instruction.ownership && instruction.ownership->place.receiver &&
            !instruction.ownership->place.projections.empty()) {
          checkedExtraction = true;
          expect(instruction.ownership->kind ==
                         lang::OwnershipEventKind::Move &&
                     !instruction.lifecycle.empty(),
                 "actual field extraction still moves physical ownership");
        }
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind ==
                lang::HirCallInputKind::RetainedReceiver)
          stages.push_back(&instruction);
      }
    }
    if (stages.empty())
      continue;
    expect(stages.size() == 2,
           "the consuming owner retains two independently consumed fields");
    if (stages.size() != 2)
      continue;
    for (const auto *stage : stages) {
      const auto *place =
          stage->operands.size() == 1
              ? function.body.findPlace(stage->operands[0].place)
              : nullptr;
      expect(stage->callInputRole == lang::MirCallInputRole::Receiver &&
                 stage->operands.size() == 1 &&
                 stage->operands[0].kind == lang::MirOperandKind::BorrowWrite &&
                 place && place->root == lang::MirPlaceRootKind::This &&
                 !place->projections.empty() && stage->ownership &&
                 stage->ownership->kind ==
                     lang::OwnershipEventKind::ConsumeReceiver &&
                 stage->ownership->before ==
                     lang::OwnershipStateSet::Available &&
                 stage->ownership->after == lang::OwnershipStateSet::Moved &&
                 stage->lifecycle.empty() && !stage->destination &&
                 !stage->preparedParameterDrop &&
                 std::none_of(function.body.dropObligations.begin(),
                              function.body.dropObligations.end(),
                              [&](const auto &drop) {
                                return drop.value == stage->hirValue &&
                                       drop.kind ==
                                           lang::MirDropObligationKind::Value;
                              }),
             "retained CallInput consumes availability without relocating or "
             "creating a cleanup owner");
    }
    for (int mutation = 0; mutation != 10; ++mutation) {
      auto forged = frontend.mir;
      auto *body =
          lang::findMirBody(forged, {lang::MirBodyKind::Function, function.id});
      lang::MirInstruction *first = nullptr;
      lang::MirInstruction *second = nullptr;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          if (instruction.id == stages[0]->id)
            first = &instruction;
          if (instruction.id == stages[1]->id)
            second = &instruction;
        }
      if (!first || !second || first->operands.empty() || !first->ownership) {
        expect(false, "retained mutation requires the canonical stage pair");
        continue;
      }
      auto place = std::find_if(
          body->places.begin(), body->places.end(), [&](const auto &candidate) {
            return candidate.id == first->operands[0].place;
          });
      if (place == body->places.end()) {
        expect(false, "retained mutation requires its receiver place");
        continue;
      }
      switch (mutation) {
      case 0: {
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            forged.functionInstances());
        for (auto &caller : functions)
          if (caller.id == function.id)
            caller.receiverMutability = lang::ReceiverMutability::ReadOnly;
        break;
      }
      case 1:
        place->root = lang::MirPlaceRootKind::Temporary;
        break;
      case 2:
        place->projections.front().kind = lang::MirProjectionKind::Dereference;
        break;
      case 3:
        first->ownership->before = lang::OwnershipStateSet::Moved;
        break;
      case 4:
        first->ownership->after = lang::OwnershipStateSet::Available;
        break;
      case 5:
        first->lifecycle.push_back(
            {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
        break;
      case 6:
        first->callInputKind = lang::HirCallInputKind::MoveValue;
        break;
      case 7:
        second->operands = first->operands;
        second->ownership = first->ownership;
        break;
      case 8:
        first->ownership->kind = lang::OwnershipEventKind::Move;
        break;
      case 9:
        first->ownership.reset();
        break;
      }
      expect(lang::rebuildMirValueUses(*body),
             "retained receiver mutation keeps a rebuildable use index");
      expect(!lang::verifyMirProgram(forged).valid(),
             "MIR rejects retained receiver mutation " +
                 std::to_string(mutation));
    }
    checkedStages = true;
  }
  expect(checkedStages && checkedExtraction,
         "fixture distinguishes retained invocation from physical extraction");

  auto reused = source;
  const auto secondUse = reused.find("std::move(this.second)()");
  expect(secondUse != std::string::npos,
         "fixture has a second-field invocation");
  if (secondUse != std::string::npos) {
    reused.replace(secondUse, std::string("std::move(this.second)()").size(),
                   "std::move(this.first)()");
    const auto rejected =
        analyzeTrustedPreludeFixture("retained-consuming-reuse.gti", reused);
    expect(!rejected.canGenerateCode() &&
               std::any_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.message.find("moved") !=
                                    std::string::npos;
                           }),
           "retained storage does not permit a second field consumption");
  }

  const auto retainedStages = [](const auto &result) {
    std::size_t count = 0;
    for (const auto &function : result.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          count += instruction.kind == lang::MirInstructionKind::CallInput &&
                   instruction.callInputKind ==
                       lang::HirCallInputKind::RetainedReceiver;
    return count;
  };
  const auto firstUse = source.find("std::move(this.first)()");
  expect(firstUse != std::string::npos, "fixture has a first-field invocation");
  if (firstUse != std::string::npos) {
    auto grouped = source;
    grouped.replace(firstUse, std::string("std::move(this.first)()").size(),
                    "((std::move(this.first)))()");
    const auto accepted =
        analyzeTrustedPreludeFixture("retained-grouped-field.gti", grouped);
    expect(accepted.canGenerateCode() && retainedStages(accepted) == 2,
           "transparent receiver grouping preserves both retained stages");

    auto member = source;
    member.replace(firstUse, std::string("std::move(this.first)()").size(),
                   "std::move(this.first).read()");
    const auto retained =
        analyzeTrustedPreludeFixture("retained-member-field.gti", member);
    expect(retained.canGenerateCode() && retainedStages(retained) == 2,
           "an immediate named method retains its consumed field like a call "
           "operator");
  }

  for (const std::string &qualifier : {std::string{}, std::string{"mut"}}) {
    for (const std::string &use :
         {std::string{"return std::move(this.field)();"},
          std::string{"int& view = std::move(this.field)(); return view;"}}) {
      const std::string borrowedSource =
          "class Borrower { int value = 1; public: Borrower() {} "
          "int& operator()() " +
          qualifier + R"( { return this.value; } };
class BorrowOwner {
  mut Borrower field;
public:
  BorrowOwner(Borrower field) : field(std::move(field)) {}
  int operator()() && {
  )" + use +
          R"(
  }
};
int main() {
  Borrower field{};
  BorrowOwner owner{std::move(field)};
  return std::move(owner)();
}
)";
      const auto rejected = analyzeTrustedPreludeFixture(
          "retained-borrowed-result.gti", borrowedSource);
      const auto diagnostic = std::find_if(
          rejected.diagnostics.begin(), rejected.diagnostics.end(),
          [](const auto &item) { return item.code == "GTI-S2022"; });
      expect(!rejected.canGenerateCode() &&
                 diagnostic != rejected.diagnostics.end(),
             "read/mutable fallback must reject borrowed results before MIR, "
             "even when the caller immediately copies the referent");
      if (diagnostic != rejected.diagnostics.end()) {
        const std::string invocation = "std::move(this.field)()";
        const auto callOffset =
            borrowedSource.find(invocation) + invocation.size() - 1;
        const auto declarationOffset = borrowedSource.find("operator");
        expect(
            diagnostic->phase == lang::DiagnosticPhase::Semantics &&
                diagnostic->severity == lang::DiagnosticSeverity::Error &&
                diagnostic->message.find(
                    "cannot return a reference or borrowed state") !=
                    std::string::npos &&
                std::filesystem::path(diagnostic->primary.source).filename() ==
                    "gti-trusted-retained-borrowed-result.gti" &&
                diagnostic->primary.start == callOffset &&
                diagnostic->primary.end == callOffset + 1 &&
                diagnostic->related.size() == 1 &&
                diagnostic->related.front().span.source ==
                    diagnostic->primary.source &&
                diagnostic->related.front().span.start == declarationOffset &&
                diagnostic->related.front().span.end == declarationOffset + 8 &&
                !diagnostic->hints.empty() &&
                diagnostic->hints.front().find("owning value") !=
                    std::string::npos &&
                diagnostic->fixes.empty(),
            "retained borrowed-result diagnostic preserves actionable spans, "
            "selected declaration, ownership hint, and no speculative fix");
      }
    }
  }

  const auto borrowedReceiver =
      analyzeTrustedPreludeFixture("retained-borrowed-owner.gti", R"(
class Piece { public: Piece() {} int operator()() && { return 1; } };
struct Container { mut Piece field{};  public: Container() = default; };
int invoke(mut Container& owner) { return std::move(owner.field)(); }
int main() { return 0; }
)");
  expect(!borrowedReceiver.canGenerateCode() &&
             std::any_of(borrowedReceiver.diagnostics.begin(),
                         borrowedReceiver.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2018";
                         }),
         "borrowed-root fields cannot manufacture retained owning receivers");
}

void checkRetainedLambdaFieldReceiver() {
  const std::string source = R"(
class Token { public: Token() {} ~Token() {} int read() { return 7; } };
class Owner<T, Args...> {
  mut T field;
public:
  Owner(T field) : field(std::move(field)) {}
  int operator()(Args... args) && { return ((std::move(this.field)))(args...); }
};
int invoke<T>(T callable) {
  Owner<T, int> owner{std::move(callable)};
  return std::move(owner)(5);
}
int main() {
  Token token{};
  auto callable = [token = std::move(token)](int value) -> int {
    return token.read() + value;
  };
  auto unrelated = [](int value) -> int { return value; };
  int unused = unrelated(0);
  return invoke(std::move(callable)) - 12;
}
)";
  for (bool pack : {true, false}) {
    auto input = source;
    if (!pack) {
      const auto at = input.find("operator()(Args... args)");
      input.replace(at, std::string("operator()(Args... args)").size(),
                    "operator()(int argument)");
      const auto use = input.find("(args...)");
      input.replace(use, std::string("(args...)").size(), "(argument)");
    }
    const auto frontend =
        analyzeTrustedPreludeFixture("retained-lambda.gti", input);
    expect(frontend.canGenerateCode(),
           "retained stored lambda reaches verified MIR");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    bool checked = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              stage.callInputKind != lang::HirCallInputKind::RetainedReceiver ||
              stage.info.type.kind != lang::SemanticType::Lambda)
            continue;
          checked = true;
          expect(stage.ownership &&
                     stage.ownership->kind ==
                         lang::OwnershipEventKind::ConsumeReceiver &&
                     stage.lifecycle.empty() && !stage.preparedParameterDrop &&
                     stage.operands.size() == 1 &&
                     stage.operands.front().kind ==
                         lang::MirOperandKind::BorrowWrite,
                 "retained lambda consumes access without transferring capture "
                 "cleanup");
          for (int mutation = 0; mutation < 6; ++mutation) {
            auto forged = frontend.mir;
            auto *body = lang::findMirBody(
                forged, {lang::MirBodyKind::Function, function.id});
            for (auto &candidate : body->blocks) {
              for (auto &instruction : candidate.instructions) {
                if (mutation == 5 && instruction.lambdaTarget &&
                    instruction.receiver &&
                    instruction.receiver->kind == lang::MirOperandKind::Value &&
                    instruction.receiver->value == *stage.result) {
                  for (const auto &lambda : forged.lambdaInstances()) {
                    if (lambda.type != stage.info.type) {
                      instruction.lambdaTarget = lambda.id;
                      break;
                    }
                  }
                }
                if (instruction.id != stage.id)
                  continue;
                if (mutation == 0)
                  instruction.ownership.reset();
                if (mutation == 1)
                  instruction.callInputKind =
                      lang::HirCallInputKind::ReadBorrow;
                if (mutation == 2)
                  instruction.operands.front().kind =
                      lang::MirOperandKind::BorrowRead;
                if (mutation == 3)
                  instruction.ownership->after =
                      lang::OwnershipStateSet::Available;
                if (mutation == 4)
                  instruction.info.traits.containsBorrowedState = true;
              }
            }
            expect(!lang::verifyMirProgram(forged).valid(),
                   "MIR rejects retained lambda capability mutation " +
                       std::to_string(mutation));
          }
        }
      }
    }
    expect(checked,
           "stored lambda fixture has an exact retained receiver checkpoint");
    auto reused = input;
    const auto at = reused.find("return ((std::move(this.field)))");
    reused.insert(at, pack ? "((std::move(this.field)))(args...); "
                           : "((std::move(this.field)))(argument); ");
    const auto rejected =
        analyzeTrustedPreludeFixture("retained-lambda-reuse.gti", reused);
    expect(!rejected.canGenerateCode() &&
               countDiagnosticCode(rejected.diagnostics, "GTI-S2018") != 0,
           "retained lambda invocation prevents subsequent field reuse");
  }
}

void checkConditionalCarrierConsumption() {
  // A borrowed carrier consumed in only one arm of ||, &&, ?: or if retires
  // its group on the arm that skips the consuming call, so every edge into the
  // join agrees. Each retirement is required: dropping any one leaves the
  // group live on its path and verification rejects the program.
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/conditional_carrier_consume.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend =
      analyzeTrustedPreludeFixture("conditional-carrier-consume.gti", source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "conditionally consumed carriers reach verified MIR");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCppText(frontend).empty(),
         "conditionally consumed carriers emit C++");
  std::size_t retirements = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    std::unordered_map<lang::MirLoanId, std::unordered_set<lang::MirBlockId>>
        endings;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
            instruction.loan)
          endings[*instruction.loan].insert(block.id);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
            !instruction.loan || endings[*instruction.loan].size() < 2 ||
            function.body.findLoan(*instruction.loan)->kind !=
                lang::MirLoanKind::Stored)
          continue;
        ++retirements;
        auto forged = frontend.mir;
        auto *body = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        for (auto &candidateBlock : body->blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.id)
              candidate.tombstone = true;
        expect(!lang::verifyMirProgram(forged).valid(),
               "every path retires a conditionally consumed carrier group");
      }
  }
  expect(retirements >= 8,
         "the fixture retires consumed groups on both sides of each join");
}

int main() {
  checkConditionalCarrierConsumption();
  checkLambdaBaseReferenceArguments();
  checkRetainedLambdaFieldReceiver();
  checkRetainedConsumingFieldReceiver();
  checkScalarPrefixBeforeOwnedFieldMove();
  checkConstructorArgumentStaging();
  checkContextualCallableArguments();
  checkVirtualConsumingReceiver();
  checkConsumingLambda("consuming_lambda_lifetime", false);
  checkConsumingLambda("consuming_lambda_pack", true);
  checkConsumingLambda("consuming_lambda_nested", false);
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/consuming_receiver_lifetime.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend =
      analyzeTrustedPreludeFixture("consuming-receiver.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "owned consuming receivers reach verified MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &function : frontend.mir.functionInstances())
      if (function.entryKind != lang::ProgramEntryKind::None)
        std::cerr << lang::MirPrinter().print(function.body);
    return 1;
  }
  bool checkedReceiver = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.callInputRole != lang::MirCallInputRole::Receiver ||
            stage.callInputKind != lang::HirCallInputKind::MoveValue)
          continue;
        const auto *value =
            function.body.findValue(stage.operands.front().value);
        const lang::MirDropObligation *lifetime = nullptr;
        for (const auto &drop : function.body.dropObligations)
          if (drop.kind == lang::MirDropObligationKind::Value &&
              value != nullptr && drop.value == value->sourceValue)
            lifetime = &drop;
        expect(lifetime && lifetime->fullExpression != 0 &&
                   !lifetime->initiallyActive && !stage.preparedParameterDrop &&
                   stage.lifecycle.empty() && !stage.destination,
               "the receiver stage aliases one caller-owned full-expression "
               "obligation without a parameter transfer");
        if (lifetime == nullptr)
          continue;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          if (mutation == 0) {
            for (auto &drop : body->dropObligations)
              if (drop.id == lifetime->id)
                drop.fullExpression = 0;
          } else if (mutation == 1) {
            for (auto &candidate : body->blocks)
              for (auto &instruction : candidate.instructions)
                if (instruction.kind == lang::MirInstructionKind::Call &&
                    instruction.callSite == stage.callSite)
                  instruction.lifecycle.push_back(
                      {.kind = lang::MirLifecycleEventKind::TransferOut,
                       .source = lifetime->id});
          } else if (mutation == 2) {
            for (auto &candidate : body->blocks)
              for (auto &instruction : candidate.instructions)
                if (instruction.id == stage.id)
                  instruction.operands.push_back(stage.operands.front());
          } else {
            for (auto &candidate : body->blocks)
              for (auto &instruction : candidate.instructions)
                if (instruction.id == value->definition)
                  instruction.lifecycle.clear();
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "MIR rejects consuming lifetime mutation " +
                     std::to_string(mutation));
        }
        checkedReceiver = true;
      }
    }
  }
  expect(checkedReceiver, "fixture exercises a retained consuming receiver");
  try {
    expect(!gti_test::emitCppText(frontend).empty(),
           "caller-owned consuming receiver storage has a complete MIR "
           "representation");
    for (const auto &qualifier : {std::string{}, std::string{"mut"}}) {
      auto fallbackSource = source;
      const auto qualifierOffset = fallbackSource.find(") && {");
      expect(qualifierOffset != std::string::npos,
             "receiver fixture has a consuming qualifier");
      if (qualifierOffset == std::string::npos)
        continue;
      fallbackSource.replace(qualifierOffset + 2, 2, qualifier);
      const auto fallback =
          analyzeTrustedPreludeFixture("consumed-fallback.gti", fallbackSource);
      expect(fallback.canGenerateCode(),
             "explicit move may select a reusable fallback on owned storage");
      if (fallback.canGenerateCode())
        expect(!gti_test::emitCppText(fallback).empty(),
               "read and mutable fallbacks use the same receiver lifetime");
    }
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  return failures == 0 ? 0 : 1;
}
