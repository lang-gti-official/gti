#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/thread_lifecycle.h"
#include "cpp_backend_test_support.h"

#include <iostream>
#include <llvm/IR/Instructions.h>

namespace {
using namespace lang;
using namespace lang::native_llvm;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void check(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_cthread_startup_tls.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(), "concurrent TLS source reaches MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "concurrent source has a verified snapshot");
  const auto &program = input->program().mir();
  const auto *body = program.hostedStartup();
  require(body && program.hostedStartupPlan(),
          "producer supplied startup authority");
  std::size_t calls = 0;
  for (const auto &block : body->blocks)
    for (const auto &instruction : block.instructions) {
      const auto *function =
          instruction.functionTarget
              ? program.findFunctionInstance(*instruction.functionTarget)
              : nullptr;
      if (!function ||
          (function->cThreadOperation != CThreadRuntimeOperation::Begin &&
           function->cThreadOperation != CThreadRuntimeOperation::End))
        continue;
      ++calls;
      require(nativeCThreadLifecycleBinding(program, *function),
              "only exact selected C lifecycle declarations are eligible");
      require(nativeCThreadLifecycleCall(program, *body, instruction),
              "sealed startup call is independently eligible");
      const auto reject = [&](auto mutate) {
        auto bad = instruction;
        mutate(bad);
        require(!nativeCThreadLifecycleCall(program, *body, bad),
                "malformed lifecycle call rejects before emission");
      };
      reject([](auto &call) { call.hostedStartupOperation = 0; });
      reject([](auto &call) { call.result = 1; });
      reject([](auto &call) { call.destination = 1; });
      reject([](auto &call) { call.parameterTypes = {SemanticType::Int32}; });
      reject([](auto &call) { call.borrowOrigin = BorrowOriginKind::Global; });
      reject([](auto &call) {
        call.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
      });
      auto badFunction = *function;
      badFunction.externalSymbol = "untrusted_lifecycle";
      require(!nativeCThreadLifecycleBinding(program, badFunction),
              "operation tag cannot authorize another native symbol");
      badFunction = *function;
      badFunction.cThreadOperation = CThreadRuntimeOperation::Create;
      require(!nativeCThreadLifecycleBinding(program, badFunction),
              "worker creation is not admitted by lifecycle binding");
      auto otherBody = *body;
      otherBody.kind = MirBodyKind::Function;
      require(!nativeCThreadLifecycleCall(program, otherBody, instruction),
              "source bodies cannot acquire the hosted lifecycle authority");
      const auto *operation = program.hostedStartupPlan()->findOperation(
          instruction.hostedStartupOperation);
      require(operation && nativeCThreadLifecycleOperation(program, *operation),
              "coverage witness requires the exact operation recipe");
      auto badOperation = *operation;
      badOperation.kind = MirHostedStartupOperationKind::CallEntry;
      require(!nativeCThreadLifecycleOperation(program, badOperation),
              "lifecycle witness cannot admit another startup operation");
    }
  require(calls == 2, "one begin and one end are source-selected");
  const auto admission = planLlvmAdmission(*input);
  if (!admission.admitted())
    throw std::runtime_error(admission.limitation());
  Session session(*input, {});
  lowerScalarProgram(session, admission);
  session.verify();
  for (const auto *name :
       {"gti_rt_cthread_begin_v1", "gti_rt_cthread_end_v1"}) {
    const auto *function = session.module().getFunction(name);
    require(function && function->arg_empty() &&
                function->getReturnType()->isVoidTy() &&
                function->doesNotThrow(),
            "real lifecycle C ABI is void(), non-unwinding");
    std::size_t emitted = 0;
    for (const auto *user : function->users())
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user)) {
        require(call->getFunction()->getName() == "main",
                "native lifecycle call belongs to the generated entry");
        ++emitted;
      }
    require(emitted == 1, "exactly one real call is emitted for each boundary");
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected repository root");
    check(argv[1]);
    std::cout << "C-thread startup contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
