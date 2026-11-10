// C++ MIR body emitter, representation snapshot and emitted text for
// function items: the confined relay's failure-form vocabulary, the private
// nominal tag, thread-adapter placement and per-standard verified-MIR
// emission. The backend-neutral HIR/MIR/LoweredProgram facts live in
// function_item_tests.cpp.

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "cpp_backend_test_support.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

void testGenericAndManagedPipelineEmission() {
  const std::string source = R"(
import std.algorithm;
import std.functional;
import std.thread;
import std.vector;

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
  expect(frontend.canGenerateCode(), "function-item fixture analyzes");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = concurrentTarget()});
  expect(optimized.valid(), "function-item fixture optimizes");
  if (!optimized.valid()) {
    return;
  }
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, concurrentTarget(),
      optimized.report.transformations);
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
}

void testPackInvocationEmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/function_item_pack_contracts.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyze("pack-function-items.gti", source);
  expect(frontend.canGenerateCode(), "pack function-item fixture analyzes");
  if (!frontend.canGenerateCode()) {
    return;
  }
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(optimized.valid(), "pack invocation survives verified optimization");
    if (!optimized.valid()) {
      continue;
    }
    const auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto snapshot =
          lang::buildCppMirRepresentationSnapshot(lowered, standard);
      expect(snapshot.valid(),
             "pack invocation satisfies production MIR backend preflight");
      if (snapshot.valid()) {
        const auto cpp = lang::CppBackend(standard).generate(lowered).contents;
        expect(cpp.find("GTI verified-MIR body") != std::string::npos,
               "production emission consumes verified MIR for the new "
               "accepted program");
      }
    }
  }
}

} // namespace

int main() {
  testGenericAndManagedPipelineEmission();
  testPackInvocationEmission();
  if (failures != 0) {
    return 1;
  }
  std::cout << "function item C++ representation tests passed\n";
  return 0;
}
