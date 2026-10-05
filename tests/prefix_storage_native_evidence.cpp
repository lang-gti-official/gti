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

// Representation-only probe: exercise the helpers directly, independently of
// source call/return lowering. Reading a stored pointer's bytes never evaluates
// its expired pointer value. The full language/native-ABI tests own transport.
constexpr std::string_view byteCopyProbe = R"(
namespace {
struct alignas(64) NativeAddressRecord { int *address; int marker; };
std::true_type __gti_byte_transport_tag(NativeAddressRecord *);
static_assert(std::is_trivially_copyable_v<NativeAddressRecord>);

const int byte_copy_probe = [] {
  using namespace gti_internal::backend;
  int *allocation = new int(42);
  const std::uintptr_t expected = reinterpret_cast<std::uintptr_t>(allocation);
  NativeAddressRecord source{allocation, 17};
  delete allocation;
  raw_address<int> captured(source.address);
  raw_address<const int> qualified(source.address);
  raw_address<const void> erased(source.address);
  if (captured != qualified || raw_address<const void>(captured) != erased ||
      captured == nullptr) std::abort();
  const auto check = [&](const NativeAddressRecord &value) {
    std::uintptr_t bits = 0;
    std::memcpy(&bits, &value.address, sizeof(bits));
    if (bits != expected || value.marker != 17) std::abort();
  };
  std::array<NativeAddressRecord, 2> fixed{};
  if (mir_checked_array_write_v1(fixed, 0, source).code != 0) std::abort();
  if (mir_checked_array_write_v1(fixed, 1, source).code != 0) std::abort();
  NativeAddressRecord observed{};
  if (mir_checked_array_read_v1(fixed, 1, &observed).code != 0) std::abort();
  check(observed);
  if (mir_checked_array_write_v1(fixed, 1, fixed[1],
                                mir_array_copy_assignment_v1).code != 0)
    std::abort();
  check(fixed[1]);
  if (mir_checked_array_write_v1(fixed, 2, source).code == 0) std::abort();
  check(fixed[1]);
  std::array<int *, 1> native_slots{};
  if (mir_checked_array_write_v1(native_slots, 0, captured).code != 0)
    std::abort();
  raw_address<const int> loaded;
  if (mir_checked_array_read_v1(native_slots, 0, &loaded).code != 0 ||
      loaded != qualified) std::abort();
  assign_representation(native_slots[0], native_slots[0]);
  NativeAddressRecord replacement{};
  mir_restoring_swap_v1(observed, replacement);
  check(replacement);
  mir_restoring_swap_v1(replacement, replacement);
  check(replacement);
  mir_lifetime_slot<NativeAddressRecord, true> exchanged;
  mir_restoring_exchange_into_v1(replacement, std::move(observed),
                                  exchanged.construction_address());
  exchanged.mark_constructed();
  check(exchanged.get());
  mir_lifetime_slot<NativeAddressRecord, true> temporary;
  temporary.construct(source);
  check(temporary.get());
  expected_result<NativeAddressRecord, int> success{source};
  auto success_copy = success;
  auto success_move = std::move(success_copy);
  success = success_move;
  check(success.value());
  check(success_move.value());
  expected_result<NativeAddressRecord, NativeAddressRecord> alternatives{source};
  expected_result<NativeAddressRecord, NativeAddressRecord> error{
      make_unexpected_result(source)};
  alternatives.swap(error);
  if (alternatives.has_value() || !error.has_value()) std::abort();
  check(alternatives.error());
  check(error.value());
  alternatives = error;
  check(alternatives.value());
  error = make_unexpected_result(source);
  check(error.error());
  expected_result<void, NativeAddressRecord> void_success;
  expected_result<void, NativeAddressRecord> void_error{
      make_unexpected_result(source)};
  void_success = void_error;
  if (void_success.has_value()) std::abort();
  check(void_success.error());
  int live = 3;
  borrowed_expected<int, NativeAddressRecord> borrowed(live);
  borrowed_expected<int, NativeAddressRecord> failed_borrow{
      make_unexpected_result(source)};
  borrowed = failed_borrow;
  if (borrowed.has_value()) std::abort();
  check(borrowed.error());
  byte_field<std::array<NativeAddressRecord, 2>> field{fixed};
  auto field_copy = field;
  field = std::move(field_copy);
  check(field.get()[0]);
  check(field.get()[1]);
  static_assert(std::is_same_v<decltype(temporary.take()), NativeAddressRecord &&>);
  mir_lifetime_slot<NativeAddressRecord, true> transferred;
  transferred.construct(temporary.take());
  check(transferred.get());
  temporary.construct(source);
  check(temporary.get());
  optional_storage<NativeAddressRecord> optional;
  optional.construct(source);
  check(optional.read());
  optional_storage<NativeAddressRecord> moved_optional(std::move(optional));
  check(moved_optional.read());
  storage<NativeAddressRecord> sparse;
  if (storage<NativeAddressRecord>::try_allocate(3, 64, &sparse) != 0)
    std::abort();
  sparse.construct(1, source);
  sparse.shift_left(0, 2);
  check(sparse.read(0));
  sparse.shift_right(0, 1);
  check(sparse.read(1));
  prefix_storage<NativeAddressRecord> prefix;
  if (!prefix_storage<NativeAddressRecord>::try_allocate(3, &prefix))
    std::abort();
  prefix.append(source);
  prefix.insert_at(0, source);
  prefix.erase_at(0);
  check(prefix.read(0));
  prefix_storage<NativeAddressRecord> destination;
  if (!prefix_storage<NativeAddressRecord>::try_allocate(3, &destination))
    std::abort();
  prefix.relocate_to(destination);
  check(destination.read(0));

  // The primitive must not bypass a selected nontrivial copy constructor.
  struct Tracked {
    int copies;
    Tracked() : copies(0) {}
    Tracked(const Tracked &other) : copies(other.copies + 1) {}
  };
  const Tracked original;
  mir_lifetime_slot<Tracked, true> copied;
  copied.construct(original);
  if (copied.get().copies != 1) std::abort();
  return 0;
}();
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
     "index_out_of_bounds in private_storage"},
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
      [&](const std::string &name, std::string_view source,
          lang::CppStandard standard =
              lang::CppStandard::Cpp23) -> std::filesystem::path {
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
    const lang::BackendArtifact artifact = gti_test::emitCpp(
        frontend, frontend.mir, frontend.mir, optimizations, standard);
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

  // Complete-object construction must not evaluate expired native pointer
  // subobjects, even when native copy elision is disabled.
  for (const std::string standard : {"c++20", "c++23"}) {
    const std::filesystem::path byteCopy = emitArtifact(
        "byte-copy-" + standard.substr(3),
        "int main() { expected<int, int> value = 0; return value.value(); }",
        standard == "c++20" ? lang::CppStandard::Cpp20
                            : lang::CppStandard::Cpp23);
    if (byteCopy.empty())
      continue;
    std::ofstream(byteCopy, std::ios::app) << byteCopyProbe;
    for (const std::string optimization : {"-O0", "-O3"}) {
      const RunResult run =
          buildAndRun(compiler, byteCopy, directory,
                      "byte-copy-" + standard.substr(3) + optimization,
                      standard, optimization, "-fno-elide-constructors");
      expect(run.built && run.exitStatus == 0,
             "byte-preserving storage transport must pass at " + standard +
                 " " + optimization);
    }
    if (standard == "c++23") {
      const RunResult run = buildAndRun(
          compiler, byteCopy, directory, "byte-copy-sanitized", "c++23", "-O1",
          "-fsanitize=address,undefined -fno-sanitize-recover=all");
      expect(
          run.built && run.exitStatus == 0,
          "byte-preserving storage transport must run clean under ASan/UBSan");
    }
  }

  // Every misuse scenario trips exactly the surface that reaches it first:
  // MIR-emitted mains route storage misuse through defined failure; sealed
  // runtime guards remain defense in depth behind those defined checks.
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
