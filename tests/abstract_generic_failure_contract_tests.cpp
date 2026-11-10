#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

namespace {

// An abstract contract root: a pure virtual, defined-failure Gti method that
// roots its own virtual family in a polymorphic abstract class.
bool abstractFailureContract(const lang::MirProgram &program,
                             const lang::MirFunctionInstance &function) {
  if (!function.owner || !function.virtualMethod || !function.pureVirtual ||
      function.overrideMethod || function.staticMember ||
      !function.mayRaiseDefinedFailure ||
      function.linkage != lang::LanguageLinkage::Gti ||
      function.definitionKind != lang::MirDefinitionKind::Declaration ||
      std::find(function.virtualRoots.begin(), function.virtualRoots.end(),
                function.declaration) == function.virtualRoots.end())
    return false;
  const lang::MirClassInstance *owner =
      program.findClassInstance(*function.owner);
  return owner != nullptr && owner->polymorphic && owner->abstract;
}

} // namespace

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/abstract_generic_failure_contract.gti";
  std::ifstream input(path);
  expect(input.good(), "abstract generic failure fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "abstract-only generic virtual calls must verify");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "abstract-only generic virtual calls must produce verified MIR");

  std::size_t contracts = 0;
  bool readContract = false;
  bool mutableContract = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!abstractFailureContract(frontend.mir, function))
      continue;
    ++contracts;
    if (function.receiverMutability == lang::ReceiverMutability::ReadOnly)
      readContract = true;
    else
      mutableContract = true;
    expect(std::none_of(frontend.mir.functionInstances().begin(),
                        frontend.mir.functionInstances().end(),
                        [&](const lang::MirFunctionInstance &candidate) {
                          return candidate.id != function.id &&
                                 candidate.overrideMethod &&
                                 std::find(candidate.virtualRoots.begin(),
                                           candidate.virtualRoots.end(),
                                           function.declaration) !=
                                     candidate.virtualRoots.end();
                        }),
           "regression must retain the contract without a concrete "
           "implementation");
  }
  expect(contracts == 2,
         "fixture must select read-only and mutable abstract contracts");
  expect(readContract && mutableContract,
         "read contract must remain read-only and mutable contract must "
         "remain mutable");
  std::string error;
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         "abstract generic primaries must reach native emission: " + error);
  if (failures != 0)
    return 1;
  std::cout << "abstract generic virtual failure ABI tests passed\n";
  return 0;
}
