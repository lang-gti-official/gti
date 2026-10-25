#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

#include <iostream>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

int main() {
  const auto frontend = Frontend().analyze("pipeline.gti", R"(
int promote(int input) { mut int local = input; return local; }
int checked(int left, int right) { return left + right; }
double strict(double left, double right, double addend) { return left * right + addend; }
double divide(double left, double right) { return left / right; }
int main() {
  if (promote(7) != 7 || checked(1, 2) != 3) { return 1; }
  if (strict(2.0d, 3.0d, 4.0d) != 10.0d || divide(4.0d, 2.0d) != 2.0d) { return 2; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid pipeline source");
  }
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid lowered pipeline source");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified pipeline source");
  unsigned levels = 0;
  for (auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    const LlvmBackendOptions options{.optimization = level};
    const auto admission = planLlvmAdmission(*input, options);
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    Session session(*input, options);
    lowerScalarProgram(session, admission);
    llvm::Function *promote = nullptr, *checked = nullptr, *strict = nullptr;
    for (const auto &function : input->program().mir().functionInstances()) {
      const auto *declaration =
          input->program().findFunctionDeclaration(function.declaration);
      if (!declaration)
        continue;
      if (declaration->qualifiedName == "promote")
        promote = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "checked")
        checked = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "strict")
        strict = session.abi().declareFunction(function.id);
    }
    if (!promote || !checked || !strict)
      throw std::runtime_error("missing pipeline function");
    const auto allocas = [](const llvm::Function &function) {
      unsigned count = 0;
      for (const auto &block : function)
        for (const auto &row : block)
          count += llvm::isa<llvm::AllocaInst>(row);
      return count;
    };
    if (allocas(*promote) == 0)
      throw std::runtime_error("pipeline fixture lost its source storage");
    const auto floating = [&] {
      unsigned multiply = 0, addition = 0;
      for (const auto &block : *strict)
        for (const auto &row : block) {
          if (const auto *operation =
                  llvm::dyn_cast<llvm::FPMathOperator>(&row))
            if (operation->getFastMathFlags().any())
              throw std::runtime_error("unverified fast math");
          multiply += row.getOpcode() == llvm::Instruction::FMul;
          addition += row.getOpcode() == llvm::Instruction::FAdd;
          if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
            if (call->getIntrinsicID() == llvm::Intrinsic::fma ||
                call->getIntrinsicID() == llvm::Intrinsic::fmuladd)
              throw std::runtime_error("implicit floating contraction");
        }
      if (!multiply || !addition)
        throw std::runtime_error("strict multiply/add was lost");
    };
    floating();
    session.verify();
    const auto artifact = session.emitObject();
    if (artifact.kind != BackendArtifactKind::Object ||
        artifact.contents.empty())
      throw std::runtime_error("pipeline did not emit a real object");
    session.verify();
    floating();
    if (level == LlvmCodegenOptimization::None ? allocas(*promote) == 0
                                               : allocas(*promote) != 0)
      throw std::runtime_error("selected O0/O1/O2/O3 pipeline did not "
                               "preserve/promote storage as expected");
    bool overflow = false;
    for (const auto &block : *checked)
      for (const auto &row : block)
        if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
          overflow |=
              call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow;
    if (!overflow)
      throw std::runtime_error(
          "unknown-input checked addition lost overflow detection");
    Session invalid(*input, options);
    auto *malformed = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(invalid.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, "unterminated", invalid.module());
    llvm::BasicBlock::Create(invalid.context(), "entry", malformed);
    bool rejected = false;
    try {
      static_cast<void>(invalid.emitObject());
    } catch (const std::logic_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error(
          "malformed IR escaped before the selected pipeline");
    ++levels;
  }
  std::cout << levels
            << " real pipelines: storage promotion, strict FP, checked "
               "overflow and rejection passed\n";
}
