#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

// The value()/error() member of the prelude's ordinary source class
// expected<T, E> that this call selects, or an empty name.
std::string_view expectedMember(const lang::FrontendResult &frontend,
                                const lang::MirBody &body,
                                const lang::MirInstruction &instruction) {
  const lang::HirFunctionInstance *target =
      instruction.kind == lang::MirInstructionKind::Call &&
              instruction.functionTarget
          ? frontend.hir.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  const lang::MirValue *receiver =
      instruction.receiver ? body.findValue(instruction.receiver->value)
                           : nullptr;
  const auto *owner =
      receiver != nullptr &&
              receiver->info.type.kind == lang::SemanticType::Class
          ? frontend.semantics.findClassType(receiver->info.type.classId)
          : nullptr;
  if (target == nullptr || target->source == nullptr || owner == nullptr ||
      owner->declaration == nullptr ||
      owner->declaration->name().lexeme != "expected")
    return {};
  const std::string_view name = target->source->name().lexeme;
  return name == "value" || name == "error" ? name : std::string_view{};
}

void testStagedExtraction(std::string_view name,
                          std::size_t expectedCallers = 1) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    std::string(name);
  std::ifstream input(path);
  expect(input.good(), "expected payload staging fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "inline expected payloads must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t tested = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    std::vector<const lang::MirInstruction *> extractions;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (!expectedMember(frontend, function.body, instruction).empty() &&
            instruction.info.type.kind == lang::SemanticType::Class &&
            instruction.result && instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value)
          extractions.push_back(&instruction);
    if (extractions.size() != 2)
      continue;
    const auto stageFor = [&](const lang::MirInstruction &extraction,
                              std::size_t index) {
      const lang::MirInstruction *found = nullptr;
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputIndex == index &&
              instruction.preparedParameterDrop &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind ==
                  lang::MirOperandKind::Value &&
              instruction.operands.front().value == *extraction.result)
            found = &instruction;
      return found;
    };
    const lang::MirInstruction *stage = stageFor(*extractions.front(), 0);
    const lang::MirInstruction *secondStage = stageFor(*extractions.back(), 1);
    // The consuming accessor's receiver is staged from the moved carrier.
    lang::MirValueId movedCarrier = 0;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.result == extractions.front()->receiver->value &&
            instruction.operands.size() == 1)
          movedCarrier = instruction.operands.front().value;
    expect(stage != nullptr && secondStage != nullptr,
           "first extracted payload must enter prepared storage");
    if (stage == nullptr || secondStage == nullptr)
      continue;
    ++tested;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(emitter.supportsFailureBodyText(address),
           "two inline payloads must support exact status-form emission");
    if (emitter.supportsFailureBodyText(address)) {
      const auto text =
          emitter.emitFailureBodyText(address, "expected-pair", 1).text;
      // Each source accessor call constructs its payload directly into its
      // own prepared argument slot.
      const std::string accessor =
          "_" +
          std::string(
              expectedMember(frontend, function.body, *extractions.front())) +
          "__gti_mir_failure(";
      const auto slot = [](const lang::MirInstruction &input) {
        return "__gti_mir_p_" + std::to_string(*input.destination) +
               ".construction_address()";
      };
      const auto first = text.find(accessor);
      const auto second = first == std::string::npos
                              ? std::string::npos
                              : text.find(accessor, first + 1);
      expect(second != std::string::npos && stage->destination &&
                 secondStage->destination &&
                 text.find(slot(*stage), first) < second &&
                 text.find(slot(*secondStage), second) != std::string::npos,
             "both checked extractions must execute separately into prepared "
             "slots");
    }
    for (int mutation = 0; mutation != 7; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      bool changed = false;
      for (auto &block : body->blocks) {
        if (mutation == 3 &&
            block.terminator.kind == lang::MirTerminatorKind::Invoke &&
            block.terminator.invokeInstruction == extractions.back()->id) {
          block.terminator.elseTarget = block.terminator.target;
          changed = true;
        }
        for (auto &instruction : block.instructions) {
          if (mutation == 4 && instruction.id == stage->id) {
            instruction.lifecycle.clear();
            changed = true;
          }
          if (mutation == 5 &&
              instruction.kind == lang::MirInstructionKind::Call &&
              instruction.callSite == stage->callSite) {
            for (auto &event : instruction.lifecycle)
              if (event.source == *stage->preparedParameterDrop) {
                event.source = 999999;
                changed = true;
              }
          }
          if (mutation == 6 && instruction.result == movedCarrier &&
              instruction.kind == lang::MirInstructionKind::Move) {
            instruction.operands.front().kind = lang::MirOperandKind::Copy;
            changed = true;
          }
          if (mutation == 0 && instruction.id == stage->id) {
            instruction.callInputIndex = 1;
            changed = true;
          }
          if (mutation != 1 && mutation != 2)
            continue;
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              instruction.lifecycle.size() != 1 ||
              instruction.lifecycle.front().source !=
                  *stage->preparedParameterDrop ||
              !instruction.lifecycle.front().failureCleanup)
            continue;
          if (mutation == 1)
            instruction.lifecycle.front().failureCleanup = false;
          else {
            instruction.kind = lang::MirInstructionKind::Lifecycle;
            instruction.destination.reset();
            instruction.lifecycle.clear();
          }
          changed = true;
        }
      }
      expect(changed, "each staging mutation must hit its intended record");
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject mismatched argument identity or failure cleanup");
      // Staging runs through ordinary prelude constructor calls; backend
      // admission (body analysis and body text) rejects what MIR rejects.
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(!rejected.analyze(address).ready() ||
                 !rejected.supportsFailureBodyText(address),
             "representation admission rejects malformed staging");
    }
  }
  expect(tested == expectedCallers,
         "fixture must exercise the expected two-extraction callers");
}

} // namespace

int main() {
  testStagedExtraction("expected_payload_pair_arguments.gti");
  testStagedExtraction("expected_payload_pair_cleanup.gti");
  testStagedExtraction("expected_passive_payload_arguments.gti", 2);
  if (failures != 0)
    return 1;
  std::cout << "expected payload pair emitter mutations passed\n";
  return 0;
}
