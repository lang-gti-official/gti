#include "cpp_mir_body_emitter_test_fixture.h"

// C++ emitter planning for the operator-place composition fixture. The
// backend-neutral contract lives in operator_place_composition_tests.cpp.
int main() {
  using namespace gti_test::cpp_mir_body_emitter_fixture;
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/operator_place_composition.gti";
  std::ifstream stream(fixture);
  const std::string source((std::istreambuf_iterator<char>(stream)), {});
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "operator-place composition satisfies frontend verification");
  if (!frontend.canGenerateCode())
    return 1;
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    const auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(optimized.valid(), "optimization preserves exact update places");
    const auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      lang::CppMirBodyEmissionMap rows(
          lang::buildCppMirBodyEmissionMapRows(lowered, standard));
      lang::CppMirBodyEmitter emitter(optimized.mir, rows);
      for (const auto &function : optimized.mir.functionInstances()) {
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        expect(emitter.analyze(address).ready() &&
                   (emitter.supportsBodyText(address) ||
                    emitter.supportsFailureBodyText(address)),
               "production backend supports verified operator-place schedules");
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
