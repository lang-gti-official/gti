// Frozen C++-backend coverage moved from cthread_tests.cpp: the C++
// emitter's constructed-field publication proof, and the C++ spelling of
// C-thread runtime boundary headers and callback thunks.
#include "../src/compiler/cpp_mir_body_emitter.h"
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

[[maybe_unused]] bool hasCode(const lang::FrontendResult &result,
                              std::string_view code) {
  return std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
}

const std::string source = R"(
import std.cthread;
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

void testFailureFreeConstructorPublication() {
  const auto frontend = analyze(R"(
import std.atomic;
struct Gate {
  std::atomic<int32_t> first;
  std::atomic<int32_t> second;
  Gate() : first(std::atomic<int32_t>(3)), second(std::atomic<int32_t>(7)) {}
};
struct Tag { public: Tag() = default; };
struct Tagged {
  std::atomic<int32_t> value;
  Tagged(Tag tag) : value(std::atomic<int32_t>(11)) {}
};
Gate global{};
Tagged tagged_global{Tag()};
int main() {
  Gate local{};
  Tagged tagged_local{Tag()};
  return global.first.load() + local.second.load() == 10 &&
         tagged_global.value.load() + tagged_local.value.load() == 22 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "unrelated atomic-field constructors "
                                     "retain verified failure publication");
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 2 ||
        constructor.body.failureRecords.size() != 2)
      continue;
    ++checked;
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    expect(bindings.size() == 2 && lang::cppMirConstructorStatusCannotFail(
                                       frontend.mir, constructor.id),
           "two no-failure member constructions retain declaration-order "
           "publication");
    for (int mutation = 0; mutation < 8; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto &body = changed->body;
      auto *entry = const_cast<lang::MirBlock *>(body.findBlock(body.entry));
      auto *alternate = const_cast<lang::MirBlock *>(
          body.findBlock(entry->terminator.elseTarget));
      auto &invoke = entry->instructions.back();
      switch (mutation) {
      case 0:
        alternate->instructions.push_back(
            {.kind = lang::MirInstructionKind::Lifecycle});
        break;
      case 1:
        alternate->terminator.failureRecord = 0;
        break;
      case 2:
        entry->terminator.target = body.entry;
        break;
      case 3:
        invoke.constructorTarget.reset();
        break;
      case 4: {
        auto *target = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(*invoke.constructorTarget));
        target->body.blocks.front()
            .instructions.front()
            .definedFailure.propagation =
            lang::FailurePropagationKind::DirectCall;
        break;
      }
      case 5:
        std::swap(changed->initializers[0], changed->initializers[1]);
        break;
      case 6:
        invoke.parameterTypes.clear();
        break;
      case 7:
        body.failureRecords.front().producerInstruction = 0;
        break;
      }
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
              .empty(),
          "cleanup, foreign failure, loops, missing targets, unknown failure, "
          "field order, argument shape and failure identity must fail closed");
      if (mutation != 5)
        expect(!lang::cppMirConstructorStatusCannotFail(malformed, changed->id),
               "malformed or genuinely unknown failure cannot become "
               "no-failure status");
    }
  }
  expect(checked == 1,
         "the user-defined two-field constructor must exercise the proof");
  std::size_t passiveCleanup = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.parameterTypes.size() != 1 ||
        constructor.parameterTypes.front().kind != lang::SemanticType::Class ||
        constructor.body.failureRecords.size() != 1)
      continue;
    ++passiveCleanup;
    expect(lang::cppMirConstructedFieldInitializerBindings(frontend.mir,
                                                           constructor)
                   .size() == 1,
           "a user-defined passive tag retains its exact shell cleanup");
    for (int mutation = 0; mutation < 10; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto &body = changed->body;
      const auto *entry = body.findBlock(body.entry);
      auto *alternate = const_cast<lang::MirBlock *>(
          body.findBlock(entry->terminator.elseTarget));
      auto &retire = alternate->instructions.front();
      auto &event = retire.lifecycle.front();
      auto *drop = const_cast<lang::MirDropObligation *>(
          body.findDropObligation(event.source));
      auto &boundary =
          body.cleanupBoundaries
              [alternate->instructions.back().cleanupBoundaryEnd - 1];
      switch (mutation) {
      case 0:
        drop->dropType.requiresActiveCleanup = true;
        break;
      case 1:
        drop->dropType.destructor = 1;
        break;
      case 2:
        changed->parameterBindings.clear();
        break;
      case 3:
        drop->initiallyActive = false;
        break;
      case 4:
        event.failureCleanup = false;
        break;
      case 5:
        event.conditional = true;
        break;
      case 6:
        event.target = drop->id;
        break;
      case 7:
        boundary.kind = lang::MirCleanupBoundaryKind::Normal;
        break;
      case 8:
        boundary.obligations.clear();
        break;
      case 9:
        retire.definedFailure.propagation =
            lang::FailurePropagationKind::Destructor;
        break;
      }
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
                  .empty() &&
              !lang::cppMirConstructorStatusCannotFail(malformed, changed->id),
          "active/user cleanup, non-parameter drops, lifecycle mismatch and "
          "missing failure-boundary authority cannot be ignored");
    }
  }
  expect(passiveCleanup == 1, "the unrelated passive-parameter constructor "
                              "must exercise cleanup proof");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      expect(!gti_test::emitCppText(frontend, standard,
                                    frontend.semantics.analysisSeal().target,
                                    level)
                  .empty(),
             "local and global user-defined immobile fields have production "
             "emission");
    }
  }
  const auto fallible = analyze(R"(
struct Checked {
  int32_t value;
  Checked(int32_t input) : value(input + 1) {}
};
struct Wrapper {
  Checked value;
  Wrapper(int32_t input) : value(Checked(input)) {}
};
int main() { Wrapper value{3}; return value.value.value; }
)");
  expect(fallible.canGenerateCode(),
         "a checked nested constructor must verify");
  std::size_t rejected = 0;
  for (const auto &constructor : fallible.mir.constructorInstances()) {
    if (constructor.initializers.size() == 1 &&
        constructor.initializers.front().targetType.kind ==
            lang::SemanticType::Class) {
      ++rejected;
      expect(!lang::cppMirConstructorStatusCannotFail(fallible.mir,
                                                      constructor.id) &&
                 lang::cppMirConstructedFieldInitializerBindings(fallible.mir,
                                                                 constructor)
                     .empty(),
             "real nested overflow must retain its failure/publication route");
    }
  }
  expect(rejected == 1,
         "the fallible nested constructor must exercise rejection");
}

void testOnce() {
  const std::string text = R"(
import std.cthread;
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
}

void testDefaultProfileHeader() {
  const auto imported = analyze(
      "import std.cthread;\nint main() { std::thrd_yield(); return 0; }",
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
}
} // namespace

int main() {
  testFailureFreeConstructorPublication();
  testOnce();
  testPipeline();
  testDefaultProfileHeader();
  return failures == 0 ? 0 : 1;
}
