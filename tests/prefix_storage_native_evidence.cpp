#include "temporary_directory.h"
// Source-defined prefix ownership evidence. Ordinary fixtures exercise the
// owner's full contract — append order,
// pop-last destruction, complete-prefix relocation, move transfer, reverse
// destruction at scope exit, and every runtime guard — through natively
// linked LLVM objects across codegen levels and an AddressSanitizer/
// undefined-memory instrumented build. C++ runtime-helper representation
// probes live in cpp_mir_prefix_storage_native_evidence.

#include "gti/frontend.h"
#include "gti/llvm_backend.h"

#include "llvm_backend_test_support.h"
#include "private_stdlib_fixture.h"

#include <array>
#include <cstdio>
#include <cstdlib>
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

std::string environmentPath(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

lang::FrontendResult analyzeTrusted(const std::string &name,
                                    const std::string &source) {
  const std::filesystem::path stdlibRoot = environmentPath("GTI_STDLIB_PATH");
  return gti_test::analyzePrivateMemoryFixture(name, source, {}, stdlibRoot);
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

struct RunResult {
  bool built = false;
  int exitStatus = -1;
  std::string stderrText;
};

RunResult buildAndRun(const std::string &compiler,
                      const std::filesystem::path &artifact,
                      const std::filesystem::path &directory,
                      const std::string &label, const std::string &extraFlags) {
  RunResult result;
  const std::filesystem::path binary = directory / ("bin-" + label);
  const std::filesystem::path linkLog = directory / (label + "-link.log");
  const std::filesystem::path runLog = directory / (label + "-run.log");
  std::string command = compiler;
  if (!extraFlags.empty()) {
    command += " " + extraFlags;
  }
  command += " \"" + artifact.string() + "\"";
  const std::string runtimeLibrary = environmentPath("GTI_RUNTIME_LIBRARY");
  if (!runtimeLibrary.empty()) {
    command += " \"" + runtimeLibrary + "\"";
  }
  command += " -o \"" + binary.string() + "\" 2>\"" + linkLog.string() + "\"";
  if (std::system(command.c_str()) != 0) {
    std::cerr << "native link failed (" << label << "):\n"
              << readFile(linkLog) << '\n';
    return result;
  }
  result.built = true;
  const std::string run =
      "\"" + binary.string() + "\" >/dev/null 2>\"" + runLog.string() + "\"";
  result.exitStatus = std::system(run.c_str());
  result.stderrText = readFile(runLog);
  return result;
}

constexpr std::string_view behaviorSource = R"(
mut int order = 0;

class Tracker {
public:
  mut int32_t id;

  Tracker(int32_t value) : id(value) {}
  ~Tracker() { order = order * 8 + this.id; }
};

int main() {
  mut int checks = 0;
  {
    mut std::detail::prefix_owner<Tracker> values =
        std::detail::prefix_owner<Tracker>(uint64_t(3));
    values.append(1);
    values.append(2);
    values.append(3);
    if (values.length() == uint64_t(3)) {
      checks = checks + 1;
    }
    if (values.read(uint64_t(0)).id == 1) {
      checks = checks + 1;
    }
    mut Tracker& second =
        values.read_mut(uint64_t(1));
    second.id = 5;
    if (values.read(uint64_t(1)).id == 5) {
      checks = checks + 1;
    }
    values.pop();
    if (values.length() == uint64_t(2)) {
      checks = checks + 1;
    }
    mut std::detail::prefix_owner<Tracker> moved =
        std::detail::prefix_owner<Tracker>(uint64_t(4));
    values.relocate_to(moved);
    if (values.length() == uint64_t(0)) {
      checks = checks + 1;
    }
    if (moved.length() == uint64_t(2)) {
      checks = checks + 1;
    }
    mut std::detail::prefix_owner<Tracker> taken = std::move(moved);
    if (taken.length() == uint64_t(2)) {
      checks = checks + 1;
    }
  }
  // Destructor order: pop fires the user destructor for 3; relocation
  // move-constructs the destination and destroys the moved-from source
  // shells with the lifecycle flag cleared, so their user destructor
  // bodies correctly do NOT run; scope exit destroys the taken prefix in
  // reverse (the mutated 5, then 1). order = (3*8+5)*8+1 = 233.
  if (checks != 7) {
    return 1;
  }
  if (order != 233) {
    return 2;
  }
  return 0;
}
)";

constexpr std::string_view editingSource = R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append(1);
  values.append(2);
  values.append(4);
  values.insert(uint64_t(2), 3);
  if (values.length() != uint64_t(4)) {
    return 1;
  }
  if (values.read(uint64_t(0)) != 1 or
      values.read(uint64_t(1)) != 2 or
      values.read(uint64_t(2)) != 3 or
      values.read(uint64_t(3)) != 4) {
    return 2;
  }
  values.erase(uint64_t(0));
  if (values.length() != uint64_t(3)) {
    return 3;
  }
  if (values.read(uint64_t(0)) != 2 or
      values.read(uint64_t(2)) != 4) {
    return 4;
  }
  values.insert(uint64_t(3), 9);
  if (values.read(uint64_t(3)) != 9) {
    return 5;
  }
  return 0;
}
)";

struct GuardScenario {
  std::string_view name;
  std::string_view source;
  std::string_view fragment;
};

constexpr GuardScenario guards[] = {
    {"append-capacity",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(1));
  values.append(1);
  values.append(2);
  return 0;
}
)",
     "invalid_storage_state in relocation_capacity"},
    {"pop-empty",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(1));
  values.pop();
  return 0;
}
)",
     "invalid_storage_state in uninitialized_access"},
    {"read-prefix",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(2));
  values.append(1);
  return int(values.read(uint64_t(1)));
}
)",
     "index_out_of_bounds in private_storage"},
    {"relocate-occupied",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> source =
      std::detail::prefix_owner<int32_t>(uint64_t(2));
  source.append(1);
  mut std::detail::prefix_owner<int32_t> destination =
      std::detail::prefix_owner<int32_t>(uint64_t(2));
  destination.append(9);
  source.relocate_to(destination);
  return 0;
}
)",
     "invalid_storage_state in occupied_relocation_destination"},
    {"insert-outside",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append(1);
  values.insert(uint64_t(2), 9);
  return 0;
}
)",
     "index_out_of_bounds in private_storage"},
    {"insert-capacity",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(1));
  values.append(1);
  values.insert(uint64_t(0), 9);
  return 0;
}
)",
     "invalid_storage_state in relocation_capacity"},
    {"erase-outside",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(2));
  values.append(1);
  values.erase(uint64_t(1));
  return 0;
}
)",
     "index_out_of_bounds in private_storage"},
    {"relocate-capacity",
     R"(
int main() {
  mut std::detail::prefix_owner<int32_t> source =
      std::detail::prefix_owner<int32_t>(uint64_t(2));
  source.append(1);
  source.append(2);
  mut std::detail::prefix_owner<int32_t> destination =
      std::detail::prefix_owner<int32_t>(uint64_t(1));
  source.relocate_to(destination);
  return 0;
}
)",
     "invalid_storage_state in relocation_capacity"},
};

} // namespace

int main(int argc, char **argv) {
  const std::string compiler = argc > 1 ? argv[1] : "c++";
  std::error_code error;
  const std::filesystem::path directory =
      gti_test::temporaryDirectory() / "gti-prefix-evidence";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

  // The fixtures lower their unoptimized MIR, so every native difference
  // comes from LLVM code generation or instrumentation.
  const auto emitArtifact =
      [&](const std::string &name, std::string_view source,
          lang::LlvmBackendOptions options = {}) -> std::filesystem::path {
    const lang::FrontendResult frontend =
        analyzeTrusted(name, std::string(source));
    if (!frontend.canGenerateCode()) {
      for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
        std::cerr << "diagnostic (" << name << "): " << diagnostic.message
                  << '\n';
      }
      expect(false, "evidence fixture should pass the frontend: " + name);
      return {};
    }
    lang::BackendArtifact artifact;
    try {
      artifact = lang::LlvmBackend(options).generate(
          gti_test::lowerProgram(frontend, frontend.mir, frontend.mir));
    } catch (const std::exception &failure) {
      expect(false, "LLVM backend should emit evidence fixture " + name + ": " +
                        failure.what());
      return {};
    }
    const std::filesystem::path path = directory / (name + ".o");
    std::ofstream(path, std::ios::binary) << artifact.contents;
    return path;
  };
  const auto codegen = [](lang::LlvmCodegenOptimization optimization) {
    return lang::LlvmBackendOptions{.optimization = optimization};
  };
  const auto sanitizedOptions = lang::LlvmBackendOptions{
      .optimization = lang::LlvmCodegenOptimization::Less,
      .instrumentation =
          lang::LlvmInstrumentationMode::AddressAndUndefinedMemory};
  // Instrumented objects need the sanitizer runtime of the LLVM that
  // instrumented them; the host C++ compiler's runtime may differ in ABI.
#ifdef GTI_TEST_LLVM_CLANGXX
  const std::string sanitizerLinker =
      std::filesystem::exists(GTI_TEST_LLVM_CLANGXX) ? GTI_TEST_LLVM_CLANGXX
                                                     : compiler;
#else
  const std::string sanitizerLinker = compiler;
#endif
  const std::string sanitizerFlags =
      "-fsanitize=address,undefined -fno-sanitize-recover=all";

  // The behavior fixture across the native optimization matrix: every
  // combination must run the identical contract to success.
  for (const auto &[label, optimization] :
       {std::pair<std::string_view, lang::LlvmCodegenOptimization>{
            "O0", lang::LlvmCodegenOptimization::None},
        {"O2", lang::LlvmCodegenOptimization::Default},
        {"O3", lang::LlvmCodegenOptimization::Aggressive}}) {
    const std::string name = "behavior-" + std::string(label);
    const auto artifact =
        emitArtifact(name, behaviorSource, codegen(optimization));
    if (artifact.empty())
      continue;
    const RunResult run = buildAndRun(compiler, artifact, directory, name, "");
    expect(run.built && run.exitStatus == 0,
           "the behavior fixture must pass at " + std::string(label));
  }
  // Dedicated generated-program sanitizer evidence: the success path must
  // be clean under AddressSanitizer and undefined-memory instrumentation.
  if (const auto behavior =
          emitArtifact("behavior-sanitized", behaviorSource, sanitizedOptions);
      !behavior.empty()) {
    const RunResult sanitized =
        buildAndRun(sanitizerLinker, behavior, directory, "behavior-sanitized",
                    sanitizerFlags);
    expect(sanitized.built && sanitized.exitStatus == 0 &&
               sanitized.stderrText.find("ERROR") == std::string::npos &&
               sanitized.stderrText.find("runtime error") == std::string::npos,
           "the behavior fixture must run clean under ASan/UBSan");
  }

  // Ordinary source insert/erase composes checked general lifetime operations.
  for (const auto &[label, optimization] :
       {std::pair<std::string_view, lang::LlvmCodegenOptimization>{
            "O0", lang::LlvmCodegenOptimization::None},
        {"O3", lang::LlvmCodegenOptimization::Aggressive}}) {
    const std::string name = "editing-" + std::string(label);
    const auto artifact =
        emitArtifact(name, editingSource, codegen(optimization));
    if (artifact.empty())
      continue;
    const RunResult run = buildAndRun(compiler, artifact, directory, name, "");
    expect(run.built && run.exitStatus == 0,
           "the editing fixture must pass at " + std::string(label));
  }
  if (const auto editing =
          emitArtifact("editing-sanitized", editingSource, sanitizedOptions);
      !editing.empty()) {
    const RunResult sanitized =
        buildAndRun(sanitizerLinker, editing, directory, "editing-sanitized",
                    sanitizerFlags);
    expect(sanitized.built && sanitized.exitStatus == 0,
           "the editing fixture must run clean under ASan/UBSan");
  }

  // Every misuse scenario trips exactly the surface that reaches it first:
  // MIR-emitted source guards select the same defined-failure outcomes as the
  // surviving general lifetime operations.
  for (const GuardScenario &guard : guards) {
    const std::filesystem::path artifact =
        emitArtifact(std::string(guard.name), guard.source);
    if (artifact.empty()) {
      continue;
    }
    const RunResult run = buildAndRun(compiler, artifact, directory,
                                      "guard-" + std::string(guard.name), "");
    expect(run.built && run.exitStatus != 0 &&
               run.stderrText.find(guard.fragment) != std::string::npos,
           "guard scenario must trip its exact diagnostic: " +
               std::string(guard.name));
  }

  if (failures != 0) {
    std::cerr << failures << " prefix-storage evidence failure(s)\n";
    return 1;
  }
  std::cout << "prefix storage native evidence passed\n";
  return 0;
}
