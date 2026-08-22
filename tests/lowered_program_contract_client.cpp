#include "lowered_program_contract_client.h"

#include "gti/lowered_program.h"
#include "gti/lowered_program_printer.h"

#include <stdexcept>

namespace gti_test {

static_assert(static_cast<std::size_t>(lang::LoweredDeclarationKind::Count) ==
              loweredDeclarationKindCount);
static_assert(static_cast<std::size_t>(lang::LoweredGeneratedItemKind::Count) ==
              loweredGeneratedItemKindCount);

LoweredProgramInventory
inspectLoweredProgram(const lang::LoweredProgram &program) {
  const std::vector<lang::LoweredProgramIssue> issues =
      lang::verifyLoweredProgram(program);
  if (!issues.empty()) {
    throw std::logic_error(
        "contract client requires a verified LoweredProgram: " +
        issues.front().detail);
  }

  LoweredProgramInventory result{
      .bodies = program.bodies().size(),
      .declarations = program.declarations().size(),
      .symbols = program.symbols().size(),
      .classInstances = program.classInstances().size(),
      .functionInstances = program.functionInstances().size(),
      .constructorInstances = program.constructorInstances().size(),
      .destructorInstances = program.destructorInstances().size(),
      .lambdaInstances = program.lambdaInstances().size(),
      .generatedItems = program.generatedItems().size(),
  };
  for (const lang::LoweredDeclaration &declaration : program.declarations()) {
    const std::size_t kind = static_cast<std::size_t>(declaration.kind);
    if (kind >= result.declarationKinds.size()) {
      throw std::logic_error("contract client observed a sentinel declaration");
    }
    ++result.declarationKinds[kind];
  }
  for (const lang::LoweredGeneratedItem &item : program.generatedItems()) {
    const std::size_t kind = static_cast<std::size_t>(item.identity.kind);
    if (kind >= result.generatedItemKinds.size()) {
      throw std::logic_error(
          "contract client observed a sentinel generated item");
    }
    ++result.generatedItemKinds[kind];
  }
  result.deterministicText = lang::LoweredProgramPrinter().print(program);
  return result;
}

} // namespace gti_test
