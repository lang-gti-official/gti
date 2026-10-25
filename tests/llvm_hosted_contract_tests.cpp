#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/hosted.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/mir_verifier.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lang {
struct MirProgramTestAccess {
  static auto &hostedPlan(MirProgram &program) {
    return program.hostedStartupPlan_;
  }
};
} // namespace lang

namespace {
using namespace lang;
using namespace lang::native_llvm;

struct Case {
  std::string_view fixture;
  int trace;
  std::string_view failure;
  unsigned line;
};

constexpr std::array cases{
    Case{"llvm_hosted_success.gti", 12354, "", 0},
    Case{"llvm_hosted_startup_failure.gti", 321, "hosted startup failure", 16},
    Case{"llvm_hosted_entry_failure.gti", 321, "hosted entry failure", 16},
};

void require(bool value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
}

void run(const std::filesystem::path &repository,
         const std::filesystem::path &scratch, const Case &test,
         LlvmCodegenOptimization optimization) {
  const auto source = repository / "tests/fixtures" / test.fixture;
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      source, std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(frontend.canGenerateCode(), "hosted source produces valid MIR");
  require(verifyMirProgram(frontend.mir).valid(), "hosted MIR verifies");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program.has_value(), "hosted source crosses the sealed boundary");
  const auto &startup = program->program().mir().hostedStartupPlan();
  require(startup && startup->kind == ProgramEntryKind::NoArguments,
          "fixture exercises the real no-argument hosted startup plan");
  {
    auto invalid = frontend.mir;
    MirProgramTestAccess::hostedPlan(invalid)->kind =
        ProgramEntryKind::OwnedArguments;
    require(!verifyMirProgram(invalid).valid(),
            "an unsupported argument shape cannot forge the no-argument plan");
    invalid = frontend.mir;
    auto &operations = MirProgramTestAccess::hostedPlan(invalid)->operations;
    const auto drain = std::find_if(
        operations.begin(), operations.end(), [](const auto &operation) {
          return operation.kind ==
                 MirHostedStartupOperationKind::DrainThreadLocals;
        });
    require(drain != operations.end(),
            "startup has an explicit drain operation");
    operations.erase(drain);
    require(!verifyMirProgram(invalid).valid(),
            "missing hosted teardown authority fails MIR verification");
  }
  const auto admission = planLlvmAdmission(*program);
  if (!admission.admitted())
    std::cerr << admission.limitation() << '\n';
  require(admission.admitted(), "hosted fixture has native admission");

  SymbolId trace = 0;
  unsigned matches = 0;
  for (const auto &symbol : program->program().symbols()) {
    if (symbol.qualifiedName != "trace")
      continue;
    ++matches;
    require(symbol.kind == SymbolKind::GlobalVariable &&
                symbol.type == SemanticType::Int32 && symbol.mutableBinding,
            "trace is an exact sealed mutable scalar global");
    trace = symbol.id;
  }
  require(matches == 1, "trace has one sealed identity");

  Session session(*program, {.optimization = optimization});
  const auto returned = std::find_if(
      startup->operations.begin(), startup->operations.end(),
      [](const auto &operation) {
        return operation.kind == MirHostedStartupOperationKind::ReturnEntry;
      });
  require(returned != startup->operations.end(),
          "startup has one exact return operation");
  llvm::IRBuilder<> invalidBuilder(session.context());
  const auto rejectReturn = [&](MirHostedStartupOperation operation,
                                llvm::Value *value) {
    bool rejected = false;
    try {
      static_cast<void>(hostedReturnStatus(invalidBuilder, operation, value));
    } catch (const std::logic_error &) {
      rejected = true;
    }
    require(rejected, "malformed hosted status lowering rejects its input");
  };
  rejectReturn(*returned, invalidBuilder.getInt64(256));
  rejectReturn(*returned, nullptr);
  auto wrongOperation = *returned;
  wrongOperation.kind = MirHostedStartupOperationKind::CallEntry;
  rejectReturn(wrongOperation, invalidBuilder.getInt32(256));
  lowerScalarProgram(session, admission);
  auto &module = session.module();
  auto *global = module.getGlobalVariable(
      "__gti_native_v1_g" + std::to_string(trace), true);
  require(global && global->getValueType()->isIntegerTy(32) &&
              !global->isConstant() && !global->isThreadLocal(),
          "observer sees the exact native trace storage");
  // The source program has no foreign calls. Only this test boundary exposes
  // its trace and observes the documented C failure observer slot.
  llvm::GlobalAlias::create(global->getValueType(), global->getAddressSpace(),
                            llvm::GlobalValue::ExternalLinkage,
                            "gti_hosted_trace", global, &module);
  auto *main = module.getFunction("main");
  require(main && main->arg_empty(), "real hosted entry has no arguments");
  main->setName("gti_hosted_entry");
  auto *pointer = session.layouts().pointer();
  auto *observer = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "gti_hosted_observer", module);
  auto *terminate =
      declareRuntimeService(session, RuntimeService::TerminateFailure);
  const auto expectedObservations = static_cast<unsigned>(std::count_if(
      startup->operations.begin(), startup->operations.end(),
      [](const auto &operation) {
        return operation.kind == MirHostedStartupOperationKind::ContainFailure;
      }));
  unsigned observations = 0;
  for (auto &block : *main)
    for (auto &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      auto *target = call ? call->getCalledFunction() : nullptr;
      if (target != terminate)
        continue;
      require(
          call->getFunctionType() == terminate->getFunctionType() &&
              call->arg_size() == 4 &&
              llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(2)) &&
              llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(3)),
          "containment retains the existing v2 observer contract");
      call->setArgOperand(2, observer);
      ++observations;
    }
  require(observations != 0 && observations == expectedObservations,
          "only the exact planned hosted containment calls are observed");

  const auto directory =
      scratch / std::string(test.fixture) /
      (optimization == LlvmCodegenOptimization::None ? "O0" : "O3");
  std::filesystem::create_directories(directory);
  const auto object = directory / "program.o";
  require(driver::writeArtifact(object, session.emitObject().contents) ==
              driver::ArtifactWriteStatus::Success,
          "verified hosted object is published");
  const auto shim = directory / "observer.c";
  const std::string consumer =
      "#include <gti/runtime_failure.h>\n"
      "#include <gti/runtime_execution.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "extern int32_t gti_hosted_trace;\n"
      "extern int gti_hosted_entry(void);\n"
      "void gti_hosted_observer(const gti_failure_record_v2 *record, void "
      "*context) {\n"
      "  if (!record || context || gti_hosted_trace != " +
      std::to_string(test.trace) +
      ") abort();\n"
      "  fputs(\"hosted observer: cleanup verified\\n\", stderr);\n"
      "  fflush(stderr);\n"
      "}\n"
      "gti_failure_observer_v2 checked_observer = gti_hosted_observer;\n"
      "int main(void) {\n"
      "  if (gti_rt_thread_locals_attached_v1()) return 91;\n"
      "  int result = gti_hosted_entry();\n"
      "  if (gti_hosted_trace != " +
      std::to_string(test.trace) +
      " || gti_rt_thread_locals_attached_v1()) return 92;\n"
      "  return result;\n"
      "}\n";
  require(driver::writeArtifact(shim, consumer) ==
              driver::ArtifactWriteStatus::Success,
          "write independent C hosted observer");
  const auto layout = driver::discoverToolchainLayout(nullptr);
  driver::NativeToolchain toolchain;
  const auto observerObject = directory / "observer.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), shim, observerObject, driver::CStandard::C17,
      OptimizationLevel::O2, {layout.runtimeInclude}, {}));
  if (!compiled.succeeded())
    std::cerr << compiled.output;
  require(compiled.succeeded(),
          "C observer matches the actual runtime headers");
  driver::NativeInputs inputs;
  inputs.finalLibraryFiles = {observerObject, layout.runtimeLibrary};
  const auto executable = directory / "program";
  const auto linked = toolchain.invoke(driver::NativeLinkRequest(
      driver::discoverNativeCompiler(), object, executable, inputs));
  if (!linked.succeeded())
    std::cerr << linked.output;
  require(linked.succeeded(), "real hosted entry links to the runtime");
  const auto executed = driver::invokeProcess({executable.string()});
  if (test.failure.empty()) {
    require(executed.succeeded() && executed.output.empty(),
            "normal startup and teardown preserve their exact order");
  } else {
    const auto location =
        std::string(test.fixture) + ":" + std::to_string(test.line) + ":";
    if (executed.exitCode != 70)
      std::cerr << executed.output;
    require(executed.exitCode == 70 && !executed.signal && !executed.timedOut &&
                !executed.driverDiagnostic &&
                executed.output.find("hosted observer: cleanup verified") !=
                    std::string::npos &&
                executed.output.find("GTI-R0017") != std::string::npos &&
                executed.output.find(location) != std::string::npos &&
                executed.output.find(test.failure) != std::string::npos,
            "panic containment observes cleanup and original source location");
  }
  std::cout << test.fixture << ' '
            << (optimization == LlvmCodegenOptimization::None ? "O0" : "O3")
            << ": native hosted lifecycle verified\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto scratch =
      std::filesystem::temp_directory_path() /
      ("gti-llvm-hosted-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{scratch};
  try {
    for (const auto &test : cases)
      for (auto optimization :
           {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive})
        run(std::filesystem::canonical(argv[1]), scratch, test, optimization);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
