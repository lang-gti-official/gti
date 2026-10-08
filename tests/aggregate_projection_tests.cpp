#include "cpp_mir_body_emitter_test_fixture.h"

#include <regex>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::size_t copies = 0, moves = 0, addresses = 0;
  for (const auto name :
       {"array_field_projections", "class_array_decomposition",
        "owning_class_array_decomposition",
        "constructor_struct_field_address"}) {
    const auto path = root / "fixtures" / (std::string(name) + ".gti");
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "aggregate fixture has verified MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
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
          for (int mutation = 0; mutation != 3; ++mutation) {
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
                } else if (move) {
                  if (mutation == 0)
                    candidate.lifecycle.clear();
                  else if (mutation == 1)
                    candidate.lifecycle.front().target =
                        candidate.lifecycle.front().source;
                  else
                    candidate.ownership.reset();
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
            if (address || copy)
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "forged address bounds/access and array copy types fail "
                     "MIR verification: " +
                         std::to_string(mutation) +
                         (address ? " address" : " copy"));
            else
              expect(!lang::CppMirBodyEmitter(malformed, rows)
                          .supportsFailureBodyText(
                              {lang::MirBodyKind::Function, function.id}),
                     "owning array publication rejects missing move, transfer "
                     "and ownership facts");
          }
        }
    }
    try {
      const auto cpp = gti_test::emitCppText(frontend);
      expect(
          !std::regex_search(
              cpp,
              std::regex(
                  R"(std::array<::__gti_program::(?:Range|Text), 2> __gti_mir_v_[0-9]+\{\};)")),
          "decomposition does not default-construct a second array SSA object");
    } catch (const std::exception &error) {
      expect(false, error.what());
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
      const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
      std::string cpp;
      try {
        cpp = gti_test::emitCppText(frontend);
      } catch (const std::exception &error) {
        expect(false, error.what());
      }
      expect(
          !std::regex_search(
              cpp,
              std::regex(
                  R"(std::array<(?:std::array<)?::__gti_program::Value, 2>(?:, 2>)? __gti_mir_v_[0-9]+\{\};)")),
          "copied class-array arguments never value-initialize an array");
      expect(cpp.find("mir_lifetime_slot<std::array<::__gti_program::Value, "
                      "2>, true> __gti_mir_v_") != std::string::npos &&
                 cpp.find("mir_lifetime_slot<std::array<std::array<::__gti_"
                          "program::Value, 2>, 2>, true> __gti_mir_v_") !=
                     std::string::npos,
             "copied class-array arguments stage in uninitialized storage");
      expect(cpp.find(": values(__gti_mir_arg_0)") != std::string::npos,
             "a passive class-array parameter copies in the member "
             "initializer");
      std::size_t staged = 0, rejected = 0;
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
              const auto plan =
                  lang::CppMirBodyEmitter(malformed, rows)
                      .buildPlan({lang::MirBodyKind::Function, function.id});
              const auto &issues = plan.analysis().issues;
              rejected +=
                  !plan.admitted() &&
                  std::any_of(issues.begin(), issues.end(),
                              [](const auto &issue) {
                                return issue.detail.find(
                                           "invent element default "
                                           "construction") != std::string::npos;
                              });
            }
          }
      }
      expect(staged == 8 && rejected == 16,
             "every copied array argument without its exact read proof fails "
             "closed instead of value-initializing elements: " +
                 std::to_string(staged) + " staged, " +
                 std::to_string(rejected) + " rejected");
    }
  }
  expect(copies == 2 && moves == 1 && addresses == 2,
         "fixtures prove independent copying, owning movement and checked "
         "address formation");
  if (failures)
    return 1;
  std::cout
      << "aggregate projection MIR and representation mutation tests passed\n";
}
