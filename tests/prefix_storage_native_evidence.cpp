// P-STORAGE-01 slice 3: the prefix-initialized storage evidence battery.
// Trusted fixtures exercise the capability's full contract — append order,
// pop-last destruction, complete-prefix relocation, move transfer, reverse
// destruction at scope exit, and every runtime guard — through natively
// compiled generated programs across C++20/C++23, -O0/-O2/-O3, and
// dedicated AddressSanitizer/UndefinedBehaviorSanitizer builds.

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

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
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / ("gti-prefix-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend().analyze(entry, source,
                                  {stdlibRoot / "prelude.gti", entry},
                                  {{entryKey, source}}, {stdlibRoot});
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
                      const std::string &label, const std::string &standard,
                      const std::string &optimization,
                      const std::string &extraFlags) {
  RunResult result;
  const std::filesystem::path binary = directory / ("bin-" + label);
  const std::filesystem::path compileLog = directory / (label + "-cc.log");
  const std::filesystem::path runLog = directory / (label + "-run.log");
  // The strict floating-point policy trio mirrors the CLI's native
  // compile command; the artifact's opt-in marker requires it.
  std::string command = compiler + " -std=" + standard + " " + optimization +
                        " -fno-fast-math -ffp-contract=off"
                        " -D__gti_strict_ieee754=1";
  if (!extraFlags.empty()) {
    command += " " + extraFlags;
  }
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
    mut gti_internal::prefix_storage<Tracker> values =
        gti_internal::allocate_prefix_storage<Tracker>(uint64_t(3));
    gti_internal::prefix_storage_append(values, 1);
    gti_internal::prefix_storage_append(values, 2);
    gti_internal::prefix_storage_append(values, 3);
    if (gti_internal::prefix_storage_length(values) == uint64_t(3)) {
      checks = checks + 1;
    }
    if (gti_internal::prefix_storage_read(values, uint64_t(0)).id == 1) {
      checks = checks + 1;
    }
    mut Tracker& second =
        gti_internal::prefix_storage_read_mut(values, uint64_t(1));
    second.id = 5;
    if (gti_internal::prefix_storage_read(values, uint64_t(1)).id == 5) {
      checks = checks + 1;
    }
    gti_internal::prefix_storage_pop(values);
    if (gti_internal::prefix_storage_length(values) == uint64_t(2)) {
      checks = checks + 1;
    }
    mut gti_internal::prefix_storage<Tracker> moved =
        gti_internal::allocate_prefix_storage<Tracker>(uint64_t(4));
    gti_internal::prefix_storage_relocate(values, moved);
    if (gti_internal::prefix_storage_length(values) == uint64_t(0)) {
      checks = checks + 1;
    }
    if (gti_internal::prefix_storage_length(moved) == uint64_t(2)) {
      checks = checks + 1;
    }
    mut gti_internal::prefix_storage<Tracker> taken = std::move(moved);
    if (gti_internal::prefix_storage_length(taken) == uint64_t(2)) {
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
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, 1);
  gti_internal::prefix_storage_append(values, 2);
  gti_internal::prefix_storage_append(values, 4);
  gti_internal::prefix_storage_insert(values, uint64_t(2), 3);
  if (gti_internal::prefix_storage_length(values) != uint64_t(4)) {
    return 1;
  }
  if (gti_internal::prefix_storage_read(values, uint64_t(0)) != 1 or
      gti_internal::prefix_storage_read(values, uint64_t(1)) != 2 or
      gti_internal::prefix_storage_read(values, uint64_t(2)) != 3 or
      gti_internal::prefix_storage_read(values, uint64_t(3)) != 4) {
    return 2;
  }
  gti_internal::prefix_storage_erase(values, uint64_t(0));
  if (gti_internal::prefix_storage_length(values) != uint64_t(3)) {
    return 3;
  }
  if (gti_internal::prefix_storage_read(values, uint64_t(0)) != 2 or
      gti_internal::prefix_storage_read(values, uint64_t(2)) != 4) {
    return 4;
  }
  gti_internal::prefix_storage_insert(values, uint64_t(3), 9);
  if (gti_internal::prefix_storage_read(values, uint64_t(3)) != 9) {
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
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(1));
  gti_internal::prefix_storage_append(values, 1);
  gti_internal::prefix_storage_append(values, 2);
  return 0;
}
)",
     "invalid_storage_state in relocation_capacity"},
    {"pop-empty",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(1));
  gti_internal::prefix_storage_pop(values);
  return 0;
}
)",
     "invalid_storage_state in uninitialized_access"},
    {"read-prefix",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(2));
  gti_internal::prefix_storage_append(values, 1);
  return int(gti_internal::prefix_storage_read(values, uint64_t(1)));
}
)",
     "prefix storage index out of the live prefix"},
    {"relocate-occupied",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> source =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(2));
  gti_internal::prefix_storage_append(source, 1);
  mut gti_internal::prefix_storage<int32_t> destination =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(2));
  gti_internal::prefix_storage_append(destination, 9);
  gti_internal::prefix_storage_relocate(source, destination);
  return 0;
}
)",
     "invalid_storage_state in occupied_relocation_destination"},
    {"insert-outside",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, 1);
  gti_internal::prefix_storage_insert(values, uint64_t(2), 9);
  return 0;
}
)",
     "index_out_of_bounds in private_storage"},
    {"insert-capacity",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(1));
  gti_internal::prefix_storage_append(values, 1);
  gti_internal::prefix_storage_insert(values, uint64_t(0), 9);
  return 0;
}
)",
     "invalid_storage_state in relocation_capacity"},
    {"erase-outside",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(2));
  gti_internal::prefix_storage_append(values, 1);
  gti_internal::prefix_storage_erase(values, uint64_t(1));
  return 0;
}
)",
     "index_out_of_bounds in private_storage"},
    {"relocate-capacity",
     R"(
int main() {
  mut gti_internal::prefix_storage<int32_t> source =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(2));
  gti_internal::prefix_storage_append(source, 1);
  gti_internal::prefix_storage_append(source, 2);
  mut gti_internal::prefix_storage<int32_t> destination =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(1));
  gti_internal::prefix_storage_relocate(source, destination);
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
      std::filesystem::temp_directory_path() / "gti-prefix-evidence";
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

  const auto emitArtifact =
      [&](const std::string &name,
          std::string_view source) -> std::filesystem::path {
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
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(frontend.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
    const std::filesystem::path path = directory / (name + ".cpp");
    std::ofstream(path) << artifact.contents;
    return path;
  };

  // The behavior fixture across the full native standard/optimization
  // matrix: every combination must run the identical contract to success.
  const std::filesystem::path behavior =
      emitArtifact("behavior", behaviorSource);
  if (!behavior.empty()) {
    for (const std::string standard : {"c++20", "c++23"}) {
      for (const std::string optimization : {"-O0", "-O2", "-O3"}) {
        const std::string label =
            "behavior-" + standard.substr(3) + optimization;
        const RunResult run = buildAndRun(compiler, behavior, directory, label,
                                          standard, optimization, "");
        expect(run.built && run.exitStatus == 0,
               "the behavior fixture must pass at " + standard + " " +
                   optimization);
      }
    }
    // Dedicated generated-program sanitizer evidence: the success path must
    // be clean under AddressSanitizer and UndefinedBehaviorSanitizer.
    const RunResult sanitized = buildAndRun(
        compiler, behavior, directory, "behavior-sanitized", "c++23", "-O1",
        "-fsanitize=address,undefined -fno-sanitize-recover=all");
    expect(sanitized.built && sanitized.exitStatus == 0 &&
               sanitized.stderrText.find("ERROR") == std::string::npos &&
               sanitized.stderrText.find("runtime error") == std::string::npos,
           "the behavior fixture must run clean under ASan/UBSan");
  }

  // The sealed insert/erase primitives preserve the prefix invariant
  // atomically across the same native matrix.
  const std::filesystem::path editing = emitArtifact("editing", editingSource);
  if (!editing.empty()) {
    for (const std::string standard : {"c++20", "c++23"}) {
      for (const std::string optimization : {"-O0", "-O3"}) {
        const std::string label =
            "editing-" + standard.substr(3) + optimization;
        const RunResult run = buildAndRun(compiler, editing, directory, label,
                                          standard, optimization, "");
        expect(run.built && run.exitStatus == 0,
               "the editing fixture must pass at " + standard + " " +
                   optimization);
      }
    }
    const RunResult sanitized = buildAndRun(
        compiler, editing, directory, "editing-sanitized", "c++23", "-O1",
        "-fsanitize=address,undefined -fno-sanitize-recover=all");
    expect(sanitized.built && sanitized.exitStatus == 0,
           "the editing fixture must run clean under ASan/UBSan");
  }

  // Every misuse scenario trips exactly the surface that reaches it
  // first: MIR-emitted mains route storage misuse through the defined
  // failure contract (GTI-R0007/GTI-R0010 records, exit 70), while a
  // scenario still reaching the sealed runtime guard pins that guard's
  // exact diagnostic — the guards remain defense in depth behind the
  // defined checks.
  for (const GuardScenario &guard : guards) {
    const std::filesystem::path artifact =
        emitArtifact(std::string(guard.name), guard.source);
    if (artifact.empty()) {
      continue;
    }
    const RunResult run =
        buildAndRun(compiler, artifact, directory,
                    "guard-" + std::string(guard.name), "c++23", "-O0", "");
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
