#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
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
    std::mutex_guard<int32_t> first = program_value.lock();
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

template <typename Function>
void forEachDrop(lang::MirProgram &program, Function &&function) {
  const auto visit = [&](lang::MirBody &body) {
    for (lang::MirDropObligation &drop :
         const_cast<std::vector<lang::MirDropObligation> &>(
             body.dropObligations)) {
      function(drop);
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

lang::MirDropType *findMutexDropRecipe(lang::MirProgram &program,
                                       lang::MirDropType &drop) {
  const lang::MirClassInstance *owner =
      drop.classInstance ? program.findClassInstance(*drop.classInstance)
                         : nullptr;
  if (owner != nullptr && owner->compilerCapability ==
                              lang::CompilerCapabilityTypeKind::MutexStorage) {
    return &drop;
  }
  for (lang::MirDropType &child : drop.aggregate) {
    if (lang::MirDropType *found = findMutexDropRecipe(program, child)) {
      return found;
    }
  }
  return nullptr;
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
  for (const lang::TargetInfo &target : {single, concurrentTarget()}) {
    const lang::FrontendResult frontend =
        analyze("dormant-mutex-import.gti",
                "#include <std/mutex>\nint main() { return 0; }\n", target);
    if (!frontend.canGenerateCode()) {
      printDiagnostics(frontend);
    }
    expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
               lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                   .valid() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "an unused mutex import should remain valid in both execution "
           "profiles");
    if (!frontend.canGenerateCode()) {
      continue;
    }
    const lang::LoweredProgram lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
    const std::string cpp =
        lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
    expect(lang::verifyLoweredProgram(lowered).empty() &&
               cpp.find("#include <mutex>") == std::string::npos &&
               cpp.find("class mutex_value final") == std::string::npos &&
               cpp.find("class mutex_lock_token final") == std::string::npos,
           "a dormant mutex import should not enable native mutex support");
    // Retained generic library classes name both representation templates,
    // so the templates must be declared for the native compiler (#164).
    const std::size_t firstUse =
        cpp.find("::gti_internal::backend::mutex_value<T>");
    const std::size_t storage =
        cpp.find("template <typename T> class mutex_value;");
    const std::size_t token =
        cpp.find("template <typename T> class mutex_lock_token;");
    expect(storage != std::string::npos && token != std::string::npos &&
               storage < firstUse && token < firstUse,
           "a dormant mutex import should declare, but not define, the mutex "
           "representations before generic declarations name them");
  }
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
  forEachInstruction(inspected, [&](lang::MirBody &body,
                                    lang::MirInstruction &instruction) {
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
                   (lock     ? lang::SynchronizationOperationKind::MutexLock
                    : unlock ? lang::SynchronizationOperationKind::MutexUnlock
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

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, optimized.sourceMir, optimized.mir,
                             target, optimized.report.transformations);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "mutex synchronization should cross the sealed LoweredProgram "
         "boundary");
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  for (const std::string *cpp : {&cpp20, &cpp23}) {
    expect(cpp->find("#include <mutex>") != std::string::npos &&
               cpp->find("class mutex_value final") != std::string::npos &&
               cpp->find("class mutex_lock_token final") != std::string::npos &&
               cpp->find("mutex_lock(") != std::string::npos &&
               cpp->find("mutex_unlock(") != std::string::npos,
           "both native standards should consume the sealed mutex and guard "
           "vocabulary");
  }
}

void testReferencePrefixBeforeOwnedField() {
  const auto frontend = analyze("mutex-reference-prefix.gti", validProgram());
  expect(frontend.canGenerateCode(), "mutex reference-prefix fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  bool found = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 2 ||
        !constructor.initializers.front().storesReference ||
        !constructor.initializers.back().ownedParameter)
      continue;
    found = true;
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor);
    expect(bindings && bindings->size() == 1 &&
               bindings->front().initializeInstruction != 0,
           "stored reference prefix preserves exact owned-field publication");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto &changed = const_cast<lang::MirConstructorInstance &>(
          *malformed.findConstructorInstance(constructor.id));
      auto &entry = *changed.body.findBlock(changed.body.entry);
      if (mutation == 0)
        std::swap(entry.instructions[0], entry.instructions[1]);
      if (mutation == 1)
        entry.instructions[0].operands.front().place = 0;
      if (mutation == 2)
        entry.instructions[1].hirValue = 0;
      if (mutation == 3)
        entry.instructions[2].fullExpressionEnd = 0;
      expect(!lang::cppMirOwnedParameterFieldBindings(malformed, changed),
             "reference prefix requires exact order, source, borrow identity "
             "and full-expression boundary");
    }
  }
  expect(found,
         "mutex guard exercises mixed reference and owner initialization");
}

// Final failure-effect lowering elides this infallible native initializer's
// Invoke. Retain its canonical conservative shape in a no-entry test program
// to exercise the backend's independently proved impossible-failure cleanup.
// A genuinely fallible constructor cannot be this positive control: native
// initializer fusion requires its exact source body to prove failure-free.
std::optional<lang::MirProgram>
retainedNativeInitializerFailure(const lang::MirProgram &program,
                                 lang::HirConstructorInstanceId owner,
                                 lang::MirInstructionId constructInstruction) {
  auto retained = program;
  auto &constructor = const_cast<lang::MirConstructorInstance &>(
      *retained.findConstructorInstance(owner));
  auto &body = constructor.body;
  if (retained.hostedStartup() || body.blocks.size() != 1 ||
      !body.failureRecords.empty())
    return std::nullopt;
  auto &entry = body.blocks.front();
  const auto construct =
      std::find_if(entry.instructions.begin(), entry.instructions.end(),
                   [&](const auto &instruction) {
                     return instruction.id == constructInstruction;
                   });
  if (construct == entry.instructions.end() || !construct->constructorTarget ||
      !construct->definedFailure.empty() ||
      !lang::cppMirConstructorStatusCannotFail(retained,
                                               *construct->constructorTarget))
    return std::nullopt;
  auto &target = const_cast<lang::MirConstructorInstance &>(
      *retained.findConstructorInstance(*construct->constructorTarget));
  target.mayRaiseDefinedFailure = true;
  construct->definedFailure.propagation =
      lang::FailurePropagationKind::Constructor;
  const auto constructValue = construct->hirValue;
  lang::MirBlock success{.id = 2};
  success.instructions.assign(std::next(construct), entry.instructions.end());
  success.terminator = entry.terminator;
  success.reachable = true;
  const auto normalDrop =
      std::find_if(success.instructions.begin(), success.instructions.end(),
                   [](const auto &instruction) {
                     return instruction.kind == lang::MirInstructionKind::Drop;
                   });
  if (normalDrop == success.instructions.end() ||
      normalDrop->lifecycle.size() != 1)
    return std::nullopt;
  auto failureDrop = *normalDrop;
  failureDrop.id = body.instructionCount() + 1;
  failureDrop.lifecycle.front().failureCleanup = true;
  const auto boundary = body.cleanupBoundaries.size() + 1;
  body.cleanupBoundaries.push_back(
      {.id = boundary,
       .kind = lang::MirCleanupBoundaryKind::Failure,
       .obligations = {failureDrop.lifecycle.front().source}});
  lang::MirInstruction marker{.id = failureDrop.id + 1,
                              .kind = lang::MirInstructionKind::Lifecycle,
                              .cleanupBoundaryEnd = boundary};
  lang::MirBlock failure{
      .id = 3,
      .failureParameter = 1,
      .activeFailure = 1,
      .instructions = {failureDrop, marker},
      .terminator = {.kind = lang::MirTerminatorKind::PropagateFailure,
                     .failureRecord = 1},
      .reachable = true};
  entry.instructions.erase(std::next(construct), entry.instructions.end());
  entry.terminator = {.kind = lang::MirTerminatorKind::Invoke,
                      .hirValue = constructValue,
                      .invokeInstruction = constructInstruction,
                      .failureRecord = 1,
                      .target = 2,
                      .elseTarget = 3};
  body.failureRecords.push_back({.id = 1,
                                 .producerBlock = 1,
                                 .producerInstruction = constructInstruction,
                                 .parameterBlock = 3});
  body.blocks.push_back(std::move(success));
  body.blocks.push_back(std::move(failure));
  for (auto &value : body.values)
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.result == value.id)
          value.definitionBlock = block.id;
  if (!lang::rebuildMirValueUses(body))
    return std::nullopt;
  const auto effects = lang::deriveMirProgramEffectSummaries(retained);
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           retained.functionInstances()))
    function.effectSummary = effects.functions[function.id - 1];
  for (auto &instance : const_cast<std::vector<lang::MirConstructorInstance> &>(
           retained.constructorInstances()))
    instance.effectSummary = effects.constructors[instance.id - 1];
  return retained;
}

void testPreparedMutexPayload() {
  const auto frontend = analyze("mutex-prepared-payload.gti", R"gti(
#include <std/mutex>
class payload {
  int value;
public:
  payload(int initial) : value(initial) {}
  payload(payload& other) = delete;
  payload(payload&& other) = default;
};
int exercise() { std::mutex<payload> value{payload(4)}; return 0; }
)gti");
  expect(frontend.canGenerateCode(), "prepared native payload lowers");
  if (!frontend.canGenerateCode())
    return;
  const auto lowered = gti_test::lowerProgram(frontend, frontend.mir,
                                              frontend.mir, concurrentTarget());
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  bool found = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.preparedParameterDrop || !stage.result)
          continue;
        const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
            frontend.mir, constructor);
        if (bindings.empty())
          continue;
        found = true;
        expect(bindings.size() == 1 && bindings.front().arguments.size() == 1 &&
                   bindings.front().arguments.front().moved,
               "native payload initializer preserves its exact parameter move");
        const auto text =
            lang::CppMirBodyEmitter(frontend.mir, map)
                .emitFailureBodyText(
                    {lang::MirBodyKind::Constructor, constructor.id},
                    "passive-native-initializer", 1)
                .text;
        expect(text.find("if (__gti_mir_construct_success_" +
                         std::to_string(bindings.front().constructInstruction) +
                         ")") == std::string::npos,
               "an initializer executed before the body must follow success, "
               "not branch on an unset body-local status flag");
        const auto retained = retainedNativeInitializerFailure(
            frontend.mir, constructor.id,
            bindings.front().constructInstruction);
        expect(retained && lang::verifyMirProgram(*retained).valid(),
               "retained conservative native initializer independently "
               "verifies before cleanup mutations");
        if (!retained)
          continue;
        const auto &retainedConstructor =
            *retained->findConstructorInstance(constructor.id);
        expect(lang::cppMirConstructedFieldInitializerBindings(
                   *retained, retainedConstructor)
                       .size() == 1,
               "failure-free source constructor proves the retained native "
               "initializer's impossible cleanup edge");
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = *retained;
          auto &changed = const_cast<lang::MirConstructorInstance &>(
              *malformed.findConstructorInstance(constructor.id));
          std::size_t changedTargets = 0;
          if (mutation == 4) {
            changed.body.dropObligations[*stage.preparedParameterDrop - 1]
                .initiallyActive = true;
            ++changedTargets;
          }
          for (auto &candidateBlock : changed.body.blocks)
            for (auto &instruction : candidateBlock.instructions) {
              if (instruction.id == stage.id) {
                if (mutation == 0) {
                  instruction.destination.reset();
                  ++changedTargets;
                }
                if (mutation == 1) {
                  instruction.lifecycle.clear();
                  ++changedTargets;
                }
                if (mutation == 2) {
                  instruction.lifecycle.front().conditional = true;
                  ++changedTargets;
                }
              }
              if (mutation == 3 &&
                  instruction.id == bindings.front().constructInstruction) {
                instruction.lifecycle.clear();
                ++changedTargets;
              }
              if (candidateBlock.failureParameter != 0 &&
                  instruction.kind == lang::MirInstructionKind::Drop &&
                  instruction.lifecycle.size() == 1) {
                const auto dropId = instruction.lifecycle.front().source;
                if (mutation == 5) {
                  instruction.lifecycle.front().failureCleanup = false;
                  ++changedTargets;
                }
                if (mutation == 6) {
                  changed.body.dropObligations[dropId - 1].initiallyActive =
                      false;
                  ++changedTargets;
                }
                if (mutation == 7) {
                  changed.body.dropObligations[dropId - 1]
                      .dropType.requiresActiveCleanup = true;
                  ++changedTargets;
                }
              }
            }
          expect(changedTargets == 1,
                 "each prepared native payload mutation changes one real "
                 "slot or cleanup target");
          expect(lang::cppMirConstructedFieldInitializerBindings(malformed,
                                                                 changed)
                     .empty(),
                 "prepared native payload needs exact slot, activation, "
                 "invocation transfer, inactive prepared state and exact "
                 "passive parameter cleanup on the impossible failure edge");
        }
      }
    }
  }
  expect(found, "fixture exercises a prepared native payload initializer");
  expect(
      !gti_test::emitCpp(frontend, lang::CppStandard::Cpp23, concurrentTarget())
           .contents.empty(),
      "a passive move-only payload must have production emission; "
      "retiring its parameter shell cannot require a copy constructor");
}

void testPassiveClosureParameterStorage() {
  const auto frontend = analyze("passive-closure-storage.gti", R"gti(
#include <std/thread>
struct MoveOnly {
  int number = 42;
  MoveOnly() = default;
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};
int main() {
  MoveOnly value{};
  mut std::thread_result<int> worker{};
  if (!worker.start([owned = std::move(value)]() -> int {
        return owned.number;
      })) return 1;
  auto result = worker.join();
  return result && result.value() == 42 ? 0 : 2;
}
)gti");
  expect(frontend.canGenerateCode(), "passive closure parameter lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const auto lowered = gti_test::lowerProgram(frontend, frontend.mir,
                                              frontend.mir, concurrentTarget());
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.info.type.kind != lang::SemanticType::Lambda ||
            !stage.preparedParameterDrop || !stage.destination ||
            stage.lifecycle.size() != 1 ||
            stage.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::Initialize)
          continue;
        const auto *prepared =
            function.body.findDropObligation(*stage.preparedParameterDrop);
        if (!prepared || prepared->dropType.requiresActiveCleanup)
          continue;
        found = true;
        expect(stage.info.traits.drop == lang::DropKind::Lexical,
               "regression retains lexical destruction without active cleanup");
        for (const auto standard :
             {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
          const lang::CppMirBodyEmissionMap map(
              lang::buildCppMirBodyEmissionMapRows(lowered, standard));
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          expect(lang::CppMirBodyEmitter(frontend.mir, map)
                     .supportsFailureBodyText(address),
                 "cleanup-free lexical closure has exact prepared storage");
          for (int mutation = 0; mutation != 5; ++mutation) {
            auto malformed = frontend.mir;
            auto &changed = const_cast<lang::MirFunctionInstance &>(
                *malformed.findFunctionInstance(function.id));
            auto &drop = changed.body.dropObligations[prepared->id - 1];
            if (mutation == 0)
              drop.dropType.requiresActiveCleanup = true;
            if (mutation == 1)
              drop.initiallyActive = true;
            if (mutation == 2)
              drop.place = 0;
            for (auto &changedBlock : changed.body.blocks)
              for (auto &instruction : changedBlock.instructions)
                if (instruction.id == stage.id) {
                  if (mutation == 3)
                    instruction.lifecycle.front().conditional = true;
                  if (mutation == 4)
                    instruction.lifecycle.front().source = prepared->id;
                }
            expect(!lang::CppMirBodyEmitter(malformed, map)
                        .supportsFailureBodyText(address),
                   "passive closure admission rejects active cleanup, stale "
                   "slot activation, and invented lifetime publication");
          }
        }
      }
    }
  }
  expect(found, "fixture exercises passive lexical closure publication");
}

void testMutexCleanupRecipe() {
  const auto frontend = analyze("mutex-cleanup-recipe.gti", R"gti(
#include <std/memory>
#include <std/mutex>
class item {
public:
  item() {}
  ~item() {}
};
class payload {
  std::unique_ptr<item> value;
public:
  payload() : value(std::make_unique<item>()) {}
};
int main() {
  std::mutex<payload> value{payload()};
  return 0;
}
)gti");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "mutex payloads with active cleanup should retain a verified nested "
         "drop recipe");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }

  lang::MirProgram malformed = frontend.mir;
  bool found = false;
  forEachDrop(malformed, [&](lang::MirDropObligation &obligation) {
    if (found) {
      return;
    }
    if (lang::MirDropType *mutex =
            findMutexDropRecipe(malformed, obligation.dropType)) {
      found = mutex->aggregate.size() == 1 &&
              mutex->aggregate.front().requiresActiveCleanup;
      mutex->aggregate.clear();
    }
  });
  expect(found,
         "the mutex drop recipe should name its one active payload child");
  expect(found && !lang::verifyMirProgram(malformed).valid(),
         "MIR should reject a mutex cleanup recipe with its payload removed");
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
  expect(!singleResult.canGenerateCode() &&
             hasCode(singleResult, "GTI-S2084") &&
             hasMessage(singleResult, "concurrent execution profile") &&
             !hasCode(singleResult, "GTI-B0001"),
         "mutex use should fail before lowering in the single-threaded "
         "profile");

  const lang::FrontendResult unsupported =
      analyze("mutex-unsupported-target.gti", use, concurrentTarget(false));
  expect(
      !unsupported.canGenerateCode() && hasCode(unsupported, "GTI-S2084") &&
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
       std::vector<std::pair<std::string, std::string>>{{"temporary", R"(
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
  expect(!transferred.canGenerateCode() && !hasCode(transferred, "GTI-B0001"),
         "a guard should not cross the thread-transfer boundary");
}

void testPayloadConcurrencyCapabilities() {
  for (const std::string policy : {"", "[[no_share]]", "[[no_transfer]]"}) {
    const auto frontend =
        analyze("mutex-payload-capabilities.gti",
                "#include <std/mutex>\n#include <std/memory>\n" + policy + R"(
struct Payload { mut int value = 0; };
int main() {
  auto owned = std::make_shared<std::mutex<Payload>>(Payload());
  return 0;
}
)");
    if (!frontend.canGenerateCode())
      printDiagnostics(frontend);
    expect(frontend.canGenerateCode(),
           "local mutex/shared ownership must accept thread-affine payloads");
    const bool transferable = policy != "[[no_transfer]]";
    bool sawStorage = false;
    bool sawShared = false;
    for (const auto &instance : frontend.mir.classInstances()) {
      if (instance.compilerCapability !=
              lang::CompilerCapabilityTypeKind::MutexStorage &&
          instance.compilerCapability !=
              lang::CompilerCapabilityTypeKind::SharedOwner)
        continue;
      sawStorage |= instance.compilerCapability ==
                    lang::CompilerCapabilityTypeKind::MutexStorage;
      sawShared |= instance.compilerCapability ==
                   lang::CompilerCapabilityTypeKind::SharedOwner;
      expect(
          instance.traits.transferCapable == transferable &&
              instance.traits.shareCapable == transferable,
          "mutex and its shared owner require payload transfer, not sharing");
    }
    expect(sawStorage && sawShared,
           "capability checks require both concrete owners");
    for (const bool transfer : {false, true}) {
      auto forged = frontend.mir;
      bool changed = false;
      for (auto &instance : const_cast<std::vector<lang::MirClassInstance> &>(
               forged.classInstances())) {
        if (instance.compilerCapability !=
            lang::CompilerCapabilityTypeKind::MutexStorage)
          continue;
        if (transfer)
          instance.traits.transferCapable = !transferable;
        else
          instance.traits.shareCapable = !transferable;
        changed = true;
      }
      expect(changed && !lang::verifyMirProgram(forged).valid(),
             "MIR must reject forged mutex payload concurrency capabilities");
    }
  }
  const auto denied = analyze("mutex-affine-worker.gti", R"(
#include <std/mutex>
#include <std/memory>
#include <std/thread>
[[no_transfer]] struct Affine { int value = 0; };
int main() {
  auto owner = std::make_shared<std::mutex<Affine>>(Affine());
  auto started = std::thread::start([value = std::move(owner)]() -> void {});
  return 0;
}
)");
  expect(!denied.canGenerateCode() && hasCode(denied, "GTI-S2084") &&
             !hasCode(denied, "GTI-B0001"),
         "a mutex must not erase its payload's thread affinity");
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
    forEachInstruction(staleTrust, [&](lang::MirBody &body,
                                       lang::MirInstruction &instruction) {
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

void testStoredReceiverPublication() {
  const std::string source = R"gti(
#include <std/mutex>
class User {
  std::mutex<int32_t>& owner;
public:
  User(std::mutex<int32_t>& owner) : owner(owner) {}
  void use() { auto guard = this.owner.lock(); }
};
int main() {
  std::mutex<int32_t> owner{7};
  User user{owner};
  user.use();
  return 0;
}
)gti";
  const auto frontend = analyze("mutex-stored-receiver.gti", source);
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a stored shared receiver preserves lifetime and callee exclusion");
  if (!frontend.canGenerateCode())
    return;
  lang::HirFunctionInstanceId functionId = 0;
  for (const auto &function : frontend.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "use")
      functionId = function.id;
  const auto *function = frontend.mir.findFunctionInstance(functionId);
  lang::MirLoanId child = 0, parent = 0;
  lang::MirInstructionId publication = 0, input = 0;
  if (function)
    for (const auto &loan : function->body.loans)
      if (loan.kind == lang::MirLoanKind::Stored && loan.lifecycleBound &&
          loan.access == lang::AccessMode::Mutable && loan.parent != 0) {
        child = loan.id;
        parent = loan.parent;
      }
  if (function)
    for (const auto &block : function->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.loan == child && instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value) {
          publication = instruction.id;
          const auto *value =
              function->body.findValue(instruction.receiver->value);
          input = value ? value->definition : 0;
        }
  expect(function && child && parent && publication && input,
         "stored receiver exercises exact shared entry and staged publication");
  if (!function || !child || !parent || !publication || !input)
    return;
  expect(function->body.findLoan(parent)->entry &&
             function->body.findLoan(parent)->access ==
                 lang::AccessMode::ReadOnly,
         "the lifetime parent remains readonly");
  const auto find = [](lang::MirBody &body, lang::MirInstructionId id) {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id)
          return &instruction;
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  for (int mutation = 0; mutation != 18; ++mutation) {
    auto malformed = frontend.mir;
    auto &changed = const_cast<lang::MirFunctionInstance &>(
        *malformed.findFunctionInstance(functionId));
    auto &result = const_cast<lang::MirLoan &>(*changed.body.findLoan(child));
    auto &ancestor =
        const_cast<lang::MirLoan &>(*changed.body.findLoan(parent));
    auto &call = *find(changed.body, publication);
    auto &stage = *find(changed.body, input);
    switch (mutation) {
    case 0:
      result.parent = 0;
      break;
    case 1:
      result.parent = child;
      break;
    case 2:
      result.source = ancestor.source;
      break;
    case 3:
      result.access = lang::AccessMode::ReadOnly;
      break;
    case 4:
      result.lifecycleBound = false;
      break;
    case 5:
      call.functionTarget.reset();
      break;
    case 6:
      call.borrowAccess = lang::AccessMode::ReadOnly;
      break;
    case 7:
      stage.callInputRole = lang::MirCallInputRole::Argument;
      break;
    case 8:
      stage.callInputIndex = 1;
      break;
    case 9:
      stage.callSite = 0;
      break;
    case 10:
      stage.operands.front().kind = lang::MirOperandKind::BorrowWrite;
      break;
    case 11:
      for (auto &block : changed.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
              instruction.loan == child)
            instruction.loan = parent;
      break;
    case 12:
      ancestor.entry = false;
      break;
    case 13:
      ancestor.access = lang::AccessMode::Mutable;
      break;
    case 14:
      result.producedBy = 0;
      break;
    case 15:
      stage.operands.front().place = ancestor.source;
      break;
    case 16:
      for (auto &type : const_cast<std::vector<lang::MirClassInstance> &>(
               malformed.classInstances()))
        type.mutexGuardBorrowCarrier = false;
      break;
    case 17: {
      auto &callee = const_cast<lang::MirFunctionInstance &>(
          *malformed.findFunctionInstance(*call.functionTarget));
      callee.returnBorrowAccess = lang::AccessMode::ReadOnly;
      for (auto &loan : callee.body.loans)
        if (loan.kind == lang::MirLoanKind::Return)
          loan.access = lang::AccessMode::ReadOnly;
      call.borrowAccess = result.access = lang::AccessMode::ReadOnly;
      break;
    }
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "stored publication rejects forged "
           "parent/input/target/access/endpoint/capability " +
               std::to_string(mutation));
  }
  for (const auto &conflict :
       {std::string("auto guard = this.owner.lock(); see(this.owner);"),
        std::string("auto guard = this.owner.lock(); auto second = "
                    "this.owner.lock();")}) {
    auto invalid = source;
    const auto position = invalid.find("auto guard = this.owner.lock();");
    invalid.replace(position,
                    std::string("auto guard = this.owner.lock();").size(),
                    conflict);
    const auto declaration = invalid.find("class User");
    invalid.insert(declaration, "void see(std::mutex<int32_t>& value) {}\n");
    const auto rejected = analyze("mutex-ancestor-conflict.gti", invalid);
    bool exactConflict = false;
    for (const auto &body : rejected.mir.functionInstances())
      for (const auto &error : lang::verifyMirBody(body.body, body.id).errors)
        exactConflict |=
            error.message.find("conflicts with active mutable loan") !=
            std::string::npos;
    // This tests the MIR firewall. The separate source-diagnostic frontier
    // records that these invalid sources still reach B0001, not a public rule.
    expect(rejected.semanticValid && rejected.hirValid && !rejected.mirValid &&
               exactConflict,
           "readonly ancestor use/publication cannot bypass its live exclusive "
           "child");
  }
  const auto nested = analyze("mutex-nested-receiver.gti", R"gti(
#include <std/mutex>
class Holder {
public:
  std::mutex<int32_t>& owner;
  Holder(std::mutex<int32_t>& owner) : owner(owner) {}
};
class User {
  Holder& holder;
public:
  User(Holder& holder) : holder(holder) {}
  void use() { auto guard = this.holder.owner.lock(); }
};
int main() {
  std::mutex<int32_t> owner{7};
  Holder holder{owner};
  User user{holder};
  user.use();
  return 0;
}
)gti");
  if (!nested.canGenerateCode())
    printDiagnostics(nested);
  expect(nested.canGenerateCode() && lang::verifyMirProgram(nested.mir).valid(),
         "nested stored receiver paths preserve exact lifetime ancestry");
  auto mutableCarrier = analyze("ordinary-exclusive-carrier.gti", R"gti(
class Lease {
  mut int& value;
public:
  Lease(mut int& value) : value(value) {}
  void apply() mut { this.value += 1; }
  ~Lease() { this.value += 1; }
};
class Holder {
  mut int& value;
public:
  Holder(mut int& value) : value(value) {}
  void apply() mut { mut Lease lease{this.value}; lease.apply(); }
};
int main() {
  mut int value = 7;
  { mut Holder holder{value}; holder.apply(); }
  return value - 9;
}
)gti");
  expect(mutableCarrier.canGenerateCode() &&
             lang::verifyMirProgram(mutableCarrier.mir).valid(),
         "unrelated mutable stored-reference construction remains valid");
}

void testStoredPublicationRecursion() {
  const auto frontend = analyze("mutex-recursive-publication.gti", R"gti(
#include <std/mutex>
class Owner {
  std::mutex<int32_t> value{7};
public:
  Owner() {}
  std::mutex_guard<int32_t> grounded(bool ignored) { return this.value.lock(); }
  std::mutex_guard<int32_t> left(bool again) {
    if (again) { return this.right(false); }
    return this.grounded(false);
  }
  std::mutex_guard<int32_t> right(bool again) {
    if (again) { return this.left(false); }
    return this.grounded(false);
  }
};
int main() {
  Owner owner{};
  { auto guard = owner.left(true); }
  { auto guard = owner.right(true); }
  return 0;
}
)gti");
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "finite recursive guard returns preserve their checked construction "
         "base");
  if (!frontend.canGenerateCode())
    return;
  lang::HirFunctionInstanceId left = 0, right = 0, grounded = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source)
      continue;
    const auto &name = function.source->name().lexeme;
    if (name == "left")
      left = function.id;
    if (name == "right")
      right = function.id;
    if (name == "grounded")
      grounded = function.id;
  }
  expect(left && right && grounded,
         "recursive publication fixture has exact targets");
  if (!left || !right || !grounded)
    return;
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto changed = frontend.mir;
    unsigned edits = 0;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             changed.functionInstances())) {
      if (function.id != left && function.id != right)
        continue;
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions) {
          if (instruction.functionTarget != grounded ||
              (mutation == 2 && function.id != left))
            continue;
          if (mutation == 2)
            instruction.borrowAccess = lang::AccessMode::ReadOnly;
          else
            instruction.functionTarget = mutation == 0         ? function.id
                                         : function.id == left ? right
                                                               : left;
          ++edits;
        }
    }
    expect(edits == (mutation == 2 ? 1U : 2U),
           "recursive publication mutation reaches exact exits");
    expect(lang::verifyMirProgram(changed).valid() == (mutation != 2),
           "legal relay-only cycles stay valid but one forged finite exit "
           "rejects");
  }
  const auto denied = analyze("ordinary-readonly-publication.gti", R"gti(
class Lease {
  mut int& value;
public:
  Lease(mut int& value) : value(value) {}
  ~Lease() {}
};
class Holder {
  int& value;
public:
  Holder(int& value) : value(value) {}
  void use() { Lease lease{this.value}; }
};
int main() {
  int value = 7;
  Holder holder{value};
  holder.use();
  return 0;
}
)gti");
  expect(!denied.canGenerateCode() &&
             (hasCode(denied, "GTI-S2012") || hasCode(denied, "GTI-S2045")),
         "ordinary readonly inputs gain no mutable reference authority");
}

} // namespace

int main() {
  testDormantImport();
  testPublicPipeline();
  testReferencePrefixBeforeOwnedField();
  testPreparedMutexPayload();
  testPassiveClosureParameterStorage();
  testMutexCleanupRecipe();
  testSemanticBoundaries();
  testPayloadConcurrencyCapabilities();
  testVerifierMutations();
  testStoredReceiverPublication();
  testStoredPublicationRecursion();
  return failures == 0 ? 0 : 1;
}
