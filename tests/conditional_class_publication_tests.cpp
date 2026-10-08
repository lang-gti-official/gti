#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_editor.h"

int main() {
  std::size_t joins = 0;
  for (const std::string fixture :
       {"conditional_class_passive", "conditional_class_cleanup"}) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / (fixture + ".gti");
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "conditional class fixture has valid MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    try {
      expect(!gti_test::emitCppText(frontend).empty(),
             "both passive and active conditional bindings emit complete C++");
    } catch (const std::exception &error) {
      expect(false, error.what());
    }
    if (fixture != "conditional_class_passive")
      continue;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &slot : function.body.places) {
        if (slot.root != lang::MirPlaceRootKind::Temporary ||
            slot.type.kind != lang::SemanticType::Class ||
            !slot.projections.empty())
          continue;
        ++joins;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
        expect(emitter.supportsFailureBodyText(address),
               "passive class diamond admits exact binding publication");
        if (emitter.supportsFailureBodyText(address)) {
          const auto text =
              emitter.emitFailureBodyText(address, "passive-join", 1);
          expect(text.emitted() &&
                     text.text.find("end passive conditional representation") !=
                         std::string::npos,
                 "the moved-from native shell ends without inventing cleanup");
        }
        std::size_t arms = 0;
        std::size_t moves = 0;
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Initialize &&
                instruction.destination == slot.id) {
              ++arms;
              expect(instruction.lifecycle.empty(),
                     "passive arms carry no semantic reparent");
            }
            if (instruction.kind == lang::MirInstructionKind::Move &&
                instruction.operands.size() == 1 &&
                instruction.operands.front().place == slot.id)
              ++moves;
          }
        expect(
            arms == 2 && moves == 1,
            "the fixture reaches two constructor arms and one terminal move");
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *changedSlot =
              const_cast<lang::MirPlace *>(changed->body.findPlace(slot.id));
          bool applied = false;
          if (mutation == 0) {
            changedSlot->initiallyAvailable = true;
            applied = true;
          } else if (mutation == 1) {
            changedSlot->traits.containsBorrowedState = true;
            applied = true;
          } else if (mutation >= 5) {
            for (auto &block : changed->body.blocks) {
              if (applied ||
                  block.terminator.kind != lang::MirTerminatorKind::Invoke)
                continue;
              const auto *success =
                  changed->body.findBlock(block.terminator.target);
              if (success == nullptr || success->instructions.size() != 1 ||
                  success->instructions.front().kind !=
                      lang::MirInstructionKind::Initialize ||
                  success->instructions.front().destination != slot.id)
                continue;
              if (mutation == 5) {
                block.terminator.successLifecycle.push_back(
                    {.kind = lang::MirLifecycleEventKind::Initialize,
                     .target = 999999});
              } else {
                // A second predecessor makes publication reachable without
                // this constructor. Rebuild uses so rejection cannot rely on
                // an obsolete value-use cache.
                block.terminator.elseTarget = success->id;
              }
              applied = true;
            }
          } else {
            for (auto &block : changed->body.blocks)
              for (auto &instruction : block.instructions) {
                if (applied)
                  continue;
                if (mutation == 2 &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.destination == slot.id) {
                  instruction.operands.front().type =
                      lang::SemanticType::Unknown;
                  applied = true;
                }
                if (mutation == 3 &&
                    instruction.kind == lang::MirInstructionKind::Move &&
                    instruction.operands.size() == 1 &&
                    instruction.operands.front().place == slot.id) {
                  instruction.operands.front().kind =
                      lang::MirOperandKind::Copy;
                  applied = true;
                }
                if (mutation == 4 &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.destination == slot.id) {
                  instruction.lifecycle.push_back(
                      {.kind = lang::MirLifecycleEventKind::Initialize,
                       .target = 999999});
                  applied = true;
                }
              }
          }
          expect(applied,
                 "each malformed publication mutation finds its target");
          (void)lang::rebuildMirValueUses(changed->body);
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(address),
                 "publication rejects forged availability, borrowing, type, "
                 "copying, lifecycle identity and success-edge publication "
                 "after rebuilding uses");
        }
      }
    }
  }
  expect(joins == 1, "one source-defined passive class diamond was exercised");
  return failures ? 1 : 0;
}
