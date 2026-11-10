// C++ MIR body emitter admission and text for conditional class
// publication, grouped producers, conditional arguments and conditionally
// moved receivers. The backend-neutral MIR/HIR facts for the same fixtures
// live in conditional_class_publication_tests.cpp.
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
  expect(argumentStages == 7,
         "six direct/grouped/member argument stages and the "
         "later-failure stage were exercised");
  // A receiver moved out of a binding inside a conditional arm or
  // short-circuit operand stages in the hidden temporary that carries its
  // conditional full-expression drop through the merge. Select each one by
  // its identity records (a Move into a Temporary destination staged as a
  // MoveValue receiver), not by a class or member spelling.
  std::size_t movedReceivers = 0;
  bool mutatedClass = false;
  bool mutatedPlain = false;
  bool mutatedClosure = false;
  {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / "consuming_conditional_receiver.gti";
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "conditional moved receivers have valid MIR");
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    for (const auto &function : frontend.mir.functionInstances()) {
      if (!frontend.canGenerateCode())
        break;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const bool failureForm = function.mayRaiseDefinedFailure;
      const auto supports = [&](const lang::MirProgram &program) {
        const lang::CppMirBodyEmitter emitter(program, rows);
        return failureForm ? emitter.supportsFailureBodyText(address)
                           : emitter.supportsBodyText(address);
      };
      const auto instruction = [&](lang::MirInstructionId id) {
        const lang::MirInstruction *found = nullptr;
        for (const auto &block : function.body.blocks)
          for (const auto &candidate : block.instructions)
            if (candidate.id == id)
              found = &candidate;
        return found;
      };
      // Admission and text are per body; emit them once for its receivers.
      std::optional<lang::CppMirBodyEmissionText> text;
      for (const auto &block : function.body.blocks) {
        for (const auto &move : block.instructions) {
          const auto *slot = move.destination
                                 ? function.body.findPlace(*move.destination)
                                 : nullptr;
          if (move.kind != lang::MirInstructionKind::Move || !move.result ||
              slot == nullptr ||
              slot->root != lang::MirPlaceRootKind::Temporary ||
              !slot->projections.empty() || move.operands.size() != 1 ||
              move.operands.front().kind != lang::MirOperandKind::Move)
            continue;
          const auto &uses = function.body.usesOf(*move.result);
          const auto *stage =
              uses.size() == 1 && uses.front().kind ==
                                      lang::MirValueUseKind::InstructionOperand
                  ? instruction(uses.front().instruction)
                  : nullptr;
          if (stage == nullptr ||
              stage->kind != lang::MirInstructionKind::CallInput ||
              stage->callInputKind != lang::HirCallInputKind::MoveValue ||
              stage->callInputRole != lang::MirCallInputRole::Receiver ||
              !stage->result)
            continue;
          const auto &calls = function.body.usesOf(*stage->result);
          const auto *call =
              calls.size() == 1 &&
                      calls.front().kind ==
                          lang::MirValueUseKind::InstructionReceiver
                  ? instruction(calls.front().instruction)
                  : nullptr;
          if (call == nullptr || !call->callableInvocation)
            continue;
          ++movedReceivers;
          if (!text) {
            expect(supports(frontend.mir),
                   "a body consuming conditionally moved receivers emits");
            const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
            text =
                failureForm
                    ? emitter.emitFailureBodyText(address, "moved-receiver", 1)
                    : emitter.emitBodyText(address, "moved-receiver", 1);
          }
          const std::string slotName =
              "__gti_mir_p_" + std::to_string(slot->id);
          // A selected read or mutable fallback invokes the same slot as an
          // lvalue; a consuming member moves from it.
          expect(text->emitted() &&
                     text->text.find(slotName + ".construct(std::move(") !=
                         std::string::npos &&
                     text->text.find(slotName + ".get()") != std::string::npos,
                 "the Move constructs the hidden temporary and the call "
                 "invokes that slot");
          // Forge the first moved class in each body form and the first
          // moved closure; every forged program copies the whole fixture.
          bool &mutatedKind = slot->type.kind == lang::SemanticType::Lambda
                                  ? mutatedClosure
                              : failureForm ? mutatedClass
                                            : mutatedPlain;
          if (mutatedKind)
            continue;
          mutatedKind = true;
          for (int mutation = 0; mutation != 7; ++mutation) {
            auto malformed = frontend.mir;
            auto &body = const_cast<lang::MirFunctionInstance *>(
                             malformed.findFunctionInstance(function.id))
                             ->body;
            auto *changedSlot =
                const_cast<lang::MirPlace *>(body.findPlace(slot->id));
            lang::MirDropObligation *obligation = nullptr;
            for (auto &drop : body.dropObligations)
              if (drop.place == slot->id)
                obligation = &drop;
            bool applied = false;
            if (mutation == 0) {
              changedSlot->initiallyAvailable = true;
              applied = true;
            } else if (mutation == 1 && obligation != nullptr) {
              obligation->initiallyActive = true;
              applied = true;
            } else if (mutation == 2 && obligation != nullptr) {
              obligation->value = 0;
              applied = true;
            }
            for (auto &changedBlock : body.blocks)
              for (auto &changedInstruction : changedBlock.instructions) {
                if (mutation == 3 && changedInstruction.id == move.id &&
                    changedInstruction.lifecycle.size() == 1) {
                  changedInstruction.lifecycle.front().kind =
                      lang::MirLifecycleEventKind::Reparent;
                  applied = true;
                }
                // The invocation also reads the hidden temporary as a place.
                if (mutation == 4 && changedInstruction.id == call->id &&
                    changedInstruction.receiver) {
                  changedInstruction.receiver->place = slot->id;
                  applied = true;
                }
                // A second instruction writes the hidden temporary.
                if (mutation == 5 && !applied &&
                    changedInstruction.kind ==
                        lang::MirInstructionKind::Initialize &&
                    changedInstruction.destination != slot->id) {
                  changedInstruction.destination = slot->id;
                  applied = true;
                }
                // No drop of the receiver remains on the ordinary path.
                if (mutation == 6 &&
                    changedInstruction.kind == lang::MirInstructionKind::Drop &&
                    changedInstruction.destination == slot->id) {
                  for (auto &event : changedInstruction.lifecycle)
                    event.failureCleanup = true;
                  applied = true;
                }
              }
            expect(applied, "each moved-receiver mutation finds its target");
            (void)lang::rebuildMirValueUses(body);
            expect(!supports(malformed),
                   "a conditionally moved receiver rejects forged mutation " +
                       std::to_string(mutation) + " of move " +
                       std::to_string(move.id));
          }
        }
      }
    }
  }
  // Both::pick consumes its two fields in place: their cleanup stays with
  // the outer receiver. They are not hidden moved temporaries. The other
  // 27 receivers still require every staging and forged-mutation check above.
  expect(movedReceivers == 27,
         "every conditionally moved receiver in the fixture was exercised: " +
             std::to_string(movedReceivers));
  expect(mutatedClass && mutatedPlain && mutatedClosure,
         "forged mutations covered moved classes in both body forms and a "
         "moved closure");
  return failures ? 1 : 0;
}
