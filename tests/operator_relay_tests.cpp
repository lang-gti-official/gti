#include "../src/compiler/mir_verification_queries.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
using namespace gti_test::cpp_mir_body_emitter_fixture;

std::string readFixture(std::string_view name) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    (std::string(name) + ".gti");
  std::ifstream stream(path);
  return {(std::istreambuf_iterator<char>(stream)), {}};
}

void checkBackend(const lang::FrontendResult &frontend) {
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "optimizer retains exact field, view and allocated-value authority");
    auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    expect(lang::verifyLoweredProgram(lowered).empty(),
           "lowered snapshot is coherent");
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      lang::CppMirBodyEmissionMap rows(
          lang::buildCppMirBodyEmissionMapRows(lowered, standard));
      lang::CppMirBodyEmitter emitter(optimized.mir, rows);
      for (const auto &function : optimized.mir.functionInstances()) {
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        expect(emitter.analyze(address).ready() &&
                   (emitter.supportsBodyText(address) ||
                    emitter.supportsFailureBodyText(address)),
               "both production standard modes admit each exact function "
               "schedule");
      }
    }
  }
}

void checkAllocationMutations(const lang::FrontendResult &frontend) {
  std::size_t count = 0;
  bool copy = false, move = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!lang::mir_detail::generatedUniqueConstruction(instruction))
          continue;
        ++count;
        copy |= instruction.constructorKind == lang::ConstructorKind::Copy;
        move |= instruction.constructorKind == lang::ConstructorKind::Move;
        expect(instruction.objectAllocation != 0 &&
                   instruction.definedFailure.empty(),
               "generated construction follows a distinct checked allocation");
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = &const_cast<lang::MirFunctionInstance *>(
                            malformed.findFunctionInstance(function.id))
                            ->body;
          auto *changed = body->findBlock(block.id);
          auto producer = std::find_if(changed->instructions.begin(),
                                       changed->instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          const auto *allocation =
              body->findDropObligation(instruction.objectAllocation);
          if (mutation == 0) {
            producer->objectAllocation = 0;
          } else if (mutation == 1) {
            producer->constructorKind = lang::ConstructorKind::Ordinary;
          } else if (mutation == 2) {
            producer->parameterTypes.front() = lang::SemanticType::Int32;
          } else if (mutation == 3) {
            producer->operands.front().kind = lang::MirOperandKind::BorrowRead;
            producer->operands.front().place = allocation->place;
          } else if (mutation == 4) {
            for (auto &candidate : body->blocks)
              std::erase_if(candidate.instructions, [&](const auto &operation) {
                return operation.kind == lang::MirInstructionKind::Lifecycle &&
                       operation.objectAllocation ==
                           instruction.objectAllocation;
              });
          } else if (mutation == 5) {
            for (auto &candidate : body->blocks)
              if (candidate.terminator.kind ==
                      lang::MirTerminatorKind::Invoke &&
                  candidate.terminator.target == block.id)
                candidate.terminator = {.kind = lang::MirTerminatorKind::Goto,
                                        .target = block.id};
          } else {
            auto owner = std::find_if(
                malformed.classInstances().begin(),
                malformed.classInstances().end(), [&](const auto &candidate) {
                  return candidate.type ==
                         instruction.info.type.arguments.front();
                });
            auto &changedOwner = const_cast<lang::MirClassInstance &>(*owner);
            if (instruction.constructorKind == lang::ConstructorKind::Copy)
              changedOwner.copyConstructor = lang::SpecialMemberStatus::Deleted;
            else
              changedOwner.moveConstructor = lang::SpecialMemberStatus::Deleted;
          }
          lang::rebuildMirReachability(*body);
          (void)lang::rebuildMirValueUses(*body);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "allocation rejects forged operation, input, policy, storage "
                 "escape or publication");
        }
      }
  expect(count >= 4 && copy && move,
         "unrelated passive/owning types exercise generated copy and move");
}

void checkFieldMutations(const lang::FrontendResult &frontend) {
  std::size_t count = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.body.rangeIterations.empty())
      continue;
    for (const auto &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          place.projections.size() < 2 ||
          place.projections.front().kind !=
              lang::MirProjectionKind::Dereference ||
          place.projections.back().kind != lang::MirProjectionKind::Field)
        continue;
      ++count;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          malformed.findFunctionInstance(function.id))
                          ->body;
        auto &changed = body->places[place.id - 1];
        if (mutation == 0)
          changed.binding = 0;
        else if (mutation == 1)
          changed.projections.back().field = 0;
        else if (mutation == 2)
          changed.projections.erase(changed.projections.begin());
        else
          changed.type = lang::SemanticType::Int32;
        (void)lang::rebuildMirValueUses(*body);
        bool rejected = !lang::verifyMirProgram(malformed).valid();
        if (!rejected) {
          const auto lowered = gti_test::lowerProgram(
              frontend, malformed, malformed, lang::TargetInfo::host());
          lang::CppMirBodyEmissionMap rows(
              lang::buildCppMirBodyEmissionMapRows(lowered,
                                                   lang::CppStandard::Cpp20));
          lang::CppMirBodyEmitter emitter(malformed, rows);
          rejected = !emitter
                          .analyze({.kind = lang::MirBodyKind::Function,
                                    .owner = function.id})
                          .ready();
        }
        expect(rejected, "wrong range owner, field, indirection or referent "
                         "type fails closed");
      }
    }
  }
  expect(count >= 2,
         "range fields retain explicit typed reference dereference paths");
}

void checkNegative(const std::string &prefix, const std::string &extra) {
  auto result = analyzeWithStandardLibrary(
      "operator-relay-negative.gti", prefix + extra + "int main(){return 0;}");
  expect(!result.canGenerateCode() &&
             std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.phase ==
                                      lang::DiagnosticPhase::Semantics &&
                                  diagnostic.severity ==
                                      lang::DiagnosticSeverity::Error;
                         }),
         "invalid owner relay is rejected by semantics");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "invalid source never reaches an internal MIR/backend failure");
}
} // namespace

int main() {
  auto range = analyzeWithStandardLibrary("range_field_access.gti",
                                          readFixture("range_field_access"));
  auto text = analyzeWithStandardLibrary("indexed_view_relay.gti",
                                         readFixture("indexed_view_relay"));
  auto allocation =
      analyzeWithStandardLibrary("existing_value_allocation.gti",
                                 readFixture("existing_value_allocation"));
  for (const auto *frontend : {&range, &text, &allocation}) {
    expect(frontend->canGenerateCode() &&
               lang::verifyMirProgram(frontend->mir).valid(),
           "positive fixture completes all compiler phases");
    if (!frontend->canGenerateCode())
      return 1;
    checkBackend(*frontend);
  }
  checkFieldMutations(range);
  checkAllocationMutations(allocation);
  const auto source = readFixture("indexed_view_relay");
  const auto prefix = source.substr(0, source.find("int main()"));
  checkNegative(prefix,
                "std::string_view bad(){Records local{}; return "
                "local.at(std::size_t(0));}");
  checkNegative(prefix,
                "std::string_view bad(){return Records().at(std::size_t(0));}");
  checkNegative(prefix,
                "[[borrowed_from(first)]] Record& bad(Records& first, "
                "Records& second){return second[std::size_t(0)];}");
  checkNegative(prefix,
                "void bad(){mut Records records{}; std::string_view "
                "held=records.at(std::size_t(0)); records.add(); "
                "std::println(held);}");
  return failures == 0 ? 0 : 1;
}
