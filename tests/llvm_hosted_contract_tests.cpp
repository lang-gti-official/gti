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
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lang {
struct MirProgramTestAccess {
  static auto &hostedPlan(MirProgram &program) {
    return program.hostedStartupPlan_;
  }
  static auto &functions(MirProgram &program) { return program.functions; }
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

void unsafeReferenceLoadContract(const std::filesystem::path &repository) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository / "tests/fixtures/llvm_hosted_unsafe_reference_load.gti",
      std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  require(frontend.canGenerateCode(), "unrelated unsafe helper is valid GTI");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program && planLlvmAdmission(*program).admitted(),
          "checked unsafe result materialization has structural admission");
  std::size_t owner = 0;
  MirBlockId blockId = 0;
  MirInstructionId loadId = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Load &&
            instruction.unsafeOperation ==
                UnsafeOperationKind::DeclaredUnsafeCall) {
          require(owner == 0, "regression isolates exactly one result load");
          owner = function.id;
          blockId = block.id;
          loadId = instruction.id;
        }
  require(owner != 0,
          "real producer retains its unsafe materialization marker");
  using Mutation =
      std::function<void(MirProgram &, MirBlock &, MirInstruction &,
                         MirInstruction &, MirPlace &, MirLoan &)>;
  const std::array<Mutation, 16> mutations{
      [](auto &, auto &, auto &, auto &, auto &place, auto &) {
        place.loan = 0;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &loan) {
        load.operands.front().place = loan.source;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        loan.kind = MirLoanKind::Local;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        loan.entry = true;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        ++loan.producedBy;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.hirValue = 0;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.rawMemoryAccess = true;
      },
      [](auto &, auto &, auto &, auto &, auto &place, auto &) {
        place.projections.push_back(
            {.kind = MirProjectionKind::RawDereference});
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.info.type = SemanticType::Int64;
      },
      [](auto &, auto &, auto &, auto &, auto &place, auto &loan) {
        place.access = loan.access = AccessMode::Mutable;
      },
      [](auto &mir, auto &, auto &, auto &call, auto &, auto &) {
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1]
            .requiresUnsafe = false;
      },
      [](auto &mir, auto &, auto &, auto &call, auto &, auto &) {
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1].linkage =
            LanguageLinkage::C;
      },
      [](auto &, auto &, auto &, auto &call, auto &, auto &) {
        call.intrinsic = IntrinsicKind::SourceLine;
      },
      [](auto &, auto &, auto &, auto &call, auto &, auto &) {
        call.dispatch = CallDispatch::Virtual;
      },
      [](auto &, auto &, auto &load, auto &call, auto &, auto &) {
        std::swap(load, call);
      },
      [](auto &, auto &block, auto &load, auto &, auto &, auto &loan) {
        const auto ended =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const auto &stage) {
                           return stage.kind == MirInstructionKind::EndBorrow &&
                                  stage.loan == loan.id;
                         });
        require(ended != block.instructions.end(),
                "result loan has an endpoint");
        std::swap(load, *ended);
      },
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    auto invalid = frontend.mir;
    auto &body = MirProgramTestAccess::functions(invalid)[owner - 1].body;
    auto &block = body.blocks[blockId - 1];
    auto &load =
        *std::find_if(block.instructions.begin(), block.instructions.end(),
                      [&](const auto &stage) { return stage.id == loadId; });
    auto &place = body.places[load.operands.front().place - 1];
    auto &loan = body.loans[place.loan - 1];
    auto &call =
        *std::find_if(block.instructions.begin(), block.instructions.end(),
                      [&](const auto &stage) {
                        return stage.kind == MirInstructionKind::Call &&
                               stage.loan == loan.id;
                      });
    mutations[index](invalid, block, load, call, place, loan);
    auto built = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, invalid, invalid,
        {}, TargetInfo::host(), true);
    bool rejected = !built.valid();
    if (built.valid()) {
      auto verified = verifiedLoweredProgram(std::move(*built.program), issues);
      rejected = !verified || !planLlvmAdmission(*verified).admitted();
    }
    require(rejected,
            "forged unsafe call-result relationship cannot be admitted");
  }
}

void sourceMetadataContract(const std::filesystem::path &repository) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository / "tests/fixtures/stdlib_surface_source_location.gti",
      std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  require(frontend.canGenerateCode(),
          "source metadata has verified source MIR");
  std::array<bool, 4> seen{};
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto literal = hostedSourceLiteral(instruction);
        if (!literal)
          continue;
        const auto &location = *instruction.sourceLocation;
        require(location.file.ends_with("stdlib_surface_source_location.gti") &&
                    location.function == "main" &&
                    (location.line == 9 || location.line == 15) &&
                    location.column == 0,
                "caller metadata is not rediscovered from the library body");
        if (instruction.intrinsic == IntrinsicKind::SourceFileName) {
          seen[0] = true;
          require(std::get<std::string>(*literal) == location.file,
                  "file metadata retains exact counted bytes");
        } else if (instruction.intrinsic == IntrinsicKind::SourceFunctionName) {
          seen[1] = true;
          require(std::get<std::string>(*literal) == location.function,
                  "function metadata retains exact counted bytes");
        } else if (instruction.intrinsic == IntrinsicKind::SourceLine) {
          seen[2] = true;
          require(std::get<std::uint64_t>(*literal) == location.line,
                  "line metadata retains its uint32 value");
        } else {
          seen[3] = true;
          require(std::get<std::uint64_t>(*literal) == location.column,
                  "unavailable column remains zero");
        }
        const auto reject = [&](MirInstruction invalid) {
          bool rejected = false;
          try {
            static_cast<void>(hostedSourceLiteral(invalid));
          } catch (const std::logic_error &) {
            rejected = true;
          }
          require(rejected, "malformed source metadata cannot reach lowering");
        };
        auto invalid = instruction;
        invalid.sourceLocation.reset();
        reject(invalid);
        invalid = instruction;
        invalid.info.type = SemanticType::Int64;
        reject(invalid);
        invalid = instruction;
        invalid.kind = MirInstructionKind::Load;
        reject(invalid);
      }
  require(
      std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
      "all four source metadata capabilities have real producer evidence");
  auto invalid = frontend.mir;
  bool removed = false;
  for (auto &function : MirProgramTestAccess::functions(invalid))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!removed && instruction.sourceLocation) {
          instruction.sourceLocation.reset();
          removed = true;
        }
  require(removed && !verifyMirProgram(invalid).valid(),
          "missing source metadata is rejected by the owning MIR verifier");
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
    unsafeReferenceLoadContract(std::filesystem::canonical(argv[1]));
    sourceMetadataContract(std::filesystem::canonical(argv[1]));
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
