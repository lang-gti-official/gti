// MIR and HIR verifier rejections of forged programs, which have no
// source-level form: a valid program's MIR (or HIR) is copied, one contract
// fact is corrupted, and the verifier must reject the result. Covers
// managed-thread, atomic and mutex intrinsic contracts, persistent storage
// and reference-valued pack inputs, native indirect calls, Expected lambda
// mappers, ephemeral carrier schedules, and the HIR plans of immediate void
// completions and bare returns. (Restored from the deleted
// managed_thread_tests.cpp, atomic_tests.cpp, mutex_tests.cpp,
// mir_persistent_storage_tests.cpp, native_callback_tests.cpp,
// expected_transform_error_tests.cpp, ephemeral_carrier_mir_tests.cpp and
// compiler_constexpr_tests.cpp, minimally adapted: whole test functions, so
// their source-level assertions run again beside the forgeries; their GTI
// sources are also tests/ui cases. Each namespace keeps its origin's helpers;
// the shared fixture helpers of compiler_test_fixture.h are at file scope.)
#include "../../src/compiler/mir_storage_effects.h"
#include "../llvm_backend_test_support.h"
#include "../lowered_program_test_support.h"
#include "../temporary_directory.h"
#include "gti/ast_printer.h"
#include "gti/backend.h"
#include "gti/binary_float.h"
#include "gti/executable_path.h"
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/native_header.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/standard_library.h"
#include "gti/support.h"
#include "unit_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

int failures = 0;

[[maybe_unused]] void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] bool containsMangledFunction(std::string_view text,
                                              std::string_view prefix,
                                              std::string_view sourceName,
                                              std::string_view suffix) {
  constexpr std::string_view legacyMarker = "__gti_fn_";
  constexpr std::string_view concreteMarker = "__gti_inst_";
  std::size_t search = 0;
  while (true) {
    const std::size_t legacy = text.find(legacyMarker, search);
    const std::size_t concrete = text.find(concreteMarker, search);
    const bool concreteName =
        concrete != std::string_view::npos &&
        (legacy == std::string_view::npos || concrete < legacy);
    const std::size_t function = concreteName ? concrete : legacy;
    if (function == std::string_view::npos) {
      return false;
    }
    const std::string_view marker =
        concreteName ? concreteMarker : legacyMarker;
    search = function + marker.size();
    if (function < prefix.size() ||
        text.substr(function - prefix.size(), prefix.size()) != prefix) {
      continue;
    }
    std::size_t tail = search;
    if (concreteName) {
      while (tail < text.size() && tail - search < 64 &&
             ((text[tail] >= '0' && text[tail] <= '9') ||
              (text[tail] >= 'a' && text[tail] <= 'f'))) {
        ++tail;
      }
      if (tail - search != 64) {
        continue;
      }
    } else {
      while (tail < text.size() && text[tail] >= '0' && text[tail] <= '9') {
        ++tail;
      }
      if (tail == search) {
        continue;
      }
    }
    if (tail >= text.size() || text[tail] != '_') {
      continue;
    }
    ++tail;
    if (text.substr(tail, sourceName.size()) != sourceName) {
      continue;
    }
    tail += sourceName.size();
    if (text.substr(tail, suffix.size()) == suffix) {
      return true;
    }
  }
}

[[maybe_unused]] std::unordered_set<std::string>
concreteBackendSymbols(std::string_view text, std::string_view marker,
                       std::string_view sourceName) {
  std::unordered_set<std::string> result;
  std::size_t search = 0;
  while ((search = text.find(marker, search)) != std::string_view::npos) {
    const std::size_t digest = search + marker.size();
    std::size_t tail = digest;
    while (tail < text.size() && tail - digest < 64 &&
           ((text[tail] >= '0' && text[tail] <= '9') ||
            (text[tail] >= 'a' && text[tail] <= 'f'))) {
      ++tail;
    }
    if (tail - digest == 64 && tail < text.size() && text[tail] == '_' &&
        text.substr(tail + 1, sourceName.size()) == sourceName) {
      const std::size_t end = tail + 1 + sourceName.size();
      result.emplace(text.substr(search, end - search));
    }
    search = digest;
  }
  return result;
}

// Analyzes a backend-neutral emission fixture and requires that it completes
// verified MIR lowering. Assert on the returned MIR/HIR/semantics, or on
// gti_test::llvmEmits for native emission.
[[maybe_unused]] lang::FrontendResult
analyzeEmissionFixture(std::string_view source,
                       lang::TargetInfo target = lang::TargetInfo::host()) {
  lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("compiler-emission-fixture.gti", std::string(source));
  expect(frontend.canGenerateCode(),
         "a semantically valid emission fixture should complete MIR "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected emission-fixture diagnostic: "
                << diagnostic.message << '\n';
    }
  } else {
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "an emission fixture should produce verified MIR");
  }
  return frontend;
}

[[maybe_unused]] bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                                    const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

[[maybe_unused]] bool
hasDiagnosticAt(const std::vector<lang::Diagnostic> &diagnostics,
                std::string_view code, std::string_view source,
                std::size_t start, std::string_view reason) {
  const bool matched =
      std::any_of(diagnostics.begin(), diagnostics.end(),
                  [&](const lang::Diagnostic &diagnostic) {
                    const std::string_view actualSource =
                        diagnostic.primary.source;
                    const std::string actualFile =
                        std::filesystem::path(diagnostic.primary.source)
                            .filename()
                            .string();
                    return diagnostic.code == code &&
                           (actualSource == source || actualFile == source) &&
                           diagnostic.primary.start == start &&
                           diagnostic.message.find(reason) != std::string::npos;
                  });
  if (!matched) {
    std::cerr << "Expected diagnostic " << code << " at " << source << ':'
              << start << " containing '" << reason << "'.\n";
    for (const lang::Diagnostic &diagnostic : diagnostics) {
      std::cerr << "  found " << diagnostic.code << " at "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  return matched;
}

[[maybe_unused]] bool
hasMirVerificationError(const lang::MirVerificationResult &result,
                        std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(reason) != std::string::npos;
                     });
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const lang::SemanticVisitor &semantic,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticContaining(const std::vector<lang::Diagnostic> &diagnostics,
                          const std::string &text) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &item) {
                      return item.message.find(text) != std::string::npos;
                    }));
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedPreludeFixture(const std::string &name, const std::string &source,
                             lang::FrontendOptions options = {}) {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / ("gti-trusted-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {standardLibraryPrelude(), entry},
               {{entryKey, source}}, {standardLibraryRoot()});
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedMemoryFixture(const std::string &name, const std::string &source,
                            lang::FrontendOptions options = {}) {
  const auto entry = standardLibraryRoot() / "__trusted_tests__" /
                     std::filesystem::path(name).filename();
  const std::string body =
      "import std.memory; import \"../std/detail/memory_capabilities.gti\";\n" +
      source;
  return lang::Frontend(std::move(options))
      .analyze(entry, body, {standardLibraryPrelude(), entry},
               {{std::filesystem::weakly_canonical(entry).string(), body}},
               {standardLibraryRoot()});
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedStandalonePreludeFixture(const std::string &name,
                                       const std::string &source,
                                       lang::FrontendOptions options = {}) {
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      ("gti-trusted-standalone-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {entry}, {{entryKey, source}});
}

[[maybe_unused]] const lang::FunctionDecl *
findTopLevelFunction(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findTopLevelClass(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findClassRecursive(const lang::StmtList &statements, const std::string &name) {
  for (const lang::StmtPtr &statement : statements) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(statement.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
    const auto *namespaceDecl =
        dynamic_cast<const lang::NamespaceDecl *>(statement.get());
    if (namespaceDecl != nullptr) {
      if (const lang::ClassDecl *nested =
              findClassRecursive(namespaceDecl->declarations(), name)) {
        return nested;
      }
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::VariableDecl *
findDirectLocal(const lang::FunctionDecl *function, const std::string &name) {
  if (function == nullptr || !function->body()) {
    return nullptr;
  }
  for (const lang::StmtPtr &statement : function->body()->statements()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(statement.get());
    if (variable != nullptr && variable->name().lexeme == name) {
      return variable;
    }
  }
  return nullptr;
}

[[maybe_unused]] std::string nonNullReferenceSource() {
  return R"(
struct Counter {
public:
  Counter() = default;
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }

int main() {
  mut Counter counter = Counter();
  mut Counter& writable = counter;
  increment(writable);
  Counter& read_only = counter;

  mut int value = 1;
  mut int& alias = value;
  alias += 2;
  if (read(read_only) == 1 and value == 3) {
    return 0;
  }
  return 1;
}
)";
}

struct NamedTest {
  std::string_view name;
  void (*run)();
};

} // namespace

namespace managed_thread {
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
  const std::filesystem::path standardLibrary = std::filesystem::path(__FILE__)
                                                    .parent_path()
                                                    .parent_path()
                                                    .parent_path() /
                                                "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

std::string validProgram() {
  return R"(
import std.thread;
import std.memory;

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
public:
  class_task() = default;
};

class fresh_task {
public:
  void operator()() { std::println("fresh class task"); }
public:
  fresh_task() = default;
};

int main() {
  auto first_started = std::thread::start(
      []() -> void { std::println("first task"); });
  if (!first_started.has_value()) { return 1; }
  std::thread first = std::move(first_started).value();
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
    std::thread fourth = std::move(fourth_started).value();
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

  lang::MirProgram missingCleanup = frontend.mir;
  bool corruptedActiveCapture = false;
  for (const auto &adapter : missingCleanup.threadTaskAdapters()) {
    if (!adapter.lambdaTarget)
      continue;
    auto *lambda = const_cast<lang::MirLambdaInstance *>(
        missingCleanup.findLambda(*adapter.lambdaTarget));
    if (!lambda || lambda->captureTypes.empty() ||
        !std::any_of(lambda->captureRequiresActiveCleanup.begin(),
                     lambda->captureRequiresActiveCleanup.end(),
                     [](bool active) { return active; }))
      continue;
    lambda->captureTypes.front() = lang::SemanticType::Unknown;
    corruptedActiveCapture = true;
    break;
  }
  const auto cleanupVerification = lang::verifyMirProgram(missingCleanup);
  expect(corruptedActiveCapture &&
             std::any_of(cleanupVerification.errors.begin(),
                         cleanupVerification.errors.end(),
                         [](const auto &error) {
                           return error.message ==
                                  "managed thread active task or result has no "
                                  "exact MIR cleanup recipe";
                         }),
         "MIR task adapter must reject active captures without an exact "
         "cleanup recipe");

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
} // namespace managed_thread

namespace atomic {
namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] lang::TargetInfo concurrentTarget(bool threads = true) {
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = threads;
  return target;
}

lang::FrontendResult analyze(std::string_view name, std::string source,
                             lang::TargetInfo target = concurrentTarget()) {
  lang::FrontendOptions options;
  options.target = std::move(target);
  const std::filesystem::path standardLibrary = std::filesystem::path(__FILE__)
                                                    .parent_path()
                                                    .parent_path()
                                                    .parent_path() /
                                                "stdlib";
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

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::string fixtureSource(std::string_view name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" /
      name;
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
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

lang::MirInstruction *findAtomic(lang::MirProgram &program,
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

void testWrappingOperations() {
  auto target = lang::TargetInfo::host();
  target.runtimeCapabilities.threads = false;
  const auto frontend = analyze(
      "atomic-wrapping.gti", fixtureSource("llvm_atomic_wrapping.gti"), target);
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode(),
         "wrapping operations use exact integer storage without host threads");
  if (!frontend.canGenerateCode())
    return;
  for (auto intrinsic : {lang::IntrinsicKind::AtomicFetchAddWrapping,
                         lang::IntrinsicKind::AtomicFetchSubWrapping}) {
    for (int mutation = 0; mutation < 6; ++mutation) {
      auto invalid = frontend.mir;
      auto *operation = findAtomic(invalid, intrinsic);
      expect(operation != nullptr, "both wrapping operations reach MIR");
      if (!operation)
        continue;
      if (mutation == 0)
        operation->synchronization.order = lang::AtomicMemoryOrder::Count;
      if (mutation == 1)
        operation->parameterTypes.clear();
      if (mutation == 2)
        operation->operands[1].type = lang::SemanticType::Bool;
      if (mutation == 3)
        operation->operands[0].place = 0;
      if (mutation == 4)
        operation->result.reset();
      if (mutation == 5)
        operation->synchronization.kind =
            lang::SynchronizationOperationKind::AtomicLoad;
      expect(!lang::verifyMirProgram(invalid).valid(),
             "wrapping RMW rejects forged type/place/result/order contracts");
    }
  }
  for (const std::string body :
       {"std::atomic<bool> x{false}; x.fetch_add_wrapping(true);",
        "std::atomic<int> x{0}; x.fetch_sub_wrapping(uint64_t(1));",
        "std::atomic<int> x{0}; x.fetch_add_wrapping(1, 0);"}) {
    const auto invalid = analyze(
        "bad-wrapping.gti",
        "import std.atomic;\nint main() { " + body + " return 0; }", target);
    expect(!invalid.canGenerateCode() && !hasCode(invalid, "GTI-B0001"),
           "invalid wrapping calls reject in semantics");
  }
}

} // namespace
} // namespace atomic

namespace mutex {
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
  const std::filesystem::path standardLibrary = std::filesystem::path(__FILE__)
                                                    .parent_path()
                                                    .parent_path()
                                                    .parent_path() /
                                                "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

std::string validProgram() {
  return R"(
import std.mutex;

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

[[maybe_unused]] lang::MirDropType *
findMutexDropRecipe(lang::MirProgram &program, lang::MirDropType &drop) {
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

} // namespace
} // namespace mutex

namespace persistent_storage {
namespace {
bool passed = true;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    passed = false;
  }
}

lang::MirInstruction *findInstruction(lang::MirBody &body,
                                      lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.id == id)
        return &instruction;
  return nullptr;
}

lang::SymbolId symbol(const lang::FrontendResult &frontend,
                      std::string_view name) {
  for (const auto &record : frontend.semantics.database().symbols())
    if (record.name == name)
      return record.id;
  return 0;
}
void persistentTransferIsIndependentlyRejected() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/move.gti", R"(
thread_local mut int32_t value = 1;
int main() { return value; }
)");
  expect(frontend.canGenerateCode(), "persistent observation fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    auto *body = lang::findMirBody(forged, address);
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Load ||
            instruction.operands.size() != 1)
          continue;
        auto &operand = instruction.operands.front();
        const auto *place = body->findPlace(operand.place);
        if (operand.kind == lang::MirOperandKind::Copy && place &&
            place->root == lang::MirPlaceRootKind::Symbol &&
            place->symbol == symbol(frontend, "value")) {
          instruction.kind = lang::MirInstructionKind::Move;
          operand.kind = lang::MirOperandKind::Move;
          changed = true;
        }
      }
  }
  expect(changed, "malformed fixture changes an actual persistent load");
  expect(!lang::verifyMirProgram(forged).valid(),
         "program verification rejects the malformed persistent move");
  const auto proof = lang::verifyMirPersistentStorageEffects(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "destructively transferred") !=
                                  std::string::npos;
                         }),
         "independent storage proof rejects an unrepresented destructive "
         "transfer");
}

void foldHirProof() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/fold-hir.gti", R"(
void touch<T>(mut int32_t& count, T& value) { count += 1; }
void folded<Args...>(mut int32_t& count, Args... args) {
  (touch(count, args), ...);
  (touch(count, args), ...);
}
int main() { mut int32_t count = 0; folded(count, 1, 2); return count - 4; }
)");
  expect(frontend.canGenerateCode(),
         "two unrelated user folds have ordinary concrete call plans");
  if (!frontend.canGenerateCode())
    return;
  lang::HirFunctionInstanceId owner = 0;
  std::vector<lang::HirValueId> folds;
  for (const auto &function : frontend.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.kind == lang::HirValueKind::PackFold) {
        owner = function.id;
        folds.push_back(value.id);
      }
  expect(owner && folds.size() == 2,
         "typed expansion fixture has two exact fold owners");
  if (!owner || folds.size() != 2)
    return;
  for (int mutation = 0; mutation != 14; ++mutation) {
    auto forged = frontend.hir;
    auto &body =
        const_cast<lang::HirBody &>(forged.findFunctionInstance(owner)->body);
    auto *fold = const_cast<lang::HirValue *>(body.findValue(folds.front()));
    auto *call = const_cast<lang::HirValue *>(
        body.findValue(fold->packFoldElements.front().call));
    auto *prefix =
        const_cast<lang::HirValue *>(body.findValue(call->operands.front()));
    auto *element =
        const_cast<lang::HirValue *>(body.findValue(call->operands.back()));
    switch (mutation) {
    case 0:
      fold->packFoldElements.front().call = 0;
      break;
    case 1:
      fold->packFoldElements.front().call =
          body.findValue(folds.back())->packFoldElements.front().call;
      fold->operands[fold->packFoldArgument + 1] =
          fold->packFoldElements.front().call;
      break;
    case 2:
      fold->packFoldElements.back().call = fold->packFoldElements.front().call;
      fold->operands.back() = fold->packFoldElements.front().call;
      break;
    case 3:
      std::swap(fold->packFoldElements.front().call,
                fold->packFoldElements.back().call);
      std::swap(fold->operands[fold->packFoldArgument + 1],
                fold->operands.back());
      break;
    case 4:
      ++prefix->symbol;
      break;
    case 5:
      prefix->info.type = lang::SemanticType::Bool;
      break;
    case 6:
      call->callPlan->arguments.front().kind = lang::HirCallInputKind::Value;
      break;
    case 7:
      element->packElement = 2;
      break;
    case 8:
      element->info.type = lang::SemanticType::Bool;
      break;
    case 9:
      element->symbol = 0;
      break;
    case 10:
      element->operands.front() = fold->operands.front();
      break;
    case 11:
      element->info.access = lang::AccessMode::Mutable;
      break;
    case 12:
      element->info.traits.copyable = !element->info.traits.copyable;
      break;
    case 13:
      call->callPlan->arguments.front().kind =
          lang::HirCallInputKind::ReadBorrow;
      break;
    }
    if (mutation == 12) {
      expect(
          !lang::MirLowerer().lower(forged, frontend.failureMetadata).valid(),
          "pack-element traits must match independently resolved MIR types");
    } else {
      expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
             "HIR fold rejects a missing, substituted, duplicated, reordered "
             "or untyped expansion component");
    }
  }
}

void referenceValuedPackInputs() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/reference-pack.gti", R"(
class Record { public: int value; Record(int value) : value(value) {} };
void consume<Values...>(Values... values) {}
void forward<Args...>(int tag, Args... args) { consume(args...); }
class Relay<R(Args...)> {
public:
  void forward(Args... args) { consume(args...); }
public:
  Relay() = default;
};
int main() {
  int value = 3;
  Record record{4};
  forward<int&>(0, value);
  forward<Record&>(0, record);
  mut int changed = 5;
  Relay<void(mut int&)> relay{};
  relay.forward(changed);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "unrelated scalar/class reference packs and readonly carriers of "
         "mutable references preserve ordinary forwarding");
  if (!frontend.canGenerateCode())
    return;
  std::size_t sharedStages = 0;
  std::size_t mutableStages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "forward")
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.info.type.kind != lang::SemanticType::Reference ||
            stage.operands.size() != 1)
          continue;
        const auto &operand = stage.operands.front();
        const auto *loan = operand.kind == lang::MirOperandKind::Loan
                               ? function.body.findLoan(operand.loan)
                               : nullptr;
        const auto *element =
            function.body.findPlace(loan ? loan->source : operand.place);
        const auto *type =
            element ? lang::mirConcretePackElementType(function.body, *element)
                    : nullptr;
        if (!type || type->kind != lang::SemanticType::Reference ||
            element->projections.size() != 1)
          continue;
        expect(type->arguments.size() == 1 &&
                   element->access == lang::AccessMode::ReadOnly,
               "reference forwarding retains an exact readonly pack carrier");
        if (stage.callInputKind == lang::HirCallInputKind::MutableBorrow) {
          ++mutableStages;
          expect(loan && loan->entry &&
                     loan->kind == lang::MirLoanKind::Parameter &&
                     loan->access == lang::AccessMode::Mutable &&
                     type->referenceAccess == lang::AccessMode::Mutable,
                 "a mutable pack referent requires its exact parameter loan");
          for (int mutation = 0; mutation != 2; ++mutation) {
            auto forged = frontend.mir;
            auto *body = lang::findMirBody(
                forged, {lang::MirBodyKind::Function, function.id});
            auto *changedStage = findInstruction(*body, stage.id);
            if (mutation == 0) {
              changedStage->operands.front().kind =
                  lang::MirOperandKind::BorrowWrite;
              changedStage->operands.front().place = element->id;
              changedStage->operands.front().loan = 0;
            } else if (loan) {
              body->loans[loan->id - 1].access = lang::AccessMode::ReadOnly;
            }
            expect(lang::rebuildMirValueUses(*body) &&
                       !lang::verifyMirProgram(forged).valid(),
                   "a readonly mutable-reference carrier cannot replace its "
                   "parameter loan with a forged place borrow or shared loan");
          }
          continue;
        }
        if (stage.callInputKind != lang::HirCallInputKind::ReadBorrow ||
            operand.kind != lang::MirOperandKind::BorrowRead)
          continue;
        ++sharedStages;
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          auto &changed = body->places[element->id - 1];
          auto *changedStage = findInstruction(*body, stage.id);
          if (mutation == 0)
            changed.type.arguments.front() = lang::SemanticType::UInt16;
          if (mutation == 1)
            changed.access = lang::AccessMode::Mutable;
          if (mutation == 2)
            changed.projections.front().constantIndex = 1;
          if (mutation == 3) {
            for (const auto &place : body->places)
              if (place.root == lang::MirPlaceRootKind::Binding &&
                  place.projections.empty() &&
                  place.type == lang::SemanticType::Int32)
                changed.binding = place.binding;
            expect(changed.binding != element->binding,
                   "pack-owner mutation selects a distinct scalar binding");
          }
          if (mutation == 4)
            changed.projections.front().kind = lang::MirProjectionKind::Index;
          if (mutation == 5) {
            for (auto &place : body->places)
              if (place.root == lang::MirPlaceRootKind::Binding &&
                  place.binding == element->binding &&
                  place.projections.empty())
                place.type.arguments.front().arguments.front() =
                    lang::SemanticType::UInt16;
            changed.type.arguments.front() = lang::SemanticType::UInt16;
            expect(lang::mirConcretePackElementType(*body, changed) != nullptr,
                   "coherent pack retagging still cannot alter the selected "
                   "reference parameter's physical width");
          }
          if (mutation == 6) {
            changedStage->callInputKind = lang::HirCallInputKind::MutableBorrow;
            changedStage->info.type.referenceAccess = lang::AccessMode::Mutable;
            changedStage->operands.front().kind =
                lang::MirOperandKind::BorrowWrite;
            changedStage->operands.front().type = changedStage->info.type;
          }
          expect(lang::rebuildMirValueUses(*body) &&
                     !lang::verifyMirProgram(forged).valid(),
                 "reference-valued pack inputs reject wrong element types, "
                 "access, indices, owners, ordinary-index disguises and "
                 "coherent width or mutable-access substitutions");
        }
      }
  }
  expect(sharedStages == 2 && mutableStages == 1,
         "pack forwarding exercises two physical shared references and one "
         "mutable referent behind its readonly carrier");
}
} // namespace
} // namespace persistent_storage

namespace native_callback {
namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

// The shipped backend accepts the verified program; reports why it did not.
bool emitsNative(const lang::FrontendResult &frontend,
                 const lang::TargetInfo &target = lang::TargetInfo::host()) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, target, &error);
  if (!emitted)
    std::cerr << "native emission: " << error << '\n';
  return emitted;
}

lang::FrontendResult
analyze(std::string_view name, std::string source,
        std::optional<lang::ExecutionProfile> profile = std::nullopt) {
  lang::FrontendOptions options;
  if (profile) {
    options.target.executionProfile = *profile;
    options.target.runtimeCapabilities.threads = true;
  }
  const std::filesystem::path standardLibrary = std::filesystem::path(__FILE__)
                                                    .parent_path()
                                                    .parent_path()
                                                    .parent_path() /
                                                "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

void expectCallbackFailure(std::string_view name, std::string source) {
  const lang::FrontendResult result = analyze(name, std::move(source));
  expect(!result.canGenerateCode() && hasCode(result, "GTI-S2076"),
         std::string(name) +
             " should be rejected by the native callback semantic boundary");
}
void testNativeIndirectVerificationAndQueries() {
  const std::string source = R"(
using Combine = (int32_t, int32_t) -> int32_t;
extern "C" { Combine obtain(); }
int main() {
  unsafe {
    Combine callback = obtain();
    return callback(2, 3);
  }
}
)";
  const auto frontend = analyze("native-indirect-contract.gti", source);
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    expect(false, "native indirect contract fixture must complete lowering");
    return;
  }
  const lang::LanguageQueries queries;
  const auto callOffset = source.rfind("callback(2, 3)");
  const auto hover =
      queries.hover(frontend, frontend.sourceGraph.entryUnit(), callOffset);
  const auto signature = queries.signatureHelp(
      frontend, frontend.sourceGraph.entryUnit(), callOffset + 12);
  const auto definition = queries.definition(
      frontend, frontend.sourceGraph.entryUnit(), callOffset);
  expect(hover &&
             hover->signature == "unsafe int32_t callback(int32_t, int32_t)" &&
             signature && signature->activeParameter == 1 &&
             signature->parameterLabels.size() == 2 && definition &&
             definition->target.start == source.find("callback ="),
         "native call queries must retain the signature and underlying "
         "callback binding identity");
  std::size_t calls = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.nativeCalleeType)
          continue;
        ++calls;
        expect(lang::isMirNativeIndirectCall(frontend.mir, function.body,
                                             instruction),
               "native call must satisfy the shared exact MIR contract");
        const auto effect = lang::effects(frontend.mir, instruction);
        expect(!effect.speculatable && !effect.removableWhenUnused &&
                   !effect.reorderable && effect.readsUnknownMemory &&
                   effect.writesUnknownMemory,
               "native address invocation must retain conservative foreign "
               "effects rather than a guessed GTI body summary");
        for (int mutation = 0; mutation < 10; ++mutation) {
          auto forged = frontend.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  forged.functionInstances());
          for (auto &changedFunction : functions) {
            if (changedFunction.id != function.id)
              continue;
            for (auto &changedBlock : changedFunction.body.blocks) {
              for (auto &changed : changedBlock.instructions) {
                if (changed.id != instruction.id)
                  continue;
                switch (mutation) {
                case 0:
                  changed.nativeCalleeType.reset();
                  break;
                case 1:
                  changed.unsafeOperation = lang::UnsafeOperationKind::None;
                  break;
                case 2:
                  changed.receiver.reset();
                  break;
                case 3:
                  changed.functionTarget = function.id;
                  break;
                case 4:
                  changed.parameterTypes[0] = lang::SemanticType::Int64;
                  break;
                case 5:
                  changed.info.type = lang::SemanticType::Int64;
                  break;
                case 6:
                  changed.receiver->type = lang::SemanticType::Int32;
                  break;
                case 7:
                  changed.callSite = 0;
                  break;
                case 8:
                  changed.definedFailure.propagation =
                      lang::FailurePropagationKind::Callable;
                  break;
                case 9:
                  changed.operands[0] = *changed.receiver;
                  break;
                }
              }
            }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "forged native invocation identity/signature/permission "
                 "must fail MIR verification");
        }
      }
    }
  }
  expect(calls == 1, "fixture must exercise one exact native indirect call");
  const auto lowered = gti_test::lowerOptimizedProgram(frontend);
  bool verifiedBodies = lang::verifyLoweredProgram(lowered).empty();
  for (const auto &function : frontend.mir.functionInstances())
    verifiedBodies =
        verifiedBodies &&
        (function.definitionKind != lang::MirDefinitionKind::Source ||
         function.body.blocks.empty() ||
         lowered.findBody({lang::MirBodyKind::Function, function.id}) !=
             nullptr);
  expect(verifiedBodies && emitsNative(frontend),
         "native invocation must be emitted through verified production MIR");

  const auto unsafeMissing = analyze("native-safe-call.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; return callback(7); }
)");
  expect(!unsafeMissing.canGenerateCode() &&
             hasCode(unsafeMissing, "GTI-S2055"),
         "scalar-only indirect native calls still require lexical unsafe");
  const auto arity = analyze("native-arity.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; unsafe { return callback(); } }
)");
  expect(!arity.canGenerateCode() && hasCode(arity, "GTI-S2005"),
         "native invocation arity is established before MIR");
  expectCallbackFailure("native-generic-arguments.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; unsafe { return callback<int32_t>(7); } }
)");
}

} // namespace
} // namespace native_callback

namespace expected_transform {
namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

// The shipped backend accepts the verified program and emits native code.
void expectNativeEmission(const lang::FrontendResult &frontend,
                          std::string_view message) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  expect(emitted, error.empty() ? std::string(message)
                                : std::string(message) + ": " + error);
}

lang::FrontendResult analyze(const std::string &source) {
  const auto library = std::filesystem::path(__FILE__)
                           .parent_path()
                           .parent_path()
                           .parent_path() /
                       "stdlib";
  return lang::Frontend().analyze("transform_error.gti", source,
                                  {library / "prelude.gti"}, {}, {library});
}

void testLambdaMappers() {
  const auto result = analyze(R"(
enum class Error { bad = 7 };
expected<int, Error> read(bool fail) {
  if (fail) { return unexpected(Error::bad); }
  return 42;
}
expected<int, int> mapped(bool fail) {
  return read(fail).transform_error(([](Error error) -> int { return error == Error::bad ? 7 : 0; }));
}
int main() {
  int offset = 3;
  auto mapper = [offset](Error error) -> int { return (error == Error::bad ? 7 : 0) + offset; };
  auto first = read(true).transform_error(mapper);
  auto second = read(false).transform_error([owned = std::move(offset)](Error error) -> int {
    return (error == Error::bad ? 7 : 0) + owned;
  });
  return first.error() + second.value() - 52;
}
)");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode(),
         "literal and named capturing lambda mappers lower");
  if (!result.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(result.mir).valid(),
         "lambda mapper MIR verifies");
  expect(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "eager lambda inputs retain the selected source order");
  // A lambda mapper is an ordinary argument of the source member, staged
  // before the call through its exact MIR call input.
  unsigned eagerPlans = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.kind == lang::MirInstructionKind::Call &&
                    instruction.functionTarget
                ? result.hir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        const auto &mapperType = instruction.operands.empty()
                                     ? lang::SemanticType::Unknown
                                     : instruction.operands.front().type;
        if (!target || !target->source ||
            target->source->name().lexeme != "transform_error" ||
            instruction.operands.size() != 1 ||
            mapperType.kind != lang::SemanticType::Reference ||
            mapperType.arguments.size() != 1 ||
            mapperType.arguments.front().kind != lang::SemanticType::Lambda)
          continue;
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.kind == lang::MirInstructionKind::CallInput &&
                candidate.result &&
                *candidate.result == instruction.operands.front().value)
              stage = &candidate;
        if (!stage)
          continue;
        ++eagerPlans;
        for (unsigned mutation = 0; mutation < 2; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions) {
              if (mutation == 0 && candidate.id == stage->id)
                candidate.callInputIndex = 1;
              if (mutation == 1 && candidate.id == instruction.id)
                candidate.operands.clear();
            }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "MIR rejects a misplaced or detached lambda mapper input");
        }
      }
    }
  }
  expect(eagerPlans == 3,
         "all three lambda mappings have verified eager plans");
  unsigned calls = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.lambdaTarget ||
            instruction.kind != lang::MirInstructionKind::Call)
          continue;
        ++calls;
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions)
              if (candidate.id == instruction.id) {
                if (mutation == 0)
                  candidate.lambdaTarget = 999999;
                else if (mutation == 1)
                  candidate.receiver.reset();
                else if (mutation == 2)
                  candidate.parameterTypes = {lang::SemanticType::Bool};
                else
                  candidate.definedFailure.propagation =
                      lang::FailurePropagationKind::DirectCall;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "lambda mapping rejects forged target, environment and "
                 "signature or callable failure propagation");
        }
      }
    }
  }
  expect(calls == 3, "all three mappers use exact lambda calls");
  expectNativeEmission(
      result, "lambda mapping emits through the production MIR backend");
}
} // namespace
} // namespace expected_transform

namespace ephemeral_carrier {
namespace {

const std::string declarations = R"gti(
struct Counter { mut int value = 4;  public: Counter() = default; };
class Service {
  mut Counter& counter;
public:
  Service(mut Counter& source) : counter(source) {}
  int read() { return this.counter.value; }
  void advance(int amount) mut { this.counter.value += amount; }
};
Service service(mut Counter& source) { return Service(source); }
class Group {
  Counter& input;
  mut Counter& left;
  mut Counter& right;
public:
  Group(Counter& source, mut Counter& a, mut Counter& b)
      : input(source), left(a), right(b) {}
  void advance(int amount) mut {
    this.left.value += amount;
    this.right.value += this.input.value;
  }
};
int amount(int divisor) { return 4 / divisor; }
mut Counter global_owner{};
class Trace {
  int divisor;
public:
  Trace(int value) : divisor(value) {}
  ~Trace() { int result = 1 / this.divisor; }
};
class FallibleService {
  mut Counter& counter;
  Trace lifetime;
public:
  FallibleService(mut Counter& source, int divisor)
      : counter(source), lifetime(Trace(1)) { int result = 1 / divisor; }
  int read() { return this.counter.value; }
};
int conditional(mut Counter& owner, bool take) {
  int result = take ? service(owner).read() : 0;
  return result;
}
expected<int, int> maybe_amount(bool fail) {
  if (fail) { return unexpected(7); }
  return 2;
}
expected<int, int> expected_call(mut Counter& owner, bool fail) {
  service(owner).advance(?maybe_amount(fail));
  return owner.value;
}
)gti";

lang::FrontendResult analyze(const std::string &body) {
  return lang::Frontend().analyze(
      "ephemeral-carrier-mir.gti",
      declarations + "\nint main() {\nmut Counter decoy{}; decoy.value = 1;\n" +
          body + "\n}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

void checkConstructorRollbackMutations() {
  const auto frontend = analyze(
      "mut Counter owner{}; int result = FallibleService(owner, 0).read(); "
      "return result;");
  expect(frontend.canGenerateCode(),
         "constructor rollback mutation seed verifies");
  if (!frontend.canGenerateCode())
    return;
  bool checkedMissing = false;
  bool checkedEarly = false;
  bool checkedSuccess = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto &body = constructor.body;
    for (const auto &loan : body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
          loan.storedField == 0)
        continue;
      for (const auto &block : body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan.id)
            continue;
          auto missing = body;
          auto &instructions = missing.blocks[block.id - 1].instructions;
          std::erase_if(instructions, [&](const auto &value) {
            return value.id == instruction.id;
          });
          expect(!lang::verifyMirBody(missing).valid(),
                 "failed construction cannot export a stored dependency");
          checkedMissing = true;
          for (const auto &targetBlock : body.blocks) {
            if (targetBlock.activeFailure != 0) {
              const auto rollback = std::find_if(
                  targetBlock.instructions.begin(),
                  targetBlock.instructions.end(), [&](const auto &value) {
                    const auto *drop = value.lifecycle.size() == 1
                                           ? body.findDropObligation(
                                                 value.lifecycle.front().source)
                                           : nullptr;
                    return value.kind == lang::MirInstructionKind::Drop &&
                           drop != nullptr &&
                           drop->kind == lang::MirDropObligationKind::
                                             ConstructionRollback;
                  });
              if (rollback == targetBlock.instructions.end())
                continue;
              auto early = missing;
              auto &earlyInstructions =
                  early.blocks[targetBlock.id - 1].instructions;
              const auto position = std::find_if(
                  earlyInstructions.begin(), earlyInstructions.end(),
                  [&](const auto &value) { return value.id == rollback->id; });
              earlyInstructions.insert(position, instruction);
              expect(
                  !lang::verifyMirBody(early).valid(),
                  "stored dependency survives every armed subobject rollback");
              checkedEarly = true;
            } else if (targetBlock.terminator.kind ==
                       lang::MirTerminatorKind::Return) {
              auto success = missing;
              success.blocks[targetBlock.id - 1].instructions.push_back(
                  instruction);
              expect(
                  !lang::verifyMirBody(success).valid(),
                  "successful constructor must export its stored dependency");
              checkedSuccess = true;
            }
          }
        }
      }
    }
  }
  expect(checkedMissing && checkedEarly && checkedSuccess,
         "constructor lifetime mutations cover failure, rollback, and "
         "publication");
}

void checkForbiddenResultMutation() {
  const auto frontend =
      lang::Frontend().analyze("ephemeral-carrier-result.gti", R"gti(
struct Owner { int value = 4;  public: Owner() = default; };
class View {
  Owner& owner;
public:
  View(Owner& source) : owner(source) {}
  int read() { return this.owner.value; }
  int* pointer() { return nullptr; }
};
int main() {
  Owner owner{};
  { View named{owner}; [[discard]] named.pointer(); }
  [[discard]] View(owner).read();
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "named carrier raw-result mutation seed verifies");
  if (!frontend.canGenerateCode())
    return;
  const auto rawTarget = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.owner &&
               function.returnType.kind == lang::SemanticType::RawPointer;
      });
  expect(rawTarget != frontend.mir.functionInstances().end(),
         "mutation seed retains a valid named-only raw-result target");
  if (rawTarget == frontend.mir.functionInstances().end())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.receiver ||
            call.receiver->kind != lang::MirOperandKind::Value || !call.result)
          continue;
        const auto *value = function.body.findValue(call.receiver->value);
        if (value == nullptr)
          continue;
        const auto *stageBlock =
            function.body.findBlock(value->definitionBlock);
        const auto stage = std::find_if(
            stageBlock->instructions.begin(), stageBlock->instructions.end(),
            [&](const auto &instruction) {
              return instruction.id == value->definition &&
                     instruction.kind == lang::MirInstructionKind::CallInput &&
                     instruction.callInputRole ==
                         lang::MirCallInputRole::Receiver &&
                     instruction.callInputKind ==
                         lang::HirCallInputKind::ReadTemporaryBorrow &&
                     instruction.info.traits.containsBorrowedState;
            });
        if (stage == stageBlock->instructions.end())
          continue;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &instructions = instance->body.blocks[block.id - 1].instructions;
        auto &changed = *std::find_if(
            instructions.begin(), instructions.end(),
            [&](const auto &instruction) { return instruction.id == call.id; });
        changed.functionTarget = rawTarget->id;
        changed.info.type = rawTarget->returnType;
        changed.info.traits = lang::semanticTraits(rawTarget->returnType);
        instance->body.values[*changed.result - 1].info = changed.info;
        expect(lang::verifyMirBody(instance->body).valid(),
               "forged raw-result call remains body-local well typed");
        const auto verification = lang::verifyMirProgram(malformed);
        expect(
            std::any_of(
                verification.errors.begin(), verification.errors.end(),
                [&](const auto &error) {
                  return error.owner == function.id &&
                         error.instruction == call.id &&
                         error.message.find(
                             "temporary receiver input does not match") !=
                             std::string::npos;
                }),
            "program verification rejects a raw result from an ephemeral "
            "borrowed receiver even when the exact target is otherwise valid");
        checked = true;
      }
    }
  }
  expect(checked, "raw-result mutation exercises one ephemeral receiver call");
}

void checkConditionalParentReborrowMutation() {
  const auto frontend =
      lang::Frontend().analyze("conditional-parent-reborrow.gti", R"gti(
struct Owner { mut int first = 4; mut int second = 7;  public: Owner() = default; };
class View {
  mut int& value;
public:
  View(mut int& source) : value(source) {}
  int read() { return this.value; }
};
int choose(mut Owner& owner, bool take) {
  int result = take ? View(owner.first).read() : 0;
  return result + owner.second;
}
int main() {
  mut Owner owner{};
  return choose(owner, true) - 11;
}
)gti");
  expect(frontend.canGenerateCode(),
         "conditional-parent mutation seed has independently verified MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      if (block.activeFailure != 0)
        continue;
      for (const auto &drop : block.instructions) {
        if (drop.kind != lang::MirInstructionKind::Drop || !drop.destination ||
            drop.lifecycle.size() != 1 || !drop.lifecycle.front().conditional)
          continue;
        const auto *place = body.findPlace(*drop.destination);
        const auto carrier =
            place == nullptr
                ? lang::MirTemporaryCarrierDependency{}
                : lang::findMirTemporaryCarrierDependency(body, *place);
        const auto *parent =
            carrier ? body.findLoan(carrier.loan->parent) : nullptr;
        const auto *source =
            carrier ? body.findPlace(carrier.loan->source) : nullptr;
        if (parent == nullptr || !parent->entry ||
            parent->access != lang::AccessMode::Mutable || source == nullptr ||
            source->projections.empty() ||
            source->projections.back().kind != lang::MirProjectionKind::Field)
          continue;
        // Borrow a disjoint sibling field: the exact Active and Suspended
        // predecessor states both permit it. Only their conditional union at
        // the merge must reject the new executable authority.
        const auto sibling = std::find_if(
            body.places.begin(), body.places.end(), [&](const auto &candidate) {
              return candidate.root == source->root &&
                     candidate.binding == source->binding &&
                     candidate.symbol == source->symbol &&
                     candidate.loan == source->loan &&
                     candidate.type == source->type &&
                     candidate.projections.size() ==
                         source->projections.size() &&
                     candidate.projections.back().kind ==
                         lang::MirProjectionKind::Field &&
                     candidate.projections.back().field !=
                         source->projections.back().field &&
                     std::equal(source->projections.begin(),
                                source->projections.end() - 1,
                                candidate.projections.begin());
            });
        if (sibling == body.places.end())
          continue;
        auto malformed = body;
        const lang::MirLoanId child = malformed.loans.size() + 1;
        malformed.loans.push_back({.id = child,
                                   .parent = parent->id,
                                   .kind = lang::MirLoanKind::Local,
                                   .source = sibling->id,
                                   .access = lang::AccessMode::Mutable});
        lang::MirInstructionId next = 1;
        for (const auto &candidateBlock : body.blocks)
          for (const auto &instruction : candidateBlock.instructions)
            next = std::max(next, instruction.id + 1);
        const lang::MirInstruction borrow{
            .id = next,
            .kind = lang::MirInstructionKind::Borrow,
            .operands = {{.kind = lang::MirOperandKind::Loan,
                          .loan = parent->id,
                          .type = body.findPlace(parent->source)->type}},
            .loan = child};
        const lang::MirInstruction end{.id = next + 1,
                                       .kind =
                                           lang::MirInstructionKind::EndBorrow,
                                       .loan = child};
        auto &instructions = malformed.blocks[block.id - 1].instructions;
        const auto position = std::find_if(
            instructions.begin(), instructions.end(),
            [&](const auto &value) { return value.id == drop.id; });
        instructions.insert(position, {borrow, end});
        const auto verification = lang::verifyMirBody(malformed, function.id);
        const bool exactFailure = std::any_of(
            verification.errors.begin(), verification.errors.end(),
            [&](const auto &error) {
              return error.owner == function.id && error.block == block.id &&
                     error.instruction == borrow.id &&
                     error.message ==
                         "conditionally live temporary dependency cannot "
                         "authorize "
                         "an executable loan use in direct child reborrow";
            });
        expect(exactFailure,
               "direct Borrow cannot turn a conditional parent into definite "
               "authority at the CFG merge");
        if (!exactFailure)
          for (const auto &error : verification.errors)
            std::cerr << error.message << '\n';
        checked = true;
      }
    }
  }
  expect(checked,
         "conditional-parent mutation exercises a disjoint direct child borrow "
         "before conditional carrier retirement");
}

void checkInlineRangeProxyPhysicalLifetime() {
  const auto frontend = lang::Frontend().analyze(
      "inline-range-proxy-mir.gti", R"gti(
class End {
public:
  End() {}
};
class Projection {
  Buffer& source;
public:
  Projection(Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
};
class Cursor {
  mut Projection projection;
  mut int position = 0;
public:
  Cursor(Buffer& source) : projection(Projection(source)) {}
  Projection& operator*() { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class MutableProjection {
  mut Buffer& source;
public:
  MutableProjection(mut Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
  mut int& write() mut { return this.source.write(); }
};
class MutableCursor {
  mut MutableProjection projection;
  mut int position = 0;
public:
  MutableCursor(mut Buffer& source) : projection(MutableProjection(source)) {}
  MutableProjection& operator*() { return this.projection; }
  mut MutableProjection& operator*() mut { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class Buffer {
  mut int number = 4;
public:
  Buffer() {}
  int& read() { return this.number; }
  mut int& write() mut { return this.number; }
  Cursor begin() { return Cursor(this); }
  MutableCursor begin_mut() mut { return MutableCursor(this); }
  End end() { int checked = this.number + 0; return End(); }
};
int inspect(Buffer& source) {
  mut int total = 0;
  for (auto& element : source) {
    for (auto& inner : source) { total += element.read(); }
  }
  return total;
}
int main() {
  mut Buffer buffer{};
  int original = inspect(buffer);
  for (mut auto& element : buffer) { element.write() += 1; }
  return original + buffer.read() - 22;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "inline readonly/exclusive proxies retain ordinary owning ranges "
         "and borrowed helper parameters");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t checked = 0;
  std::size_t siblingCursorChecks = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &range : body.rangeIterations) {
      const auto *element = body.findLoan(range.elementLoan);
      const auto *cursor = element == nullptr
                               ? nullptr
                               : body.findLoan(element->storedDependency);
      expect(element != nullptr && cursor != nullptr &&
                 element->kind == lang::MirLoanKind::CallResult &&
                 element->source == range.iterator && element->parent == 0 &&
                 cursor->kind == lang::MirLoanKind::Stored &&
                 cursor->parent == range.rangeLoan,
             "proxy referent, physical cursor and traversal authority are "
             "separate checked identities");
      if (element == nullptr || cursor == nullptr)
        continue;
      ++checked;
      const auto reject = [&](const lang::MirProgram &malformed,
                              const std::string &reason) {
        const auto *instance = malformed.findFunctionInstance(function.id);
        expect(instance != nullptr &&
                   !lang::verifyMirBody(instance->body, function.id).valid(),
               reason);
        expect(!lang::verifyMirProgram(malformed).valid(), reason);
      };
      for (int mutation = 0; mutation < 8; ++mutation) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body;
        switch (mutation) {
        case 0:
          changed.loans[element->id - 1].storedDependency = 0;
          break;
        case 1:
          changed.loans[element->id - 1].source = range.rangeCarrier;
          break;
        case 2:
          changed.loans[cursor->id - 1].parent = 0;
          break;
        case 3:
          changed.loans[element->id - 1].escapes = true;
          break;
        case 4:
          changed.loans[cursor->id - 1].carriers.clear();
          break;
        case 5:
          changed.loans[element->id - 1].access =
              range.elementAccess == lang::AccessMode::ReadOnly
                  ? lang::AccessMode::Mutable
                  : lang::AccessMode::ReadOnly;
          break;
        case 6:
          changed.loans[element->id - 1].storedDependency = range.rangeLoan;
          break;
        case 7:
          changed.loans[cursor->id - 1].carriers.clear();
          changed.loans[element->id - 1].storedDependency = range.rangeLoan;
          break;
        }
        reject(malformed,
               "forged proxy range origins, authority and escape fail closed");
      }
      for (const auto &other : body.rangeIterations) {
        if (other.id == range.id || other.elementAccess != range.elementAccess)
          continue;
        const auto *otherElement = body.findLoan(other.elementLoan);
        const auto *otherCursor =
            otherElement == nullptr
                ? nullptr
                : body.findLoan(otherElement->storedDependency);
        if (otherCursor == nullptr)
          continue;
        auto wrongCursor = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            wrongCursor.findFunctionInstance(function.id));
        instance->body.loans[element->id - 1].storedDependency =
            otherCursor->id;
        reject(wrongCursor,
               "a proxy cannot substitute another live same-type cursor shell");
        auto wrongProducer = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            wrongProducer.findFunctionInstance(function.id));
        instance->body.loans[element->id - 1].producedBy =
            otherElement->producedBy;
        reject(wrongProducer,
               "a proxy cannot substitute another same-type element producer");
        ++siblingCursorChecks;
      }
      bool earlyEndChecked = false;
      for (const auto &block : body.blocks) {
        const auto selected = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.loan == element->id;
            });
        if (selected == block.instructions.end())
          continue;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body;
        lang::MirInstructionId last = 0;
        for (const auto &candidate : changed.blocks)
          for (const auto &instruction : candidate.instructions)
            last = std::max(last, instruction.id);
        auto &instructions = changed.blocks[block.id - 1].instructions;
        instructions.insert(instructions.begin() +
                                (selected - block.instructions.begin()),
                            {.id = last + 1,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .loan = cursor->id});
        reject(malformed,
               "a physical cursor cannot retire before proxy publication");
        earlyEndChecked = true;
      }
      expect(earlyEndChecked,
             "proxy regression exercises premature physical cursor retirement");
      if (range.endpointParking) {
        const auto &parking = *range.endpointParking;
        expect(!parking.failureRestores.empty(),
               "fallible endpoint restores the parked cursor on its primary "
               "failure edge before shared cleanup");
        for (int mutation = 0; mutation < 12; ++mutation) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = instance->body;
          auto &inventory =
              *changed.rangeIterations[range.id - 1].endpointParking;
          const auto instruction =
              [&](lang::MirInstructionId id) -> lang::MirInstruction & {
            for (auto &block : changed.blocks)
              for (auto &candidate : block.instructions)
                if (candidate.id == id)
                  return candidate;
            std::abort();
          };
          switch (mutation) {
          case 0:
            instruction(parking.park).kind =
                lang::MirInstructionKind::RestoreRangeCursor;
            break;
          case 1:
            instruction(parking.restore).kind =
                lang::MirInstructionKind::ParkRangeCursor;
            break;
          case 2:
            instruction(parking.park).loan = range.rangeLoan;
            break;
          case 3:
            inventory.cursor = range.rangeLoan;
            instruction(parking.park).loan = range.rangeLoan;
            instruction(parking.restore).loan = range.rangeLoan;
            for (auto id : parking.failureRestores)
              instruction(id).loan = range.rangeLoan;
            break;
          case 4:
            instruction(parking.park).hirValue = parking.endValue + 1;
            break;
          case 5:
            instruction(parking.restore).hirStatement = range.coreLoop;
            break;
          case 6:
            inventory.beginCall = parking.endCall;
            break;
          case 7:
            inventory.endReceiver = parking.beginCall;
            break;
          case 8:
            instruction(parking.endReceiver).callInputKind =
                lang::HirCallInputKind::MutableBorrow;
            instruction(parking.endReceiver).operands.front().kind =
                lang::MirOperandKind::BorrowWrite;
            break;
          case 9:
            instruction(parking.endCall).borrowOrigin =
                lang::BorrowOriginKind::Receiver;
            break;
          case 10:
            instruction(parking.park).rawMemoryAccess = true;
            break;
          case 11:
            inventory.restore = parking.park;
            break;
          }
          reject(malformed, "forged range parking authority, endpoint identity "
                            "and restoration fail closed");
        }
        for (auto failedRestore : parking.failureRestores) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &block : instance->body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == failedRestore)
                instruction.kind = lang::MirInstructionKind::EndBorrow;
          reject(malformed, "failed endpoint restores its cursor before any "
                            "cleanup or retirement");
          const lang::MirBlock *failurePrefix = nullptr;
          for (const auto &block : body.blocks)
            if (!block.instructions.empty() &&
                block.instructions.front().id == failedRestore)
              failurePrefix = &block;
          expect(failurePrefix != nullptr &&
                     failurePrefix->failureParameter != 0 &&
                     body.findFailureRecord(failurePrefix->failureParameter) !=
                         nullptr &&
                     body.findFailureRecord(failurePrefix->failureParameter)
                             ->kind == lang::MirFailureRecordKind::Producer,
                 "endpoint Restore stays in the exact primary failure "
                 "prefix, never a shareable continuation parameter");
          if (failurePrefix != nullptr &&
              failurePrefix->terminator.kind ==
                  lang::MirTerminatorKind::ContinueFailure) {
            const auto *suffix =
                body.findBlock(failurePrefix->terminator.target);
            expect(
                suffix != nullptr &&
                    std::none_of(
                        suffix->instructions.begin(),
                        suffix->instructions.end(),
                        [](const auto &step) {
                          return step.kind ==
                                 lang::MirInstructionKind::RestoreRangeCursor;
                        }),
                "shared cleanup suffix never replays a primary endpoint "
                "permission transition");
            auto delayed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                delayed.findFunctionInstance(function.id));
            auto &prefixBlock = changed->body.blocks[failurePrefix->id - 1];
            const auto restore = prefixBlock.instructions.front();
            prefixBlock.instructions.erase(prefixBlock.instructions.begin());
            auto &suffixBlock = changed->body.blocks[suffix->id - 1];
            suffixBlock.instructions.insert(suffixBlock.instructions.begin(),
                                            restore);
            reject(delayed, "endpoint Restore cannot be delayed across a "
                            "shared failure continuation");
          }
        }
        auto earlyCursorUse = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            earlyCursorUse.findFunctionInstance(function.id));
        for (auto &block : instance->body.blocks) {
          const auto receiver =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.id == parking.endReceiver;
                           });
          if (receiver == block.instructions.end())
            continue;
          lang::MirInstructionId last = 0;
          for (const auto &candidate : instance->body.blocks)
            for (const auto &instruction : candidate.instructions)
              last = std::max(last, instruction.id);
          block.instructions.insert(
              receiver, {.id = last + 1,
                         .kind = lang::MirInstructionKind::EndBorrow,
                         .loan = parking.cursor});
          break;
        }
        reject(earlyCursorUse, "parked cursor cannot be used or destroyed "
                               "before the endpoint completes");
      }
    }
  }
  expect(
      checked == 3,
      "owning inline-proxy fixture exercises shared and exclusive range loans");
  expect(siblingCursorChecks == 2,
         "nested shared traversal exercises exact same-type cursor identities");
}

void checkInlineProxyCannotBecomeAbstractStoredRange() {
  const std::string source = R"gti(
class End { public: End() {} };
class Projection {
  Buffer& source;
public:
  Projection(Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
};
class Cursor {
  mut Projection projection;
  mut int position = 0;
public:
  Cursor(Buffer& source) : projection(Projection(source)) {}
  Projection& operator*() { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class Buffer {
  mut int number = 4;
public:
  Buffer() {}
  int& read() { return this.number; }
  Cursor begin() { return Cursor(this); }
  End end() { return End(); }
};
expected<Buffer&, int> lookup(Buffer& source) { return source; }
int main() {
  Buffer buffer{};
  auto borrowed = lookup(buffer);
  auto& stable = borrowed.value();
  mut int total = 0;
  for (auto& element : stable) { total += element.read(); }
  return total - 8;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "inline-proxy-stored-parent.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "an inline proxy over a borrowed carrier keeps a separate physical "
         "cursor despite its Stored parent");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &range : function.body.rangeIterations) {
      const auto *element = function.body.findLoan(range.elementLoan);
      const auto *parent = function.body.findLoan(range.rangeLoan);
      const auto *cursor =
          element == nullptr
              ? nullptr
              : function.body.findLoan(element->storedDependency);
      const auto *iterator = function.body.findPlace(range.iterator);
      if (!element || !parent || !cursor || !iterator)
        continue;
      // expected<T&, E>::value() is a source accessor: the range borrows its
      // call result, which reborrows the carrier's Stored reference.
      const auto *carrier = function.body.findLoan(parent->parent);
      expect(
          parent->kind == lang::MirLoanKind::CallResult && carrier &&
              carrier->kind == lang::MirLoanKind::Stored &&
              cursor->id != parent->id &&
              cursor->kind == lang::MirLoanKind::Stored &&
              cursor->parent == parent->id,
          "physical inline proxy and abstract parent have distinct identities");
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         malformed.findFunctionInstance(function.id))
                         ->body;
        if (mutation <= 3) {
          body.loans[cursor->id - 1].carriers.clear();
          body.loans[element->id - 1].storedDependency = parent->id;
          if (mutation >= 1)
            body.loans[parent->id - 1].carriers.push_back(iterator->binding);
        }
        for (auto &block : body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::Call &&
                instruction.loan == element->id) {
              expect(instruction.info.traits.containsBorrowedState,
                     "selected inline proxy has authoritative nominal borrowed "
                     "traits");
              if (mutation == 2 || mutation == 3 || mutation == 4)
                instruction.info.traits.containsBorrowedState = false;
              if (mutation == 3)
                body.rangeIterations[range.id - 1].elementAccessSource = 0;
              if (mutation == 5)
                instruction.functionTarget = function.id;
            }
        const auto result = lang::verifyMirProgram(malformed);
        expect(!result.valid(),
               "a physical cursor cannot be forged into abstract stored "
               "authority: " +
                   std::to_string(mutation));
        if (mutation == 2 || mutation == 3 || mutation == 4)
          expect(
              std::any_of(
                  result.errors.begin(), result.errors.end(),
                  [](const auto &error) {
                    return error.message ==
                           "reference iteration element has no exact nominal "
                           "traits and selected dereference declaration";
                  }),
              "the program catalog independently rejects falsified nominal "
              "proxy traits even when cursor authority is also rewired");
      }
      ++checked;
    }
  }
  expect(checked == 1,
         "Stored-parent proxy forgery fixture exercises one range");
  auto virtualSource = source;
  virtualSource.insert(virtualSource.find("Projection& operator*()"),
                       "virtual ");
  const auto virtualFrontend = lang::Frontend().analyze(
      "virtual-inline-proxy-stored-parent.gti", virtualSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(virtualFrontend.canGenerateCode(),
         "nominal element validation preserves virtual checked dereference");
  bool virtualProducer = false;
  for (const auto &function : virtualFrontend.mir.functionInstances())
    for (const auto &range : function.body.rangeIterations)
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          virtualProducer |=
              instruction.kind == lang::MirInstructionKind::Call &&
              instruction.loan == range.elementLoan &&
              instruction.dispatch == lang::CallDispatch::Virtual;
  expect(virtualProducer,
         "virtual reference-yield fixture exercises the selected dynamic call");
}

} // namespace
} // namespace ephemeral_carrier

namespace consteval_completion {
namespace {

// Backend-neutral oracles: assertions about emitted programs are made on the
// verified MIR (and the shipped LLVM backend), never on generated C++ text.
[[maybe_unused]] const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend,
                 const lang::MirProgram &program,
                 std::string_view qualifiedName) {
  for (const lang::MirFunctionInstance &function :
       program.functionInstances()) {
    const lang::FunctionInfo *info =
        frontend.semantics.findFunction(function.declaration);
    if (info != nullptr && info->qualifiedName == qualifiedName) {
      return &function;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend,
                 std::string_view qualifiedName) {
  return mirFunctionNamed(frontend, frontend.mir, qualifiedName);
}

[[maybe_unused]] const lang::MirClassInstance *
mirClassNamed(const lang::FrontendResult &frontend,
              std::string_view qualifiedName) {
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info != nullptr && info->qualifiedName == qualifiedName) {
      return &instance;
    }
  }
  return nullptr;
}

template <typename Predicate>
[[maybe_unused]] bool anyMirInstruction(const lang::MirBody &body,
                                        Predicate predicate) {
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (!instruction.tombstone && predicate(instruction)) {
        return true;
      }
    }
  }
  return false;
}

// Every executable body of `program`: functions, constructors, destructors,
// lambdas, class field initializers and the merged program module.
template <typename Visitor>
[[maybe_unused]] void forEachMirBody(const lang::MirProgram &program,
                                     Visitor visitor) {
  visitor(program.module());
  for (const auto &function : program.functionInstances())
    visitor(function.body);
  for (const auto &constructor : program.constructorInstances())
    visitor(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    visitor(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    visitor(lambda.body);
  for (const auto &instance : program.classInstances()) {
    visitor(instance.fieldInitializers);
    visitor(instance.staticFieldInitializers);
  }
}

template <typename Predicate>
[[maybe_unused]] bool anyProgramInstruction(const lang::MirProgram &program,
                                            Predicate predicate) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || anyMirInstruction(body, predicate);
  });
  return found;
}

[[maybe_unused]] bool isMirIntegerLiteral(const lang::MirInstruction &item,
                                          std::uint64_t magnitude) {
  if (item.operation != lang::MirOperation::Literal || !item.literal)
    return false;
  const auto *value = std::get_if<std::uint64_t>(&*item.literal);
  return value != nullptr && *value == magnitude;
}

[[maybe_unused]] bool hasMirIntegerLiteral(const lang::MirBody &body,
                                           std::uint64_t magnitude) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    return isMirIntegerLiteral(item, magnitude);
  });
}

[[maybe_unused]] bool hasMirIntegerLiteral(const lang::MirProgram &program,
                                           std::uint64_t magnitude) {
  return anyProgramInstruction(program, [&](const lang::MirInstruction &item) {
    return isMirIntegerLiteral(item, magnitude);
  });
}

// Text reaching the program as a MIR string literal or a constant program
// storage initializer.
[[maybe_unused]] bool mirContainsText(const lang::MirProgram &program,
                                      std::string_view text) {
  const bool literal =
      anyProgramInstruction(program, [&](const lang::MirInstruction &item) {
        const auto *value =
            item.literal ? std::get_if<std::string>(&*item.literal) : nullptr;
        return value != nullptr && value->find(text) != std::string::npos;
      });
  const auto &steps = program.programInitializationPlan().steps;
  return literal ||
         std::any_of(steps.begin(), steps.end(), [&](const auto &step) {
           const auto *value =
               step.dataConstant ? std::get_if<std::string>(&*step.dataConstant)
                                 : nullptr;
           return value != nullptr && value->find(text) != std::string::npos;
         });
}

// The defined-failure outcomes an instruction's checked operation can raise.
[[maybe_unused]] std::vector<lang::DefinedFailureOutcome>
mirFailureOutcomes(const lang::MirProgram &program,
                   const lang::MirInstruction &instruction) {
  std::vector<lang::DefinedFailureOutcome> outcomes;
  for (const lang::FailureSiteId site : instruction.localFailureSites) {
    for (const lang::FailureSiteDescriptor &descriptor :
         program.failureMetadata().sites()) {
      if (descriptor.id == site) {
        outcomes.insert(outcomes.end(), descriptor.outcomes.begin(),
                        descriptor.outcomes.end());
      }
    }
  }
  return outcomes;
}

// A Compute of `operation` whose checked evaluation can raise exactly the
// given defined failure (and, when `type` is not Unknown, in that domain).
[[maybe_unused]] bool hasCheckedMirOperation(
    const lang::MirProgram &program, const lang::MirBody &body,
    lang::MirOperation operation, lang::DefinedFailureCode code,
    lang::DefinedFailureDetail detail,
    const lang::SemanticType &type = lang::SemanticType::Unknown) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    if (item.operation != operation ||
        (type != lang::SemanticType::Unknown && item.info.type != type))
      return false;
    const auto outcomes = mirFailureOutcomes(program, item);
    return std::any_of(outcomes.begin(), outcomes.end(), [&](auto outcome) {
      return outcome.code == code && outcome.detail == detail;
    });
  });
}

[[maybe_unused]] bool hasCheckedMirOperation(
    const lang::MirProgram &program, lang::MirOperation operation,
    lang::DefinedFailureCode code, lang::DefinedFailureDetail detail,
    const lang::SemanticType &type = lang::SemanticType::Unknown) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || hasCheckedMirOperation(program, body, operation, code,
                                            detail, type);
  });
  return found;
}

// A Compute of `operation` with no checked failure: it lowers directly.
[[maybe_unused]] bool hasUncheckedMirOperation(const lang::MirBody &body,
                                               lang::MirOperation operation) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    return item.kind == lang::MirInstructionKind::Compute &&
           item.operation == operation && item.localFailureSites.empty();
  });
}

[[maybe_unused]] bool hasUncheckedMirOperation(const lang::MirProgram &program,
                                               lang::MirOperation operation) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || hasUncheckedMirOperation(body, operation);
  });
  return found;
}

[[maybe_unused]] bool
callsMirFunction(const lang::MirBody &body,
                 const lang::MirFunctionInstance *callee) {
  return callee != nullptr &&
         anyMirInstruction(body, [&](const lang::MirInstruction &item) {
           return item.kind == lang::MirInstructionKind::Call &&
                  item.functionTarget == callee->id;
         });
}

[[maybe_unused]] const lang::MirProgramInitializationStep *
mirStorageStep(const lang::FrontendResult &frontend,
               const lang::MirProgram &program,
               std::string_view qualifiedName) {
  for (const auto &step : program.programInitializationPlan().steps) {
    const lang::SymbolRecord *symbol =
        frontend.semantics.database().findSymbol(step.symbol);
    if (symbol != nullptr && symbol->qualifiedName == qualifiedName) {
      return &step;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::MirProgramInitializationStep *
mirStorageStep(const lang::FrontendResult &frontend,
               std::string_view qualifiedName) {
  return mirStorageStep(frontend, frontend.mir, qualifiedName);
}

// The exact constant integer a program-storage step publishes as data.
[[maybe_unused]] bool
storesMirInteger(const lang::MirProgramInitializationStep *step, bool negative,
                 std::uint64_t magnitude) {
  const auto *value =
      step == nullptr || !step->dataConstant
          ? nullptr
          : std::get_if<lang::ConstantInteger>(&*step->dataConstant);
  return value != nullptr && value->negative == negative &&
         value->magnitude == magnitude;
}

// True when the shipped native backend accepts the program; reports why not.
[[maybe_unused]] bool
llvmAccepts(const lang::FrontendResult &frontend,
            lang::OptimizationLevel level = lang::OptimizationLevel::O0,
            const lang::TargetInfo &target = lang::TargetInfo::host()) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(frontend, level, target, &error);
  if (!emitted) {
    std::cerr << "LLVM emission failed: " << error << '\n';
  }
  return emitted;
}

// A user-defined accumulator, so the limit and reuse rules are proven with
// ordinary source rather than a library type.
const std::string kCompileLimitsPrelude = R"(
struct Tally {
  uint64_t count;
  uint64_t sum;
  constexpr Tally(uint64_t c, uint64_t s) : count(c), sum(s) {}
};
constexpr Tally accumulate(uint64_t n, uint64_t seed) {
  mut uint64_t count = 0;
  mut uint64_t sum = seed;
  while (count < n) {
    count = count + 1;
    sum = sum + count;
  }
  return Tally(count, sum);
}
constexpr uint64_t spin(uint64_t n) { return accumulate(n, uint64_t(0)).count; }
)";

void testConstevalStatementCompletion() {
  const auto completed =
      lang::Frontend().analyze("consteval-statement-completion.gti", R"(
mut int global = 7;
constexpr int choose(bool read) { return read ? global : 2; }
consteval void validate(bool read) { int checked = choose(read); }
struct Port { int value; implicit consteval Port() : value(42) {} };
Port make() { return; }
int main() {
  mut int& held = global;
  validate(false);
  return held + make().value - 49;
}
)");
  if (!completed.canGenerateCode())
    for (const auto &diagnostic : completed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(completed.canGenerateCode() &&
             lang::verifyHirProgramPlans(completed.semantics, completed.hir)
                 .valid() &&
             lang::verifyMirProgram(completed.mir).valid(),
         "standalone immediate void calls and implicit bare returns publish "
         "through verified HIR/MIR without runtime effects");
  if (completed.canGenerateCode()) {
    const auto rejectVoidMutation = [&](auto mutate) {
      auto changed = completed.hir;
      bool found = false;
      for (const auto &function : changed.functionInstances()) {
        if (function.body.immediateVoidCompletions.empty())
          continue;
        auto &body = const_cast<lang::HirBody &>(function.body);
        mutate(body);
        found = true;
        break;
      }
      expect(found && !lang::verifyHirProgramPlans(completed.semantics, changed)
                           .valid(),
             "void completion HIR must retain its exact statement proof");
    };
    rejectVoidMutation(
        [](lang::HirBody &body) { body.immediateVoidCompletions.clear(); });
    rejectVoidMutation([](lang::HirBody &body) {
      body.immediateVoidCompletions.push_back(
          body.immediateVoidCompletions.front());
    });
    rejectVoidMutation([](lang::HirBody &body) {
      body.immediateVoidCompletions.front().source = nullptr;
    });
    rejectVoidMutation([](lang::HirBody &body) {
      auto &entry = body.immediateVoidCompletions.front();
      for (auto &statement : body.statements)
        if (statement.source == entry.source) {
          statement.kind = lang::HirStatementKind::Return;
          break;
        }
    });
    const auto rejectReturnMutation = [&](auto mutate) {
      auto changed = completed.hir;
      bool found = false;
      for (const auto &function : changed.functionInstances()) {
        if (function.body.emptyReturnConstructions.empty())
          continue;
        auto &body = const_cast<lang::HirBody &>(function.body);
        mutate(body);
        found = true;
        break;
      }
      expect(found && !lang::verifyHirProgramPlans(completed.semantics, changed)
                           .valid(),
             "immediate bare return HIR must retain its exact constant proof");
    };
    rejectReturnMutation(
        [](lang::HirBody &body) { body.emptyReturnConstructions.clear(); });
    rejectReturnMutation([](lang::HirBody &body) {
      body.emptyReturnConstructions.front().source = nullptr;
    });
    rejectReturnMutation([](lang::HirBody &body) {
      auto *value = const_cast<lang::HirValue *>(
          body.findValue(body.emptyReturnConstructions.front().value));
      value->constant.reset();
    });
    rejectReturnMutation([](lang::HirBody &body) {
      auto *value = const_cast<lang::HirValue *>(
          body.findValue(body.emptyReturnConstructions.front().value));
      value->programConstantSubstitution = false;
    });
  }

  const auto generic =
      lang::Frontend().analyze("consteval-statement-generics.gti", R"(
struct Port<uint64_t N> {
  int value;
  implicit consteval Port() : value(int(N)) {}
};
Port<N> make<uint64_t N>() { return; }
consteval void validate(int n) { int checked = 100 / n; }
void call<uint64_t N>() { validate(int(N)); }
int main() { call<2>(); return make<42>().value - 42; }
)");
  expect(generic.canGenerateCode() &&
             lang::verifyMirProgram(generic.mir).valid(),
         "dependent void completion and implicit construction are checked "
         "for each concrete generic instance");

  const auto method =
      lang::Frontend().analyze("consteval-statement-method.gti", R"(
struct Validator {
  int limit;
  constexpr Validator(int value) : limit(value) {}
  consteval void check(int value) {
    int proof = 100 / (value == this.limit ? 1 : 0);
  }
};
int main() { Validator(5).check(5); return 0; }
)");
  expect(method.canGenerateCode() && lang::verifyMirProgram(method.mir).valid(),
         "immediate void methods use their exact constant receiver in the "
         "statement completion profile");

  for (const std::string source :
       {"consteval void check(int n){int x=100/n;} int main(){check(0);"
        "return 0;}",
        "consteval void check(int n){int x=100/n;} int main(){int n=2;"
        "check(n);return 0;}",
        "consteval void check(){} int main(){(check());return 0;}",
        "consteval void recurse(int n){recurse(n+1);} int main(){"
        "recurse(0);return 0;}",
        "struct Port{int value;implicit consteval Port():value(1/0){}};"
        "Port make(){return;} int main(){return make().value;}",
        "mut int global=7; struct Port{int value;implicit consteval "
        "Port(int x=global):value(x){}}; Port make(){return;} "
        "int main(){return make().value;}",
        "struct Owner{int value;implicit consteval Owner():value(1){} "
        "~Owner(){}}; Owner make(){return;} int main(){return 0;}",
        "struct Port<uint64_t N>{int value;implicit consteval Port():"
        "value(100/int(N)) {}}; Port<N> make<uint64_t N>(){return;} "
        "int main(){return make<0>().value;}",
        "consteval void check(int n){int x=100/n;} "
        "void call<uint64_t N>(){check(int(N));} "
        "int main(){call<0>();return 0;}"}) {
    const auto rejected =
        lang::Frontend().analyze("consteval-statement-invalid.gti", source);
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, "GTI-S2057") &&
               std::none_of(rejected.diagnostics.begin(),
                            rejected.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code.starts_with("GTI-B");
                            }),
           "invalid immediate statement or bare return rejects in semantics: " +
               source);
  }
}

} // namespace
} // namespace consteval_completion

GTI_UNIT_TEST(managed_thread_verifier_mutations) {
  managed_thread::testVerifierMutations();
  return managed_thread::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(atomic_wrapping_contracts) {
  atomic::testWrappingOperations();
  return atomic::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(mutex_verifier_mutations) {
  mutex::testVerifierMutations();
  return mutex::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(persistent_reference_pack_inputs) {
  persistent_storage::referenceValuedPackInputs();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(persistent_transfer_forgery) {
  persistent_storage::persistentTransferIsIndependentlyRejected();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(persistent_fold_hir_proof) {
  persistent_storage::foldHirProof();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(native_indirect_call_contracts) {
  native_callback::testNativeIndirectVerificationAndQueries();
  return native_callback::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(expected_lambda_mapper_contracts) {
  expected_transform::testLambdaMappers();
  return expected_transform::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ephemeral_carrier_check_forbidden_result_mutation) {
  ephemeral_carrier::checkForbiddenResultMutation();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(
    ephemeral_carrier_check_inline_proxy_cannot_become_abstract_stored_range) {
  ephemeral_carrier::checkInlineProxyCannotBecomeAbstractStoredRange();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ephemeral_carrier_check_inline_range_proxy_physical_lifetime) {
  ephemeral_carrier::checkInlineRangeProxyPhysicalLifetime();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ephemeral_carrier_check_conditional_parent_reborrow_mutation) {
  ephemeral_carrier::checkConditionalParentReborrowMutation();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(ephemeral_carrier_check_constructor_rollback_mutations) {
  ephemeral_carrier::checkConstructorRollbackMutations();
  return ::failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(consteval_statement_completion_plans) {
  consteval_completion::testConstevalStatementCompletion();
  return ::failures == 0 ? 0 : 1;
}

// A consumed carrier's secondary stored owner cannot escape a singleton
// return contract merely because the primary owner is the declared supplier.
GTI_UNIT_TEST(singleton_return_supplier_forgery) {
  const auto frontend = analyzeEmissionFixture(R"(
class Two<lifetime o> {
  int&<o> first;
  int&<o> second;
public:
  Two(int&<o> a, int&<o> b) : first(a), second(b) {}
  int&<o> pick(bool choose) && {
    if (choose) { return this.first; }
    return this.second;
  }
};
int&<o> relay<lifetime o>(int&<o> a, int&<o> b) {
  return Two(a, b).pick(false);
}
int main() { return 0; }
)");
  if (!frontend.canGenerateCode())
    return 1;
  bool exercised = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "relay")
      continue;
    expect(function.returnBorrowParameters.size() == 2,
           "valid relay permits both caller-owned suppliers");
    auto forged = frontend.mir;
    auto *changed = const_cast<lang::MirFunctionInstance *>(
        forged.findFunctionInstance(function.id));
    changed->returnBorrowParameters.clear();
    changed->returnBorrowParameter = 0;
    // The optional complete claim is absent in the legacy contract. Keeping
    // the old two-supplier claim would make the forged declaration internally
    // inconsistent and test a different verifier obligation.
    changed->completeReturnBorrow.reset();
    // Preserve executable constructor, call and return dependencies. Only the
    // enclosing contract is narrowed; its secondary owner must be rejected.
    const auto verified = lang::verifyMirProgram(forged);
    expect(!verified.valid() &&
               std::any_of(verified.errors.begin(), verified.errors.end(),
                           [](const auto &error) {
                             return error.message.find(
                                        "full formal dependency set") !=
                                    std::string::npos;
                           }),
           std::string("singleton returned contract independently checks "
                       "secondary owners") +
               (verified.valid() ? " (forged MIR was accepted)" : ""));
    // A syntactically well-formed complete claim is not evidence when
    // executable reconstruction cannot normalize its undeclared supplier.
    // Keep a coherent singleton claim and require the same body-owned proof.
    changed->completeReturnBorrow = function.completeReturnBorrow;
    expect(changed->completeReturnBorrow.has_value(),
           "valid relay exposes its complete caller-relative contract");
    if (changed->completeReturnBorrow) {
      std::erase_if(changed->completeReturnBorrow->dependencies,
                    [](const auto &dependency) {
                      return dependency.origin ==
                                 lang::BorrowOriginKind::Argument &&
                             dependency.parameter != 0;
                    });
      const auto claimed = lang::verifyMirProgram(forged);
      expect(!claimed.valid() &&
                 std::any_of(claimed.errors.begin(), claimed.errors.end(),
                             [](const auto &error) {
                               return error.message.find(
                                          "full formal dependency set") !=
                                      std::string::npos;
                             }),
             "an unproved complete claim cannot waive secondary-owner checks");
    }
    exercised = true;
  }
  expect(exercised, "forgery exercises the concrete relay body");
  return ::failures == 0 ? 0 : 1;
}

// Read-only references to mutable globals cannot acquire static supplier proof.
GTI_UNIT_TEST(static_return_supplier_storage_forgery) {
  const auto frontend = analyzeEmissionFixture(R"(
class Two<lifetime o> {
  int&<o> first;
  int&<o> second;
public:
  Two(int&<o> a, int&<o> b) : first(a), second(b) {}
  int&<o> pick(bool choose) && {
    if (choose) { return this.first; }
    return this.second;
  }
};
int global = 7;
int&<o> relay<lifetime o>(int&<o> keep) {
  return Two(keep, global).pick(false);
}
int main() { return 0; }
)");
  if (!frontend.canGenerateCode())
    return 1;
  auto forged = frontend.mir;
  auto &module = const_cast<lang::MirBody &>(forged.module());
  bool changed = false;
  for (const auto &step : forged.programInitializationPlan().steps) {
    if (step.storageKind != lang::ProgramStorageKind::NamespaceGlobal ||
        step.storagePlace == 0)
      continue;
    auto *place =
        const_cast<lang::MirPlace *>(module.findPlace(step.storagePlace));
    if (!place || place->type != lang::SemanticType::Int32)
      continue;
    place->access = lang::AccessMode::Mutable;
    changed = true;
  }
  expect(changed, "forgery changes the actual global storage permission");
  const auto verified = lang::verifyMirProgram(forged);
  expect(
      !verified.valid() &&
          std::any_of(verified.errors.begin(), verified.errors.end(),
                      [](const auto &error) {
                        return error.message.find("composed return contains an "
                                                  "unproved owner origin") !=
                               std::string::npos;
                      }),
      "read-only supplier views do not prove mutable globals live statically");
  return ::failures == 0 ? 0 : 1;
}

// Observed field selection comes from the executed address, not a retained loan
// claim.
GTI_UNIT_TEST(projected_return_address_forgery) {
  const auto frontend = analyzeEmissionFixture(R"(
struct Cell { int value; Cell(int v) : value(v) {} };
struct Pair<lifetime a, lifetime b> {
  Cell&<a> first;
  Cell&<b> second;
  Pair(Cell&<a> x, Cell&<b> y) : first(x), second(y) {}
};
class Carrier<lifetime a, lifetime b> {
  Pair<a,b> pair;
  Pair<b,a> swapped;
public:
  Carrier(Cell&<a> x, Cell&<b> y)
      : pair(Pair<a,b>(x,y)), swapped(Pair<b,a>(y,x)) {}
  int&<a> get() && {
    auto& unused = this.pair.second.value;
    return this.pair.first.value;
  }
};
int&<a> relay<lifetime a, lifetime b>(Cell&<a> x, Cell&<b> y) {
  return Carrier(x,y).get();
}
int main() { Cell x{1}; Cell y{2}; return relay(x,y); }
)");
  if (!frontend.canGenerateCode())
    return 1;
  bool exercised = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "get")
      continue;
    for (const auto &block : function.body.blocks) {
      if (!block.terminator.returnLoan)
        continue;
      auto forged = frontend.mir;
      auto *body =
          lang::findMirBody(forged, {lang::MirBodyKind::Function, function.id});
      for (auto &candidateBlock : body->blocks)
        for (auto &instruction : candidateBlock.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Borrow ||
              instruction.loan != block.terminator.returnLoan ||
              instruction.operands.size() != 1)
            continue;
          auto &operand = instruction.operands.front();
          const auto *original = body->findPlace(operand.place);
          if (!original)
            continue;
          for (const auto &other : body->places) {
            if (other.id == original->id ||
                other.root != lang::MirPlaceRootKind::This ||
                other.type != original->type ||
                other.projections.size() != original->projections.size() ||
                other.projections == original->projections)
              continue;
            operand.place = other.id;
            exercised = true;
            break;
          }
        }
      expect(lang::rebuildMirValueUses(*body) &&
                 !lang::verifyMirProgram(forged).valid(),
             "a different executed referent cannot inherit the selected "
             "field's owner proof");
    }
  }
  expect(exercised, "forgery selects another typed projected referent");
  return ::failures == 0 ? 0 : 1;
}
