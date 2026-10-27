#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/layout.h"
#include "../src/compiler/llvm/thread_lifecycle.h"
#include "../src/compiler/llvm/threads.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "cpp_backend_test_support.h"
#include "gti/runtime_thread.h"

#include <cstddef>
#include <set>

#include <iostream>
#include <tuple>
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
  for (const auto &[service, arguments, returnsKind] : {
           std::tuple{RuntimeService::EnterThreadFailureSink, 3U, false},
           std::tuple{RuntimeService::LeaveThreadFailureSink, 1U, false},
           std::tuple{RuntimeService::PublishThreadFailure, 1U, false},
           std::tuple{RuntimeService::ThreadCleanupFailureKind, 0U, true},
           std::tuple{RuntimeService::CopyThreadCleanupPrimary, 2U, true},
           std::tuple{RuntimeService::QuarantineThread, 1U, false},
           std::tuple{RuntimeService::ProgramStorageRetirementReady, 0U, true}}) {
    const auto *function = declareRuntimeService(session, service);
    require(function->arg_size() == arguments && function->doesNotThrow() &&
                !function->hasFnAttribute(llvm::Attribute::NoReturn) &&
                (returnsKind ? function->getReturnType()->isIntegerTy(32)
                             : function->getReturnType()->isVoidTy()),
            "worker outcome C declaration matches the nonthrowing runtime header");
    for (const auto &argument : function->args())
      require(argument.getType()->isPointerTy(),
              "worker outcome C arguments are borrowed records or callbacks");
    require(declareRuntimeService(session, service) == function,
            "worker service identity reuses only its exact C declaration");
  }
}
// Managed/scoped worker operations consume only exact sealed MIR facts and
// trusted capability identities; malformed stages reject before emission.
void checkManagedThreads(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  std::size_t operations = 0, adapters = 0;
  for (const auto *fixture :
       {"llvm_thread_managed.gti", "llvm_thread_results.gti",
        "llvm_thread_scoped.gti"}) {
    const auto frontend = Frontend(options).analyze(
        root / "tests/fixtures" / fixture, std::nullopt,
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    require(frontend.canGenerateCode(), "managed-thread source reaches MIR");
    std::vector<LoweredProgramIssue> issues;
    const auto input = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                               options.target),
        issues);
    require(input.has_value(), "managed-thread source is verified");
    const auto &program = input->program().mir();
    for (const auto &adapter : program.threadTaskAdapters()) {
      ++adapters;
      require(nativeThreadTaskAdapter(program, adapter),
              "sealed managed task adapter is eligible");
      const auto reject = [&](auto mutate, const char *message) {
        auto bad = adapter;
        mutate(bad);
        require(!nativeThreadTaskAdapter(program, bad), message);
      };
      reject([](auto &task) {
        task.invocation = CallableInvocationCapability::Read;
      }, "an adapter must consume its task once");
      reject([](auto &task) { task.catchesWorkerFailure = false; },
             "uncontained worker failure cannot cross the C boundary");
      reject([](auto &task) { task.taskType = SemanticType::Int32; },
             "an adapter cannot be rebound to another task type");
      reject([](auto &task) {
        task.lambdaTarget = 1;
        task.functionTarget = 1;
      }, "an adapter selects exactly one entry");
    }
    for (const auto address : enumerateMirBodyAddresses(program)) {
      const auto *body = findMirBody(program, address);
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.tombstone ||
              !isNativeThreadIntrinsic(instruction.intrinsic) ||
              !nativeThreadCall(program, address, *body, instruction))
            continue;
          ++operations;
          const auto reject = [&](auto mutate, const char *message) {
            auto bad = instruction;
            mutate(bad);
            require(!nativeThreadCall(program, address, *body, bad), message);
          };
          reject([](auto &call) { call.functionTarget = 1; },
                 "a thread operation names no source target");
          reject([](auto &call) {
            call.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
          }, "a thread operation is not an unsafe call");
          reject([](auto &call) {
            call.synchronization.order = AtomicMemoryOrder::Relaxed;
          }, "thread synchronization carries no atomic order");
          reject([](auto &call) { call.dispatch = CallDispatch::Virtual; },
                 "a thread operation is statically dispatched");
          if (instruction.intrinsic == IntrinsicKind::ThreadSpawn) {
            reject([](auto &call) { call.threadTaskAdapter.reset(); },
                   "spawn requires its exact sealed adapter");
            reject([](auto &call) {
              call.operands[0].kind = MirOperandKind::BorrowRead;
            }, "spawn requires exclusive handle access");
            reject([](auto &call) {
              call.definedFailure.propagation = FailurePropagationKind::TaskJoin;
            }, "spawn has no failure edge");
          }
          if (instruction.intrinsic == IntrinsicKind::ThreadJoin ||
              instruction.intrinsic == IntrinsicKind::ThreadAutoJoin)
            reject([](auto &call) {
              call.definedFailure.propagation = FailurePropagationKind::None;
            }, "join re-raises through its verified TaskJoin edge");
          if (instruction.intrinsic == IntrinsicKind::ThreadScope)
            reject([](auto &call) { call.scopedThreadTasks.pop_back(); },
                   "every scoped operand needs its adapter");
          if (instruction.intrinsic == IntrinsicKind::ThreadHandleEmpty)
            require(nativeThreadHandle(program, instruction.info.type).has_value(),
                    "the handle is the trusted capability");
        }
    }
    const auto admission = planLlvmAdmission(*input);
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    Session session(*input, {});
    lowerScalarProgram(session, admission);
    session.verify();
    const auto types = threadRuntimeTypes(session);
    const auto &layout = session.dataLayout();
    const auto size = [&](llvm::Type *type) {
      return layout.getTypeAllocSize(type).getFixedValue();
    };
    require(size(types.spawnResult) == sizeof(gti_thread_spawn_result_v1) &&
                size(types.joinResult) == sizeof(gti_thread_join_result_v2) &&
                size(types.valueJoin) ==
                    sizeof(gti_thread_value_join_result_v2) &&
                size(types.scopeResult) == sizeof(gti_thread_scope_result_v1) &&
                size(types.scopedTask) == sizeof(gti_thread_scoped_task_v1) &&
                layout.getStructLayout(types.joinResult)->getElementOffset(4) ==
                    offsetof(gti_thread_join_result_v2, failure) &&
                layout.getStructLayout(types.joinResult)->getElementOffset(5) ==
                    offsetof(gti_thread_join_result_v2, cleanup_failure) &&
                layout.getStructLayout(types.joinResult)->getElementOffset(6) ==
                    offsetof(gti_thread_join_result_v2, primary_artifact) &&
                layout.getStructLayout(types.scopeResult)->getElementOffset(4) ==
                    offsetof(gti_thread_scope_result_v1, worker),
            "managed-thread runtime records match the C declarations");
    for (const auto *name : {"gti_rt_thread_join_v2", "gti_rt_thread_join_value_v2",
                             "gti_rt_thread_scope_v1"})
      if (const auto *function = session.module().getFunction(name))
        require(function->hasStructRetAttr() && function->doesNotThrow(),
                "large runtime records return through an indirect result");
    for (const auto *name : {"gti_rt_thread_spawn_v1",
                             "gti_rt_thread_spawn_value_v1"})
      if (const auto *function = session.module().getFunction(name))
        require(function->doesNotThrow() &&
                    session.dataLayout().getTypeAllocSize(
                        function->getReturnType()) == 16,
                "spawn status/state returns in two integer registers");
  }
  require(operations >= 10 && adapters >= 6,
          "fixtures exercise every managed-thread operation family");
}

// Manual C-thread services bind only their exact trusted C declarations.
void checkManualCThreadBindings(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  std::set<CThreadRuntimeOperation> bound;
  for (const auto *fixture : {"cthread_create_join.gti", "cthread_mutex.gti",
                              "cthread_condition.gti"}) {
    const auto frontend = Frontend(options).analyze(
        root / "tests/fixtures" / fixture, std::nullopt,
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    require(frontend.canGenerateCode(), "manual C-thread source reaches MIR");
    std::vector<LoweredProgramIssue> issues;
    const auto input = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                               options.target),
        issues);
    require(input.has_value(), "manual C-thread source is verified");
    const auto &program = input->program().mir();
    for (const auto &function : program.functionInstances()) {
      if (function.cThreadOperation == CThreadRuntimeOperation::None)
        continue;
      const bool lifecycle =
          function.cThreadOperation == CThreadRuntimeOperation::Begin ||
          function.cThreadOperation == CThreadRuntimeOperation::End;
      require(nativeCThreadOperationBinding(program, function) != lifecycle,
              "each trusted C-thread declaration has exactly one binding");
      if (lifecycle)
        continue;
      bound.insert(function.cThreadOperation);
      const auto reject = [&](auto mutate, const char *message) {
        auto bad = function;
        mutate(bad);
        require(!nativeCThreadOperationBinding(program, bad), message);
      };
      reject([](auto &row) { row.externalSymbol += "_shadow"; },
             "an operation tag cannot authorize another native symbol");
      reject([](auto &row) { row.linkage = LanguageLinkage::Gti; },
             "the operation is a C declaration");
      reject([](auto &row) { row.parameterTypes.push_back(SemanticType::Int32); },
             "the exact C signature is required");
      reject([](auto &row) {
        row.cThreadOperation = row.cThreadOperation ==
                                       CThreadRuntimeOperation::Create
                                   ? CThreadRuntimeOperation::Join
                                   : CThreadRuntimeOperation::Create;
      }, "a declaration cannot be rebound to another operation");
      reject([](auto &row) { row.definitionKind = MirDefinitionKind::Source; },
             "a source body is not the runtime service");
    }
  }
  for (const auto operation :
       {CThreadRuntimeOperation::Create, CThreadRuntimeOperation::Join,
        CThreadRuntimeOperation::MutexInit, CThreadRuntimeOperation::MutexLock,
        CThreadRuntimeOperation::MutexUnlock,
        CThreadRuntimeOperation::ConditionInit,
        CThreadRuntimeOperation::ConditionWait})
    require(bound.contains(operation),
            "fixtures exercise the manual C-thread service families");
  require(cThreadOperationSynchronization(CThreadRuntimeOperation::Create) ==
                  SynchronizationOperationKind::ThreadSpawn &&
              cThreadOperationSynchronization(
                  CThreadRuntimeOperation::MutexTimedLock) ==
                  SynchronizationOperationKind::MutexLock &&
              cThreadOperationSynchronization(
                  CThreadRuntimeOperation::ConditionBroadcast) ==
                  SynchronizationOperationKind::ConditionNotify &&
              cThreadOperationSynchronization(
                  CThreadRuntimeOperation::MutexInit) ==
                  SynchronizationOperationKind::None,
          "C-thread synchronization records match semantic resolution");
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected repository root");
    check(argv[1]);
    checkManagedThreads(argv[1]);
    checkManualCThreadBindings(argv[1]);
    std::cout << "C-thread startup and managed-thread contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
