#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_editor.h"

int main() {
  std::size_t joins = 0;
  for (const std::string fixture :
       {"conditional_class_passive", "conditional_class_cleanup",
        "conditional_class_passive_failure"}) {
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
    if (fixture == "conditional_class_cleanup")
      continue;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &slot : function.body.places) {
        if (slot.root != lang::MirPlaceRootKind::Temporary ||
            slot.type.kind != lang::SemanticType::Class ||
            !slot.projections.empty())
          continue;
        std::size_t armInitializations = 0;
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::Initialize &&
                instruction.destination == slot.id)
              ++armInitializations;
        // Imported library functions contain unrelated class temporaries.
        // Select the diamond by its two publications, not by a public name.
        if (armInitializations != 2)
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
  expect(joins == 3,
         "nonraising and raising passive class diamonds were exercised");
  std::size_t groupedBodies = 0;
  for (const std::string fixture :
       {"grouped_class_passive", "grouped_class_cleanup",
        "grouped_class_failure", "grouped_class_argument",
        "conditional_class_passive", "conditional_class_cleanup"}) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / (fixture + ".gti");
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "grouped fixture has valid MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    try {
      expect(!gti_test::emitCppText(frontend).empty(),
             "grouped class values emit without default construction or copy");
    } catch (const std::exception &error) {
      expect(false, error.what());
    }
    bool exercised = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &identity : block.instructions) {
          if (exercised || identity.kind != lang::MirInstructionKind::Compute ||
              identity.operation != lang::MirOperation::Identity ||
              identity.info.type.kind != lang::SemanticType::Class ||
              !identity.result)
            continue;
          exercised = true;
          ++groupedBodies;
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                     .supportsFailureBodyText(address),
                 "grouped body preserves exact publication");
          const int mutationCount = identity.lifecycle.empty() ? 4 : 6;
          for (int mutation = 0; mutation != mutationCount; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks) {
              for (auto &instruction : changedBlock.instructions) {
                if (instruction.id != identity.id)
                  continue;
                if (mutation == 0)
                  instruction.operands.front().type =
                      lang::SemanticType::Unknown;
                else if (mutation == 1)
                  instruction.operands.front().value = *instruction.result;
                else if (mutation == 2)
                  instruction.operands.push_back(instruction.operands.front());
                else if (mutation == 3)
                  instruction.lifecycle.push_back(
                      {.kind = lang::MirLifecycleEventKind::Reparent,
                       .source = 999998,
                       .target = 999999});
                else if (mutation == 4)
                  instruction.lifecycle.clear();
                else
                  instruction.lifecycle.front().source =
                      instruction.lifecycle.front().target;
              }
            }
            (void)lang::rebuildMirValueUses(changed->body);
            expect(!lang::CppMirBodyEmitter(malformed, rows)
                        .supportsFailureBodyText(address),
                   "grouped publication rejects wrong types, cycles, extra "
                   "operands and forged or missing lifecycle transfers");
          }
        }
      }
    }
  }
  expect(groupedBodies == 6,
         "all grouped producer and conditional-move bodies were exercised");
  std::size_t argumentStages = 0;
  for (const std::string fixture :
       {"conditional_class_argument", "conditional_class_argument_failure"}) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / (fixture + ".gti");
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "conditional arguments have valid MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              !stage.preparedParameterDrop || !stage.destination ||
              stage.info.type.kind != lang::SemanticType::Class ||
              stage.operands.size() != 1 ||
              stage.operands.front().kind != lang::MirOperandKind::Value)
            continue;
          // Follow only exact identity records to select a moved temporary,
          // without depending on a library or user-defined class spelling.
          const auto *value =
              function.body.findValue(stage.operands.front().value);
          const lang::MirInstruction *move = nullptr;
          for (std::size_t depth = 0;
               value && depth < function.body.values.size(); ++depth) {
            const lang::MirInstruction *definition = nullptr;
            const auto *definitionBlock =
                function.body.findBlock(value->definitionBlock);
            if (definitionBlock)
              for (const auto &candidate : definitionBlock->instructions)
                if (candidate.id == value->definition)
                  definition = &candidate;
            if (!definition)
              break;
            if (definition->kind == lang::MirInstructionKind::Move) {
              move = definition;
              break;
            }
            if (definition->kind != lang::MirInstructionKind::Compute ||
                definition->operation != lang::MirOperation::Identity ||
                definition->operands.size() != 1 ||
                definition->operands.front().kind !=
                    lang::MirOperandKind::Value)
              break;
            value = function.body.findValue(definition->operands.front().value);
          }
          if (!move || move->operands.size() != 1)
            continue;
          const auto *place =
              function.body.findPlace(move->operands.front().place);
          if (!place || place->root != lang::MirPlaceRootKind::Temporary)
            continue;
          ++argumentStages;
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                     .supportsFailureBodyText(address),
                 "conditional argument body emits through prepared storage");
          for (int mutation = 0; mutation != 6; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &instruction : changedBlock.instructions) {
                if (instruction.id == stage.id) {
                  if (mutation == 0)
                    instruction.preparedParameterDrop.reset();
                  else if (mutation == 1)
                    instruction.lifecycle.clear();
                  else if (mutation == 2)
                    instruction.destination = place->id;
                  else if (mutation == 3)
                    instruction.operands.front().type =
                        lang::SemanticType::Unknown;
                  else if (mutation == 4)
                    instruction.lifecycle.front().conditional = true;
                }
                if (mutation == 5 && instruction.id == move->id)
                  instruction.operands.front().kind =
                      lang::MirOperandKind::Copy;
              }
            (void)lang::rebuildMirValueUses(changed->body);
            expect(!lang::CppMirBodyEmitter(malformed, rows)
                        .supportsFailureBodyText(address),
                   "conditional argument rejects mutation " +
                       std::to_string(mutation) + " at stage " +
                       std::to_string(stage.id) + " in " + fixture);
          }
        }
      }
    }
  }
  expect(argumentStages == 5, "four direct/grouped argument stages and the "
                              "later-failure stage were exercised");
  return failures ? 1 : 0;
}
