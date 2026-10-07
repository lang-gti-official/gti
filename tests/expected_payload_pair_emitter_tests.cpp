#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

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
        if ((instruction.intrinsic == lang::IntrinsicKind::ExpectedValue ||
             instruction.intrinsic == lang::IntrinsicKind::ExpectedError) &&
            instruction.info.type.kind == lang::SemanticType::Class &&
            instruction.result && instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value)
          extractions.push_back(&instruction);
    if (extractions.size() != 2)
      continue;
    const lang::MirInstruction *stage = nullptr;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputIndex == 0 &&
            instruction.preparedParameterDrop &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Value &&
            instruction.operands.front().value == *extractions.front()->result)
          stage = &instruction;
    expect(stage != nullptr,
           "first extracted payload must enter prepared storage");
    if (stage == nullptr)
      continue;
    ++tested;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(emitter.supportsFailureBodyText(address),
           "two inline payloads must support exact status-form emission");
    if (emitter.supportsFailureBodyText(address)) {
      const auto text =
          emitter.emitFailureBodyText(address, "expected-pair", 1).text;
      const std::string helper =
          extractions.front()->intrinsic == lang::IntrinsicKind::ExpectedValue
              ? "mir_expected_value_move_v1"
              : "mir_expected_error_move_v1";
      const auto first = text.find(helper);
      expect(first != std::string::npos &&
                 text.find(helper, first + 1) != std::string::npos,
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
          if (mutation == 6 &&
              instruction.result == extractions.front()->receiver->value &&
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
      expect(
          !lang::CppMirBodyEmitter(malformed, map)
               .supportsFailureBodyText(address),
          "representation proof must independently reject malformed staging");
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
