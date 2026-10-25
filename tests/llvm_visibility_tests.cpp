#include "../src/compiler/llvm/abi.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>

#include <iostream>
#include <map>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

// Policy-only LLVM bodies are never executed or published as GTI semantics.
// Source identities, entry mode and export selection are genuinely verified.
void policy(bool library, bool embedding) {
  FrontendOptions frontendOptions;
  frontendOptions.requireEntryPoint = !library;
  if (embedding) {
    frontendOptions.program.kind = ProgramKind::Library;
    frontendOptions.program.exports.push_back(
        {.function = "private_helper", .symbol = "host_helper_v1"});
  }
  std::string source = R"(
int private_helper(int value) { return value; }
int addressed(int value) { return value; }
int callback(int value) { return value; }
using Unary = (int) -> int;
extern "C" { int foreign(int value); void install(Unary value); }
)";
  if (!library)
    source += "int main() { unsafe { install(callback); } return "
              "private_helper(0) + addressed(0) + "
              "foreign(0); }\n";
  const auto frontend =
      Frontend(frontendOptions).analyze("visibility.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid visibility source");
  }
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid visibility lowered source");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified visibility source");
  if (bool(input->program().mir().hostedStartupPlan()) == library ||
      (embedding && !input->program().embeddingPlan()) ||
      (!library && input->program().mir().nativeCallbackAdapters().empty()))
    throw std::runtime_error("fixture lost sealed entry/export mode");
  Session session(*input, {.optimization = LlvmCodegenOptimization::None});
  std::map<std::string, llvm::Function *> functions;
  for (const auto &source : input->program().mir().functionInstances()) {
    auto *function = session.abi().declareFunction(source.id);
    functions.emplace(input->program()
                          .findFunctionDeclaration(source.declaration)
                          ->qualifiedName,
                      function);
    if (source.definitionKind != MirDefinitionKind::Source)
      continue;
    // Models the single coordinated ABI registration hunk until integrated.
    session.requireSourceDefinition(source.id, *function);
    session.requireSourceDefinition(source.id, *function);
    auto *block =
        llvm::BasicBlock::Create(session.context(), "entry", function);
    llvm::IRBuilder<> ir(block);
    if (function->getReturnType()->isVoidTy())
      ir.CreateRetVoid();
    else
      ir.CreateRet(llvm::Constant::getNullValue(function->getReturnType()));
  }
  new llvm::GlobalVariable(session.module(),
                           functions.at("addressed")->getType(), true,
                           llvm::GlobalValue::ExternalLinkage,
                           functions.at("addressed"), "foreign_callback_root");
  auto *unknown = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::ExternalLinkage, "unregistered_external_root",
      session.module());
  llvm::IRBuilder<>(
      llvm::BasicBlock::Create(session.context(), "entry", unknown))
      .CreateRetVoid();
  bool rejected = false;
  try {
    session.requireSourceDefinition(0, *unknown);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("unsealed source identity was accepted");
  const auto artifact = session.emitObject();
  if (artifact.contents.empty() ||
      functions.at("private_helper")->hasInternalLinkage() == library ||
      !functions.at("addressed")->hasExternalLinkage() ||
      !functions.at("callback")->hasExternalLinkage() ||
      !functions.at("foreign")->hasExternalLinkage() ||
      (!library && !functions.at("main")->hasExternalLinkage()) ||
      !unknown->hasExternalLinkage())
    throw std::runtime_error(
        "entry/foreign/address/library retention policy failed");
}

int main() {
  policy(false, false);
  policy(true, false);
  policy(true, true);
  std::cout << "sealed hosted/private, entry, address, foreign, library and "
               "embedding policy passed\n";
}
