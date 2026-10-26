#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/instrumentation.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace lang;
using namespace lang::native_llvm;

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  const bool threadControls =
      argc == 5 && std::string_view(argv[4]) == "thread-controls";
  if (argc == 5 && !threadControls)
    return 2;
  const auto mode = static_cast<InstrumentationMode>(std::stoi(argv[2]));
  static_cast<void>(instrumentsAddress(mode));
  const auto level = std::stoi(argv[3]);
  if (level != 1 && level != 3)
    return 2;
  const auto frontend = Frontend().analyze("native-memory.gti", R"(
unsafe int read(int* address, int offset) { return unsafe { address[offset] }; }
unsafe void write(int* address, int offset, int value) { unsafe { address[offset] = value; } }
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid sanitizer GTI source");
  }
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid sanitizer lowered source");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified sanitizer source");
  const auto admission = planLlvmAdmission(*input, {});
  if (!admission.admitted())
    throw std::runtime_error(admission.limitation());
  Session session(*input, {});
  lowerScalarProgram(session, admission);
  unsigned shims = 0;
  for (const auto &source : input->program().mir().functionInstances()) {
    const auto *declaration =
        input->program().findFunctionDeclaration(source.declaration);
    if (!declaration || (declaration->qualifiedName != "read" &&
                         declaration->qualifiedName != "write"))
      continue;
    const auto native =
        session.abi().nativeC(source.returnType, source.parameterTypes);
    const auto plan =
        session.abi().gti(source.returnType, source.parameterTypes,
                          source.mayRaiseDefinedFailure);
    auto *shim = llvm::Function::Create(
        native.type, llvm::GlobalValue::ExternalLinkage,
        "gti_test_" + declaration->qualifiedName, session.module());
    native.applyTo(*shim);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", shim));
    llvm::Value *result = nullptr;
    std::vector<llvm::Value *> arguments;
    for (const auto &parameter : plan.parameters) {
      switch (parameter.channel) {
      case AbiChannel::Argument:
        if (parameter.indirect)
          throw std::runtime_error(
              "sanitizer fixture unexpectedly needs an indirect scalar");
        arguments.push_back(shim->getArg(parameter.sourceIndex));
        break;
      case AbiChannel::ResultDestination:
        result =
            ir.CreateAlloca(session.layouts().get(source.returnType).storage);
        arguments.push_back(result);
        break;
      case AbiChannel::FailureDestination:
        arguments.push_back(ir.CreateAlloca(session.abi().failureChannel()));
        break;
      default:
        throw std::runtime_error("unexpected sanitizer fixture ABI channel");
      }
    }
    auto *call = ir.CreateCall(
        plan.type, session.abi().declareFunction(source.id), arguments);
    plan.applyTo(*call);
    if (plan.statusResult) {
      auto *done = llvm::BasicBlock::Create(session.context(), "done", shim);
      auto *bad = llvm::BasicBlock::Create(session.context(),
                                           "unexpected.gti.failure", shim);
      ir.CreateCondBr(call, done, bad);
      ir.SetInsertPoint(bad);
      ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>(),
                         llvm::ArrayRef<llvm::Value *>());
      ir.CreateUnreachable();
      ir.SetInsertPoint(done);
    }
    if (source.returnType == SemanticType::Void)
      ir.CreateRetVoid();
    else if (result)
      ir.CreateRet(ir.CreateLoad(
          session.layouts().get(source.returnType).storage, result));
    else
      ir.CreateRet(call);
    ++shims;
  }
  if (shims != 2)
    throw std::runtime_error("missing real GTI memory fixture identity");
  // These atomic transport controls are explicit LLVM IR, not admitted GTI
  // atomics. They test compiler-rt interoperability without claiming O13 proof.
  if (threadControls) {
    auto &context = session.context();
    auto *pointer = llvm::PointerType::get(context, 0);
    auto *wide = llvm::Type::getInt64Ty(context);
    for (unsigned bits : {8, 16, 32, 64}) {
      auto *integer = llvm::IntegerType::get(context, bits);
      auto *store = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                  {pointer, wide}, false),
          llvm::GlobalValue::ExternalLinkage,
          "gti_test_atomic_store" + std::to_string(bits), session.module());
      llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", store));
      auto *value = ir.CreateTruncOrBitCast(store->getArg(1), integer);
      auto *write =
          ir.CreateAlignedStore(value, store->getArg(0), llvm::Align(bits / 8));
      write->setAtomic(llvm::AtomicOrdering::Release);
      ir.CreateRetVoid();
      auto *load = llvm::Function::Create(
          llvm::FunctionType::get(wide, {pointer}, false),
          llvm::GlobalValue::ExternalLinkage,
          "gti_test_atomic_load" + std::to_string(bits), session.module());
      ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", load));
      auto *read =
          ir.CreateAlignedLoad(integer, load->getArg(0), llvm::Align(bits / 8));
      read->setAtomic(llvm::AtomicOrdering::Acquire);
      ir.CreateRet(ir.CreateZExtOrBitCast(read, wide));
    }
  }
  std::size_t memoryChecks = 0;
  if (instrumentsUndefinedMemory(mode))
    memoryChecks = instrumentUndefinedMemory(session.module());
  session.verify();
  // Direct component gate until the shared Session/option registration lands.
  // This is actual GTI lowering, not a synthetic language body or production
  // FFI.
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  auto pipeline = builder.buildPerModuleDefaultPipeline(
      level == 1 ? llvm::OptimizationLevel::O1 : llvm::OptimizationLevel::O3);
  pipeline.run(session.module(), modules);
  if (instrumentsAddress(mode))
    instrumentAddress(session.module(), modules);
  if (instrumentsThread(mode))
    instrumentThread(session.module(), modules);
  session.verify();
  const std::filesystem::path path(argv[1]);
  std::error_code error;
  llvm::raw_fd_ostream ir(path.string() + ".ll", error, llvm::sys::fs::OF_None);
  if (error)
    throw std::runtime_error(error.message());
  session.module().print(ir, nullptr);
  ir.flush();
  const auto artifact = session.emitObject();
  std::ofstream object(path, std::ios::binary);
  object.write(artifact.contents.data(), artifact.contents.size());
  if (!object || artifact.contents.empty())
    throw std::runtime_error("failed sanitizer object publication");
  std::cout << "real GTI load/store object: mode=" << argv[2] << " IR-O"
            << level << " undefined-memory-sites=" << memoryChecks << '\n';
}
