#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/native_header.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
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

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

void testCAndCppHeaderSurface() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-header.gti", R"(
using NativeInt = int32_t;

[[c_opaque]] struct RootHandle;

[[c_abi]]
struct RootRecord {
  mut NativeInt value;
  mut uint8_t bytes[4];
};

namespace graphics {
[[c_opaque]] struct Context;

[[c_abi]]
struct Pair {
  mut RootRecord root;
  mut const uint8_t* bytes;
  mut Context* context;
};

extern "C" {
  Context* cpp_context_create();
  void cpp_context_destroy(Context* context);
  Pair cpp_pair_scale(Pair value, float scale);
}

}

extern "C" {
  RootHandle* c_root_open(int32_t initial);
  void c_root_close(RootHandle* handle);
  RootRecord c_root_roundtrip(RootRecord value);
  int32_t c_no_arguments();
}

int main() { return 0; }
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the native-header fixture should complete frontend analysis");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, lang::TargetInfo::host());
  lang::NativeHeaderBackend headerBackend;
  const lang::BackendArtifact first = headerBackend.generate(lowered);
  const lang::BackendArtifact second = headerBackend.generate(lowered);
  const std::string &header = first.contents;

  expect(first.extension == ".h" &&
             first.kind == lang::BackendArtifactKind::Source &&
             first.contents == second.contents,
         "native-header generation should be a deterministic source artifact");
  expect(
      header.find("#ifdef __cplusplus") != std::string::npos &&
          header.find("namespace graphics {") != std::string::npos &&
          header.find("struct RootHandle;") != std::string::npos &&
          header.find("struct Context;") != std::string::npos &&
          header.find("struct Pair {") != std::string::npos &&
          header.find("std::uint8_t bytes[4];") != std::string::npos &&
          header.find("uint8_t bytes[4];") != std::string::npos &&
          header.find("::graphics::Context* cpp_context_create(") !=
              std::string::npos &&
          header.find("::graphics::Pair cpp_pair_scale(") !=
              std::string::npos &&
          header.find("extern \"C\"") != std::string::npos &&
          header.find("using ::__gti_program::graphics::cpp_context_create") ==
              std::string::npos,
      "the C++ branch should preserve source namespaces, exact record "
      "identity, directly declared public C linkage, and portable "
      "definition lookup");
  expect(header.find("typedef struct RootRecord RootRecord;") !=
                 std::string::npos &&
             header.find("typedef struct RootHandle RootHandle;") !=
                 std::string::npos &&
             header.find("graphics::Context (opaque handle)") !=
                 std::string::npos &&
             header.find("/* graphics::Pair */") != std::string::npos &&
             header.find("c_root_roundtrip(RootRecord value)") !=
                 std::string::npos &&
             header.find("c_no_arguments(void)") != std::string::npos &&
             header.find("_Static_assert(offsetof(") != std::string::npos,
         "the C branch should expose readable root records, deterministic "
         "namespaced records, strict empty parameter lists, and layout "
         "assertions");

  const lang::BackendArtifact cpp = lang::CppBackend().generate(lowered);
  const std::size_t record = cpp.contents.find("struct RootRecord {");
  const std::size_t recordEnd = cpp.contents.find("};", record);
  const std::string recordBody =
      record == std::string::npos || recordEnd == std::string::npos
          ? std::string{}
          : cpp.contents.substr(record, recordEnd - record);
  expect(recordBody.find("std::int32_t value;") != std::string::npos &&
             recordBody.find("std::uint8_t bytes[4];") != std::string::npos &&
             recordBody.find("std::array<std::uint8_t, 4>") ==
                 std::string::npos &&
             recordBody.find("RootRecord()") == std::string::npos &&
             recordBody.find("NativeInt") == std::string::npos,
         "emitted native records should use the same canonical, policy-free "
         "C++ definition as the generated header");
  expect(cpp.contents.find("struct RootHandle;") != std::string::npos &&
             cpp.contents.find("struct RootHandle {") == std::string::npos &&
             cpp.contents.find("struct Context;") != std::string::npos &&
             cpp.contents.find("struct Context {") == std::string::npos,
         "opaque native handles should remain forward declarations in the "
         "generated GTI C++ translation unit");
}

void testCStringBoundarySurface() {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "native-c-string.gti", R"(
extern "C" {
  c_string native_name();
  int32_t native_name_length(c_string value);
  void native_replace_name(c_string* value);
}

int main() {
  mut c_string value = nullptr;
  value = native_name();
  if (value == nullptr || native_name_length("gti") != 3) {
    return 1;
  }
  unsafe { native_replace_name(&value); }
  return value == nullptr ? 2 : 0;
}
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "c_string should cross extern C by value and through one mutable "
         "out-pointer");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, lang::TargetInfo::host());
  const std::string header =
      lang::NativeHeaderBackend().generate(lowered).contents;
  const std::string cpp = lang::CppBackend().generate(lowered).contents;
  expect(header.find("const char* native_name(void)") != std::string::npos &&
             header.find("native_name_length(const char* value)") !=
                 std::string::npos &&
             header.find("native_replace_name(const char** value)") !=
                 std::string::npos,
         "native headers should spell c_string as const char* in both value "
         "and out-pointer positions");
  expect(cpp.find(".data();") != std::string::npos,
         "verified MIR should materialize a string-view literal as a "
         "NUL-terminated c_string call input");
}

void testCStringBoundaryRejectsUnprovedUses() {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto analyze = [&](std::string_view name, std::string source) {
    return lang::Frontend().analyze(
        std::filesystem::path(name), std::move(source),
        {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  };

  const lang::FrontendResult field = analyze("c-string-field.gti", R"(
[[c_abi]] struct InvalidRecord { mut c_string value; };
int main() { return 0; }
)");
  expect(!field.canGenerateCode() && hasCode(field, "GTI-S2064"),
         "c_string should remain a boundary pointer contract rather than an "
         "admitted native-record field scalar");

  const lang::FrontendResult indexed = analyze("c-string-index.gti", R"(
extern "C" { c_string native_name(); }
int main() {
  c_string value = native_name();
  return int32_t(value[0]);
}
)");
  expect(!indexed.canGenerateCode(),
         "nullable c_string values should not expose unchecked indexing");

  const lang::FrontendResult retainedLiteral =
      analyze("c-string-retained-literal.gti", R"(
int main() {
  c_string value = "not a call boundary";
  return value == nullptr ? 1 : 0;
}
)");
  expect(!retainedLiteral.canGenerateCode() &&
             hasCode(retainedLiteral, "GTI-S2003"),
         "the initial c_string slice should not generalize a contextual call "
         "conversion into unrestricted retained conversions");
}

void testNativeArrayBoundarySurface() {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "native-c-array.gti", R"(
[[c_opaque]] struct NativeHandle;
[[c_abi]] struct NativePoint { mut int32_t x; mut int32_t y; };

extern "C" {
  [[c_array(count)]] NativeHandle** native_handles(int32_t* count);
  [[c_array(count)]] const NativePoint* native_points(uint32_t* count);
  [[c_array(count)]] c_string* native_names(uint32_t* count);
}

int main() {
  mut int32_t handle_count = 0;
  mut uint32_t point_count = 0;
  mut uint32_t name_count = 0;
  unsafe {
    auto handles = native_handles(&handle_count);
    auto points = native_points(&point_count);
    auto names = native_names(&name_count);
    if (handles == nullptr || points == nullptr || names == nullptr) {
      return 1;
    }
  }
  return 0;
}
)",
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "c_array should pair returned native arrays with exact integer "
         "out-count parameters");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, lang::TargetInfo::host());
  const std::string header =
      lang::NativeHeaderBackend().generate(lowered).contents;
  expect(header.find("NativeHandle** native_handles(int32_t* count)") !=
                 std::string::npos &&
             header.find("const NativePoint* native_points(uint32_t* count)") !=
                 std::string::npos &&
             header.find("const char** native_names(uint32_t* count)") !=
                 std::string::npos,
         "native headers should erase c_array metadata into exact recursive C "
         "pointer declarations");

  const auto nativeHandles =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &instance) {
                     return instance.externalSymbol == "native_handles";
                   });
  expect(nativeHandles != frontend.mir.functionInstances().end() &&
             nativeHandles->cArrayCountParameter ==
                 std::optional<std::size_t>{0} &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should retain and verify the exact native-array count parameter");
  if (nativeHandles != frontend.mir.functionInstances().end()) {
    const std::size_t functionIndex = static_cast<std::size_t>(
        std::distance(frontend.mir.functionInstances().begin(), nativeHandles));

    lang::MirProgram missingPair = frontend.mir;
    auto &missingFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            missingPair.functionInstances());
    missingFunctions[functionIndex].cArrayCountParameter.reset();
    expect(!lang::verifyMirProgram(missingPair).valid(),
           "MIR verification should reject a nested return after a pass drops "
           "its native-array pair metadata");

    lang::MirProgram invalidPair = frontend.mir;
    auto &invalidFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            invalidPair.functionInstances());
    invalidFunctions[functionIndex].cArrayCountParameter =
        invalidFunctions[functionIndex].parameterTypes.size();
    expect(!lang::verifyMirProgram(invalidPair).valid(),
           "MIR verification should reject an out-of-range native-array count "
           "parameter identity");
  }
}

void testNativeArrayBoundaryRejectsInvalidPairs() {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto analyze = [&](std::string_view name, std::string source) {
    return lang::Frontend().analyze(
        std::filesystem::path(name), std::move(source),
        {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  };

  const lang::FrontendResult missing = analyze("c-array-missing.gti", R"(
extern "C" {
  [[c_array(missing)]] int32_t* native_values(int32_t* count);
}
int main() { return 0; }
)");
  expect(!missing.canGenerateCode() && hasCode(missing, "GTI-S2073"),
         "c_array should reject a count name outside its declaration");

  const lang::FrontendResult byValue = analyze("c-array-value.gti", R"(
extern "C" {
  [[c_array(count)]] int32_t* native_values(int32_t count);
}
int main() { return 0; }
)");
  expect(!byValue.canGenerateCode() && hasCode(byValue, "GTI-S2073"),
         "c_array should require a writable integer out-pointer");

  const lang::FrontendResult nonNative = analyze("c-array-gti.gti", R"(
[[c_array(count)]] int32_t* values(int32_t* count) { return nullptr; }
int main() { return 0; }
)");
  expect(!nonNative.canGenerateCode() && hasCode(nonNative, "GTI-S2073"),
         "c_array should remain confined to C-linkage declarations");

  const lang::FrontendResult unannotated = analyze("c-array-nested.gti", R"(
[[c_opaque]] struct NativeHandle;
extern "C" { NativeHandle** native_handles(int32_t* count); }
int main() { return 0; }
)");
  expect(!unannotated.canGenerateCode() && hasCode(unannotated, "GTI-S2074"),
         "two-level pointer syntax should require the c_array boundary");

  const lang::FrontendResult ordinaryPositions =
      analyze("c-array-ordinary-nested.gti", R"(
class NestedField { int32_t** value = nullptr; };
void consume(int32_t** value) {}
int main() {
  int32_t** local = nullptr;
  return 0;
}
)");
  const std::size_t nestedDiagnostics = static_cast<std::size_t>(
      std::count_if(ordinaryPositions.diagnostics.begin(),
                    ordinaryPositions.diagnostics.end(),
                    [](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == "GTI-S2074";
                    }));
  expect(!ordinaryPositions.canGenerateCode() && nestedDiagnostics == 3,
         "two-level pointers should remain rejected for fields, parameters, "
         "and locals");

  const lang::FrontendResult readOnlyCount =
      analyze("c-array-const-count.gti", R"(
extern "C" {
  [[c_array(count)]] int32_t* native_values(const int32_t* count);
}
int main() { return 0; }
)");
  expect(!readOnlyCount.canGenerateCode() &&
             hasCode(readOnlyCount, "GTI-S2073"),
         "c_array should require a writable count pointee");

  const lang::FrontendResult nestedCString = analyze("c-array-c-string.gti", R"(
extern "C" {
  [[c_array(count)]] c_string** native_names(uint32_t* count);
}
int main() { return 0; }
)");
  expect(!nestedCString.canGenerateCode() &&
             hasCode(nestedCString, "GTI-S2073"),
         "c_array should use c_string* for const char** rather than admitting "
         "a third physical pointer level");
}

} // namespace

int main() {
  testCAndCppHeaderSurface();
  testCStringBoundarySurface();
  testCStringBoundaryRejectsUnprovedUses();
  testNativeArrayBoundarySurface();
  testNativeArrayBoundaryRejectsInvalidPairs();
  if (failures != 0) {
    std::cerr << failures << " native-header test(s) failed\n";
    return 1;
  }
  std::cout << "All native-header tests passed\n";
  return 0;
}
