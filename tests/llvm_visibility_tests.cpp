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
void policy(bool library, bool embedding, bool existing,
            LlvmCodegenOptimization level) {
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
  Session session(*input, {.optimization = level});
  std::map<std::string, llvm::Function *> functions;
  for (const auto &source : input->program().mir().functionInstances()) {
    const auto &name = input->program()
                           .findFunctionDeclaration(source.declaration)
                           ->qualifiedName;
    llvm::Function *seed = nullptr;
    if (existing && name == "private_helper") {
      const auto plan = session.abi().gti(source.returnType, source.parameterTypes,
                                         source.mayRaiseDefinedFailure);
      seed = llvm::Function::Create(
          plan.type, llvm::GlobalValue::ExternalLinkage,
          "__gti_native_v" + std::to_string(executionAbiVersion) + "_f" +
              std::to_string(source.id),
          session.module());
    }
    auto *function = session.abi().declareFunction(source.id);
    if ((seed && seed != function) ||
        function != session.abi().declareFunction(source.id))
      throw std::runtime_error("existing ABI declaration was not reused");
    functions.emplace(name, function);
    if (source.definitionKind != MirDefinitionKind::Source)
      continue;
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
  llvm::IRBuilder<> root(
      llvm::BasicBlock::Create(session.context(), "entry", unknown));
  root.CreateCall(functions.at("foreign"),
                  {llvm::Constant::getNullValue(
                      functions.at("foreign")->getFunctionType()->getParamType(0))});
  root.CreateRetVoid();
  bool rejected = false;
  try {
    session.requireSourceDefinition(0, *unknown);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("unsealed source identity was accepted");
  const auto privateName = functions.at("private_helper")->getName().str();
  const auto artifact = session.emitObject();
  const auto *privateFunction = session.module().getFunction(privateName);
  const bool privateInternal =
      !privateFunction || privateFunction->hasInternalLinkage();
  if (artifact.contents.empty() ||
      privateInternal == library ||
      !functions.at("addressed")->hasExternalLinkage() ||
      !functions.at("callback")->hasExternalLinkage() ||
      !functions.at("foreign")->hasExternalLinkage() ||
      (!library && !functions.at("main")->hasExternalLinkage()) ||
      !unknown->hasExternalLinkage())
    throw std::runtime_error(
        "entry/foreign/address/library retention policy failed");
}

void existingSourceObligation() {
  const auto frontend = Frontend().analyze(
      "source-obligation.gti", "int pending(int value) { return value; }");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  if (!frontend.canGenerateCode() || !lowered.valid())
    throw std::runtime_error("invalid source obligation fixture");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified source obligation fixture");
  Session session(*input, {});
  for (const auto &source : input->program().mir().functionInstances()) {
    const auto plan = session.abi().gti(source.returnType, source.parameterTypes,
                                       source.mayRaiseDefinedFailure);
    auto *function = llvm::Function::Create(
        plan.type, llvm::GlobalValue::ExternalLinkage,
        "__gti_native_v" + std::to_string(executionAbiVersion) + "_f" +
            std::to_string(source.id),
        session.module());
    if (session.abi().declareFunction(source.id) != function)
      throw std::runtime_error("existing source declaration was not reused");
  }
  bool rejected = false;
  try {
    static_cast<void>(session.emitObject());
  } catch (const BackendUnsupportedError &) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("existing source declaration lost its body obligation");
}

int main() {
  existingSourceObligation();
  for (auto level : {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
                     LlvmCodegenOptimization::Default,
                     LlvmCodegenOptimization::Aggressive})
    for (bool existing : {false, true}) {
      policy(false, false, existing, level);
      policy(true, false, existing, level);
      policy(true, true, existing, level);
    }
  std::cout << "sealed hosted/private, entry, address, foreign, library and "
               "embedding policy passed\n";
}
