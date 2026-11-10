// Frozen C++ backend: the generated GTI C++ translation unit's spelling of
// native records, opaque handles, c_string call inputs and opaque out
// arguments. The backend-neutral native header and MIR contracts for the same
// programs live in native_header_tests.cpp.
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
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

lang::LoweredProgram lower(const lang::FrontendResult &frontend) {
  return gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                                lang::TargetInfo::host());
}

void testNativeRecordAndHandleSpelling() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-header.gti", R"(
using NativeInt = int32_t;

[[c_opaque]] struct RootHandle;
[[c_abi]] union RootPayload { mut uint64_t number; mut double real; };

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
  [[link_name("cpp_context_create")]] Context* context_create();
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
  expect(frontend.canGenerateCode(), "native-header fixture analyzes");
  if (!frontend.canGenerateCode())
    return;
  const lang::LoweredProgram lowered = lower(frontend);
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

void testCStringCallInputSpelling() {
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
  expect(frontend.canGenerateCode(), "c_string fixture analyzes");
  if (!frontend.canGenerateCode())
    return;
  const std::string cpp = lang::CppBackend().generate(lower(frontend)).contents;
  expect(cpp.find(".data();") != std::string::npos,
         "verified MIR should materialize a string-view literal as a "
         "NUL-terminated c_string call input");
}

void testOpaqueOutArgumentSpelling() {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto analyze = [&](std::string_view name, std::string source) {
    return lang::Frontend().analyze(
        std::filesystem::path(name), std::move(source),
        {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
  };
  const lang::FrontendResult frontend = analyze("native-handle-out.gti", R"(
[[c_opaque]] struct NativeDatabase;

extern "C" {
  int32_t database_open(c_string path, NativeDatabase** out_database);
  void database_close(NativeDatabase* database);
}

int main() {
  mut NativeDatabase* database = nullptr;
  unsafe {
    int32_t opened = database_open(":memory:", &database);
    if (opened != 0 || database == nullptr) {
      return 1;
    }
    database_close(database);
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(), "opaque out fixture analyzes");
  if (!frontend.canGenerateCode())
    return;
  const lang::LoweredProgram lowered = lower(frontend);
  const std::string header =
      lang::NativeHeaderBackend().generate(lowered).contents;
  const std::string cpp = lang::CppBackend().generate(lowered).contents;
  expect(header.find("NativeDatabase** out_database") != std::string::npos &&
             cpp.find("NativeDatabase** out_database") != std::string::npos,
         "native header and C++ representation should preserve the exact "
         "opaque-handle out-parameter ABI");
  expect(cpp.find("native_pointer_value(") != std::string::npos &&
             cpp.find("raw_pointer_slot_copy(") != std::string::npos &&
             cpp.find("native_slot_address()") != std::string::npos &&
             cpp.find("__gti_native_out_") == std::string::npos,
         "opaque out arguments address real pointer slots without "
         "contextual staging or carrier reinterpretation");
}

} // namespace

int main() {
  testNativeRecordAndHandleSpelling();
  testCStringCallInputSpelling();
  testOpaqueOutArgumentSpelling();
  if (failures != 0)
    return 1;
  std::cout << "All C++ native-header representation tests passed\n";
  return 0;
}
