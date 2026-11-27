// Unoptimized debug locations expose initialized storage and source frame
// names.
#include "../../src/compiler/llvm/body.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "unit_test.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugProgramInstruction.h>

#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

} // namespace

GTI_UNIT_TEST(llvm_debug_locations_and_native_return_types) {
  using namespace lang;
  using namespace lang::native_llvm;
  LoweredProgramBuild lowered;
  {
    auto frontend =
        Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
            .analyze("debug-locations.gti", R"(
namespace example {
  int doubled(int input) {
    int local = input * 2;
    return local;
  }
  int identity(int input) { return input; }
}
int main() {
  int answer = example::doubled(21);
  return example::identity(answer) - 42;
}
)");
    require(frontend.canGenerateCode(), "invalid debug locations fixture");
    const auto *originalFunctions = frontend.mir.functionInstances().data();
    lowered = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, frontend.mir,
        std::move(frontend.mir), {}, TargetInfo::host());
    require(lowered.valid(), "invalid debug locations lowered program");
    require(lowered.program->mir().functionInstances().data() ==
                originalFunctions,
            "sealing copied the optimized function inventory");
  }
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(std::move(*lowered.program), issues);
  require(input.has_value(), "unverified debug locations program");
  Session session(*input, {.debugInfo = true});
  lowerScalarProgram(session, planLlvmAdmission(*input, session.options()));

  const llvm::Function *doubled = nullptr;
  const llvm::Function *identity = nullptr;
  for (const auto &function : session.module()) {
    const auto *scope = function.getSubprogram();
    if (!scope)
      continue;
    if (scope->getName() == "example::doubled")
      doubled = &function;
    if (scope->getName() == "example::identity")
      identity = &function;
  }
  require(doubled && identity,
          "debug frames lost their qualified source names");
  require(doubled->getSubprogram()->getLinkageName().empty(),
          "a private execution symbol acquired a debug linkage name");
  require(
      doubled->getReturnType()->isIntegerTy(1) &&
          !doubled->getSubprogram()->getType()->getTypeArray()[0],
      "the private success register was described as a source return value");
  require(identity->getReturnType()->isIntegerTy(32) &&
              identity->getSubprogram()->getType()->getTypeArray()[0],
          "the directly returned source value lost its debug type");

  const auto *startup = session.module().getFunction("main")->getSubprogram();
  require(startup && startup->getName() == "<GTI program startup>" &&
              (startup->getFlags() & llvm::DINode::FlagArtificial),
          "the hosted entry frame masquerades as the source main");

  bool parameterAvailable = false;
  bool localAvailable = false;
  for (const auto &block : *doubled)
    for (const auto &instruction : block)
      for (const auto &record : instruction.getDbgRecordRange()) {
        const auto *variable = llvm::dyn_cast<llvm::DbgVariableRecord>(&record);
        if (!variable || variable->isKillLocation())
          continue;
        const auto name = variable->getVariable()->getName();
        if (name != "input" && name != "local")
          continue;
        const auto *expression = variable->getExpression();
        require(variable->isDbgValue() && expression->getNumElements() == 1 &&
                    expression->getElement(0) == llvm::dwarf::DW_OP_deref,
                "readability was emitted as an unbounded storage declaration");
        parameterAvailable |= name == "input";
        localAvailable |= name == "local";
      }
  require(parameterAvailable && localAvailable,
          "verified readable storage lost its debug location");
  return 0;
}
