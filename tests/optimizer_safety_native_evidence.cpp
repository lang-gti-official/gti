#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimization/safety.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string environmentPath(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

lang::FrontendResult analyzeTrusted(std::string_view source) {
  const std::filesystem::path stdlibRoot = environmentPath("GTI_STDLIB_PATH");
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      "gti-optimizer-safety-evidence.gti";
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend().analyze(
      entry, std::string(source), {stdlibRoot / "prelude.gti", entry},
      {{entryKey, std::string(source)}}, {stdlibRoot});
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

const lang::HirFunctionInstance *
findFunction(const lang::FrontendResult &frontend, std::string_view name) {
  const auto found =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [&](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == name;
                   });
  return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
}

std::size_t functionDischarges(const lang::FrontendResult &frontend,
                               const lang::OptimizedProgram &optimized,
                               std::string_view name,
                               lang::MirSafetyCheckFamily family) {
  const lang::HirFunctionInstance *hir = findFunction(frontend, name);
  const lang::MirFunctionInstance *function =
      hir == nullptr ? nullptr : optimized.mir.findFunctionInstance(hir->id);
  std::size_t count = 0;
  if (function == nullptr) {
    return count;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      count += static_cast<std::size_t>(
          std::count_if(instruction.safetyDischarges.begin(),
                        instruction.safetyDischarges.end(),
                        [family](const lang::MirSafetyDischarge &discharge) {
                          return discharge.family == family;
                        }));
    }
  }
  return count;
}

struct EmittedProgram {
  lang::OptimizedProgram optimized;
  std::string cpp;
  std::filesystem::path artifact;
};

EmittedProgram emitProgram(const lang::FrontendResult &frontend,
                           lang::OptimizationLevel level,
                           const std::filesystem::path &directory,
                           std::string_view label) {
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir, level);
  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .compatibility = &compatibility});
  if (!optimized.valid()) {
    expect(false,
           "optimizer should produce coherent MIR for " + std::string(label));
    return {.optimized = std::move(optimized)};
  }
  const lang::BackendArtifact generated =
      gti_test::emitCpp(frontend, optimized.sourceMir, optimized.mir,
                        optimized.report.transformations);
  expect(!generated.contents.empty(),
         "C++ backend should accept verified safety proofs for " +
             std::string(label));
  const std::filesystem::path artifact =
      directory / (std::string(label) + ".cpp");
  std::ofstream(artifact) << generated.contents;
  return {.optimized = std::move(optimized),
          .cpp = generated.contents,
          .artifact = artifact};
}

struct RunResult {
  bool built = false;
  int exitStatus = -1;
  std::string stdoutText;
  std::string stderrText;
};

std::filesystem::path build(const std::string &compiler,
                            const std::filesystem::path &artifact,
                            const std::filesystem::path &directory,
                            std::string_view label) {
  const std::filesystem::path binary =
      directory / ("bin-" + std::string(label));
  const std::filesystem::path compileLog =
      directory / (std::string(label) + "-cc.log");
  std::string command = compiler +
                        " -std=c++23 -O0 -fno-fast-math -ffp-contract=off "
                        "-D__gti_strict_ieee754=1";
  const std::string runtimeInclude = environmentPath("GTI_RUNTIME_INCLUDE");
  const std::string runtimeLibrary = environmentPath("GTI_RUNTIME_LIBRARY");
  if (!runtimeInclude.empty()) {
    command += " -I\"" + runtimeInclude + "\"";
  }
  command += " \"" + artifact.string() + "\"";
  if (!runtimeLibrary.empty()) {
    command += " \"" + runtimeLibrary + "\"";
  }
  command +=
      " -o \"" + binary.string() + "\" 2>\"" + compileLog.string() + "\"";
  if (std::system(command.c_str()) != 0) {
    std::cerr << "native compile failed (" << label << "):\n"
              << readFile(compileLog) << '\n';
    return {};
  }
  return binary;
}

RunResult run(const std::filesystem::path &binary,
              const std::filesystem::path &directory, std::string_view label,
              std::string_view arguments) {
  RunResult result;
  result.built = !binary.empty();
  if (!result.built) {
    return result;
  }
  const std::filesystem::path stdoutLog =
      directory / (std::string(label) + "-stdout.log");
  const std::filesystem::path stderrLog =
      directory / (std::string(label) + "-stderr.log");
  const std::string command =
      "\"" + binary.string() + "\"" + std::string(arguments) + " >\"" +
      stdoutLog.string() + "\" 2>\"" + stderrLog.string() + "\"";
  result.exitStatus = std::system(command.c_str());
  result.stdoutText = readFile(stdoutLog);
  result.stderrText = readFile(stderrLog);
  return result;
}

constexpr std::string_view source = R"(
#include <std/string>
#include <std/vector>
#include <std/memory>

mut int32_t destroyed = 0;
mut int32_t constructed_value = 0;

class Tracker {
  int32_t value;

public:
  Tracker(int32_t initial) : value(initial) {}
  ~Tracker() { destroyed += this.value; }

  int32_t get() { return this.value; }
};

class MutatesOnConstruct {
public:
  MutatesOnConstruct() { constructed_value = 2147483647; }
};

int32_t fixed_success() {
  int32_t values[4] = {1, 2, 3, 4};
  mut uint64_t index = 0;
  mut int32_t total = 0;
  while (index < 4) {
    total += values[index];
    index++;
  }
  return total;
}

int32_t storage_success() {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<Tracker>(std::size_t(1));
  if (!allocated) { return -1; }
  mut auto values = std::move(allocated).value();
  values.emplace(std::size_t(0), int32_t(7));
  int32_t observed = values.get(std::size_t(0)).get();
  values.destroy(std::size_t(0));
  return observed;
}

int32_t arithmetic_success(int32_t value) {
  if (value >= 0) {
    if (value < 10) {
      return value + 1;
    }
  }
  return 0;
}

int32_t remainder_success(int32_t value) {
  return value % int32_t(-1);
}

int32_t remainder_assign_success(int32_t value) {
  mut int32_t values[1] = {value};
  values[0] %= int32_t(-1);
  return values[0];
}

int32_t fixed_write_success(int32_t value) {
  mut int32_t values[4] = {};
  for (mut uint64_t index = 0; index < 4; index++) {
    values[index] = (value & int32_t(255)) + int32_t(1);
  }
  return values[0];
}

void closed_vector_scale(mut std::vector<int32_t>& values,
                         int32_t multiplier) {
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] *= multiplier;
  }
}

int32_t closed_vector_success() {
  mut std::vector<int32_t> values =
      std::vector<int32_t>(std::size_t(4));
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] =
        int32_t(index % std::size_t(4)) + int32_t(1);
  }
  closed_vector_scale(values, int32_t(-1));
  return values[std::size_t(3)];
}

int32_t storage_constructor_barrier() {
  if (constructed_value < 10) {
    std::allocator allocator{};
    auto allocated = allocator.try_storage<MutatesOnConstruct>(std::size_t(1));
    if (!allocated) { return 1; }
    mut auto values = std::move(allocated).value();
    values.emplace(std::size_t(0));
    return constructed_value + 1;
  }
  return 0;
}

int32_t bounds_failure(uint64_t index) {
  int32_t values[2] = {1, 2};
  return values[index];
}

int32_t storage_failure() {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return -1; }
  mut auto values = std::move(allocated).value();
  values.construct(std::size_t(0), int32_t(9));
  values.destroy(std::size_t(0));
  return values.get(std::size_t(0));
}

int32_t overflow_failure(int32_t value) {
  return value + 1;
}

int32_t remainder_failure(int32_t value, int32_t divisor) {
  return value % divisor;
}

int32_t vector_failure(int32_t selector) {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.reserve(std::size_t(1));
  if (selector == 0) {
    return values.front();
  }
  if (selector == 1) {
    return values.back();
  }
  return values.at(std::size_t(0));
}

int main(int argc, std::vector<std::string> argv) {
  if (argc == 2) {
    return bounds_failure(uint64_t(2));
  }
  if (argc == 3) {
    return storage_failure();
  }
  if (argc == 4) {
    return overflow_failure(int32_t(2147483647));
  }
  if (argc == 5) {
    return remainder_failure(int32_t(7), int32_t(0));
  }
  if (argc == 6) {
    return vector_failure(int32_t(0));
  }
  if (argc == 7) {
    return vector_failure(int32_t(1));
  }
  if (argc == 8) {
    return vector_failure(int32_t(2));
  }
  if (fixed_success() != 10) { return 1; }
  if (closed_vector_success() != -4) { return 8; }
  if (storage_success() != 7) { return 2; }
  if (destroyed != 7) { return 3; }
  if (arithmetic_success(4) != 5) { return 4; }
  if (remainder_success(int32_t(-2147483647 - 1)) != 0) { return 5; }
  if (remainder_assign_success(int32_t(-2147483647 - 1)) != 0) { return 6; }
  if (fixed_write_success(511) != 256) { return 7; }
  return 0;
}
)";

constexpr std::string_view closedCallNearMissSource = R"(
#include <std/vector>

extern "C" {
  int32_t external_factor();
}

void conflicting_scale(mut std::vector<int32_t>& values,
                       int32_t multiplier) {
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] *= multiplier;
  }
}

void recursive_scale(mut std::vector<int32_t>& values,
                     int32_t multiplier,
                     bool recurse) {
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] *= multiplier;
  }
  if (recurse) {
    recursive_scale(values, multiplier, false);
  }
}

void owning_scale(mut std::vector<int32_t> values, int32_t multiplier) {
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] *= multiplier;
  }
}

void native_scale(mut std::vector<int32_t>& values, int32_t multiplier) {
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] *= multiplier;
  }
}

class VirtualScaler {
public:
  virtual void scale(mut std::vector<int32_t>& values,
                     int32_t multiplier) mut {
    for (mut std::size_t index = 0; index < values.size(); index++) {
      values[index] *= multiplier;
    }
  }
};

int main() {
  mut std::vector<int32_t> conflicting =
      std::vector<int32_t>(std::size_t(4));
  for (mut std::size_t index = 0; index < conflicting.size(); index++) {
    conflicting[index] =
        int32_t(index % std::size_t(4)) + int32_t(1);
  }
  conflicting_scale(conflicting, int32_t(2));
  conflicting_scale(conflicting, int32_t(1000000000));

  mut std::vector<int32_t> recursive =
      std::vector<int32_t>(std::size_t(4));
  recursive_scale(recursive, int32_t(2), false);

  mut std::vector<int32_t> owned =
      std::vector<int32_t>(std::size_t(4));
  owning_scale(std::move(owned), int32_t(2));

  mut std::vector<int32_t> native_values =
      std::vector<int32_t>(std::size_t(4));
  native_scale(native_values, external_factor());

  mut std::vector<int32_t> virtual_values =
      std::vector<int32_t>(std::size_t(4));
  mut VirtualScaler scaler = VirtualScaler();
  scaler.scale(virtual_values, int32_t(2));
  return 0;
}
)";

constexpr std::string_view boundedLogicalSizeSource = R"(
#include <std/vector>

int main() {
  mut std::vector<int32_t> values =
      std::vector<int32_t>(std::size_t(4));
  for (mut std::size_t index = 0; index < values.size(); index++) {
    values[index] = int32_t(index % std::size_t(4));
  }
  return 0;
}
)";

void testBoundedLogicalSizeEmission(const std::string &compiler,
                                    const std::filesystem::path &directory) {
  const lang::FrontendResult frontend =
      analyzeTrusted(boundedLogicalSizeSource);
  expect(frontend.canGenerateCode(),
         "bounded logical-size fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const EmittedProgram o0 = emitProgram(frontend, lang::OptimizationLevel::O0,
                                        directory, "bounded-o0");
  const EmittedProgram o2 = emitProgram(frontend, lang::OptimizationLevel::O2,
                                        directory, "bounded-o2");
  constexpr std::string_view checkedBounds =
      "::gti_internal::backend::mir_checked_index_bounds_v1(";
  expect(occurrences(o2.cpp, checkedBounds) <
             occurrences(o0.cpp, checkedBounds),
         "a proved public logical-size check should emit no target operation");

  const std::filesystem::path o0Binary =
      build(compiler, o0.artifact, directory, "bounded-o0");
  const std::filesystem::path o2Binary =
      build(compiler, o2.artifact, directory, "bounded-o2");
  const RunResult baseline = run(o0Binary, directory, "bounded-o0", "");
  const RunResult optimized = run(o2Binary, directory, "bounded-o2", "");
  expect(baseline.built && optimized.built && baseline.exitStatus == 0 &&
             optimized.exitStatus == baseline.exitStatus &&
             optimized.stdoutText == baseline.stdoutText &&
             optimized.stderrText == baseline.stderrText,
         "a discharged public logical-size check should preserve native O0 "
         "behavior");
}

// Lengths and indices come from argc, so no interval proves a check; only
// the dominating comparison between each check's operands can. Phase 1 also
// reaches a non-strict guard that admits `index == live`, whose check must
// fail exactly as it does unoptimized.
constexpr std::string_view relationalBoundsSource = R"(
#include <std/vector>
#include <std/string>

uint64_t looped(uint64_t live) {
  mut uint64_t total = 0;
  for (mut uint64_t index = 0; index < live; index++) {
    gti_internal::index_bounds_check(index, live);
    total += index;
  }
  return total;
}

uint64_t guarded(uint64_t index, uint64_t live) {
  if (live > index) {
    gti_internal::index_bounds_check(index, live + uint64_t(1));
    return index;
  }
  return uint64_t(0);
}

uint64_t non_strict(uint64_t index, uint64_t live) {
  if (index <= live) {
    gti_internal::index_bounds_check(index, live);
  }
  return index;
}

int main(int argc, std::vector<std::string> argv) {
  uint64_t phase = uint64_t(argc) - uint64_t(1);
  uint64_t live = uint64_t(argc) * uint64_t(5);
  std::println(looped(live));
  std::println(guarded(live - uint64_t(1), live));
  if (phase == uint64_t(1)) {
    std::println(non_strict(live, live));
  }
  return 0;
}
)";

void testRelationalBoundsEmission(const std::string &compiler,
                                  const std::filesystem::path &directory) {
  const lang::FrontendResult frontend = analyzeTrusted(relationalBoundsSource);
  expect(frontend.canGenerateCode(),
         "relational bounds fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const EmittedProgram o0 = emitProgram(frontend, lang::OptimizationLevel::O0,
                                        directory, "relational-o0");
  const EmittedProgram o2 = emitProgram(frontend, lang::OptimizationLevel::O2,
                                        directory, "relational-o2");
  constexpr std::string_view checkedBounds =
      "::gti_internal::backend::mir_checked_index_bounds_v1(";
  // The whole program also carries library checks other proofs may remove;
  // optimizer_safety pins the per-function counts.
  expect(occurrences(o0.cpp, checkedBounds) >=
             occurrences(o2.cpp, checkedBounds) + 2,
         "O2 should emit no target operation for the two relationally proved "
         "checks (O0 " +
             std::to_string(occurrences(o0.cpp, checkedBounds)) + ", O2 " +
             std::to_string(occurrences(o2.cpp, checkedBounds)) + ")");

  const std::filesystem::path o0Binary =
      build(compiler, o0.artifact, directory, "relational-o0");
  const std::filesystem::path o2Binary =
      build(compiler, o2.artifact, directory, "relational-o2");
  for (const std::string_view arguments : {"", " fail"}) {
    const std::string suffix = arguments.empty() ? "pass" : "fail";
    const RunResult baseline =
        run(o0Binary, directory, "relational-o0-" + suffix, arguments);
    const RunResult optimized =
        run(o2Binary, directory, "relational-o2-" + suffix, arguments);
    expect(baseline.built && optimized.built &&
               (arguments.empty() ? baseline.exitStatus == 0
                                  : baseline.exitStatus != 0) &&
               optimized.exitStatus == baseline.exitStatus &&
               optimized.stdoutText == baseline.stdoutText &&
               optimized.stderrText == baseline.stderrText,
           "relationally discharged checks should preserve native O0 "
           "behavior, including the retained check's failure (" +
               suffix + ")");
  }
}

void testClosedCallNearMisses() {
  const lang::FrontendResult frontend =
      analyzeTrusted(closedCallNearMissSource);
  expect(frontend.canGenerateCode(),
         "closed-call near-miss fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClosedCallSafetyAnalysis analysis =
      lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  const auto bodyFor = [&](std::string_view name) {
    const lang::HirFunctionInstance *function = findFunction(frontend, name);
    return function == nullptr
               ? static_cast<const lang::MirClosedCallBodyAnalysis *>(nullptr)
               : analysis.find({.kind = lang::MirBodyKind::Function,
                                .owner = function->id});
  };
  const auto remainsChecked = [&](std::string_view name) {
    const lang::MirClosedCallBodyAnalysis *body = bodyFor(name);
    const lang::MirBody *mirBody =
        body == nullptr ? nullptr : lang::findMirBody(frontend.mir, body->body);
    if (body == nullptr || mirBody == nullptr) {
      return false;
    }
    bool foundMultiplication = false;
    for (const lang::MirBlock &block : mirBody->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        // Checked-reference assignment can lower to a Multiply followed by
        // Assign rather than a single MultiplyAssign. A missing operation
        // must not make this negative pass vacuously.
        if (instruction.tombstone ||
            (instruction.operation != lang::MirOperation::Multiply &&
             instruction.operation != lang::MirOperation::MultiplyAssign)) {
          continue;
        }
        foundMultiplication = true;
        if (std::any_of(
                body->safety.proofs.begin(), body->safety.proofs.end(),
                [&](const lang::MirSafetyProof &proof) {
                  return proof.instruction == instruction.id &&
                         proof.discharge.family ==
                             lang::MirSafetyCheckFamily::ArithmeticOverflow;
                })) {
          return false;
        }
      }
    }
    return foundMultiplication;
  };

  const lang::MirClosedCallBodyAnalysis *conflicting =
      bodyFor("conflicting_scale");
  const lang::MirClosedCallBodyAnalysis *recursive = bodyFor("recursive_scale");
  const lang::MirClosedCallBodyAnalysis *owning = bodyFor("owning_scale");
  const lang::MirClosedCallBodyAnalysis *native = bodyFor("native_scale");
  const lang::MirClosedCallBodyAnalysis *virtualCall = bodyFor("scale");
  expect(analysis.converged && analysis.iterations <= 32 &&
             conflicting != nullptr && recursive != nullptr &&
             !recursive->closed && owning != nullptr && !owning->closed &&
             native != nullptr && virtualCall != nullptr &&
             !virtualCall->closed && remainsChecked("conflicting_scale") &&
             remainsChecked("recursive_scale") &&
             remainsChecked("owning_scale") && remainsChecked("native_scale") &&
             remainsChecked("scale"),
         "conflicting contexts and recursive, owning-cleanup, native-fed, or "
         "virtual call families should terminate within the deterministic "
         "budget and retain their checked arithmetic");
}

void testClosedCallAfterSourceAllocation(
    const std::string &compiler, const std::filesystem::path &directory) {
  // Earlier incomplete summaries prevented discovery of this independent
  // workload after source allocation. That was an implementation limitation,
  // not an observable rule: recover the same two proofs in either order.
  // Actual sparse-slot checks and runtime-failing near misses remain controls.
  constexpr std::string_view vectorCheck =
      "  if (closed_vector_success() != -4) { return 8; }\n";
  constexpr std::string_view storageCheck =
      "  if (destroyed != 7) { return 3; }\n";
  std::string allocatorFirst(source);
  const std::size_t vectorPosition = allocatorFirst.find(vectorCheck);
  expect(vectorPosition != std::string::npos,
         "source-allocation variant must retain the exact vector control");
  if (vectorPosition == std::string::npos) {
    return;
  }
  allocatorFirst.erase(vectorPosition, vectorCheck.size());
  const std::size_t storagePosition = allocatorFirst.find(storageCheck);
  expect(storagePosition != std::string::npos,
         "source-allocation variant must retain exact cleanup observation");
  if (storagePosition == std::string::npos) {
    return;
  }
  allocatorFirst.insert(storagePosition + storageCheck.size(), vectorCheck);

  const lang::FrontendResult frontend = analyzeTrusted(allocatorFirst);
  expect(frontend.canGenerateCode(),
         "source-allocation context barrier should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::HirFunctionInstance *scale =
      findFunction(frontend, "closed_vector_scale");
  expect(scale != nullptr,
         "source-allocation context barrier must retain vector scale");
  if (scale == nullptr) {
    return;
  }
  const lang::MirClosedCallSafetyAnalysis contexts =
      lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  const auto *context =
      contexts.find({.kind = lang::MirBodyKind::Function, .owner = scale->id});
  expect(contexts.converged && context != nullptr && context->reached,
         "independent source-allocation continuation must discover a "
         "converged vector scale context");

  RunResult baseline;
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const std::string label =
        level == lang::OptimizationLevel::O0   ? "allocator-first-o0"
        : level == lang::OptimizationLevel::O2 ? "allocator-first-o2"
                                               : "allocator-first-o3";
    const EmittedProgram program =
        emitProgram(frontend, level, directory, label);
    const auto arithmetic =
        functionDischarges(frontend, program.optimized, "closed_vector_scale",
                           lang::MirSafetyCheckFamily::ArithmeticOverflow);
    expect(arithmetic == (level == lang::OptimizationLevel::O0 ? 0U : 2U),
           label +
               " must retain O0 checks and recover both independent "
               "vector arithmetic proofs at O2/O3, found " +
               std::to_string(arithmetic));
    if (level != lang::OptimizationLevel::O0) {
      bool incrementProved = false;
      bool multiplicationProved = false;
      const auto *optimizedScale =
          program.optimized.mir.findFunctionInstance(scale->id);
      if (optimizedScale) {
        for (const auto &block : optimizedScale->body.blocks) {
          for (const auto &instruction : block.instructions) {
            const bool proved = std::any_of(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &discharge) {
                  return discharge.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
            incrementProved |=
                proved && instruction.operation == lang::MirOperation::Add;
            multiplicationProved |=
                proved &&
                (instruction.operation == lang::MirOperation::Multiply ||
                 instruction.operation == lang::MirOperation::MultiplyAssign);
          }
        }
      }
      expect(incrementProved && multiplicationProved,
             label + " must prove the guarded increment and initialized "
                     "element multiplication, not two unrelated operations");
    }
    const std::filesystem::path binary =
        build(compiler, program.artifact, directory, label);
    const RunResult actual = run(binary, directory, label, "");
    if (level == lang::OptimizationLevel::O0) {
      baseline = actual;
      expect(actual.built && actual.exitStatus == 0,
             "allocator-first O0 must preserve result and exact cleanup");
    } else {
      expect(actual.built && actual.exitStatus == baseline.exitStatus &&
                 actual.stdoutText == baseline.stdoutText &&
                 actual.stderrText == baseline.stderrText,
             "allocator-first O2/O3 must preserve O0 behavior and exact "
             "cleanup across the independent source-allocation call");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string compiler = argc > 1 ? argv[1] : "c++";
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "gti-optimizer-safety-evidence";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

  testClosedCallNearMisses();
  testBoundedLogicalSizeEmission(compiler, directory);
  testRelationalBoundsEmission(compiler, directory);
  testClosedCallAfterSourceAllocation(compiler, directory);

  const lang::FrontendResult frontend = analyzeTrusted(source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "native safety evidence should pass the trusted frontend");
  if (!frontend.canGenerateCode()) {
    return 1;
  }

  std::vector<EmittedProgram> programs;
  programs.push_back(
      emitProgram(frontend, lang::OptimizationLevel::O0, directory, "gti-o0"));
  programs.push_back(
      emitProgram(frontend, lang::OptimizationLevel::O2, directory, "gti-o2"));
  programs.push_back(
      emitProgram(frontend, lang::OptimizationLevel::O3, directory, "gti-o3"));

  const EmittedProgram &o0 = programs[0];
  const EmittedProgram &o2 = programs[1];
  const EmittedProgram &o3 = programs[2];
  const auto dischargeCount = [&](std::string_view function,
                                  lang::MirSafetyCheckFamily family) {
    return functionDischarges(frontend, o2.optimized, function, family);
  };
  const auto expectDischarges = [&](std::string_view function,
                                    lang::MirSafetyCheckFamily family,
                                    std::size_t expected) {
    const std::size_t actual = dischargeCount(function, family);
    expect(actual == expected,
           std::string(function) + " should have " + std::to_string(expected) +
               " " + std::string(lang::mirSafetyCheckFamilyName(family)) +
               " discharge(s), found " + std::to_string(actual));
  };
  expectDischarges("fixed_success", lang::MirSafetyCheckFamily::Bounds, 1);
  expectDischarges("arithmetic_success",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow, 1);
  expectDischarges("remainder_success",
                   lang::MirSafetyCheckFamily::ArithmeticDomain, 1);
  expectDischarges("remainder_assign_success",
                   lang::MirSafetyCheckFamily::ArithmeticDomain, 1);
  expectDischarges("closed_vector_scale",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow, 2);
  expect(dischargeCount("fixed_write_success",
                        lang::MirSafetyCheckFamily::Bounds) > 0,
         "O2 should prove at least one native-emitted fixed-array write");
  expect(
      functionDischarges(frontend, o2.optimized, "bounds_failure",
                         lang::MirSafetyCheckFamily::Bounds) == 0 &&
          functionDischarges(frontend, o2.optimized, "overflow_failure",
                             lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
              0 &&
          functionDischarges(
              frontend, o2.optimized, "storage_constructor_barrier",
              lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0 &&
          functionDischarges(frontend, o2.optimized, "remainder_failure",
                             lang::MirSafetyCheckFamily::ArithmeticDomain) == 0,
      "runtime-failing near misses and storage lifecycle mutation barriers "
      "must retain their exact checks");

  const auto retainedSourcePolicyChecks = [](const lang::MirProgram &program) {
    std::size_t count = 0;
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(program)) {
      const lang::MirBody *body = lang::findMirBody(program, address);
      if (body == nullptr) {
        continue;
      }
      for (const lang::MirBlock &block : body->blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          const auto failure =
              lang::intrinsicBooleanFailureOutcome(instruction.intrinsic);
          if (!failure ||
              failure->code != lang::DefinedFailureCode::InvalidStorageState) {
            continue;
          }
          ++count;
          expect(
              !instruction.tombstone && instruction.safetyDischarges.empty() &&
                  std::any_of(instruction.definedFailure.localOrigins.begin(),
                              instruction.definedFailure.localOrigins.end(),
                              [&](const lang::DefinedFailureOrigin &origin) {
                                return std::find(origin.outcomes.begin(),
                                                 origin.outcomes.end(),
                                                 *failure) !=
                                       origin.outcomes.end();
                              }),
              "source slot-policy checks must retain their exact outcome; "
              "a removed private initialized-set model is not proof");
        }
      }
    }
    return count;
  };
  const std::size_t policyChecks = retainedSourcePolicyChecks(frontend.mir);
  expect(policyChecks > 0 &&
             retainedSourcePolicyChecks(o0.optimized.mir) == policyChecks &&
             retainedSourcePolicyChecks(o2.optimized.mir) == policyChecks &&
             retainedSourcePolicyChecks(o3.optimized.mir) == policyChecks,
         "source-defined storage must preserve policy checks at every level");
  for (const EmittedProgram &program : programs) {
    expect(std::none_of(program.optimized.report.passes.begin(),
                        program.optimized.report.passes.end(),
                        [](const lang::OptimizationPassReport &pass) {
                          return pass.name == "discharge-storage-state-checks";
                        }),
           "source-defined storage must not retain a private sparse-slot pass");
  }

  constexpr std::string_view checkedArrayCall =
      " = ::gti_internal::backend::mir_checked_array_read_v1(";
  constexpr std::string_view checkedAddCall =
      " = ::gti_internal::backend::mir_checked_add_v1";
  constexpr std::string_view checkedArrayWriteCall =
      " = ::gti_internal::backend::mir_checked_array_write_v1(";
  constexpr std::string_view checkedRemainderCall =
      "::gti_internal::backend::mir_checked_remainder_v1";
  constexpr std::string_view nonzeroRemainderCall =
      "::gti_internal::backend::mir_nonzero_remainder_v1";
  constexpr std::string_view publicPrefixBoundsCall =
      "::gti_internal::backend::mir_checked_index_bounds_v1(";
  for (const auto &program : programs)
    for (const auto retired :
         {"::gti_internal::backend::mir_prefix_read_v1(",
          "::gti_internal::backend::mir_prefix_read_mut_v1(",
          "::gti_internal::backend::prefix_storage_read_unchecked(",
          "::gti_internal::backend::prefix_storage_read_mut_unchecked("})
      expect(program.cpp.find(retired) == std::string::npos,
             "source prefix access must not revive a retired native helper");
  // Compound assignments now stage their read, arithmetic and write as
  // separate MIR operations. Observe the checked scalar multiply and its
  // discharged expression, not the retired fused compound-assignment spelling.
  constexpr std::string_view checkedMultiply =
      " = ::gti_internal::backend::mir_checked_multiply_v1<std::int32_t>(";
  constexpr std::string_view dischargedMultiply = " * __gti_mir_v_";
  const auto multiplyCounts = [&](std::string_view needle) {
    return " (O0=" + std::to_string(occurrences(o0.cpp, needle)) +
           ", O2=" + std::to_string(occurrences(o2.cpp, needle)) +
           ", O3=" + std::to_string(occurrences(o3.cpp, needle)) + ")";
  };
  expect(occurrences(o2.cpp, checkedMultiply) <
                 occurrences(o0.cpp, checkedMultiply) &&
             occurrences(o2.cpp, checkedMultiply) ==
                 occurrences(o3.cpp, checkedMultiply),
         "proved staged multiplication must remove native checked calls" +
             multiplyCounts(checkedMultiply));
  expect(occurrences(o2.cpp, dischargedMultiply) >
                 occurrences(o0.cpp, dischargedMultiply) &&
             occurrences(o2.cpp, dischargedMultiply) ==
                 occurrences(o3.cpp, dischargedMultiply),
         "proved staged multiplication must emit native scalar multiplication" +
             multiplyCounts(dischargedMultiply));
  expect(occurrences(o2.cpp, checkedArrayCall) <
                 occurrences(o0.cpp, checkedArrayCall) &&
             occurrences(o2.cpp, checkedAddCall) <
                 occurrences(o0.cpp, checkedAddCall) &&
             occurrences(o2.cpp, checkedArrayWriteCall) <
                 occurrences(o0.cpp, checkedArrayWriteCall) &&
             occurrences(o2.cpp, checkedRemainderCall) <
                 occurrences(o0.cpp, checkedRemainderCall) &&
             occurrences(o0.cpp, nonzeroRemainderCall) == 0 &&
             occurrences(o2.cpp, nonzeroRemainderCall) > 0 &&
             occurrences(o2.cpp, publicPrefixBoundsCall) <
                 occurrences(o0.cpp, publicPrefixBoundsCall) &&
             occurrences(o2.cpp, publicPrefixBoundsCall) > 0 &&
             occurrences(o2.cpp, checkedArrayCall) ==
                 occurrences(o3.cpp, checkedArrayCall) &&
             occurrences(o2.cpp, checkedAddCall) ==
                 occurrences(o3.cpp, checkedAddCall) &&
             occurrences(o2.cpp, checkedArrayWriteCall) ==
                 occurrences(o3.cpp, checkedArrayWriteCall) &&
             occurrences(o2.cpp, checkedRemainderCall) ==
                 occurrences(o3.cpp, checkedRemainderCall) &&
             occurrences(o2.cpp, nonzeroRemainderCall) ==
                 occurrences(o3.cpp, nonzeroRemainderCall) &&
             occurrences(o2.cpp, publicPrefixBoundsCall) ==
                 occurrences(o3.cpp, publicPrefixBoundsCall),
         "native -O0 source should expose removed safe calls while preserving "
         "the checked near-miss calls");

  std::vector<std::filesystem::path> binaries;
  for (std::size_t index = 0; index < programs.size(); ++index) {
    const std::string label = index == 0   ? "gti-o0"
                              : index == 1 ? "gti-o2"
                                           : "gti-o3";
    binaries.push_back(
        build(compiler, programs[index].artifact, directory, label));
    expect(!binaries.back().empty(),
           "native -O0 should compile the " + label + " artifact");
  }

  const std::vector<std::pair<std::string, std::string>> scenarios{
      {"success", ""},
      {"bounds", " bounds"},
      {"storage", " storage extra"},
      {"overflow", " overflow extra arguments"},
      {"remainder", " remainder extra arguments here"},
      {"vector-front", " vector front public detail here"},
      {"vector-back", " vector back remains public detail here"},
      {"vector-at", " vector at remains public detail here now"}};
  std::vector<RunResult> baseline;
  for (const auto &[name, arguments] : scenarios) {
    baseline.push_back(run(binaries[0], directory, "o0-" + name, arguments));
  }
  expect(baseline[0].built && baseline[0].exitStatus == 0,
         "O0 success mode should preserve result and deterministic cleanup");
  expect(baseline[1].exitStatus != 0 &&
             baseline[1].stderrText.find(
                 "index_out_of_bounds in fixed_array") != std::string::npos &&
             baseline[2].exitStatus != 0 &&
             baseline[2].stderrText.find(
                 "invalid_storage_state in uninitialized_access") !=
                 std::string::npos &&
             baseline[3].exitStatus != 0 &&
             baseline[3].stderrText.find("integer_overflow in addition") !=
                 std::string::npos &&
             baseline[4].exitStatus != 0 &&
             baseline[4].stderrText.find("modulo_by_zero in integer_modulo") !=
                 std::string::npos &&
             baseline[5].exitStatus != 0 &&
             baseline[5].stderrText.find("index_out_of_bounds in vector") !=
                 std::string::npos &&
             baseline[6].exitStatus != 0 &&
             baseline[6].stderrText.find("index_out_of_bounds in vector") !=
                 std::string::npos &&
             baseline[7].exitStatus != 0 &&
             baseline[7].stderrText.find("index_out_of_bounds in vector") !=
                 std::string::npos,
         "O0 near misses should expose the exact checked-operation or sealed "
         "storage-guard identity, including public vector access failures");

  for (std::size_t level = 1; level < binaries.size(); ++level) {
    for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario) {
      const std::string label =
          (level == 1 ? "o2-" : "o3-") + scenarios[scenario].first;
      const RunResult actual =
          run(binaries[level], directory, label, scenarios[scenario].second);
      expect(actual.built &&
                 actual.exitStatus == baseline[scenario].exitStatus &&
                 actual.stdoutText == baseline[scenario].stdoutText &&
                 actual.stderrText == baseline[scenario].stderrText,
             "O2/O3 should preserve O0 result, cleanup, failure category, "
             "and source origin for " +
                 scenarios[scenario].first);
    }
  }

  if (failures != 0) {
    std::cerr << failures << " optimizer safety native evidence failure(s)\n";
    return 1;
  }
  std::cout << "optimizer safety native evidence passed\n";
  return 0;
}
