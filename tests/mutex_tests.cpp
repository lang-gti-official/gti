#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
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

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::string validProgram() {
  return R"(
#include <std/mutex>

std::mutex<int32_t> program_value{1};

int main() {
  {
    mut std::mutex_guard<int32_t> first = program_value.lock();
    mut std::mutex_guard<int32_t> moved = std::move(first);
    mut int32_t& current = moved.get();
    current = current + 1;
  }
  std::mutex_guard<int32_t> guard = program_value.lock();
  return guard.get() == 2 ? 0 : 1;
}
)";
}

template <typename Function>
void forEachInstruction(lang::MirProgram &program, Function &&function) {
  const auto visit = [&](lang::MirBody &body) {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        function(body, instruction);
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

bool isMutex(lang::IntrinsicKind kind) {
  return kind == lang::IntrinsicKind::MutexLock ||
         kind == lang::IntrinsicKind::MutexUnlock ||
         kind == lang::IntrinsicKind::MutexGuardActive ||
         kind == lang::IntrinsicKind::MutexGuardRead ||
         kind == lang::IntrinsicKind::MutexGuardReadMut ||
         kind == lang::IntrinsicKind::MutexLockFailure;
}

lang::MirInstruction *findMutex(lang::MirProgram &program,
                                lang::IntrinsicKind kind) {
  lang::MirInstruction *found = nullptr;
  forEachInstruction(program,
                     [&](lang::MirBody &, lang::MirInstruction &instruction) {
                       if (found == nullptr && instruction.intrinsic == kind) {
                         found = &instruction;
                       }
                     });
  return found;
}

void testDormantImport() {
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult frontend =
      analyze("dormant-mutex-import.gti",
              "#include <std/mutex>\nint main() { return 0; }\n", single);
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "an unused mutex import should remain valid in the default "
         "single-threaded profile");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, single);
  const std::string cpp =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(lang::verifyLoweredProgram(lowered).empty() &&
             cpp.find("#include <mutex>") == std::string::npos &&
             cpp.find("class mutex_value") == std::string::npos,
         "a dormant mutex import should not enable native mutex support");
}

void testPublicPipeline() {
  const lang::FrontendResult frontend =
      analyze("mutex-public-pipeline.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "public mutex, movable guard, and owner-bound access should complete "
         "semantics, HIR, and MIR verification");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::size_t mutexInstructions = 0;
  std::size_t untrustedMutexBodies = 0;
  lang::MirProgram inspected = frontend.mir;
  forEachInstruction(
      inspected, [&](lang::MirBody &body, lang::MirInstruction &instruction) {
        if (!isMutex(instruction.intrinsic)) {
          return;
        }
        ++mutexInstructions;
        const bool lock = instruction.intrinsic == lang::IntrinsicKind::MutexLock;
        const bool unlock =
            instruction.intrinsic == lang::IntrinsicKind::MutexUnlock;
        untrustedMutexBodies +=
            (lock || unlock) && !body.compilerTrustedSynchronization ? 1 : 0;
        expect(instruction.synchronization.kind ==
                   (lock ? lang::SynchronizationOperationKind::MutexLock
                         : unlock
                               ? lang::SynchronizationOperationKind::MutexUnlock
                               : lang::SynchronizationOperationKind::None) &&
                   !instruction.synchronization.order &&
                   !instruction.synchronization.failureOrder,
               "each private mutex intrinsic should retain its exact "
               "synchronization identity");
      });
  expect(mutexInstructions >= 6 && untrustedMutexBodies == 0,
         "public mutex wrappers should lower lock/unlock only through trusted "
         "private synchronization bodies");

  const lang::TargetInfo target = concurrentTarget();
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = target});
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "O3 should preserve the verified guard lifecycle and mutex barriers");

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, target,
      optimized.report.transformations);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "mutex synchronization should cross the sealed LoweredProgram "
         "boundary");
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  for (const std::string *cpp : {&cpp20, &cpp23}) {
    expect(cpp->find("#include <mutex>") != std::string::npos &&
               cpp->find("class mutex_value") != std::string::npos &&
               cpp->find("class mutex_lock_token") != std::string::npos &&
               cpp->find("mutex_lock(") != std::string::npos &&
               cpp->find("mutex_unlock(") != std::string::npos,
           "both native standards should consume the sealed mutex and guard "
           "vocabulary");
  }
}

void testSemanticBoundaries() {
  const std::string use = R"(
#include <std/mutex>
int main() {
  std::mutex<int32_t> value{1};
  std::mutex_guard<int32_t> guard = value.lock();
  return guard.get();
}
)";
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult singleResult =
      analyze("mutex-single-profile.gti", use, single);
  expect(!singleResult.canGenerateCode() && hasCode(singleResult, "GTI-S2084") &&
             hasMessage(singleResult, "concurrent execution profile") &&
             !hasCode(singleResult, "GTI-B0001"),
         "mutex use should fail before lowering in the single-threaded "
         "profile");

  const lang::FrontendResult unsupported =
      analyze("mutex-unsupported-target.gti", use, concurrentTarget(false));
  expect(!unsupported.canGenerateCode() &&
             hasCode(unsupported, "GTI-S2084") &&
             hasMessage(unsupported,
                        "does not provide thread-capable concurrency support") &&
             !hasCode(unsupported, "GTI-B0001"),
         "mutex use should fail before lowering on a target without threads");

  const lang::FrontendResult privateStorage =
      analyze("mutex-private-storage.gti", R"(
#include <std/mutex>
int main() {
  gti_internal::mutex_storage<int32_t> storage{1};
  return 0;
}
)");
  expect(!privateStorage.canGenerateCode() &&
             hasMessage(privateStorage, "Compiler-private") &&
             !hasCode(privateStorage, "GTI-B0001"),
         "application code should not name private mutex storage");

  const lang::FrontendResult copied = analyze("mutex-copy.gti", R"(
#include <std/mutex>
int main() {
  std::mutex<int32_t> first{1};
  std::mutex<int32_t> second = first;
  return 0;
}
)");
  expect(!copied.canGenerateCode() && !hasCode(copied, "GTI-B0001"),
         "the public mutex owner should be move-only");

  const lang::FrontendResult relocked = analyze("mutex-relock.gti", R"(
#include <std/mutex>
int main() {
  std::mutex<int32_t> value{1};
  std::mutex_guard<int32_t> first = value.lock();
  std::mutex_guard<int32_t> second = value.lock();
  return 0;
}
)");
  expect(!relocked.canGenerateCode() && hasCode(relocked, "GTI-S2017") &&
             !hasCode(relocked, "GTI-B0001"),
         "an outstanding guard should statically exclude relocking its owner");

  const lang::FrontendResult movedOwner = analyze("mutex-move-owner.gti", R"(
#include <std/mutex>
int main() {
  std::mutex<int32_t> value{1};
  std::mutex_guard<int32_t> guard = value.lock();
  std::mutex<int32_t> moved = std::move(value);
  return 0;
}
)");
  expect(!movedOwner.canGenerateCode() && hasCode(movedOwner, "GTI-S2017") &&
             !hasCode(movedOwner, "GTI-B0001"),
         "an outstanding guard should statically exclude moving its owner");

  for (const auto &[name, source] :
       std::vector<std::pair<std::string, std::string>>{
           {"temporary", R"(
#include <std/mutex>
int32_t& escape(std::mutex<int32_t>& value) {
  return value.lock().get();
}
)"},
           {"local", R"(
#include <std/mutex>
int32_t& escape(std::mutex<int32_t>& value) {
  std::mutex_guard<int32_t> guard = value.lock();
  return guard.get();
}
)"}}) {
    const lang::FrontendResult escaped =
        analyze("mutex-escape-" + name + ".gti", source);
    expect(!escaped.canGenerateCode() && hasCode(escaped, "GTI-S2017") &&
               !hasCode(escaped, "GTI-B0001"),
           "guard-derived access should not outlive the temporary or local "
           "guard");
  }

  const lang::FrontendResult transferred = analyze("mutex-transfer.gti", R"(
#include <std/mutex>
#include <std/thread>
int main() {
  std::mutex<int32_t> value{1};
  mut std::mutex_guard<int32_t> guard = value.lock();
  auto started = std::thread::start([owned = std::move(guard)]() -> void {});
  return 0;
}
)");
  expect(!transferred.canGenerateCode() &&
             !hasCode(transferred, "GTI-B0001"),
         "a guard should not cross the thread-transfer boundary");
}

void testVerifierMutations() {
  const lang::FrontendResult frontend =
      analyze("mutex-verifier.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram wrongLock = frontend.mir;
  lang::MirInstruction *lock =
      findMutex(wrongLock, lang::IntrinsicKind::MutexLock);
  if (lock != nullptr) {
    lock->synchronization.kind =
        lang::SynchronizationOperationKind::MutexUnlock;
  }
  expect(lock != nullptr && !lang::verifyMirProgram(wrongLock).valid(),
         "MIR should reject a forged mutex lock operation identity");

  lang::MirProgram orderedUnlock = frontend.mir;
  lang::MirInstruction *unlock =
      findMutex(orderedUnlock, lang::IntrinsicKind::MutexUnlock);
  if (unlock != nullptr) {
    unlock->synchronization.order =
        lang::AtomicMemoryOrder::SequentiallyConsistent;
  }
  expect(unlock != nullptr && !lang::verifyMirProgram(orderedUnlock).valid(),
         "MIR should reject an atomic order attached to mutex unlock");

  lang::MirProgram strippedIntrinsic = frontend.mir;
  lang::MirInstruction *stripped =
      findMutex(strippedIntrinsic, lang::IntrinsicKind::MutexGuardRead);
  if (stripped != nullptr) {
    stripped->intrinsic = lang::IntrinsicKind::None;
  }
  expect(stripped != nullptr &&
             !lang::verifyMirProgram(strippedIntrinsic).valid(),
         "MIR should reject a guard loan detached from its private intrinsic");

  lang::MirProgram staleTrust = frontend.mir;
  lang::MirInstruction *read =
      findMutex(staleTrust, lang::IntrinsicKind::MutexGuardRead);
  bool changedTrust = false;
  if (read != nullptr) {
    forEachInstruction(
        staleTrust,
        [&](lang::MirBody &body, lang::MirInstruction &instruction) {
          if (&instruction == read) {
            body.compilerTrustedSynchronization = true;
            changedTrust = true;
          }
        });
  }
  expect(changedTrust && !lang::verifyMirProgram(staleTrust).valid(),
         "MIR should reject a stale trusted-synchronization marker on a "
         "guard-read body with no synchronization operation");
}

} // namespace

int main() {
  testDormantImport();
  testPublicPipeline();
  testSemanticBoundaries();
  testVerifierMutations();
  return failures == 0 ? 0 : 1;
}
