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
    for (const auto &function : frontend.mir.functionInstances())
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
  expect(copies == 1 && moves == 1 && addresses == 2,
         "fixtures prove independent copying, owning movement and checked "
         "address formation");
  if (failures)
    return 1;
  std::cout
      << "aggregate projection MIR and representation mutation tests passed\n";
}
