// A reconstructed alias must retain both its resolved type and its visibility.
#include "gti/compiled_interface_types.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "unit_test.h"
#include <algorithm>
#include <stdexcept>

using namespace lang;
namespace {
LoweredProgram lowerAlias(std::string declaration) {
  const auto source = std::filesystem::current_path() / "alias-contract.gti";
  auto frontend = Frontend().analyze(source, "export module metrics;\n" +
                                                 declaration + "\n");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid alias fixture");
  auto result = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!result.valid())
    throw std::runtime_error("alias fixture did not lower");
  return std::move(*result.program);
}
} // namespace
GTI_UNIT_TEST(compiled_public_alias_contract) {
  const auto producer = lowerAlias("export using Score = int;");
  const auto &declarations = producer.declarations();
  const auto row =
      std::find_if(declarations.begin(), declarations.end(), [](const auto &d) {
        return std::holds_alternative<LoweredTypeAliasDeclaration>(d.payload) &&
               d.name == "Score";
      });
  if (row == declarations.end())
    throw std::runtime_error("alias fixture omitted its declaration");
  const auto &alias = std::get<LoweredTypeAliasDeclaration>(row->payload);
  const auto unit = alias.sourceUnit;
  const PersistentDeclarationOwner owner{"metrics", "metrics", "metrics", {}};
  const CompiledInterfaceAlias selected{alias.qualifiedName, alias.type};
  auto metadata = projectCompiledInterfaceTypes(producer, unit, owner, {}, {},
                                                std::span(&selected, 1));
  const auto projected = lowerAlias(compiledInterfaceTypeSource(metadata));
  (void)compiledInterfaceTypeRegistry(projected, unit, metadata);
  for (const auto *changed :
       {"export using Score = bool;", "using Score = int;"}) {
    const auto other = lowerAlias(changed);
    try {
      (void)compiledInterfaceTypeRegistry(other, unit, metadata);
    } catch (const std::invalid_argument &) {
      continue;
    }
    throw std::runtime_error("accepted different public alias");
  }
  return 0;
}
