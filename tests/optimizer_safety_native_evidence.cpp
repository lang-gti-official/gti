#include "gti/cpp_backend.h"
#include "gti/frontend.h"
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
  const std::string vendorInclude = environmentPath("GTI_VENDOR_INCLUDE");
  const std::string runtimeLibrary = environmentPath("GTI_RUNTIME_LIBRARY");
  if (!runtimeInclude.empty()) {
    command += " -I\"" + runtimeInclude + "\"";
  }
  if (!vendorInclude.empty()) {
    command += " -I\"" + vendorInclude + "\"";
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
  mut gti_internal::storage<Tracker> values =
      gti_internal::allocate_storage<Tracker>(uint64_t(1));
  gti_internal::storage_construct(values, uint64_t(0), int32_t(7));
  int32_t observed =
      gti_internal::storage_read(values, uint64_t(0)).get();
  gti_internal::storage_destroy(values, uint64_t(0));
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

int32_t storage_constructor_barrier() {
  if (constructed_value < 10) {
    mut gti_internal::storage<MutatesOnConstruct> values =
        gti_internal::allocate_storage<MutatesOnConstruct>(uint64_t(1));
    gti_internal::storage_construct(values, uint64_t(0));
    return constructed_value + 1;
  }
  return 0;
}

int32_t bounds_failure(uint64_t index) {
  int32_t values[2] = {1, 2};
  return values[index];
}

int32_t storage_failure() {
  mut gti_internal::storage<int32_t> values =
      gti_internal::allocate_storage<int32_t>(uint64_t(1));
  gti_internal::storage_construct(values, uint64_t(0), int32_t(9));
  gti_internal::storage_destroy(values, uint64_t(0));
  return gti_internal::storage_read(values, uint64_t(0));
}

int32_t overflow_failure(int32_t value) {
  return value + 1;
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
  if (fixed_success() != 10) { return 1; }
  if (storage_success() != 7) { return 2; }
  if (destroyed != 7) { return 3; }
  if (arithmetic_success(4) != 5) { return 4; }
  return 0;
}
)";

} // namespace

int main(int argc, char **argv) {
  const std::string compiler = argc > 1 ? argv[1] : "c++";
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "gti-optimizer-safety-evidence";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

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
  expect(
      functionDischarges(frontend, o2.optimized, "fixed_success",
                         lang::MirSafetyCheckFamily::Bounds) == 1 &&
          functionDischarges(frontend, o2.optimized, "storage_success",
                             lang::MirSafetyCheckFamily::Bounds) == 1 &&
          functionDischarges(frontend, o2.optimized, "storage_success",
                             lang::MirSafetyCheckFamily::StorageState) == 1 &&
          functionDischarges(frontend, o2.optimized, "arithmetic_success",
                             lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
              1,
      "O2 should prove one native-emitted check from every safety family");
  expect(
      functionDischarges(frontend, o2.optimized, "bounds_failure",
                         lang::MirSafetyCheckFamily::Bounds) == 0 &&
          functionDischarges(frontend, o2.optimized, "storage_failure",
                             lang::MirSafetyCheckFamily::StorageState) == 0 &&
          functionDischarges(frontend, o2.optimized, "overflow_failure",
                             lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
              0 &&
          functionDischarges(
              frontend, o2.optimized, "storage_constructor_barrier",
              lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
      "runtime-failing near misses and storage lifecycle mutation barriers "
      "must retain their exact checks");

  constexpr std::string_view checkedArrayCall =
      " = ::gti_internal::backend::mir_checked_array_read_v1(";
  constexpr std::string_view checkedAddCall =
      " = ::gti_internal::backend::mir_checked_add_v1";
  constexpr std::string_view checkedStorageCall =
      "&::gti_internal::backend::storage_read(";
  constexpr std::string_view uncheckedStorageCall =
      "&::gti_internal::backend::storage_read_unchecked(";
  expect(occurrences(o2.cpp, checkedArrayCall) <
                 occurrences(o0.cpp, checkedArrayCall) &&
             occurrences(o2.cpp, checkedAddCall) <
                 occurrences(o0.cpp, checkedAddCall) &&
             occurrences(o2.cpp, checkedStorageCall) <
                 occurrences(o0.cpp, checkedStorageCall) &&
             occurrences(o0.cpp, uncheckedStorageCall) == 0 &&
             occurrences(o2.cpp, uncheckedStorageCall) > 0 &&
             occurrences(o2.cpp, checkedArrayCall) ==
                 occurrences(o3.cpp, checkedArrayCall) &&
             occurrences(o2.cpp, checkedAddCall) ==
                 occurrences(o3.cpp, checkedAddCall) &&
             occurrences(o2.cpp, uncheckedStorageCall) ==
                 occurrences(o3.cpp, uncheckedStorageCall),
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
      {"overflow", " overflow extra arguments"}};
  std::vector<RunResult> baseline;
  for (const auto &[name, arguments] : scenarios) {
    baseline.push_back(run(binaries[0], directory, "o0-" + name, arguments));
  }
  expect(baseline[0].built && baseline[0].exitStatus == 0,
         "O0 success mode should preserve result and deterministic cleanup");
  expect(
      baseline[1].exitStatus != 0 &&
          baseline[1].stderrText.find("index_out_of_bounds in fixed_array") !=
              std::string::npos &&
          baseline[2].exitStatus != 0 &&
          baseline[2].stderrText.find(
              "accessed an uninitialized storage slot") != std::string::npos &&
          baseline[3].exitStatus != 0 &&
          baseline[3].stderrText.find("integer_overflow in addition") !=
              std::string::npos,
      "O0 near misses should expose the exact checked-operation or sealed "
      "storage-guard identity");

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
