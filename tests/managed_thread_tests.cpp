#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_printer.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::TargetInfo concurrentTarget(bool threads = true) {
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = threads;
  return target;
}

lang::FrontendResult analyze(std::string_view name, std::string source,
                             lang::TargetInfo target = concurrentTarget()) {
  lang::FrontendOptions options;
  options.target = std::move(target);
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

bool hasHint(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return std::any_of(
                           diagnostic.hints.begin(), diagnostic.hints.end(),
                           [&](const std::string &hint) {
                             return hint.find(text) != std::string::npos;
                           });
                     });
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::string validProgram() {
  return R"(
#include <std/thread>
#include <std/memory>

class resource {
  int32_t value;
public:
  resource(int32_t value) : value(value) {}
  int32_t read() { return this.value; }
};

class class_task {
public:
  void operator()() { std::println("class read task"); }
  void operator()() mut { std::println("class mutable task"); }
  void operator()() && { std::println("class once task"); }
};

class fresh_task {
public:
  void operator()() { std::println("fresh class task"); }
};

int main() {
  auto first_started = std::thread::start(
      []() -> void { std::println("first task"); });
  if (!first_started.has_value()) { return 1; }
  mut std::thread first = std::move(first_started).value();
  mut std::thread moved = std::move(first);
  auto first_join = moved.join();
  if (!first_join.has_value() || moved.joinable()) { return 2; }

  auto owner = std::make_unique<resource>(42);
  auto second_started = std::thread::start(
      [owned = std::move(owner)]() -> void {
        std::println(owned->read());
      });
  if (!second_started.has_value()) { return 3; }
  mut std::thread second = std::move(second_started).value();
  auto second_join = second.join();
  if (!second_join.has_value()) { return 4; }

  class_task task = class_task();
  auto third_started = std::thread::start(std::move(task));
  if (!third_started.has_value()) { return 5; }
  mut std::thread third = std::move(third_started).value();
  auto third_join = third.join();
  if (!third_join.has_value()) { return 6; }

  {
    auto fourth_started = std::thread::start(fresh_task());
    if (!fourth_started.has_value()) { return 7; }
    mut std::thread fourth = std::move(fourth_started).value();
  }
  std::println("after automatic join");
  return 0;
}
)";
}

template <typename Function>
void forEachInstruction(lang::MirProgram &program, Function &&function) {
  auto visit = [&](lang::MirBody &body) {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        function(instruction);
      }
    }
  };
  visit(const_cast<lang::MirBody &>(program.module()));
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances())) {
    visit(instance.body);
  }
  for (lang::MirConstructorInstance &instance :
       const_cast<std::vector<lang::MirConstructorInstance> &>(
           program.constructorInstances())) {
    visit(instance.body);
  }
  for (lang::MirDestructorInstance &instance :
       const_cast<std::vector<lang::MirDestructorInstance> &>(
           program.destructorInstances())) {
    visit(instance.body);
  }
  for (lang::MirLambdaInstance &instance :
       const_cast<std::vector<lang::MirLambdaInstance> &>(
           program.lambdaInstances())) {
    visit(instance.body);
  }
}

std::size_t synchronizationCount(const lang::MirProgram &program,
                                 lang::SynchronizationOperationKind kind) {
  std::size_t count = 0;
  lang::MirProgram copy = program;
  forEachInstruction(copy, [&](const lang::MirInstruction &instruction) {
    count += instruction.synchronization.kind == kind ? 1 : 0;
  });
  return count;
}

void testDormantImportInSingleThreadedProfile() {
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult frontend =
      analyze("single-threaded-thread-import.gti",
              "#include <std/thread>\nint main() { return 0; }\n", single);
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }

  std::size_t trustedBodies = 0;
  std::size_t untrustedSynchronization = 0;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(frontend.mir)) {
    const lang::MirBody *body = lang::findMirBody(frontend.mir, address);
    if (body == nullptr) {
      continue;
    }
    bool containsSynchronization = false;
    for (const lang::MirBlock &block : body->blocks) {
      containsSynchronization =
          containsSynchronization ||
          std::any_of(block.instructions.begin(), block.instructions.end(),
                      [](const lang::MirInstruction &instruction) {
                        return instruction.synchronization.kind !=
                               lang::SynchronizationOperationKind::None;
                      });
    }
    trustedBodies += body->compilerTrustedSynchronization ? 1 : 0;
    untrustedSynchronization +=
        containsSynchronization && !body->compilerTrustedSynchronization ? 1
                                                                         : 0;
  }

  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "an unused <std/thread> import should remain valid in the default "
         "single-threaded profile");
  expect(trustedBodies >= 2 && untrustedSynchronization == 0 &&
             synchronizationCount(
                 frontend.mir,
                 lang::SynchronizationOperationKind::ThreadSpawn) == 0 &&
             synchronizationCount(
                 frontend.mir,
                 lang::SynchronizationOperationKind::ThreadJoin) >= 2 &&
             frontend.mir.threadTaskAdapters().empty(),
         "dormant library join wrappers should retain exact trusted MIR "
         "records without creating executable task adapters");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, single);
  expect(lang::verifyLoweredProgram(lowered).empty() &&
             !lang::CppBackend(lang::CppStandard::Cpp23)
                  .generate(lowered)
                  .contents.empty(),
         "the dormant thread surface should cross the complete backend "
         "frontier without enabling concurrent execution");
}

void testPublicPipelineAndInventory() {
  const lang::FrontendResult frontend =
      analyze("managed-thread.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the public thread workflow should complete frontend analysis");
  if (!frontend.canGenerateCode()) {
    return;
  }

  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "HIR should verify exact managed-task transport identities");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should verify managed-thread synchronization and adapters");
  expect(frontend.hir.threadTaskAdapters().size() == 4 &&
             frontend.mir.threadTaskAdapters().size() == 4,
         "four concrete task types should produce four exact adapters");
  const std::size_t functionEntries = static_cast<std::size_t>(
      std::count_if(frontend.mir.threadTaskAdapters().begin(),
                    frontend.mir.threadTaskAdapters().end(),
                    [](const lang::MirThreadTaskAdapter &adapter) {
                      return adapter.functionTarget.has_value() &&
                             !adapter.lambdaTarget.has_value();
                    }));
  const std::size_t lambdaEntries = static_cast<std::size_t>(
      std::count_if(frontend.mir.threadTaskAdapters().begin(),
                    frontend.mir.threadTaskAdapters().end(),
                    [](const lang::MirThreadTaskAdapter &adapter) {
                      return !adapter.functionTarget.has_value() &&
                             adapter.lambdaTarget.has_value();
                    }));
  expect(functionEntries == 2 && lambdaEntries == 2,
         "every task adapter should retain one exact function or lambda entry");
  expect(std::any_of(frontend.mir.threadTaskAdapters().begin(),
                     frontend.mir.threadTaskAdapters().end(),
                     [](const lang::MirThreadTaskAdapter &adapter) {
                       return adapter.functionTarget.has_value() &&
                              adapter.targetInvocation ==
                                  lang::CallableInvocationCapability::Once;
                     }),
         "the overloaded class task should retain its exact consuming entry");
  const std::size_t sourceSpawnCount = synchronizationCount(
      frontend.mir, lang::SynchronizationOperationKind::ThreadSpawn);
  const std::size_t sourceJoinCount = synchronizationCount(
      frontend.mir, lang::SynchronizationOperationKind::ThreadJoin);
  expect(sourceSpawnCount == 4 && sourceJoinCount >= 2,
         "spawn and join should remain explicit backend-independent records");

  const lang::TargetInfo target = concurrentTarget();
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "LoweredProgram should accept the complete managed-task inventory");
  std::vector<std::size_t> adapterIds;
  for (const lang::LoweredGeneratedItem &item : lowered.generatedItems()) {
    if (item.identity.kind !=
        lang::LoweredGeneratedItemKind::ThreadTaskAdapter) {
      continue;
    }
    const auto *payload =
        std::get_if<lang::LoweredThreadTaskAdapterItem>(&item.payload);
    if (payload != nullptr) {
      adapterIds.push_back(payload->adapter.id);
    }
  }
  expect(adapterIds == std::vector<std::size_t>({1, 2, 3, 4}),
         "lowering should publish a deterministic atomic adapter inventory");

  const std::string loweredText = lang::LoweredProgramPrinter().print(lowered);
  expect(loweredText.find("payload=thread-task:") != std::string::npos,
         "the deterministic lowered dump should expose task adapter payloads");

  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = target});
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "O3 should retain a valid managed-thread MIR program");
  expect(
      synchronizationCount(optimized.mir,
                           lang::SynchronizationOperationKind::ThreadSpawn) ==
              sourceSpawnCount &&
          synchronizationCount(
              optimized.mir, lang::SynchronizationOperationKind::ThreadJoin) ==
              sourceJoinCount &&
          optimized.mir.threadTaskAdapters() ==
              frontend.mir.threadTaskAdapters(),
      "O3 must not remove, duplicate, retarget, or reorder task adapters");

  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(
      cpp20.find("managed_thread_spawn<") != std::string::npos &&
          cpp20.find("managed_thread_join(") != std::string::npos &&
          cpp23.find("managed_thread_task_adapter<") != std::string::npos &&
          cpp23.find("gti_rt_failure_terminate_v1") != std::string::npos,
      "both C++ standards should consume lowered task and failure authority");
}

void testTypedResultPipeline() {
  const lang::FrontendResult frontend = analyze("managed-thread-result.gti", R"(
#include <std/thread>

class result_task {
  int32_t value;
public:
  result_task(int32_t value) : value(value) {}
  int32_t operator()() && { return this.value; }
};

int main() {
  mut std::thread_result<int32_t> first =
      std::thread_result<int32_t>();
  auto first_started = first.start(result_task(42));
  if (!first_started.has_value()) { return 1; }

  auto first_joined = first.join();
  if (!first_joined.has_value()) { return 2; }

  auto restarted = first.start([]() -> int32_t { return 9; });
  if (!restarted.has_value()) { return 3; }
  auto rejoined = first.join();
  if (!rejoined.has_value()) { return 4; }

  mut std::thread_result<int32_t> abandoned =
      std::thread_result<int32_t>();
  auto abandoned_started = abandoned.start(result_task(7));
  if (!abandoned_started.has_value()) { return 5; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "typed result tasks should retain exact semantic, HIR, and MIR "
         "contracts");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.threadTaskAdapters().size() == 2 &&
             std::all_of(frontend.mir.threadTaskAdapters().begin(),
                         frontend.mir.threadTaskAdapters().end(),
                         [](const lang::MirThreadTaskAdapter &adapter) {
                           return adapter.resultType ==
                                  lang::SemanticType::Int32;
                         }),
         "named and closure result tasks should instantiate two exact int32 "
         "adapters");
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, concurrentTarget());
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "typed task results should retain exact lowered-program authority");
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(cpp20.find("managed_thread_spawn_value<") != std::string::npos &&
             cpp20.find("managed_thread_spawn_value<"
                        "__gti_thread_task_adapter, std::int32_t>") !=
                 std::string::npos &&
             cpp20.find("managed_thread_take_join_result<") !=
                 std::string::npos &&
             cpp23.find("gti_rt_thread_join_value_v1") != std::string::npos &&
             cpp23.find("managed_thread_abandon_join_result(") !=
                 std::string::npos,
         "both C++ standards should emit typed spawn, join, and abandon "
         "ownership transitions");
}

void expectThreadRejection(std::string_view name, std::string source,
                           std::string_view message,
                           bool requireRelatedCause = false) {
  const lang::FrontendResult result = analyze(name, std::move(source));
  const bool hasRelatedCause = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2084" && !diagnostic.related.empty();
      });
  const bool rejected =
      !result.canGenerateCode() && hasCode(result, "GTI-S2084") &&
      hasMessage(result, message) && !hasCode(result, "GTI-B0001") &&
      (!requireRelatedCause || hasRelatedCause);
  if (!rejected) {
    std::cerr << "Diagnostics for " << name << ":\n";
    printDiagnostics(result);
  }
  expect(rejected, std::string(name) +
                       " should fail at the managed-thread semantic boundary");
}

void testSemanticRejections() {
  const std::string basic = R"(
#include <std/thread>
int main() {
  auto started = std::thread::start([]() -> void {});
  return 0;
}
)";
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult singleResult =
      analyze("single-threaded-thread.gti", basic, single);
  expect(!singleResult.canGenerateCode() &&
             hasCode(singleResult, "GTI-S2084") &&
             hasMessage(singleResult, "concurrent execution profile") &&
             !hasCode(singleResult, "GTI-B0001"),
         "the default execution profile should reject thread before backend "
         "entry");

  lang::TargetInfo unsupportedTarget = lang::TargetInfo::host();
  unsupportedTarget.executionProfile = lang::ExecutionProfile::Concurrent;
  const lang::FrontendResult unsupported =
      analyze("unsupported-thread-target.gti", basic, unsupportedTarget);
  expect(!unsupported.canGenerateCode() && hasCode(unsupported, "GTI-S2084") &&
             hasMessage(unsupported, "does not provide managed threads") &&
             !hasCode(unsupported, "GTI-B0001") &&
             !unsupportedTarget.runtimeCapabilities.threads,
         "a raw target's safe-default runtime profile should reject managed "
         "threads before lowering");

  expectThreadRejection("nonvoid-thread-task.gti", R"(
#include <std/thread>
int main() {
  auto started = std::thread::start([]() -> int32_t { return 1; });
  return 0;
}
)",
                        "exact void() task");
  expectThreadRejection("parameter-thread-task.gti", R"(
#include <std/thread>
int main() {
  auto started = std::thread::start([](int32_t value) -> void {});
  return 0;
}
)",
                        "exact void() task");
  expectThreadRejection("borrowed-thread-capture.gti", R"(
#include <std/thread>
int main() {
  std::string_view text = "borrowed";
  auto task = [text]() -> void {};
  auto started = std::thread::start(std::move(task));
  return 0;
}
)",
                        "transfer-safe exact void() task", true);
  expectThreadRejection("raw-thread-capture.gti", R"(
#include <std/thread>
int main() {
  int32_t value = 1;
  unsafe {
    int32_t* raw = &value;
    auto task = [raw]() -> void {};
    auto started = std::thread::start(std::move(task));
  }
  return 0;
}
)",
                        "transfer-safe exact void() task", true);
  expectThreadRejection("nontransfer-thread-capture.gti", R"(
#include <std/thread>
[[no_transfer, no_share]] class local_only { int32_t value = 0; };
int main() {
  local_only state = local_only();
  auto task = [state]() -> void {};
  auto started = std::thread::start(std::move(task));
  return 0;
}
)",
                        "transfer-safe exact void() task", true);

  expectThreadRejection("mismatched-thread-result-task.gti", R"(
#include <std/thread>
int main() {
  mut std::thread_result<int32_t> worker =
      std::thread_result<int32_t>();
  auto started = worker.start([]() -> bool { return true; });
  return 0;
}
)",
                        "return type matches");
  const lang::FrontendResult borrowedResultType =
      analyze("borrowed-thread-result.gti", R"(
#include <std/thread>
int main() {
  mut std::thread_result<int32_t&> worker =
      std::thread_result<int32_t&>();
  return 0;
}
)");
  expect(!borrowedResultType.canGenerateCode() &&
             hasCode(borrowedResultType, "GTI-S2017") &&
             hasMessage(borrowedResultType,
                        "References cannot be nested inside another type") &&
             !hasCode(borrowedResultType, "GTI-B0001"),
         "a borrowed result should fail at the general nested-reference type "
         "boundary before managed-thread lowering");
  expectThreadRejection("raw-thread-result.gti", R"(
#include <std/thread>
int main() {
  mut std::thread_result<int32_t*> worker =
      std::thread_result<int32_t*>();
  return 0;
}
)",
                        "owned non-void value type");
  expectThreadRejection("nontransfer-thread-result.gti", R"(
#include <std/thread>
class local_result {
  int32_t value;
public:
  local_result(int32_t value) : value(value) {}
  local_result(local_result& other) = delete;
  local_result(local_result&& other) = default;
  ~local_result() {}
};
int main() {
  mut std::thread_result<local_result> worker =
      std::thread_result<local_result>();
  auto started = worker.start([]() -> local_result {
    return local_result(1);
  });
  return 0;
}
)",
                        "transfer-safe");

  const lang::FrontendResult missingMove =
      analyze("thread-task-needs-move.gti", R"(
#include <std/thread>
int main() {
  auto task = []() -> void {};
  auto started = std::thread::start(task);
  return 0;
}
)");
  expect(!missingMove.canGenerateCode() && hasCode(missingMove, "GTI-S2046") &&
             hasMessage(missingMove, "explicit std::move") &&
             !hasCode(missingMove, "GTI-B0001"),
         "a named thread task should require one explicit ownership move");

  const lang::FrontendResult copiedHandle = analyze("copy-thread.gti", R"(
#include <std/thread>
int main() {
  auto started = std::thread::start([]() -> void {});
  if (!started.has_value()) { return 1; }
  mut std::thread first = std::move(started).value();
  std::thread copied = first;
  return 0;
}
)");
  expect(!copiedHandle.canGenerateCode() && !hasCode(copiedHandle, "GTI-B0001"),
         "the public thread handle should be move-only");

  const lang::FrontendResult assignedHandle =
      analyze("move-assign-thread.gti", R"(
#include <std/thread>
int main() {
  auto first_started = std::thread::start([]() -> void {});
  auto second_started = std::thread::start([]() -> void {});
  if (!first_started.has_value() || !second_started.has_value()) { return 1; }
  mut std::thread first = std::move(first_started).value();
  mut std::thread second = std::move(second_started).value();
  first = std::move(second);
  return 0;
}
)");
  expect(!assignedHandle.canGenerateCode() &&
             hasCode(assignedHandle, "GTI-S2003") &&
             hasHint(assignedHandle, "does not support move assignment") &&
             !hasHint(assignedHandle, "use std::move(owner)") &&
             !hasCode(assignedHandle, "GTI-B0001"),
         "thread move assignment should stop in semantics with a correction "
         "that does not repeat the already-written std::move");

  const lang::FrontendResult privateHandle =
      analyze("private-thread-handle.gti", R"(
int main() {
  gti_internal::thread_handle handle;
  return 0;
}
)");
  expect(!privateHandle.canGenerateCode() &&
             hasMessage(privateHandle, "Compiler-private") &&
             !hasCode(privateHandle, "GTI-B0001"),
         "application source should not be able to name the private handle");
}

lang::MirInstruction *findIntrinsic(lang::MirProgram &program,
                                    lang::IntrinsicKind kind) {
  lang::MirInstruction *found = nullptr;
  forEachInstruction(program, [&](lang::MirInstruction &instruction) {
    if (found == nullptr && instruction.intrinsic == kind) {
      found = &instruction;
    }
  });
  return found;
}

void testVerifierMutations() {
  const lang::FrontendResult frontend =
      analyze("managed-thread-verifier.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram missing = frontend.mir;
  const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
      missing.threadTaskAdapters())
      .clear();
  expect(!lang::verifyMirProgram(missing).valid(),
         "MIR should reject a missing task adapter inventory");

  lang::MirProgram stripped = frontend.mir;
  lang::MirInstruction *strippedSpawn =
      findIntrinsic(stripped, lang::IntrinsicKind::ThreadSpawn);
  if (strippedSpawn != nullptr) {
    strippedSpawn->threadTaskAdapter.reset();
  }
  const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
      stripped.threadTaskAdapters())
      .clear();
  expect(strippedSpawn != nullptr && !lang::verifyMirProgram(stripped).valid(),
         "MIR should reject a spawn whose adapter reference and inventory row "
         "were both removed");

  lang::MirProgram duplicated = frontend.mir;
  auto &duplicateAdapters =
      const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
          duplicated.threadTaskAdapters());
  duplicateAdapters.push_back(duplicateAdapters.front());
  expect(!lang::verifyMirProgram(duplicated).valid(),
         "MIR should reject a duplicated task adapter identity");

  lang::MirProgram reordered = frontend.mir;
  auto &reorderedAdapters =
      const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
          reordered.threadTaskAdapters());
  std::swap(reorderedAdapters[0], reorderedAdapters[1]);
  expect(!lang::verifyMirProgram(reordered).valid(),
         "MIR should reject reordered adapter rows whose IDs no longer index "
         "them");

  lang::MirProgram retargeted = frontend.mir;
  auto &retargetedAdapters =
      const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
          retargeted.threadTaskAdapters());
  retargetedAdapters.front().spawnInstruction = 0;
  expect(!lang::verifyMirProgram(retargeted).valid(),
         "MIR should reject an adapter detached from its exact spawn");

  lang::MirProgram missingEntry = frontend.mir;
  auto &missingEntryAdapters =
      const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
          missingEntry.threadTaskAdapters());
  missingEntryAdapters.front().lambdaTarget.reset();
  missingEntryAdapters.front().functionTarget.reset();
  expect(!lang::verifyMirProgram(missingEntry).valid(),
         "MIR should reject a task adapter without its exact callable entry");

  lang::MirProgram forgedEntryCapability = frontend.mir;
  auto &forgedEntryAdapters =
      const_cast<std::vector<lang::MirThreadTaskAdapter> &>(
          forgedEntryCapability.threadTaskAdapters());
  forgedEntryAdapters.back().targetInvocation =
      lang::CallableInvocationCapability::Once;
  expect(!lang::verifyMirProgram(forgedEntryCapability).valid(),
         "MIR should reject a task adapter with a forged entry capability");

  lang::MirProgram forgedSpawn = frontend.mir;
  lang::MirInstruction *spawn =
      findIntrinsic(forgedSpawn, lang::IntrinsicKind::ThreadSpawn);
  if (spawn != nullptr) {
    spawn->synchronization.kind =
        lang::SynchronizationOperationKind::ThreadJoin;
  }
  expect(spawn != nullptr && !lang::verifyMirProgram(forgedSpawn).valid(),
         "MIR should reject forged spawn synchronization identity");

  lang::MirProgram forgedJoin = frontend.mir;
  lang::MirInstruction *join =
      findIntrinsic(forgedJoin, lang::IntrinsicKind::ThreadJoin);
  if (join != nullptr) {
    join->synchronization.kind = lang::SynchronizationOperationKind::None;
  }
  expect(join != nullptr && !lang::verifyMirProgram(forgedJoin).valid(),
         "MIR should reject a join without its synchronization record");

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, concurrentTarget());
  lang::LoweredProgram loweredMissing = lowered;
  auto &missingItems = const_cast<std::vector<lang::LoweredGeneratedItem> &>(
      loweredMissing.generatedItems());
  const auto missingItem =
      std::find_if(missingItems.begin(), missingItems.end(),
                   [](const lang::LoweredGeneratedItem &item) {
                     return item.identity.kind ==
                            lang::LoweredGeneratedItemKind::ThreadTaskAdapter;
                   });
  if (missingItem != missingItems.end()) {
    missingItems.erase(missingItem);
  }
  expect(!lang::verifyLoweredProgram(loweredMissing).empty(),
         "LoweredProgram should reject a missing generated task item");

  lang::LoweredProgram loweredReordered = lowered;
  auto &items = const_cast<std::vector<lang::LoweredGeneratedItem> &>(
      loweredReordered.generatedItems());
  std::vector<std::size_t> threadRows;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (items[index].identity.kind ==
        lang::LoweredGeneratedItemKind::ThreadTaskAdapter) {
      threadRows.push_back(index);
    }
  }
  if (threadRows.size() >= 2) {
    std::swap(items[threadRows[0]], items[threadRows[1]]);
  }
  expect(threadRows.size() >= 2 &&
             !lang::verifyLoweredProgram(loweredReordered).empty(),
         "LoweredProgram should reject reordered generated inventory");
}

} // namespace

int main() {
  testDormantImportInSingleThreadedProfile();
  testPublicPipelineAndInventory();
  testTypedResultPipeline();
  testSemanticRejections();
  testVerifierMutations();
  return failures == 0 ? 0 : 1;
}
