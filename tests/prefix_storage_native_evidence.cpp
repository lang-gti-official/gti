// Source-defined prefix ownership evidence. Ordinary fixtures exercise the
// owner's full contract — append order,
// pop-last destruction, complete-prefix relocation, move transfer, reverse
// destruction at scope exit, and every runtime guard — through natively
// compiled generated programs across C++20/C++23, -O0/-O2/-O3, and
// dedicated AddressSanitizer/UndefinedBehaviorSanitizer builds.

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <array>
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
  return lang::Frontend().analyze(entry, "import std.memory;\n" + source,
                                  {stdlibRoot / "prelude.gti"}, {},
                                  {stdlibRoot});
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
  // Source-defined result classes use the ordinary payload-enum carrier.
  // Byte-bearing fields retain the same transport contract independently of
  // any public library name or obsolete private result representation.
  using NativeField = byte_field<NativeAddressRecord>;
  using Result = payload_variant<NativeField, NativeField, std::monostate>;
  Result success{std::in_place_index<0>, source};
  auto success_copy = success;
  auto success_move = std::move(success_copy);
  success = success_move;
  check(payload_get<0>(success).get());
  check(payload_get<0>(success_move).get());
  Result alternatives{std::in_place_index<0>, source};
  Result error{std::in_place_index<1>, source};
  std::swap(alternatives, error);
  if (alternatives.index() != 1 || error.index() != 0) std::abort();
  check(payload_get<1>(alternatives).get());
  check(payload_get<0>(error).get());
  alternatives = error;
  check(payload_get<0>(alternatives).get());
  error = Result{std::in_place_index<1>, source};
  check(payload_get<1>(error).get());
  using VoidResult = payload_variant<std::monostate, NativeField, std::monostate>;
  VoidResult void_success{std::in_place_index<0>};
  VoidResult void_error{std::in_place_index<1>, source};
  void_success = void_error;
  if (void_success.index() != 1) std::abort();
  check(payload_get<1>(void_success).get());
  int live = 3;
  using BorrowedResult = payload_variant<int*, NativeField, std::monostate>;
  BorrowedResult borrowed{std::in_place_index<0>, &live};
  BorrowedResult failed_borrow{std::in_place_index<1>, source};
  borrowed = failed_borrow;
  if (borrowed.index() != 1) std::abort();
  check(payload_get<1>(borrowed).get());
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
  // Sparse policy is source-defined. Its shifts now use the general typed
  // lifetime primitives, which must preserve the same inert pointer bytes.
  uninitialized_storage<NativeAddressRecord, true, true> slots[3];
  const auto address = [&](std::size_t index) {
    return reinterpret_cast<NativeAddressRecord *>(&slots[index]);
  };
  construct_representation_at(address(1), source);
  mir_object_relocate_v1(address(0), std::launder(address(1)));
  check(*std::launder(address(0)));
  mir_object_relocate_v1(address(1), std::launder(address(0)));
  check(*std::launder(address(1)));
  std::destroy_at(std::launder(address(1)));
  // Prefix insertion/erasure now compose these same exact-address lifetime
  // operations in GTI source. Check the former representation path's inert
  // pointer bytes through a whole shift/erase/transfer sequence, without
  // restoring a second native container implementation.
  construct_representation_at(address(0), source);
  mir_object_relocate_v1(address(1), std::launder(address(0)));
  construct_representation_at(address(0), source);
  check(*std::launder(address(0)));
  check(*std::launder(address(1)));
  std::destroy_at(std::launder(address(0)));
  mir_object_relocate_v1(address(0), std::launder(address(1)));
  mir_object_relocate_v1(address(2), std::launder(address(0)));
  check(*std::launder(address(2)));
  std::destroy_at(std::launder(address(2)));

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

  // Retirement empties the MIR slot even when a trivial representation can
  // remain alive until storage reuse. Const subobjects and a nontrivial copy
  // constructor must still observe the newly constructed object each time.
  struct Passive {
    const int id;
    explicit Passive(int value) : id(value) {}
    Passive(const Passive &other) : id(other.id + 1) {}
  };
  static_assert(std::is_trivially_destructible_v<Passive>);
  static_assert(!std::is_trivially_copyable_v<Passive>);
  mir_lifetime_slot<Passive> source_slot;
  mir_lifetime_slot<Passive> destination_slot;
  for (int i = 0; i != 128; ++i) {
    source_slot.construct(i);
    destination_slot.construct(std::move(source_slot.get()));
    source_slot.destroy();
    if (destination_slot.get().id != i + 1) std::abort();
    destination_slot.destroy();
  }
  int destructions = 0;
  struct Nontrivial {
    int *count;
    explicit Nontrivial(int *value, bool fail = false) : count(value) {
      if (fail) throw 2;
    }
    ~Nontrivial() { ++*count; }
  };
  static_assert(!std::is_trivially_destructible_v<Nontrivial>);
  {
    mir_lifetime_slot<Nontrivial, true> slot;
    slot.construct(&destructions);
    slot.destroy();
    if (destructions != 1) std::abort();
    try { slot.construct(&destructions, true); } catch (int) {}
    if (destructions != 1) std::abort();
    slot.construct(&destructions);
  }
  if (destructions != 2) std::abort();
  try {
    mir_lifetime_slot<Nontrivial, true> slot;
    slot.construct(&destructions);
    throw 1;
  } catch (int) {}
  if (destructions != 3) std::abort();
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
      const auto selected = standard == "c++20"
                                ? emitArtifact("behavior-20", behaviorSource,
                                               lang::CppStandard::Cpp20)
                                : behavior;
      if (selected.empty())
        continue;
      for (const std::string optimization : {"-O0", "-O2", "-O3"}) {
        const std::string label =
            "behavior-" + standard.substr(3) + optimization;
        const RunResult run = buildAndRun(compiler, selected, directory, label,
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

  // Ordinary source insert/erase composes checked general lifetime operations.
  // Emit the matching representation for each native language mode.
  const std::filesystem::path editing = emitArtifact("editing", editingSource);
  if (!editing.empty()) {
    for (const std::string standard : {"c++20", "c++23"}) {
      const auto selected = standard == "c++20"
                                ? emitArtifact("editing-20", editingSource,
                                               lang::CppStandard::Cpp20)
                                : editing;
      if (selected.empty())
        continue;
      for (const std::string optimization : {"-O0", "-O3"}) {
        const std::string label =
            "editing-" + standard.substr(3) + optimization;
        const RunResult run = buildAndRun(compiler, selected, directory, label,
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

  // Physical trivial objects may outlive retirement; logical slot guards
  // must continue rejecting access, double retirement and live-slot reuse.
  for (const auto &[name, operation, fragment] :
       {std::array<std::string_view, 3>{
            "retired-read", "slot.construct(7); slot.destroy(); slot.get();",
            "verified MIR lifetime slot was read while empty"},
        {"retired-drop", "slot.construct(7); slot.destroy(); slot.destroy();",
         "verified MIR lifetime slot was dropped while empty"},
        {"live-reuse", "slot.construct(7); slot.construct(8);",
         "verified MIR lifetime slot was constructed twice"},
        {"live-escape", "slot.construct(7);",
         "verified MIR lifetime slot escaped without Drop"}}) {
    const auto artifact =
        emitArtifact(std::string(name), "int main() { return 0; }");
    if (artifact.empty())
      continue;
    std::ofstream(artifact, std::ios::app)
        << "\nnamespace { const int retirement_guard = [] { "
           "::gti_internal::backend::mir_lifetime_slot<int> slot; "
        << operation << " return 0; }(); }\n";
    const auto run = buildAndRun(compiler, artifact, directory,
                                 std::string(name), "c++23", "-O3", "");
    expect(run.built && run.exitStatus != 0 &&
               run.stderrText.find(fragment) != std::string::npos,
           "retirement must preserve the slot guard: " + std::string(name));
  }

  // Representation-only scope retirement still runs its destructor. A
  // semantic slot instead retires only through its explicit Drop operation.
  for (const bool emergency : {false, true}) {
    const std::string name = emergency ? "slot-emergency" : "slot-retirement";
    const auto artifact = emitArtifact(name, "int main() { return 0; }");
    if (artifact.empty())
      continue;
    if (emergency) {
      // Supply the private handoff state to the generated helper in this
      // native probe; managed-worker runtime tests establish its real producer.
      std::string text = readFile(artifact);
      const std::string original =
          "inline bool mir_cleanup_emergency_abandonment() noexcept {\n"
          "  return false;\n}";
      const auto position = text.find(original);
      expect(position != std::string::npos,
             "native slot probe finds the single-thread emergency provider");
      if (position == std::string::npos)
        continue;
      text.replace(position, original.size(),
                   "inline bool mir_cleanup_emergency_abandonment() noexcept { "
                   "return true; }");
      std::ofstream(artifact) << text;
    }
    std::ofstream(artifact, std::ios::app)
        << "\nnamespace { int retired = 0; struct SlotTracked { "
           "~SlotTracked() { ++retired; } }; const int slot_probe = [] { "
           "{ ::gti_internal::backend::mir_lifetime_slot<SlotTracked, true> "
           "slot; slot.construct(); } "
           "{ ::gti_internal::backend::mir_lifetime_slot<SlotTracked> slot; "
           "slot.construct(); "
        << (emergency ? "" : "slot.destroy(); ")
        << "} if (retired != " << (emergency ? 0 : 2)
        << ") std::abort(); return 0; }(); }\n";
    for (const std::string standard : {"c++20", "c++23"}) {
      const auto run = buildAndRun(compiler, artifact, directory,
                                   name + standard, standard, "-O3", "");
      expect(run.built && run.exitStatus == 0,
             "slot outlining preserves retirement/emergency behavior: " + name);
    }
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
