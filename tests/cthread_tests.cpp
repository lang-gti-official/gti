#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string source, bool concurrent = true,
                             bool threads = true) {
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  lang::FrontendOptions options;
  options.target.executionProfile =
      concurrent ? lang::ExecutionProfile::Concurrent
                 : lang::ExecutionProfile::SingleThreaded;
  options.target.runtimeCapabilities.threads = threads;
  return lang::Frontend(options).analyze(
      "cthread.gti", std::move(source), {stdlib / "prelude.gti"}, {}, {stdlib});
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
}

const std::string source = R"(
#include <std/cthread>
int32_t worker(void* context) { return context == nullptr ? 42 : 7; }
int main() {
  mut std::thrd_t handle = 0;
  mut int32_t result = 0;
  unsafe {
    if (std::thrd_create(handle, worker, nullptr) != std::thrd_success) { return 1; }
    if (std::thrd_join(handle, result) != std::thrd_success) { return 2; }
  }
  return result == 42 ? 0 : 3;
}
)";

void testIdentity() {
  const std::string identitySource = R"(
#include <std/cthread>
int main() {
  std::thrd_t identity = std::thrd_current();
  return std::thrd_equal(identity, std::thrd_current()) == 1 ? 0 : 1;
}
)";
  const auto frontend = analyze(identitySource, false, false);
  expect(frontend.canGenerateCode(),
         "safe identity queries require neither concurrency profile nor thread "
         "creation support");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.hostedStartupPlan() &&
             frontend.mir.hostedStartupPlan()->cThreadBegin == 0 &&
             frontend.mir.nativeCallbackAdapters().empty(),
         "identity observation starts no service and authorizes no callback");
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.functionTarget) {
          continue;
        }
        const auto *target =
            frontend.mir.findFunctionInstance(*instruction.functionTarget);
        if (!target || target->externalSymbol != "gti_rt_cthread_current_v1") {
          continue;
        }
        found = true;
        const auto effect = lang::effects(frontend.mir, instruction);
        expect(instruction.intrinsic == lang::IntrinsicKind::None &&
                   instruction.synchronization.kind ==
                       lang::SynchronizationOperationKind::None &&
                   target->cThreadOperation ==
                       lang::CThreadRuntimeOperation::None &&
                   !effect.removableWhenUnused && !effect.reorderable,
               "identity is an ordinary retained native call, not "
               "synchronization or attachment");
      }
    }
  }
  expect(found, "identity wrapper has an exact MIR native target");
  const auto concurrent = analyze(identitySource);
  expect(concurrent.canGenerateCode(),
         "identity also works in the concurrent profile");
  if (!concurrent.canGenerateCode()) {
    return;
  }
  auto changed = concurrent.mir;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           changed.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (!instruction.functionTarget) {
          continue;
        }
        const auto *target =
            changed.findFunctionInstance(*instruction.functionTarget);
        if (target && target->externalSymbol == "gti_rt_cthread_current_v1") {
          instruction.synchronization.kind =
              lang::SynchronizationOperationKind::ThreadJoin;
        }
      }
    }
  }
  expect(!lang::verifyMirProgram(changed).valid(),
         "identity cannot be forged into a synchronization operation");
}

void testSleep() {
  const auto frontend = analyze(R"(
#include <std/cthread>
int main() {
  mut std::timespec duration = std::timespec();
  return std::thrd_sleep(duration, duration);
}
)",
                                false, false);
  expect(frontend.canGenerateCode(),
         "safe sleep and in-place retry need no concurrent profile or "
         "thread-creation capability");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.hostedStartupPlan() &&
             frontend.mir.hostedStartupPlan()->cThreadBegin == 0 &&
             frontend.mir.nativeCallbackAdapters().empty(),
         "sleep acquires no manual service lifecycle or callback authority");
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.functionTarget) {
          continue;
        }
        const auto *target =
            frontend.mir.findFunctionInstance(*instruction.functionTarget);
        if (!target || target->externalSymbol != "gti_rt_cthread_sleep_v1") {
          continue;
        }
        found = true;
        const auto effect = lang::effects(frontend.mir, instruction);
        expect(instruction.intrinsic == lang::IntrinsicKind::None &&
                   instruction.synchronization.kind ==
                       lang::SynchronizationOperationKind::None &&
                   target->cThreadOperation ==
                       lang::CThreadRuntimeOperation::None &&
                   !effect.removableWhenUnused && !effect.reorderable,
               "sleep stays an exact native call and conservative effect "
               "barrier without synchronization meaning");
      }
    }
  }
  expect(found, "the source sleep wrapper retains its native MIR target");
  const auto unsafeCall = analyze(R"(
#include <std/cthread>
int main() { std::timespec* pointer = nullptr; return std::thrd_sleep(pointer, pointer); }
)",
                                  false);
  expect(!unsafeCall.canGenerateCode() && hasCode(unsafeCall, "GTI-S2055"),
         "raw sleep requires the caller's explicit unsafe acknowledgement");
  const auto readOnlyOutput = analyze(R"(
#include <std/cthread>
int main() { std::timespec value = std::timespec(); return std::thrd_sleep(value, value); }
)",
                                      false);
  expect(!readOnlyOutput.canGenerateCode(),
         "safe sleep cannot write an immutable output");
}

void testMutex() {
  const std::string mutexSource = R"(
#include <std/cthread>
int main() {
  mut std::mtx_t mutex = 0;
  unsafe {
    if (std::mtx_init(mutex, std::mtx_timed) != std::thrd_success) { return 1; }
    if (std::mtx_lock(mutex) != std::thrd_success) { return 2; }
    if (std::mtx_unlock(mutex) != std::thrd_success) { return 3; }
    std::mtx_destroy(mutex);
  }
  return 0;
}
)";
  const auto frontend = analyze(mutexSource);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "manual mutex wrappers reach verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }
  std::size_t acquires = 0;
  std::size_t releases = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.synchronization.kind ==
                lang::SynchronizationOperationKind::MutexLock ||
            instruction.synchronization.kind ==
                lang::SynchronizationOperationKind::MutexUnlock) {
          const auto effect = lang::effects(frontend.mir, instruction);
          expect(instruction.intrinsic == lang::IntrinsicKind::None &&
                     instruction.functionTarget && effect.maySynchronize &&
                     !effect.removableWhenUnused && !effect.reorderable,
                 "native mutex synchronization remains an optimizer barrier");
          if (instruction.synchronization.kind ==
              lang::SynchronizationOperationKind::MutexLock) {
            ++acquires;
          } else {
            ++releases;
          }
        }
      }
    }
  }
  expect(acquires == 3 && releases == 1,
         "lock, try, timed acquire and unlock keep exact records");
  const auto single = analyze(mutexSource, false);
  const auto unavailable = analyze(mutexSource, true, false);
  expect(!single.canGenerateCode() && hasCode(single, "GTI-S2084") &&
             !unavailable.canGenerateCode() &&
             hasCode(unavailable, "GTI-S2084"),
         "every manual mutex operation requires concurrent profile and target "
         "support");
  const auto safe = analyze(R"(
#include <std/cthread>
int main() { return std::mtx_lock(uint64_t(0)); }
)");
  expect(!safe.canGenerateCode() && hasCode(safe, "GTI-S2055"),
         "scalar handles do not erase manual lock/lifetime caller obligations");
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto changed = frontend.mir;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             changed.functionInstances())) {
      if (function.cThreadOperation ==
          lang::CThreadRuntimeOperation::MutexLock) {
        if (mutation == 2) {
          function.externalSymbol = "untrusted_lock";
        }
        if (mutation == 3) {
          function.parameterTypes = {lang::SemanticType::Int32};
        }
      }
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.synchronization.kind ==
              lang::SynchronizationOperationKind::MutexLock) {
            if (mutation == 0) {
              instruction.synchronization = {};
            }
            if (mutation == 1) {
              instruction.synchronization.kind =
                  lang::SynchronizationOperationKind::MutexUnlock;
            }
          }
        }
      }
    }
    expect(!lang::verifyMirProgram(changed).valid(),
           "missing/reversed synchronization and forged native "
           "identity/signature fail closed");
  }
}

void testCondition() {
  const std::string text = R"(
#include <std/cthread>
int main() {
  mut std::cnd_t condition = 0;
  mut std::mtx_t mutex = 0;
  unsafe {
    if (std::cnd_init(condition) != 0) { return 1; }
    if (std::mtx_init(mutex, std::mtx_plain) != 0) { return 2; }
    if (std::mtx_lock(mutex) != 0) { return 3; }
    int32_t status = std::cnd_timedwait(condition, mutex, std::timespec(int64_t(0), int64_t(0)));
    if (std::mtx_unlock(mutex) != 0) { return 5; }
    std::cnd_destroy(condition);
    std::mtx_destroy(mutex);
    if (status != std::thrd_timedout) { return 4; }
  }
  return 0;
}
)";
  const auto frontend = analyze(text);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(), "condition operations reach verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }
  std::size_t waits = 0, notifications = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto kind = instruction.synchronization.kind;
        if (kind == lang::SynchronizationOperationKind::ConditionWait ||
            kind == lang::SynchronizationOperationKind::ConditionNotify) {
          const auto effect = lang::effects(frontend.mir, instruction);
          expect(instruction.intrinsic == lang::IntrinsicKind::None &&
                     instruction.functionTarget && effect.maySynchronize &&
                     !effect.removableWhenUnused && !effect.reorderable,
                 "condition calls are exact native optimizer barriers");
          kind == lang::SynchronizationOperationKind::ConditionWait
              ? ++waits
              : ++notifications;
        }
      }
    }
  }
  expect(waits == 2 && notifications == 2,
         "both wait and notification forms retain records");
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             frontend.semantics.analysisSeal().target);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "condition records survive detachment");
  for (const auto operation :
       {lang::CThreadRuntimeOperation::ConditionInit,
        lang::CThreadRuntimeOperation::ConditionDestroy,
        lang::CThreadRuntimeOperation::ConditionSignal,
        lang::CThreadRuntimeOperation::ConditionBroadcast,
        lang::CThreadRuntimeOperation::ConditionWait,
        lang::CThreadRuntimeOperation::ConditionTimedWait}) {
    for (int mutation = 0; mutation < 2; ++mutation) {
      auto changed = frontend.mir;
      bool found = false;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               changed.functionInstances())) {
        if (function.cThreadOperation == operation) {
          found = true;
          if (mutation == 0) {
            function.externalSymbol = "untrusted_condition";
          } else {
            function.parameterTypes = {lang::SemanticType::Int32};
          }
        }
      }
      expect(found && !lang::verifyMirProgram(changed).valid(),
             "every native condition identity/signature is verified");
    }
  }
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = frontend.mir;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             changed.functionInstances())) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.synchronization.kind ==
              lang::SynchronizationOperationKind::ConditionWait) {
            if (mutation == 0) {
              instruction.synchronization = {};
            }
            if (mutation == 1) {
              instruction.synchronization.kind =
                  lang::SynchronizationOperationKind::MutexLock;
            }
            if (mutation == 2) {
              instruction.synchronization.order =
                  lang::AtomicMemoryOrder::Acquire;
            }
          }
        }
      }
    }
    expect(!lang::verifyMirProgram(changed).valid(),
           "wait cannot lose release/reacquire identity or gain atomic order");
  }
  const auto single = analyze(text, false);
  const auto unsupported = analyze(text, true, false);
  expect(!single.canGenerateCode() && hasCode(single, "GTI-S2084") &&
             !unsupported.canGenerateCode() &&
             hasCode(unsupported, "GTI-S2084"),
         "conditions require concurrent profile and target support");
  const auto safe = analyze("#include <std/cthread>\nint main() { return "
                            "std::cnd_wait(uint64_t(0), uint64_t(0)); }");
  expect(!safe.canGenerateCode() && hasCode(safe, "GTI-S2055"),
         "manual waits require unsafe");
}

void testOnce() {
  const std::string text = R"(
#include <std/cthread>
std::once_flag shared{std::ONCE_FLAG_INIT};
void initialize() {}
int main() {
  std::once_flag local{};
  unsafe {
    std::call_once(shared, initialize);
    std::call_once(&local, initialize);
  }
  return 0;
}
)";
  const auto frontend = analyze(text);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "fresh immobile local/global once flags compile");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(
      lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid() &&
          lang::verifyMirProgram(frontend.mir).valid(),
      "once source atomics and registered initializer verify");
  expect(frontend.mir.nativeCallbackAdapters().size() == 1 &&
             frontend.mir.nativeCallbackAdapters().front().cThreadCallback ==
                 lang::CThreadCallbackKind::OnceInitializer,
         "void initializer has its distinct callback role");
  bool acquire = false, release = false, compare = false, native = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.cThreadOperation ==
        lang::CThreadRuntimeOperation::InvokeOnceInitializer) {
      native = function.externalSymbol == "gti_rt_once_invoke_v1" &&
               function.parameterTypes.size() == 1 &&
               function.parameterTypes.front().cThreadCallbackKind() ==
                   lang::CThreadCallbackKind::OnceInitializer;
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto &sync = instruction.synchronization;
        acquire |=
            sync.kind == lang::SynchronizationOperationKind::AtomicLoad &&
            sync.order == lang::AtomicMemoryOrder::Acquire;
        release |=
            sync.kind == lang::SynchronizationOperationKind::AtomicStore &&
            sync.order == lang::AtomicMemoryOrder::Release;
        compare |=
            sync.kind ==
                lang::SynchronizationOperationKind::AtomicCompareExchange &&
            sync.order == lang::AtomicMemoryOrder::Acquire;
      }
    }
  }
  expect(acquire && release && compare && native,
         "once synchronization is ordinary MIR atomics plus exact native "
         "invocation");
  for (const auto role : {lang::CThreadCallbackKind::None,
                          lang::CThreadCallbackKind::ThreadEntry}) {
    auto mir = frontend.mir;
    auto &adapters = const_cast<std::vector<lang::MirNativeCallbackAdapter> &>(
        mir.nativeCallbackAdapters());
    adapters.front().cThreadCallback = role;
    expect(!lang::verifyMirProgram(mir).valid(),
           "erased or swapped once callback role rejected in MIR");
    auto hir = frontend.hir;
    auto &hirAdapters =
        const_cast<std::vector<lang::HirNativeCallbackAdapter> &>(
            hir.nativeCallbackAdapters());
    hirAdapters.front().cThreadCallback = role;
    expect(!lang::verifyHirProgramPlans(frontend.semantics, hir).valid(),
           "erased or swapped once callback role rejected in HIR");
  }
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const auto generated =
        gti_test::emitCppText(frontend, lang::CppStandard::Cpp23,
                              frontend.semantics.analysisSeal().target, level);
    expect(generated.find("gti_rt_once_enter_v1(&__gti_native_callback_") !=
                   std::string::npos &&
               generated.find("gti_rt_once_leave_v1(&__gti_native_callback_") !=
                   std::string::npos &&
               generated.find("// GTI verified-MIR body:") != std::string::npos,
           "production once thunk consumes verified role at every optimization "
           "level");
  }
  const auto single = analyze(text, false);
  const auto unavailable = analyze(text, true, false);
  expect(!single.canGenerateCode() && hasCode(single, "GTI-S2084") &&
             !unavailable.canGenerateCode() &&
             hasCode(unavailable, "GTI-S2084"),
         "once requires concurrent profile and target support");
  const auto safe = analyze(R"(
#include <std/cthread>
void initialize() {}
int main() { std::once_flag flag{}; std::call_once(flag, initialize); return 0; }
)");
  expect(!safe.canGenerateCode() && hasCode(safe, "GTI-S2055"),
         "once requires lexical unsafe");
  for (const std::string initializer : {"original", "std::move(original)"}) {
    const auto invalid = analyze(
        "#include <std/cthread>\nint main() { mut std::once_flag original{}; "
        "std::once_flag copied = " +
        initializer + "; return 0; }");
    expect(!invalid.canGenerateCode(),
           "a live once flag cannot be copied or moved");
  }
  const auto hidden = analyze(R"(
#include <std/cthread>
int main() { gti_internal::once_access access{}; return 0; }
)");
  expect(!hidden.canGenerateCode(),
         "private implementation token cannot escape into applications");
}

void testPipeline() {
  const auto frontend = analyze(source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "C-style create/join completes the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "HIR retains exact runtime and callback contracts");
  expect(lang::verifyMirProgram(frontend.mir).valid(), "MIR verifies");
  const auto dump = lang::MirPrinter().print(frontend.mir);
  expect(dump.find('\0') == std::string::npos &&
             dump.find("cthread-operation=1") != std::string::npos &&
             dump.find("cthread-callback=1") != std::string::npos,
         "C-thread operation and attachment contracts serialize as text");
  expect(frontend.mir.nativeCallbackAdapters().size() == 1 &&
             frontend.mir.nativeCallbackAdapters().front().cThreadCallback ==
                 lang::CThreadCallbackKind::ThreadEntry,
         "C-thread callback requires exact registered entry");
  const auto &plan = frontend.mir.hostedStartupPlan();
  expect(plan && plan->cThreadBegin != 0 && plan->cThreadEnd != 0,
         "hosted MIR owns runtime startup and shutdown");
  std::size_t synchronization = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.synchronization.kind ==
                lang::SynchronizationOperationKind::ThreadSpawn ||
            instruction.synchronization.kind ==
                lang::SynchronizationOperationKind::ThreadJoin) {
          ++synchronization;
          expect(instruction.intrinsic == lang::IntrinsicKind::None &&
                     instruction.functionTarget.has_value(),
                 "manual C-thread operations remain exact native calls");
        }
      }
    }
  }
  expect(synchronization == 2,
         "create and join each retain a synchronization edge");
  const auto target = frontend.semantics.analysisSeal().target;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "detached lowered contract verifies");
  const auto generated =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp23, target);
  expect(generated.find("#include <gti/cthread.h>") != std::string::npos,
         "the concurrent runtime contract selects its native boundary header");
  expect(
      generated.find("gti_rt_cthread_enter_v1(&__gti_native_callback_") !=
              std::string::npos &&
          generated.find("gti_rt_cthread_leave_v1(&__gti_native_callback_") !=
              std::string::npos,
      "production thunk consumes verified attachment policy");

  auto changed = frontend.mir;
  auto &adapters = const_cast<std::vector<lang::MirNativeCallbackAdapter> &>(
      changed.nativeCallbackAdapters());
  adapters.front().cThreadCallback = lang::CThreadCallbackKind::None;
  expect(!lang::verifyMirProgram(changed).valid(),
         "missing callback attachment is rejected");

  changed = frontend.mir;
  auto &changedPlan =
      const_cast<lang::MirHostedStartupPlan &>(*changed.hostedStartupPlan());
  std::swap(changedPlan.cThreadBegin, changedPlan.cThreadEnd);
  expect(!lang::verifyMirProgram(changed).valid(),
         "swapped runtime boundaries are rejected");

  changed = frontend.mir;
  auto &schedule =
      const_cast<lang::MirHostedStartupPlan &>(*changed.hostedStartupPlan());
  for (auto &operation : schedule.operations) {
    if (operation.kind ==
        lang::MirHostedStartupOperationKind::BeginCThreadRuntime) {
      operation.kind = lang::MirHostedStartupOperationKind::EndCThreadRuntime;
      break;
    }
  }
  expect(!lang::verifyMirProgram(changed).valid(),
         "reordered lifecycle operation roles are rejected");

  auto forgedLowered = lowered;
  for (auto &declaration : const_cast<std::vector<lang::LoweredDeclaration> &>(
           forgedLowered.declarations())) {
    auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (function &&
        function->cThreadOperation == lang::CThreadRuntimeOperation::Create) {
      function->compilerPrivate = false;
    }
  }
  expect(!lang::verifyLoweredProgram(forgedLowered).empty(),
         "runtime operations cannot lose private declaration identity after "
         "detachment");

  changed = frontend.mir;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           changed.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.synchronization.kind ==
            lang::SynchronizationOperationKind::ThreadSpawn) {
          instruction.synchronization = {};
        }
      }
    }
  }
  expect(!lang::verifyMirProgram(changed).valid(),
         "erased spawn synchronization is rejected");

  changed = frontend.mir;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           changed.functionInstances())) {
    if (function.cThreadOperation == lang::CThreadRuntimeOperation::Create) {
      function.externalSymbol = "not_the_runtime";
    }
  }
  expect(!lang::verifyMirProgram(changed).valid(),
         "forged runtime identity is rejected");
}

void testDiagnostics() {
  const auto single = analyze(source, false);
  expect(!single.canGenerateCode() && hasCode(single, "GTI-S2084"),
         "create/join requires concurrent profile through ordinary wrappers");
  const auto profileDiagnostic = std::find_if(
      single.diagnostics.begin(), single.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2084"; });
  if (profileDiagnostic != single.diagnostics.end()) {
    const auto closing = source.find(')', source.find("std::thrd_create("));
    expect(profileDiagnostic->primary.source ==
                   std::filesystem::absolute("cthread.gti").string() &&
               profileDiagnostic->primary.start == closing &&
               profileDiagnostic->primary.end == closing + 1 &&
               profileDiagnostic->message.find(
                   "concurrent execution profile") != std::string::npos &&
               !profileDiagnostic->hints.empty() &&
               profileDiagnostic->fixes.empty(),
           "profile diagnostic retains exact call span and no unsafe "
           "mechanical fix");
  }
  const auto unavailable = analyze(source, true, false);
  expect(!unavailable.canGenerateCode() && hasCode(unavailable, "GTI-S2084"),
         "C-thread use requires target thread support");
  const auto imported = analyze(
      "#include <std/cthread>\nint main() { std::thrd_yield(); return 0; }",
      false);
  expect(imported.canGenerateCode() && imported.mir.hostedStartupPlan() &&
             imported.mir.hostedStartupPlan()->cThreadBegin == 0,
         "default-profile import/yield does not initialize the thread service");
  if (imported.canGenerateCode()) {
    const auto text =
        gti_test::emitCppText(imported, lang::CppStandard::Cpp23,
                              imported.semantics.analysisSeal().target);
    expect(
        text.find("#include <gti/cthread.h>") == std::string::npos,
        "default-profile yield does not require the callback boundary header");
  }
  const auto safe = analyze(R"(
#include <std/cthread>
int main() { std::thrd_t handle = 0; mut int32_t result = 0; return std::thrd_join(handle, result); }
)");
  expect(!safe.canGenerateCode() && hasCode(safe, "GTI-S2055"),
         "manual lifetime contracts cannot be called from safe code");
  const auto callback = analyze(R"(
#include <std/cthread>
using Wrong = (int32_t) -> int32_t;
int32_t work(int32_t value) { return value; }
int main() { Wrong callback = work; return 0; }
)");
  expect(!callback.canGenerateCode() && hasCode(callback, "GTI-S2076"),
         "concurrent native entry does not admit unrelated signatures");
  const auto ordinary = analyze(R"(
namespace std {
  [[unsafe]] int32_t thrd_join(uint64_t handle, mut int32_t& result) { return 0; }
}
int main() {
  uint64_t handle = 0;
  mut int32_t result = 0;
  unsafe { return std::thrd_join(handle, result); }
}
)",
                                false);
  expect(ordinary.canGenerateCode() && !ordinary.mir.requiresCThreadRuntime(),
         "public std spelling does not confer compiler runtime identity");
  const std::string genericSource = R"(
#include <std/cthread>
[[unsafe]] int32_t relay<T>(T ignored, std::thrd_t handle) {
  unsafe { return std::thrd_join(handle, nullptr); }
}
int main() { std::thrd_t handle = 0; unsafe { return relay(1, handle); } }
)";
  const auto generic = analyze(genericSource, false);
  expect(
      !generic.canGenerateCode() && hasCode(generic, "GTI-S2084"),
      "concrete generic reanalysis preserves the runtime profile requirement");
  const auto concurrentGeneric = analyze(genericSource);
  expect(
      concurrentGeneric.canGenerateCode() &&
          lang::verifyMirProgram(concurrentGeneric.mir).valid(),
      "the same concrete generic wrapper is valid in the concurrent profile");
  const auto missingRuntime = analyze(R"(
using Entry = (void*) -> int32_t;
int32_t worker(void* context) { return 0; }
int main() { Entry entry = worker; return 0; }
)");
  expect(!missingRuntime.canGenerateCode() &&
             hasCode(missingRuntime, "GTI-S2076"),
         "concurrent callback conversion requires the runtime declarations");
}
} // namespace

int main() {
  testSleep();
  testMutex();
  testCondition();
  testOnce();
  testIdentity();
  testPipeline();
  testDiagnostics();
  return failures == 0 ? 0 : 1;
}
