#include "cpp_mir_body_emitter_test_fixture.h"

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

  std::size_t contracts = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.pureVirtual || lang::cppMirVirtualFailureContractRoot(
                                     frontend.mir, function) != &function)
      continue;
    ++contracts;
    expect(std::none_of(frontend.mir.functionInstances().begin(),
                        frontend.mir.functionInstances().end(),
                        [&](const lang::MirFunctionInstance &candidate) {
                          return candidate.id != function.id &&
                                 lang::cppMirVirtualFailureContractRoot(
                                     frontend.mir, candidate) == &function;
                        }),
           "regression must retain the contract without a concrete "
           "implementation");
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      if (mutation == 0)
        changed->virtualRoots.clear();
      else if (mutation == 1)
        changed->mayRaiseDefinedFailure = false;
      else
        changed->definitionKind = lang::MirDefinitionKind::Source;
      expect(lang::cppMirVirtualFailureContractRoot(malformed, *changed) ==
                 nullptr,
             "abstract ABI retention requires the exact verified virtual "
             "failure root");
    }
  }
  expect(contracts == 2,
         "fixture must select read-only and mutable abstract contracts");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const std::string text = gti_test::emitCppText(frontend, standard);
    const auto read = text.find("virtual bool probe_read__gti_mir_failure(");
    const auto mut = text.find("virtual bool probe_mut__gti_mir_failure(");
    expect(read != std::string::npos && mut != std::string::npos,
           "both abstract generic primaries must declare the status ABI");
    if (read != std::string::npos)
      expect(text.substr(read, text.find('\n', read) - read)
                 .ends_with("const = 0;"),
             "read contract must remain const and pure virtual");
    if (mut != std::string::npos)
      expect(text.substr(mut, text.find('\n', mut) - mut).ends_with(") = 0;"),
             "mutable contract must remain non-const and pure virtual");
  }
  if (failures != 0)
    return 1;
  std::cout << "abstract generic virtual failure ABI tests passed\n";
  return 0;
}
