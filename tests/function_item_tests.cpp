#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::TargetInfo concurrentTarget() {
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  return target;
}

lang::FrontendResult
analyze(std::string_view name, std::string source,
        lang::TargetInfo target = lang::TargetInfo::host()) {
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

template <typename Function>
void forEachInstruction(const lang::MirProgram &program, Function &&function) {
  const auto visit = [&](const lang::MirBody &body) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        function(instruction);
      }
    }
  };
  visit(program.module());
  for (const lang::MirFunctionInstance &instance :
       program.functionInstances()) {
    visit(instance.body);
  }
  for (const lang::MirConstructorInstance &instance :
       program.constructorInstances()) {
    visit(instance.body);
  }
  for (const lang::MirDestructorInstance &instance :
       program.destructorInstances()) {
    visit(instance.body);
  }
  for (const lang::MirLambdaInstance &instance : program.lambdaInstances()) {
    visit(instance.body);
  }
}

lang::MirInstruction *firstFunctionItem(lang::MirProgram &program) {
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances())) {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(instance.body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        if (instruction.operation == lang::MirOperation::FunctionItem) {
          return &instruction;
        }
      }
    }
  }
  return nullptr;
}

lang::MirFunctionInstance *functionItemRelay(lang::MirProgram &program) {
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances())) {
    if (instance.body.returnType.hasFunctionItemShape()) {
      return &instance;
    }
  }
  return nullptr;
}

lang::MirInstruction *functionItemRelayMove(lang::MirProgram &program) {
  lang::MirFunctionInstance *relay = functionItemRelay(program);
  if (relay == nullptr) {
    return nullptr;
  }
  for (lang::MirBlock &block :
       const_cast<std::vector<lang::MirBlock> &>(relay->body.blocks)) {
    for (lang::MirInstruction &instruction :
         const_cast<std::vector<lang::MirInstruction> &>(block.instructions)) {
      if (instruction.kind == lang::MirInstructionKind::Move &&
          instruction.info.type.hasFunctionItemShape()) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

lang::MirTerminator *functionItemRelayReturn(lang::MirProgram &program) {
  lang::MirFunctionInstance *relay = functionItemRelay(program);
  if (relay == nullptr) {
    return nullptr;
  }
  for (lang::MirBlock &block :
       const_cast<std::vector<lang::MirBlock> &>(relay->body.blocks)) {
    if (block.terminator.kind == lang::MirTerminatorKind::Return &&
        block.terminator.value &&
        block.terminator.value->type.hasFunctionItemShape()) {
      return &block.terminator;
    }
  }
  return nullptr;
}

void testGenericAndManagedPipeline() {
  const std::string source = R"(
#include <std/algorithm>
#include <std/functional>
#include <std/thread>
#include <std/vector>

int main() {
  std::vector<int32_t> values = std::vector<int32_t>({1, 2, 3});
  auto count = std::count_if(values, tasks::positive);
  if (count != 3) { return 1; }
  [[discard]] std::for_each(values, tasks::observe);
  std::invoke_once(tasks::run);

  auto started = std::thread::start(tasks::run);
  if (!started.has_value()) { return 2; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  if (!joined.has_value()) { return 3; }

  mut std::thread_result<int32_t> result =
      std::thread_result<int32_t>();
  auto result_started = result.start(tasks::answer);
  if (!result_started.has_value()) { return 4; }
  auto answer = result.join();
  if (!answer.has_value() || answer.value() != 42) { return 5; }
  return 0;
}

namespace tasks {
bool positive(int32_t& value) { return value > 0; }
void observe(int32_t& value) {}
void run() {}
int32_t answer() { return 42; }
}
)";
  const lang::FrontendResult frontend =
      analyze("function-items.gti", source, concurrentTarget());
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "qualified free-function items should work before their definitions "
         "through an algorithm and both managed-thread result forms");
  if (!frontend.canGenerateCode()) {
    return;
  }

  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "HIR should verify every nominal function-item target");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should verify function items and managed-task adapters");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const std::size_t selectedName = source.find("tasks::positive");
  const std::size_t selectedOffset =
      selectedName + std::string("tasks::").size() + 1;
  const std::size_t declarationOffset = source.rfind("positive(int32_t&");
  const lang::LanguageQueries queries;
  const std::optional<lang::HoverInfo> hover =
      queries.hover(frontend, unit, selectedOffset);
  const std::optional<lang::DefinitionInfo> definition =
      queries.definition(frontend, unit, selectedOffset);
  if (!hover || !definition) {
    std::cerr << "function-item tooling hover=" << bool(hover)
              << " definition=" << bool(definition) << '\n';
  } else if (hover->signature.find("positive(int32_t& value)") ==
                 std::string::npos ||
             definition->target.start != declarationOffset) {
    std::cerr << "function-item tooling signature=" << hover->signature
              << " target=" << definition->target.start
              << " expected=" << declarationOffset << '\n';
  }
  expect(hover &&
             hover->signature.find("positive(int32_t& value)") !=
                 std::string::npos &&
             definition && definition->target.start == declarationOffset,
         "hover and go-to-definition should resolve a contextual function "
         "item to its exact namespace declaration");

  std::size_t hirItems = 0;
  bool exactHirItems = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      if (value.kind != lang::HirValueKind::FunctionItem) {
        continue;
      }
      ++hirItems;
      const lang::HirFunctionInstance *target =
          value.functionTarget
              ? frontend.hir.findFunctionInstance(*value.functionTarget)
              : nullptr;
      exactHirItems = exactHirItems && target != nullptr && !target->owner &&
                      value.info.type.hasFunctionItemShape() &&
                      value.info.type.functionId == target->declaration;
    }
  }
  expect(hirItems == 5 && exactHirItems,
         "concrete HIR should retain five exact, owner-free function-item "
         "identities");

  std::size_t mirItems = 0;
  bool exactMirItems = true;
  forEachInstruction(
      frontend.mir, [&](const lang::MirInstruction &instruction) {
        if (instruction.operation != lang::MirOperation::FunctionItem) {
          return;
        }
        ++mirItems;
        const lang::MirFunctionInstance *target =
            instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        exactMirItems = exactMirItems && target != nullptr &&
                        instruction.info.type.hasFunctionItemShape() &&
                        instruction.info.type.functionId == target->declaration;
      });
  expect(mirItems == 5 && exactMirItems,
         "verified MIR should retain each function item as a nominal "
         "zero-state value with one exact target");

  const std::size_t itemTasks = static_cast<std::size_t>(
      std::count_if(frontend.mir.threadTaskAdapters().begin(),
                    frontend.mir.threadTaskAdapters().end(),
                    [](const lang::MirThreadTaskAdapter &adapter) {
                      return adapter.taskType.hasFunctionItemShape() &&
                             adapter.functionTarget.has_value() &&
                             !adapter.lambdaTarget.has_value() &&
                             adapter.targetInvocation ==
                                 lang::CallableInvocationCapability::Read;
                    }));
  expect(itemTasks == 2,
         "managed-thread MIR should retain exact void and result function "
         "entries without a callable object");

  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = concurrentTarget()});
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "optimization should preserve function-item identity and task "
         "adapters");
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, concurrentTarget(),
      optimized.report.transformations);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "LoweredProgram should retain the complete function-item inventory");
  {
    lang::CppMirBodyEmissionMap rows(lang::buildCppMirBodyEmissionMapRows(
        lowered, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter emitter(optimized.mir, rows);
    bool foundRelay = false;
    bool exactRelaySupport = true;
    for (const lang::MirFunctionInstance &instance :
         optimized.mir.functionInstances()) {
      if (!instance.returnType.hasFunctionItemShape()) {
        continue;
      }
      foundRelay = true;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = instance.id};
      const lang::CppMirBodyEmissionAnalysis analysis =
          emitter.analyze(address);
      const bool supported = analysis.ready() &&
                             !emitter.supportsBodyText(address) &&
                             emitter.supportsFailureBodyText(address) &&
                             instance.mayRaiseDefinedFailure;
      exactRelaySupport = exactRelaySupport && supported;
      if (!supported) {
        for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
          std::cerr << "function-item relay issue: " << issue.detail << '\n';
        }
      }
    }
    expect(foundRelay && exactRelaySupport,
           "the discarded confined relay should have one exact failure-form "
           "C++ MIR vocabulary and no incomplete plain form");
  }
  const lang::CppMirRepresentationSnapshotBuild snapshot =
      lang::buildCppMirRepresentationSnapshot(lowered,
                                              lang::CppStandard::Cpp23);
  if (!snapshot.valid()) {
    for (const lang::CppMirRepresentationSnapshotIssue &issue :
         snapshot.issues) {
      std::cerr << "function-item snapshot: " << issue.detail << '\n';
    }
  }
  expect(snapshot.valid(),
         "the complete function-item program should pass the sealed C++ MIR "
         "representation preflight");
  const std::string cpp =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  const std::size_t programEnd = cpp.find("} // namespace __gti_program");
  const std::size_t adapterDefinition =
      cpp.find("struct managed_thread_task_adapter<", programEnd);
  expect(cpp.find("::gti_internal::backend::function_item<") !=
                 std::string::npos &&
             programEnd != std::string::npos &&
             adapterDefinition != std::string::npos &&
             adapterDefinition > programEnd,
         "the C++ backend should use a private nominal tag and define thread "
         "adapters only after exact GTI target declarations");

  lang::MirProgram missingTarget = frontend.mir;
  lang::MirInstruction *missingTargetItem = firstFunctionItem(missingTarget);
  if (missingTargetItem != nullptr) {
    missingTargetItem->functionTarget.reset();
  }
  expect(missingTargetItem != nullptr &&
             !lang::verifyMirProgram(missingTarget).valid(),
         "MIR verification should reject a function item detached from its "
         "nominal declaration");

  lang::MirProgram wrongIdentity = frontend.mir;
  lang::MirInstruction *wrongIdentityItem = firstFunctionItem(wrongIdentity);
  if (wrongIdentityItem != nullptr) {
    ++wrongIdentityItem->info.type.functionId;
  }
  expect(wrongIdentityItem != nullptr &&
             !lang::verifyMirProgram(wrongIdentity).valid(),
         "MIR verification should reject a function item whose nominal "
         "FunctionId disagrees with its target row");

  lang::MirProgram wrongSignature = frontend.mir;
  lang::MirInstruction *wrongSignatureItem = firstFunctionItem(wrongSignature);
  if (wrongSignatureItem != nullptr &&
      !wrongSignatureItem->info.type.arguments.empty()) {
    wrongSignatureItem->info.type.arguments.front() =
        lang::SemanticType::UInt64;
  }
  expect(wrongSignatureItem != nullptr &&
             !lang::verifyMirProgram(wrongSignature).valid(),
         "MIR verification should reject a function item whose exact "
         "signature disagrees with its target row");

  const auto ownedTarget =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &instance) {
                     return instance.owner.has_value();
                   });
  lang::MirProgram wrongOwner = frontend.mir;
  lang::MirInstruction *wrongOwnerItem = firstFunctionItem(wrongOwner);
  if (wrongOwnerItem != nullptr &&
      ownedTarget != frontend.mir.functionInstances().end()) {
    wrongOwnerItem->functionTarget = ownedTarget->id;
  }
  expect(wrongOwnerItem != nullptr &&
             ownedTarget != frontend.mir.functionInstances().end() &&
             !lang::verifyMirProgram(wrongOwner).valid(),
         "MIR verification should reject a function item retargeted to a "
         "class-owned function");

  lang::MirProgram wrongLinkage = frontend.mir;
  lang::MirInstruction *wrongLinkageItem = firstFunctionItem(wrongLinkage);
  lang::MirFunctionInstance *wrongLinkageTarget = nullptr;
  if (wrongLinkageItem != nullptr && wrongLinkageItem->functionTarget) {
    auto &instances = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongLinkage.functionInstances());
    const auto found =
        std::find_if(instances.begin(), instances.end(), [&](const auto &row) {
          return row.id == *wrongLinkageItem->functionTarget;
        });
    if (found != instances.end()) {
      wrongLinkageTarget = &*found;
      wrongLinkageTarget->linkage = lang::LanguageLinkage::C;
    }
  }
  expect(wrongLinkageTarget != nullptr &&
             !lang::verifyMirProgram(wrongLinkage).valid(),
         "MIR verification should reject a function item retargeted across "
         "the native linkage boundary");

  lang::MirProgram wrongRelayMove = frontend.mir;
  lang::MirInstruction *relayMove = functionItemRelayMove(wrongRelayMove);
  if (relayMove != nullptr && !relayMove->operands.empty()) {
    ++relayMove->operands.front().type.functionId;
  }
  expect(relayMove != nullptr && !relayMove->operands.empty() &&
             !lang::verifyMirProgram(wrongRelayMove).valid(),
         "MIR verification should reject a confined relay move whose source "
         "has a different nominal function identity");

  lang::MirProgram wrongRelayReturn = frontend.mir;
  lang::MirTerminator *relayReturn = functionItemRelayReturn(wrongRelayReturn);
  if (relayReturn != nullptr && relayReturn->value) {
    ++relayReturn->value->type.functionId;
  }
  expect(relayReturn != nullptr && relayReturn->value &&
             !lang::verifyMirProgram(wrongRelayReturn).valid(),
         "MIR verification should reject a confined relay Return whose value "
         "has a different nominal function identity");
}

void testContextualDiagnosticsAndBoundaries() {
  const lang::FrontendResult noContext =
      analyze("function-item-context.gti", R"(
void work() {}
int main() {
  auto stored = work;
  auto captured = [stored]() -> void { stored(); };
  stored();
  captured();
  return 0;
}
)");
  if (!noContext.canGenerateCode())
    printDiagnostics(noContext);
  expect(noContext.canGenerateCode(),
         "an unambiguous namespace function can be stored and captured with "
         "its exact callable identity");

  const std::string ambiguousStorageSource =
      "void work(int value) {}\nvoid work(bool value) {}\n"
      "int main() { auto item = work; return 0; }\n";
  const auto ambiguousStorage =
      analyze("ambiguous-storage.gti", ambiguousStorageSource);
  expect(!ambiguousStorage.canGenerateCode() &&
             hasCode(ambiguousStorage, "GTI-S2090") &&
             hasHint(ambiguousStorage, "explicitly typed lambda"),
         "an overload set cannot become a stored value without exact context");
  bool exactAmbiguity = false;
  for (const auto &diagnostic : ambiguousStorage.diagnostics) {
    if (diagnostic.code != "GTI-S2090")
      continue;
    exactAmbiguity =
        diagnostic.phase == lang::DiagnosticPhase::Semantics &&
        diagnostic.primary.source.ends_with("ambiguous-storage.gti") &&
        diagnostic.primary.start == ambiguousStorageSource.rfind("work") &&
        diagnostic.primary.end == diagnostic.primary.start + 4 &&
        diagnostic.related.size() == 2 && diagnostic.fixes.empty();
  }
  expect(exactAmbiguity, "function-item ambiguity preserves its source span, "
                         "both candidates and no speculative fix-it");
  for (const std::string declaration :
       {"unsafe void work() {}", "void work<T>(T value) {}",
        "extern \"C\" { void work(); }"}) {
    const auto invalid =
        analyze("ineligible-stored-item.gti",
                declaration + "\nint main() { auto item = work; return 0; }\n");
    expect(!invalid.canGenerateCode() && hasCode(invalid, "GTI-S2090") &&
               !hasCode(invalid, "GTI-B0001"),
           "stored items cannot erase unsafe contracts, infer generic targets "
           "without context, or acquire a C ABI address");
  }

  const lang::FrontendResult mismatch =
      analyze("function-item-mismatch.gti", R"(
void invoke<Function>(Function function) { function(1); }
void selected(bool value) {}
int main() {
  invoke(selected);
  return 0;
}
)");
  expect(!mismatch.canGenerateCode() && hasCode(mismatch, "GTI-S2090") &&
             hasMessage(mismatch, "No eligible namespace-scope GTI function"),
         "a contextually selected item should reject an inexact signature "
         "without conversion ranking");

  const lang::FrontendResult ambiguous =
      analyze("function-item-ambiguous.gti", R"(
void invoke<Function>(Function function) {
  int32_t value = 1;
  function(value);
}
void selected(int32_t value) {}
void selected(int32_t& value) {}
int main() {
  invoke(selected);
  return 0;
}
)");
  expect(!ambiguous.canGenerateCode() && hasCode(ambiguous, "GTI-S2090") &&
             hasMessage(ambiguous, "function item is ambiguous"),
         "a callable context admitting two overloads should diagnose "
         "ambiguity instead of ranking them");

  const lang::FrontendResult escape = analyze("function-item-escape.gti", R"(
Function relay<Function>(Function function) {
  return std::move(function);
}
void task() {}
int main() {
  auto escaped = relay(task);
  escaped();
  return 0;
}
)");
  if (!escape.canGenerateCode())
    printDiagnostics(escape);
  expect(escape.canGenerateCode(),
         "a stateless function item can enter exact owned generic transport");

  const lang::FrontendResult storedResult = analyze("function-item-storage.gti",
                                                    R"(
#include <std/algorithm>
#include <std/vector>
void observe(int32_t& value) {}
int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  auto retained = std::for_each(values, observe);
  int value = 7;
  retained(value);
  return 0;
}
)");
  if (!storedResult.canGenerateCode())
    printDiagnostics(storedResult);
  expect(storedResult.canGenerateCode(),
         "a function item returned by a confined algorithm can be retained "
         "and invoked with its original exact signature");

  const lang::FrontendResult native = analyze("function-item-native.gti", R"(
using Callback = () -> void;
void task() {}
int main() {
  Callback callback = task;
  return 0;
}
)");
  std::size_t nativeCallbacks = 0;
  std::size_t functionItems = 0;
  for (const lang::HirFunctionInstance &instance :
       native.hir.functionInstances()) {
    nativeCallbacks += static_cast<std::size_t>(
        std::count_if(instance.body.values.begin(), instance.body.values.end(),
                      [](const lang::HirValue &value) {
                        return value.kind == lang::HirValueKind::NativeCallback;
                      }));
    functionItems += static_cast<std::size_t>(
        std::count_if(instance.body.values.begin(), instance.body.values.end(),
                      [](const lang::HirValue &value) {
                        return value.kind == lang::HirValueKind::FunctionItem;
                      }));
  }
  expect(native.canGenerateCode() && nativeCallbacks == 1 && functionItems == 0,
         "a named native-function context should keep the existing typed C "
         "callback conversion distinct from GTI function items");

  const lang::FrontendResult direct =
      analyze("function-item-direct-call.gti", R"(
int32_t choose(int32_t value) { return value + 1; }
bool choose(bool value) { return !value; }
int main() { return choose(1) - 2; }
)");
  expect(direct.canGenerateCode() && !hasCode(direct, "GTI-S2090"),
         "ordinary exact overloaded direct calls should remain unchanged");
}

void testConstructorValueArguments() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/function_item_construction.gti";
  std::ifstream input(path);
  expect(input.good(), "constructor function-item fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto valid = analyze("function-item-construction.gti", source);
  if (!valid.canGenerateCode())
    printDiagnostics(valid);
  expect(valid.canGenerateCode(),
         "parenthesized constructors analyze a deferred function item and "
         "its following values in every concrete generic body");

  const auto ambiguous = analyze("ambiguous-constructor-item.gti", R"(
class Box {
public:
  Box<Callable>(Callable callable) {}
};
void work(int value) {}
void work(bool value) {}
int main() { auto value = Box(work); return 0; }
)");
  expect(!ambiguous.canGenerateCode() && hasCode(ambiguous, "GTI-S2090") &&
             !hasCode(ambiguous, "GTI-B0001"),
         "an ambiguous constructor function argument fails in semantics, "
         "not as an unresolved MIR value");
}

} // namespace

int main() {
  testGenericAndManagedPipeline();
  testContextualDiagnosticsAndBoundaries();
  testConstructorValueArguments();
  return failures == 0 ? 0 : 1;
}
