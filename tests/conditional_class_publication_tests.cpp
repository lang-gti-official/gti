#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

#include "gti/hir.h"
#include "gti/mir_editor.h"

namespace {

// The shipped backend accepts the verified MIR program.
void expectBackendAccepts(const lang::FrontendResult &frontend,
                          const std::string &message) {
  std::string error;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         message + " (MIR verifies)");
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         message + (error.empty() ? "" : ": " + error));
}

lang::FrontendResult analyzeFixture(const std::string &fixture) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    (fixture + ".gti");
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  return analyzeWithStandardLibrary(path.string(), source);
}

} // namespace

int main() {
  {
    const auto frontend = analyze("conditional-native-input.gti", R"gti(
[[c_abi]] union Data { uint64_t number; uint8_t bytes[16]; };
extern "C" { Data obtain(); }
struct Wrapper {
  Data data;
  unsafe Wrapper(Data input) : data(input) {}
  uint64_t number() { unsafe { return this.data.number; } }
};
int main() {
  Data data = obtain();
  unsafe {
    return (true ? Wrapper(data) : Wrapper(data)).number() == uint64_t(42) ? 0 : 1;
  }
}
)gti");
    expect(frontend.canGenerateCode(),
           "conditional native inputs have valid MIR");
    if (frontend.canGenerateCode()) {
      expectBackendAccepts(
          frontend,
          "prepared constructor inputs preserve conditional publication");
      std::size_t checked = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            if (instruction.kind != lang::MirInstructionKind::Construct ||
                instruction.lifecycle.size() != 1 ||
                instruction.lifecycle.front().kind !=
                    lang::MirLifecycleEventKind::TransferOut)
              continue;
            ++checked;
            for (int mutation = 0; mutation < 4; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              for (auto &changedBlock : changed->body.blocks) {
                for (auto &item : changedBlock.instructions) {
                  if (item.id == instruction.id) {
                    if (mutation == 0)
                      item.lifecycle.clear();
                    if (mutation == 1)
                      item.lifecycle.front().source = 999999;
                    if (mutation == 2)
                      item.lifecycle.push_back(item.lifecycle.front());
                  }
                  if (mutation == 3 &&
                      item.kind == lang::MirInstructionKind::CallInput &&
                      item.callSite == instruction.callSite)
                    item.callSite = 999999;
                }
              }
              expect(
                  !lang::verifyMirProgram(malformed).valid(),
                  "constructor publication rejects missing, wrong, duplicate "
                  "and unrelated prepared-input transfers");
            }
          }
        }
      }
      expect(checked == 2, "both constructor arms carry prepared transfers");
    }
  }
  std::size_t joins = 0;
  for (const std::string fixture :
       {"conditional_class_passive", "conditional_class_cleanup",
        "conditional_class_passive_failure"}) {
    const auto frontend = analyzeFixture(fixture);
    expect(frontend.canGenerateCode(),
           "conditional class fixture has valid MIR");
    if (!frontend.canGenerateCode())
      continue;
    expectBackendAccepts(frontend, "both passive and active conditional "
                                   "bindings compile through verified MIR");
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
        // The passive diamond publishes the binding without inventing a
        // cleanup obligation for the moved-from shell.
        expect(!slot.initiallyAvailable && !slot.traits.containsBorrowedState,
               "the passive diamond slot starts unavailable and unborrowed");
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
      }
    }
  }
  expect(joins == 3,
         "nonraising and raising passive class diamonds were exercised");
  {
    // A parenthesized lambda is likewise a non-executing HIR source alias:
    // no Grouping value of lambda type exists, and the wrapper resolves to
    // the exact closure, binding or argument value it groups.
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / "grouped_lambda_values.gti";
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "grouped lambda fixture has valid MIR");
    std::size_t lambdaAliases = 0;
    for (const auto &function : frontend.hir.functionInstances()) {
      if (function.source == nullptr ||
          function.source->name().source != path.string())
        continue;
      expect(std::none_of(
                 function.body.values.begin(), function.body.values.end(),
                 [](const auto &value) {
                   return value.kind == lang::HirValueKind::Grouping &&
                          value.info.type.kind == lang::SemanticType::Lambda;
                 }),
             "a parenthesized lambda adds no Grouping value");
      for (const auto &alias : function.body.sourceAliases) {
        const auto *group = lang::exprAs<lang::Grouping>(alias.source);
        if (group == nullptr)
          continue;
        ++lambdaAliases;
        const auto *producer = function.body.findValue(alias.value);
        expect(producer != nullptr &&
                   producer->info.type.kind == lang::SemanticType::Lambda &&
                   function.body.valueForSource(*alias.source) == producer &&
                   function.body.valueForSource(*group->expression()) ==
                       producer,
               "a grouped lambda resolves to its operand's exact value");
      }
    }
    expect(lambdaAliases >= 6,
           "binding, nested, owning, argument, copy and callee groupings "
           "are all exercised");
    if (frontend.canGenerateCode()) {
      expectBackendAccepts(frontend, "grouped lambdas emit from verified MIR");
    }
  }
  std::size_t groupedFixtures = 0;
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
    expectBackendAccepts(
        frontend,
        "grouped class values compile without default construction or copy");
    bool exercised = false;
    // Class grouping is now a non-executing HIR source alias. Requiring an
    // Identity instruction would test the obsolete grouping representation.
    for (const auto &function : frontend.hir.functionInstances()) {
      if (function.source == nullptr ||
          function.source->name().source != path.string())
        continue;
      const auto *mir = frontend.mir.findFunctionInstance(function.id);
      for (std::size_t index = 0;
           index < function.body.directInitializations.size(); ++index) {
        const auto &alias = function.body.directInitializations[index];
        const auto *group = lang::exprAs<lang::Grouping>(alias.source);
        if (group == nullptr)
          continue;
        exercised = true;
        const auto *producer = function.body.findValue(alias.value);
        expect(producer != nullptr &&
                   function.body.valueForSource(*group->expression()) ==
                       producer &&
                   function.body.valueForSource(*alias.source) == producer &&
                   producer->info.type == alias.type &&
                   !alias.constructionProof &&
                   std::none_of(
                       function.body.values.begin(), function.body.values.end(),
                       [&](const auto &v) { return v.source == alias.source; }),
               "grouping shares the exact producer, not another executable "
               "value or constructor");
        expect(std::count_if(function.body.dropObligations.begin(),
                             function.body.dropObligations.end(),
                             [&](const auto &drop) {
                               return drop.kind ==
                                          lang::HirDropObligationKind::Value &&
                                      drop.value == alias.value;
                             }) <= 1,
               "nested grouping cannot duplicate its producer's value drop");
        std::size_t producers = 0;
        if (mir != nullptr)
          for (const auto &block : mir->body.blocks)
            for (const auto &instruction : block.instructions) {
              if (instruction.hirValue != alias.value || !instruction.result ||
                  instruction.info.type != alias.type)
                continue;
              producers +=
                  instruction.kind == lang::MirInstructionKind::Call ||
                  instruction.kind == lang::MirInstructionKind::Construct ||
                  instruction.kind == lang::MirInstructionKind::Move;
              expect(instruction.kind != lang::MirInstructionKind::Compute ||
                         instruction.operation != lang::MirOperation::Identity,
                     "grouping does not add a MIR class identity lifetime");
            }
        expect(producers == 1,
               "the grouped constructor, call or conditional move executes "
               "once in MIR");
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto malformed = frontend.hir;
          auto &body = const_cast<lang::HirFunctionInstance *>(
                           malformed.findFunctionInstance(function.id))
                           ->body;
          auto &changed = body.directInitializations[index];
          if (mutation == 0)
            changed.type = lang::SemanticType::Unknown;
          else if (mutation == 1)
            changed.producer = nullptr;
          else if (mutation == 2)
            changed.value = 0;
          else if (mutation == 3)
            std::erase_if(body.values,
                          [&](const auto &v) { return v.id == alias.value; });
          else if (mutation == 4)
            body.directInitializations.push_back(changed);
          else if (mutation == 5)
            changed.source = changed.producer;
          else if (producer != nullptr) {
            auto extra = *producer;
            extra.id = 1;
            for (const auto &value : body.values)
              extra.id = std::max(extra.id, value.id + 1);
            extra.source = alias.source;
            body.values.push_back(std::move(extra));
          }
          const auto verified =
              lang::verifyHirProgramPlans(frontend.semantics, malformed);
          expect(std::any_of(verified.errors.begin(), verified.errors.end(),
                             [](const auto &error) {
                               return error.find("object result alias") !=
                                      std::string::npos;
                             }),
                 "grouping rejects wrong type, missing source/value, erased "
                 "producer, duplicate/retargeted alias or extra execution");
        }
      }
    }
    expect(exercised,
           "each fixture exercises its own grouping aliases: " + fixture);
    groupedFixtures += exercised;
  }
  expect(
      groupedFixtures == 6,
      "all six grouped producer and conditional-move fixtures were exercised");
  std::size_t argumentStages = 0;
  for (const std::string fixture :
       {"conditional_class_argument", "conditional_class_argument_failure"}) {
    const auto frontend = analyzeFixture(fixture);
    expect(frontend.canGenerateCode(), "conditional arguments have valid MIR");
    if (!frontend.canGenerateCode())
      continue;
    expectBackendAccepts(frontend,
                         "conditional arguments compile through prepared "
                         "storage: " +
                             fixture);
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
          // The prepared parameter stage owns a distinct destination and
          // records the ownership transfer of the moved temporary.
          expect(*stage.destination != place->id && !stage.lifecycle.empty() &&
                     move->operands.front().kind == lang::MirOperandKind::Move,
                 "conditional argument stages a moved temporary into "
                 "distinct prepared storage in " +
                     fixture);
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
  {
    const auto frontend = analyzeFixture("consuming_conditional_receiver");
    expect(frontend.canGenerateCode(),
           "conditional moved receivers have valid MIR");
    if (frontend.canGenerateCode())
      expectBackendAccepts(
          frontend, "a body consuming conditionally moved receivers compiles");
    for (const auto &function : frontend.mir.functionInstances()) {
      if (!frontend.canGenerateCode())
        break;
      const auto instruction = [&](lang::MirInstructionId id) {
        const lang::MirInstruction *found = nullptr;
        for (const auto &block : function.body.blocks)
          for (const auto &candidate : block.instructions)
            if (candidate.id == id)
              found = &candidate;
        return found;
      };
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
          // The hidden temporary starts unavailable; the Move constructs it,
          // and its conditional drop obligation is not active on entry.
          const lang::MirDropObligation *obligation = nullptr;
          for (const auto &drop : function.body.dropObligations)
            if (drop.place == slot->id)
              obligation = &drop;
          expect(!slot->initiallyAvailable &&
                     (obligation == nullptr || !obligation->initiallyActive),
                 "a conditionally moved receiver's hidden temporary is "
                 "constructed by its Move, not available on entry (move " +
                     std::to_string(move.id) + ")");
        }
      }
    }
  }
  // Both::pick consumes its two fields in place: their cleanup stays with
  // the outer receiver. They are not hidden moved temporaries. The other
  // 27 receivers still require every staging check above.
  expect(movedReceivers == 27,
         "every conditionally moved receiver in the fixture was exercised: " +
             std::to_string(movedReceivers));
  return failures ? 1 : 0;
}
