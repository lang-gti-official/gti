// Frozen C++ backend: the C++ spelling of passive native records, C array
// declarators, native layout guards and opaque handles. Native-record
// semantics, layout facts and MIR live in native_record_tests.cpp.
#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"
#include "native_symbol_spelling.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::string(name), std::move(source));
}

std::string validSource() {
  return R"(
[[c_abi]]
struct Vec2 {
  mut float x;
  mut float y;
};

[[c_abi]]
struct Packet {
  mut uint8_t tag;
  mut uint64_t serial;
  mut uint16_t flags;
  mut Vec2 point;
};

[[c_abi]] union NativeOverlay { mut Packet packet; mut uint8_t bytes[48]; };

[[c_abi]]
struct Link {
  mut Packet* next;
  mut uint32_t code;
};

using NativePacket = Packet;

extern "C" {
  Packet packet_roundtrip(Packet value);
  uint64_t packet_checksum(Packet value);
  void packet_translate(Packet* value, float dx, float dy);
  Link link_roundtrip(Link value);
}

int main() {
  uint64_t vec_size = sizeof(Vec2);
  uint64_t packet_size = sizeof(NativePacket);
  uint64_t packet_alignment = alignof(Packet);
  uint64_t link_size = sizeof(Link);
  return int32_t(vec_size + packet_size + packet_alignment + link_size - 64);
}
)";
}

void testPassiveRecordRepresentation() {
  const lang::FrontendResult result =
      analyze("native-record-valid.gti", validSource());
  expect(result.canGenerateCode(), "native record fixture analyzes");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  expect(
      artifact.contents.find("static_assert(sizeof(Packet) == 32") !=
              std::string::npos &&
          artifact.contents.find("static_assert(alignof(Packet) == 8") !=
              std::string::npos &&
          artifact.contents.find("__builtin_offsetof(Packet, point) == 20") !=
              std::string::npos &&
          artifact.contents.find(
              "::__gti_program::Packet " +
              gti_test::nativeSpelling(artifact.contents, "packet_roundtrip") +
              "(::__gti_program::Packet") != std::string::npos &&
          artifact.contents.find("static_cast<std::uint64_t>(32)") !=
              std::string::npos,
      "the C++ backend should emit a passive record, C signatures, native "
      "layout guards, and frontend-resolved query constants");
}

void testRecordArrayFieldRepresentation() {
  const std::string source = R"(
[[c_abi]]
struct Cell {
  mut float x;
  mut float y;
};

[[c_abi]]
struct Board {
  mut uint8_t tag;
  mut Cell cells[3];
  mut void* scratch[2];
  mut Cell grid[2][2];
};

int main() {
  uint64_t size = sizeof(Board);
  uint64_t alignment = alignof(Board);
  return int32_t(size + alignment - 88);
}
)";
  const lang::FrontendResult result =
      analyze("native-record-array-fields.gti", source);
  expect(result.canGenerateCode(), "record-array fixture analyzes");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  expect(artifact.contents.find("static_assert(sizeof(Board) == 80") !=
                 std::string::npos &&
             artifact.contents.find("__builtin_offsetof(Board, grid) == 48") !=
                 std::string::npos &&
             artifact.contents.find("grid[2][2];") != std::string::npos &&
             artifact.contents.find("scratch[2];") != std::string::npos,
         "the backend should emit direct C array declarators and audit the "
         "frontend layout with native assertions");
}

void testNativeArrayRowRepresentation() {
  const auto result = analyze("native-array-row.gti", R"(
[[c_abi]] struct Matrix { int32_t values[2][3]; };
extern "C" { Matrix native_matrix(); }
int main() {
  auto matrix = native_matrix();
  return int(matrix.values[1][2]);
}
)");
  expect(result.canGenerateCode(), "native row fixture analyzes");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    return;
  }
  const auto optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O0);
  const auto artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  const auto &text = artifact.contents;
  expect(
      text.find("raw_address<const std::array<std::int32_t, 3>>") ==
              std::string::npos &&
          text.find(
              "static_cast<void>(::gti_internal::backend::raw_address_of(&") !=
              std::string::npos,
      "unused native row addresses must evaluate without invented std::array "
      "pointer storage");
  expect(
      text.find(" = ::gti_internal::backend::mir_checked_array_bound_v1(") !=
              std::string::npos &&
          text.find(" = ::gti_internal::backend::mir_checked_array_read_v1(") !=
              std::string::npos,
      "eliding unused address storage must retain prefix and terminal bounds "
      "checks");
}

void testOpaqueHandleRepresentation() {
  const lang::FrontendResult valid = analyze("opaque-handle-valid.gti", R"(
[[c_opaque]] struct NativeEngine;
[[c_abi]] struct NativeRequest { NativeEngine* engine; int32_t value; };
extern "C" {
  NativeEngine* engine_create(int32_t initial);
  void engine_destroy(NativeEngine* engine);
  int32_t engine_read(const NativeEngine* engine);
  NativeRequest request_echo(NativeRequest request);
}
int main() {
  uint64_t pointer_size = sizeof(NativeEngine*);
  unsafe {
    NativeEngine* engine = engine_create(int32_t(pointer_size));
    int32_t value = engine_read(engine);
    engine_destroy(engine);
    return value - int32_t(pointer_size);
  }
}
)");
  expect(valid.canGenerateCode(), "opaque handle fixture analyzes");
  if (valid.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(valid.hir,
                                         lang::OptimizationLevel::O1);
    const lang::BackendArtifact cpp =
        gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
    expect(cpp.contents.find("struct NativeEngine;") != std::string::npos &&
               cpp.contents.find("struct NativeEngine {") ==
                   std::string::npos &&
               cpp.contents.find(
                   "::NativeEngine* " +
                   gti_test::nativeSpelling(cpp.contents, "engine_create")) !=
                   std::string::npos,
           "the C++ backend should preserve the opaque handle as an "
           "incomplete type used only by pointer");
  }
}

} // namespace

int main() {
  testPassiveRecordRepresentation();
  testRecordArrayFieldRepresentation();
  testNativeArrayRowRepresentation();
  testOpaqueHandleRepresentation();
  return failures == 0 ? 0 : 1;
}
