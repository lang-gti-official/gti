#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/native_header.h"
#include "gti/optimizer.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::BackendInput backendInput(const lang::FrontendResult &frontend,
                                const lang::OptimizationResult &optimizations) {
  return {.program = frontend.program,
          .semantics = frontend.semantics,
          .hir = frontend.hir,
          .mir = frontend.mir,
          .optimizations = optimizations,
          .target = lang::TargetInfo::host()};
}

void testCAndCppHeaderSurface() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-header.gti", R"(
using NativeInt = int32_t;

[[c_opaque]] struct RootHandle;

[[c_abi]]
struct RootRecord {
  mut NativeInt value;
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  lang::NativeHeaderBackend headerBackend;
  const lang::BackendArtifact first =
      headerBackend.generate(backendInput(frontend, optimizations));
  const lang::BackendArtifact second =
      headerBackend.generate(backendInput(frontend, optimizations));
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

  const lang::BackendArtifact cpp =
      lang::CppBackend().generate(backendInput(frontend, optimizations));
  const std::size_t record = cpp.contents.find("struct RootRecord {");
  const std::size_t recordEnd = cpp.contents.find("};", record);
  const std::string recordBody =
      record == std::string::npos || recordEnd == std::string::npos
          ? std::string{}
          : cpp.contents.substr(record, recordEnd - record);
  expect(recordBody.find("std::int32_t value;") != std::string::npos &&
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

} // namespace

int main() {
  testCAndCppHeaderSurface();
  if (failures != 0) {
    std::cerr << failures << " native-header test(s) failed\n";
    return 1;
  }
  std::cout << "All native-header tests passed\n";
  return 0;
}
