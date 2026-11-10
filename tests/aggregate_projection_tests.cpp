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

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::size_t copies = 0, moves = 0, addresses = 0;
  for (const auto name :
       {"array_field_projections", "class_array_decomposition",
        "owning_class_array_decomposition", "guarded_class_array_decomposition",
        "constructor_struct_field_address"}) {
    const auto path = root / "fixtures" / (std::string(name) + ".gti");
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "aggregate fixture has verified MIR");
    if (!frontend.canGenerateCode())
      continue;
    expectBackendAccepts(frontend,
                         "aggregate fixture compiles: " + std::string(name));
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        const auto index = std::find_if(
            place.projections.begin(), place.projections.end(),
            [](const auto &projection) {
              return projection.kind == lang::MirProjectionKind::Index &&
                     projection.index && projection.constantIndex;
            });
        if (index == place.projections.end())
          continue;
        const auto *indexValue = function.body.findValue(index->index);
        const auto *indexBlock =
            indexValue ? function.body.findBlock(indexValue->definitionBlock)
                       : nullptr;
        if (!indexBlock ||
            !std::any_of(
                indexBlock->instructions.begin(),
                indexBlock->instructions.end(), [&](const auto &definition) {
                  return definition.id == indexValue->definition &&
                         definition.operation == lang::MirOperation::Literal &&
                         definition.literal &&
                         std::get_if<std::uint64_t>(&*definition.literal);
                }))
          continue;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        changed->body.places[place.id - 1]
            .projections[static_cast<std::size_t>(index -
                                                  place.projections.begin())]
            .constantIndex = *index->constantIndex ^ 1;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "constant projection ordinal cannot contradict its literal "
               "index value");
      }
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          const bool copy =
              instruction.kind == lang::MirInstructionKind::Load &&
              instruction.info.type.kind == lang::SemanticType::Array;
          const bool move =
              instruction.kind == lang::MirInstructionKind::Move &&
              instruction.info.type.kind == lang::SemanticType::Array;
          const bool address =
              instruction.operation == lang::MirOperation::AddressOf &&
              !instruction.localFailureSites.empty();
          if (!copy && !move && !address)
            continue;
          copies += copy;
          moves += move;
          addresses += address;
          if (move) {
            // An owning array publication records the move, a transfer into
            // a distinct destination and the moved ownership facts.
            expect(!instruction.lifecycle.empty() &&
                       instruction.lifecycle.front().target !=
                           instruction.lifecycle.front().source &&
                       instruction.ownership.has_value(),
                   "owning array publication retains its move, transfer and "
                   "ownership facts");
            for (const auto &owner : frontend.mir.classInstances()) {
              if (instruction.info.type.arguments.size() != 1 ||
                  owner.type != instruction.info.type.arguments.front() ||
                  !owner.requiresActiveDropState)
                continue;
              expect(!lang::mirTypeMoveIsDefinedFailureFree(frontend.mir,
                                                            owner.type) &&
                         lang::mirTypeRelocationIsDefinedFailureFree(
                             frontend.mir, owner.type),
                     "guarded cleanup needs the exact vacant-destination "
                     "relocation proof, not passive move/assignment authority");
            }
          }
          if (address) {
            expect(function.body.loans.empty(),
                   "forming raw element addresses introduces no checked loans");
            expect(
                instruction.localFailureSites.size() == 1 &&
                    instruction.definedFailure.localOrigins.front().outcomes ==
                        std::vector<lang::DefinedFailureOutcome>{
                            {lang::DefinedFailureCode::IndexOutOfBounds,
                             lang::DefinedFailureDetail::FixedArray}},
                "address formation retains the semantic bounds origin");
          }
          // Forged owning-array move facts are backend admission's to
          // reject; cpp_mir_aggregate_projection_tests.cpp covers them.
          for (int mutation = 0; !move && mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (address) {
                  if (mutation == 0) {
                    candidate.definedFailure = {};
                    candidate.localFailureSites.clear();
                  } else if (mutation == 1) {
                    candidate.definedFailure.localOrigins.front()
                        .outcomes.front()
                        .detail = lang::DefinedFailureDetail::StringView;
                  } else {
                    candidate.info.type.pointerAccess =
                        lang::AccessMode::ReadOnly;
                  }
                } else {
                  if (mutation == 0)
                    candidate.operands.front().kind =
                        lang::MirOperandKind::Move;
                  else if (mutation == 1)
                    candidate.info.type.arrayLength += 1;
                  else
                    candidate.operands.front().type = lang::SemanticType::Int32;
                }
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "forged address bounds/access and array copy types fail "
                   "MIR verification: " +
                       std::to_string(mutation) +
                       (address ? " address" : " copy"));
          }
        }
    }
  }
  {
    // Copied passive class-array arguments and parameter-to-field copies use
    // exact uninitialized staging; a Value element has no default constructor.
    const auto path = root / "fixtures" / "class_array_arguments.gti";
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "class array argument fixture has MIR");
    if (frontend.canGenerateCode()) {
      expectBackendAccepts(frontend, "copied class-array arguments compile");
      std::size_t staged = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        for (const auto &block : function.body.blocks)
          for (std::size_t index = 0; index + 1 < block.instructions.size();
               ++index) {
            const auto &load = block.instructions[index];
            const auto &stage = block.instructions[index + 1];
            if (load.kind != lang::MirInstructionKind::Load ||
                load.info.type.kind != lang::SemanticType::Array ||
                stage.kind != lang::MirInstructionKind::CallInput)
              continue;
            ++staged;
            for (int mutation = 0; mutation != 2; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              for (auto &candidateBlock : changed->body.blocks)
                for (auto &candidate : candidateBlock.instructions) {
                  if (candidate.id == load.id && mutation == 0 &&
                      candidate.ownership)
                    candidate.ownership->reachable = false;
                  if (candidate.id == load.id && mutation == 1)
                    candidate.ownership.reset();
                }
              // Ownership facts are emitter evidence, not MIR validity.
              expect(lang::verifyMirProgram(malformed).valid(),
                     "read-fact mutation remains verified MIR");
            }
          }
      }
      expect(staged == 8, "every copied array argument stages its exact "
                          "array read: " +
                              std::to_string(staged) + " staged");
    }
  }
  expect(copies == 2 && moves == 2 && addresses == 2,
         "fixtures prove independent copying, owning movement and checked "
         "address formation");
  {
    const auto path = root / "fixtures" / "checked_class_copy_arguments.gti";
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "checked class copies have verified MIR");
    if (frontend.canGenerateCode()) {
      std::size_t checkedCopies = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        const auto dominance = lang::computeMirDominance(function.body);
        for (const auto &block : function.body.blocks)
          for (const auto &stage : block.instructions) {
            if (stage.kind != lang::MirInstructionKind::CallInput ||
                stage.callInputKind != lang::HirCallInputKind::CopyValue ||
                !stage.preparedParameterDrop || stage.operands.size() != 1)
              continue;
            const auto *place =
                function.body.findPlace(stage.operands[0].place);
            if (!place ||
                std::none_of(place->projections.begin(),
                             place->projections.end(), [](const auto &part) {
                               return part.kind ==
                                      lang::MirProjectionKind::Index;
                             }))
              continue;
            ++checkedCopies;
            const lang::MirInstruction *check = nullptr;
            const lang::MirBlock *checkBlock = nullptr;
            for (const auto &candidateBlock : function.body.blocks)
              for (const auto &candidate : candidateBlock.instructions)
                if (candidate.operation == lang::MirOperation::AddressOf &&
                    candidate.hirValue == stage.hirValue &&
                    candidate.operands.size() == 1 &&
                    candidate.operands[0].place == place->id) {
                  check = &candidate;
                  checkBlock = &candidateBlock;
                }
            expect(check && checkBlock && dominance &&
                       checkBlock->terminator.kind ==
                           lang::MirTerminatorKind::Invoke &&
                       checkBlock->terminator.invokeInstruction == check->id &&
                       dominance->dominates(checkBlock->terminator.target,
                                            block.id),
                   "exact indexed-source check succeeds before prepared copy");
            expect(stage.definedFailure.empty() &&
                       stage.localFailureSites.empty(),
                   "prepared copy does not duplicate its completed selection "
                   "failure");
            if (!check || !checkBlock)
              continue;
            expect(
                check->definedFailure.localOrigins.size() == 1 &&
                    check->localFailureSites.size() == 1 &&
                    frontend.mir.failureMetadata().siteFor(
                        check->definedFailure.localOrigins[0]) ==
                        check->localFailureSites[0],
                "preflight retains the exact source failure-site assignment");
            lang::FailureSiteId otherSelectionSite = 0;
            for (const auto &otherBlock : function.body.blocks)
              for (const auto &other : otherBlock.instructions)
                if (other.operation == lang::MirOperation::AddressOf &&
                    other.localFailureSites.size() == 1 &&
                    other.localFailureSites != check->localFailureSites)
                  otherSelectionSite = other.localFailureSites.front();
            expect(otherSelectionSite != 0,
                   "regression has another valid bounds site to forge");
            for (int mutation = 0; mutation < 5; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              for (auto &changedBlock : changed->body.blocks)
                for (auto &instruction : changedBlock.instructions) {
                  if (instruction.id == check->id) {
                    if (mutation == 0) {
                      instruction.definedFailure = {};
                      instruction.localFailureSites.clear();
                    } else if (mutation == 1) {
                      instruction.definedFailure.localOrigins[0]
                          .outcomes[0]
                          .detail = lang::DefinedFailureDetail::StringView;
                    } else if (mutation == 2) {
                      instruction.localFailureSites[0] = otherSelectionSite;
                    }
                  }
                  if (instruction.id == stage.id && mutation == 3)
                    instruction.lifecycle.clear();
                  if (instruction.id == stage.id && mutation == 4)
                    instruction.preparedParameterDrop.reset();
                }
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "checked copy rejects forged selection or publication: " +
                         std::to_string(mutation));
            }
          }
      }
      expect(checkedCopies == 2,
             "local and field array copies retain exact preflights");
      expectBackendAccepts(frontend, "checked prepared copies compile");
    }
  }
  if (failures)
    return 1;
  std::cout
      << "aggregate projection MIR and representation mutation tests passed\n";
}
