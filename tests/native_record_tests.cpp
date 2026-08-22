#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

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

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

const lang::Diagnostic *findCode(const lang::FrontendResult &result,
                                 std::string_view code) {
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == code;
                   });
  return found == result.diagnostics.end() ? nullptr : &*found;
}

lang::FrontendResult
analyze(std::string_view name, std::string source,
        std::optional<lang::TargetInfo> target = std::nullopt) {
  lang::FrontendOptions options;
  if (target) {
    options.target = std::move(*target);
  }
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source));
}

const lang::ClassDecl *findClass(const lang::Program &program,
                                 std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *type = dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (type != nullptr && type->name().lexeme == name) {
      return type;
    }
  }
  return nullptr;
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

void testSemanticLayoutAndAbi() {
  const lang::FrontendResult result =
      analyze("native-record-valid.gti", validSource());
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode() && result.diagnostics.empty(),
         "passive native records and by-value/pointer C signatures should "
         "complete the compiler pipeline");
  if (!result.canGenerateCode()) {
    return;
  }

  const lang::ClassDecl *vecSyntax = findClass(result.program, "Vec2");
  const lang::ClassDecl *packetSyntax = findClass(result.program, "Packet");
  const lang::ClassDecl *linkSyntax = findClass(result.program, "Link");
  const lang::ClassTypeInfo *vec =
      vecSyntax == nullptr ? nullptr
                           : result.semantics.findClassType(*vecSyntax);
  const lang::ClassTypeInfo *packet =
      packetSyntax == nullptr ? nullptr
                              : result.semantics.findClassType(*packetSyntax);
  const lang::ClassTypeInfo *link =
      linkSyntax == nullptr ? nullptr
                            : result.semantics.findClassType(*linkSyntax);
  expect(vec != nullptr && vec->cAbiRecord && vec->cAbiLayout &&
             vec->cAbiLayout->sizeBytes == 8 &&
             vec->cAbiLayout->abiAlignmentBytes == 4 &&
             vec->cAbiLayout->fields.size() == 2 &&
             vec->cAbiLayout->fields[0].offsetBytes == 0 &&
             vec->cAbiLayout->fields[1].offsetBytes == 4,
         "Vec2 should have frontend-owned C field offsets and tail size");
  expect(packet != nullptr && packet->cAbiRecord && packet->cAbiLayout &&
             packet->cAbiLayout->sizeBytes == 32 &&
             packet->cAbiLayout->abiAlignmentBytes == 8 &&
             packet->cAbiLayout->fields.size() == 4 &&
             packet->cAbiLayout->fields[0].offsetBytes == 0 &&
             packet->cAbiLayout->fields[1].offsetBytes == 8 &&
             packet->cAbiLayout->fields[2].offsetBytes == 16 &&
             packet->cAbiLayout->fields[3].offsetBytes == 20,
         "nested C records should use source order, ABI padding, and the "
         "nested record layout");
  expect(link != nullptr && link->cAbiRecord && link->cAbiLayout &&
             link->cAbiLayout->sizeBytes == 16 &&
             link->cAbiLayout->abiAlignmentBytes == 8 &&
             link->cAbiLayout->fields.size() == 2 &&
             link->cAbiLayout->fields[0].offsetBytes == 0 &&
             link->cAbiLayout->fields[1].offsetBytes == 8,
         "a one-level pointer to a native record should occupy one target "
         "pointer field");

  const auto matchingHir = [&](const lang::ClassTypeInfo *type) {
    return type == nullptr
               ? result.hir.classInstances().end()
               : std::find_if(result.hir.classInstances().begin(),
                              result.hir.classInstances().end(),
                              [&](const lang::HirClassInstance &instance) {
                                return instance.declaration == type->id;
                              });
  };
  const auto packetHir = matchingHir(packet);
  expect(packetHir != result.hir.classInstances().end() &&
             packetHir->cAbiRecord && packetHir->cAbiLayout &&
             packetHir->cAbiLayout->sizeBytes == 32,
         "HIR should retain the selected native-record layout and identity");
  const auto packetMir =
      packetHir == result.hir.classInstances().end()
          ? result.mir.classInstances().end()
          : std::find_if(result.mir.classInstances().begin(),
                         result.mir.classInstances().end(),
                         [&](const lang::MirClassInstance &instance) {
                           return instance.id == packetHir->id;
                         });
  expect(packetMir != result.mir.classInstances().end() &&
             packetMir->cAbiRecord && packetMir->cAbiLayout &&
             packetMir->cAbiLayout->fields[3].offsetBytes == 20 &&
             lang::verifyMirProgram(result.mir).valid(),
         "MIR should retain and verify native-record layout metadata");

  lang::MirProgram activeNativeRecord = result.mir;
  auto &activeNativeClasses = const_cast<std::vector<lang::MirClassInstance> &>(
      activeNativeRecord.classInstances());
  if (packetMir != result.mir.classInstances().end()) {
    activeNativeClasses[packetMir->id - 1].requiresActiveCleanup = true;
  }
  expect(packetMir != result.mir.classInstances().end() &&
             !lang::verifyMirProgram(activeNativeRecord).valid(),
         "MIR verification should reject active-cleanup metadata forged onto "
         "a passive native record");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = result.program,
                                   .semantics = result.semantics,
                                   .hir = result.hir,
                                   .mir = result.mir,
                                   .sourceMir = &result.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("static_assert(sizeof(Packet) == 32") !=
              std::string::npos &&
          artifact.contents.find("static_assert(alignof(Packet) == 8") !=
              std::string::npos &&
          artifact.contents.find("__builtin_offsetof(Packet, point) == 20") !=
              std::string::npos &&
          artifact.contents.find("::__gti_program::Packet "
                                 "packet_roundtrip(::__gti_program::Packet") !=
              std::string::npos &&
          artifact.contents.find("static_cast<std::uint64_t>(32)") !=
              std::string::npos,
      "the C++ backend should emit a passive record, C signatures, native "
      "layout guards, and frontend-resolved query constants");
}

void testTargetMatrix() {
  const std::array<std::string_view, 6> triples = {
      "aarch64-apple-darwin",      "x86_64-apple-darwin",
      "aarch64-unknown-linux-gnu", "x86_64-unknown-linux-gnu",
      "aarch64-pc-windows-msvc",   "x86_64-pc-windows-msvc"};
  bool allMatch = true;
  for (const std::string_view triple : triples) {
    std::optional<lang::TargetInfo> target = lang::parseTargetTriple(triple);
    if (!target) {
      allMatch = false;
      continue;
    }
    const lang::FrontendResult result =
        analyze("native-record-target.gti", validSource(), std::move(target));
    const lang::ClassDecl *packetSyntax = findClass(result.program, "Packet");
    const lang::ClassTypeInfo *packet =
        packetSyntax == nullptr ? nullptr
                                : result.semantics.findClassType(*packetSyntax);
    allMatch = allMatch && result.canGenerateCode() && packet != nullptr &&
               packet->cAbiLayout && packet->cAbiLayout->sizeBytes == 32 &&
               packet->cAbiLayout->abiAlignmentBytes == 8 &&
               packet->cAbiLayout->fields[3].offsetBytes == 20;
  }
  expect(allMatch,
         "all six supported OS/architecture target selections should derive "
         "the same bounded native-record facts");
}

void expectNativeRecordFailure(std::string_view name, std::string source,
                               std::string_view messageFragment) {
  const lang::FrontendResult result = analyze(name, std::move(source));
  const lang::Diagnostic *diagnostic = findCode(result, "GTI-S2064");
  const bool focused =
      !result.canGenerateCode() && diagnostic != nullptr &&
      diagnostic->message.find(messageFragment) != std::string::npos &&
      !diagnostic->hints.empty() && diagnostic->fixes.empty() &&
      countCode(result, "GTI-B0001") == 0;
  if (!focused) {
    printDiagnostics(result);
  }
  expect(focused, "invalid native record '" + std::string(name) +
                      "' should fail in semantics with focused GTI-S2064");
}

void expectNativeArrayFieldFailure(std::string_view name, std::string source,
                                   std::string_view code,
                                   std::string_view primarySpelling,
                                   std::string_view messageFragment) {
  const std::size_t expectedStart = source.find(primarySpelling);
  const std::filesystem::path expectedSource =
      (std::filesystem::current_path() / name).lexically_normal();
  const lang::FrontendResult result = analyze(name, std::move(source));
  const lang::Diagnostic *diagnostic = findCode(result, code);
  const bool focused =
      !result.canGenerateCode() && expectedStart != std::string::npos &&
      diagnostic != nullptr &&
      diagnostic->phase == lang::DiagnosticPhase::Semantics &&
      std::filesystem::path(diagnostic->primary.source) == expectedSource &&
      diagnostic->primary.start == expectedStart &&
      diagnostic->primary.end == expectedStart + primarySpelling.size() &&
      diagnostic->message.find(messageFragment) != std::string::npos &&
      !diagnostic->hints.empty() && diagnostic->fixes.empty() &&
      countCode(result, code) == 1 && countCode(result, "GTI-B0001") == 0;
  if (!focused) {
    printDiagnostics(result);
    if (diagnostic != nullptr) {
      std::cerr << "primary=" << diagnostic->primary.source << ':'
                << diagnostic->primary.start << ".." << diagnostic->primary.end
                << " phase=" << static_cast<int>(diagnostic->phase)
                << " expected=" << expectedStart << ".."
                << expectedStart + primarySpelling.size() << '\n';
    }
  }
  expect(focused, "invalid native array field '" + std::string(name) +
                      "' should fail in semantics with " + std::string(code));
}

void expectOpaqueHandleFailure(std::string_view name, std::string source,
                               std::string_view messageFragment) {
  const lang::FrontendResult result = analyze(name, std::move(source));
  const lang::Diagnostic *diagnostic = findCode(result, "GTI-S2065");
  const bool focused =
      !result.canGenerateCode() && diagnostic != nullptr &&
      diagnostic->message.find(messageFragment) != std::string::npos &&
      !diagnostic->hints.empty() && diagnostic->fixes.empty() &&
      countCode(result, "GTI-B0001") == 0;
  if (!focused) {
    printDiagnostics(result);
  }
  expect(focused, "invalid opaque handle '" + std::string(name) +
                      "' should fail in semantics with focused GTI-S2065");
}

void expectNativeIdentifierFailure(std::string_view name, std::string source,
                                   std::string_view code,
                                   std::string_view spelling,
                                   std::string_view messageFragment) {
  const std::size_t expectedStart = source.find(spelling);
  const lang::FrontendResult result = analyze(name, std::move(source));
  const lang::Diagnostic *diagnostic = findCode(result, code);
  const bool focused =
      !result.canGenerateCode() && expectedStart != std::string::npos &&
      diagnostic != nullptr && diagnostic->primary.start == expectedStart &&
      diagnostic->primary.end == expectedStart + spelling.size() &&
      diagnostic->message.find(messageFragment) != std::string::npos &&
      !diagnostic->hints.empty() && diagnostic->fixes.empty() &&
      countCode(result, "GTI-B0001") == 0;
  if (!focused) {
    printDiagnostics(result);
  }
  expect(focused, "native-facing identifier '" + std::string(spelling) +
                      "' should fail before native-header emission");
}

void testNativeRecordDiagnostics() {
  expectNativeRecordFailure(
      "wrong-kind.gti",
      "[[c_abi]] class Bad { public: int32_t value = 0; }; "
      "int main() { return 0; }",
      "only to passive struct");
  expectNativeRecordFailure(
      "generic.gti",
      "[[c_abi]] struct Bad<T> { T value; }; int main() { return 0; }",
      "cannot declare generic");
  expectNativeRecordFailure("empty.gti",
                            "[[c_abi]] struct Bad {}; int main() { return 0; }",
                            "at least one field");
  expectNativeRecordFailure(
      "method.gti",
      "[[c_abi]] struct Bad { int32_t value; int32_t read() { return value; } "
      "}; int main() { return 0; }",
      "cannot declare methods");
  expectNativeRecordFailure("access.gti",
                            "[[c_abi]] struct Bad { private: int32_t value; }; "
                            "int main() { return 0; }",
                            "cannot declare access sections");
  expectNativeRecordFailure(
      "static.gti",
      "[[c_abi]] struct Bad { int32_t value; static int32_t count = 0; }; "
      "int main() { return 0; }",
      "cannot declare static fields");
  expectNativeRecordFailure(
      "constructor.gti",
      "[[c_abi]] struct Bad { int32_t value; Bad(int32_t input) : "
      "value(input) {} }; int main() { return 0; }",
      "cannot declare constructors");
  expectNativeRecordFailure(
      "cleanup.gti",
      "[[c_abi]] struct Bad { int32_t value; ~Bad() {} }; "
      "int main() { return 0; }",
      "cannot declare cleanup");
  expectNativeRecordFailure(
      "bool-field.gti",
      "[[c_abi]] struct Bad { bool value; }; int main() { return 0; }",
      "outside the bounded");
  expectNativeArrayFieldFailure("zero-array-field.gti",
                                "[[c_abi]] struct Bad { int32_t values[0]; }; "
                                "int main() { return 0; }",
                                "GTI-S2069", "0", "positive concrete extent");
  expectNativeArrayFieldFailure("invalid-array-element.gti",
                                "[[c_abi]] struct Bad { bool values[4]; }; "
                                "int main() { return 0; }",
                                "GTI-S2070", "bool", "element type outside");
  expectNativeRecordFailure(
      "ordinary-field.gti",
      "struct Ordinary { int32_t value; }; [[c_abi]] struct Bad { "
      "Ordinary value; }; int main() { return 0; }",
      "outside the bounded");
  expectNativeRecordFailure(
      "recursive.gti",
      "[[c_abi]] struct Node { Node next; }; int main() { return 0; }",
      "recursive by-value");
  expectNativeRecordFailure(
      "mixed-policy.gti",
      "[[c_abi, no_share]] struct Bad { int32_t value; }; "
      "int main() { return 0; }",
      "cannot be combined");
  expectNativeRecordFailure("field-initializer.gti",
                            "[[c_abi]] struct Bad { int32_t value = 1; }; "
                            "int main() { return 0; }",
                            "cannot have a GTI initializer");

  const lang::FrontendResult ordinarySignature =
      analyze("ordinary-signature.gti",
              "struct Ordinary { int32_t value; }; extern \"C\" { Ordinary "
              "roundtrip(Ordinary value); } int main() { return 0; }");
  expect(!ordinarySignature.canGenerateCode() &&
             countCode(ordinarySignature, "GTI-S2054") >= 1 &&
             countCode(ordinarySignature, "GTI-B0001") == 0,
         "ordinary GTI records should remain outside extern C signatures");
}

void testNativeIdentifierPortability() {
  expectNativeIdentifierFailure(
      "native-record-support-name.gti",
      "[[c_abi]] struct gti_c_string_view { int32_t value; }; "
      "int main() { return 0; }",
      "GTI-S2064", "gti_c_string_view", "support type name");
  expectNativeIdentifierFailure(
      "opaque-support-name.gti",
      "[[c_opaque]] struct size_t; int main() { return 0; }", "GTI-S2065",
      "size_t", "support type name");
  expectNativeIdentifierFailure("native-record-keyword-field.gti",
                                "[[c_abi]] struct Value { int32_t restrict; }; "
                                "int main() { return 0; }",
                                "GTI-S2064", "restrict", "C17 keyword");
  expectNativeIdentifierFailure(
      "extern-c-keyword-symbol.gti",
      "extern \"C\" { void restrict(); } int main() { return 0; }", "GTI-S2054",
      "restrict", "C17 keyword");
  expectNativeIdentifierFailure(
      "extern-c-keyword-parameter.gti",
      "extern \"C\" { void inspect(int32_t restrict); } "
      "int main() { return 0; }",
      "GTI-S2054", "restrict", "C17 keyword");
  expectNativeIdentifierFailure("native-record-header-macro.gti",
                                "[[c_abi]] struct Value { int32_t NULL; }; "
                                "int main() { return 0; }",
                                "GTI-S2064", "NULL", "support macro");
  expectNativeIdentifierFailure("extern-c-generated-prefix.gti",
                                "extern \"C\" { void gti_cabi_deadbeef(); } "
                                "int main() { return 0; }",
                                "GTI-S2054", "gti_cabi_deadbeef",
                                "native-record prefix");
  expectNativeIdentifierFailure("native-record-reserved-everywhere.gti",
                                "[[c_abi]] struct Value { int32_t _Field; }; "
                                "int main() { return 0; }",
                                "GTI-S2064", "_Field",
                                "implementation-reserved");
  expectNativeIdentifierFailure("native-record-reserved-file-scope.gti",
                                "[[c_abi]] struct _value { int32_t field; }; "
                                "int main() { return 0; }",
                                "GTI-S2064", "_value", "C file-scope");
  expectNativeIdentifierFailure(
      "opaque-reserved-everywhere.gti",
      "namespace handles { [[c_opaque]] struct __Handle; } "
      "int main() { return 0; }",
      "GTI-S2065", "__Handle", "implementation-reserved");
  expectNativeIdentifierFailure(
      "extern-c-reserved-file-scope.gti",
      "extern \"C\" { void _inspect(); } int main() { return 0; }", "GTI-S2054",
      "_inspect", "C file-scope");
  expectNativeIdentifierFailure(
      "extern-c-function-macro.gti",
      "extern \"C\" { void INT32_C(); } int main() { return 0; }", "GTI-S2054",
      "INT32_C", "support macro");
  expectNativeIdentifierFailure(
      "extern-c-stddef-function-macro.gti",
      "extern \"C\" { void offsetof(); } int main() { return 0; }", "GTI-S2054",
      "offsetof", "support macro");
  expectNativeIdentifierFailure(
      "native-namespace-host-type.gti",
      "namespace FILE { [[c_opaque]] struct Handle; } "
      "int main() { return 0; }",
      "GTI-S2054", "FILE", "support type name");
  expectNativeIdentifierFailure(
      "native-nested-namespace-host-macro.gti",
      "namespace bridge { namespace EOF { "
      "[[c_abi]] struct Value { int32_t field; }; } } "
      "int main() { return 0; }",
      "GTI-S2054", "EOF", "support macro");
  expectNativeIdentifierFailure(
      "native-namespace-header-control-macro.gti",
      "namespace GTI_NATIVE_HEADER_NO_SOURCE_NAMES { "
      "[[c_opaque]] struct Handle; } int main() { return 0; }",
      "GTI-S2054", "GTI_NATIVE_HEADER_NO_SOURCE_NAMES", "support macro");
  expectNativeIdentifierFailure(
      "native-field-header-control-macro.gti",
      "[[c_abi]] struct Value { "
      "int32_t GTI_NATIVE_HEADER_NO_SOURCE_NAMES; }; "
      "int main() { return 0; }",
      "GTI-S2064", "GTI_NATIVE_HEADER_NO_SOURCE_NAMES", "support macro");
  expectNativeIdentifierFailure(
      "native-namespace-reserved.gti",
      "namespace bridge { namespace __native { "
      "extern \"C\" { void inspect(); } } } int main() { return 0; }",
      "GTI-S2054", "__native", "implementation-reserved");
  expectNativeIdentifierFailure(
      "native-root-namespace-underscore.gti",
      "namespace _native { [[c_opaque]] struct Handle; } "
      "int main() { return 0; }",
      "GTI-S2054", "_native", "C file-scope");

  const lang::FrontendResult scopedNames =
      analyze("scoped-native-identifiers.gti", R"(
namespace records {
[[c_abi]] struct restrict { int32_t size_t; };
[[c_abi]] struct _record { int32_t offsetof; int32_t INT32_C; int32_t _field; };
}
namespace handles {
[[c_opaque]] struct restrict;
[[c_opaque]] struct _handle;
}
namespace outer {
namespace size_t {
[[c_opaque]] struct Handle;
}
}
namespace offsetof {
[[c_abi]] struct Value { int32_t field; };
}
extern "C" {
  void inspect(int32_t size_t, int32_t offsetof, int32_t INT32_C,
               int32_t _parameter);
}
int main() { return 0; }
)");
  if (!scopedNames.canGenerateCode()) {
    printDiagnostics(scopedNames);
  }
  expect(scopedNames.canGenerateCode() && scopedNames.diagnostics.empty(),
         "a namespaced native type may use a C-only keyword because its C "
         "name is encoded; lower-case underscore names outside C file scope "
         "and function-like macro names not followed by '(' remain valid; "
         "fields, parameters, and non-root namespace components may shadow "
         "support typedef names");

  const lang::FrontendResult ordinaryHostNames =
      analyze("ordinary-host-name-namespaces.gti", R"(
namespace FILE { struct Local { int32_t value = 0; }; }
namespace EOF { int32_t value = 1; }
int main() { return EOF::value; }
)");
  if (!ordinaryHostNames.canGenerateCode()) {
    printDiagnostics(ordinaryHostNames);
  }
  expect(ordinaryHostNames.canGenerateCode() &&
             ordinaryHostNames.diagnostics.empty(),
         "host support names should remain valid ordinary GTI namespaces "
         "when they do not participate in a native header surface");
}

void testUnsafeBoundary() {
  const lang::FrontendResult safeByValue =
      analyze("native-record-safe-call.gti", R"(
[[c_abi]] struct Value { int32_t number; };
extern "C" { Value echo(Value value); }
int main() { return 0; }
)");
  expect(safeByValue.canGenerateCode(),
         "a pointer-free native record should remain a safe by-value C "
         "signature");

  const lang::FrontendResult pointerCall =
      analyze("native-record-pointer-call.gti", R"(
[[c_abi]] struct Value { int32_t number; };
extern "C" { void mutate(Value* value); }
int invoke(Value* value) { mutate(value); return 0; }
int main() { return 0; }
)");
  expect(!pointerCall.canGenerateCode() &&
             countCode(pointerCall, "GTI-S2055") == 1 &&
             countCode(pointerCall, "GTI-B0001") == 0,
         "a pointer to a native record should retain the lexical unsafe call "
         "requirement");

  const lang::FrontendResult pointerCarrierCall =
      analyze("native-record-pointer-carrier-call.gti", R"(
[[c_abi]] struct Value { int32_t number; };
[[c_abi]] struct Carrier { Value* pointer; uint32_t tag; };
extern "C" { Carrier echo(Carrier value); }
int invoke(Carrier value) { [[discard]] echo(value); return 0; }
int main() { return 0; }
)");
  expect(!pointerCarrierCall.canGenerateCode() &&
             countCode(pointerCarrierCall, "GTI-S2055") == 1 &&
             countCode(pointerCarrierCall, "GTI-B0001") == 0,
         "raw pointers nested in a by-value native record should not bypass "
         "the unsafe C boundary");
}

void testOpaqueHandles() {
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
  if (!valid.canGenerateCode()) {
    printDiagnostics(valid);
  }
  const lang::ClassDecl *handleSyntax =
      findClass(valid.program, "NativeEngine");
  const lang::ClassTypeInfo *handle =
      handleSyntax == nullptr ? nullptr
                              : valid.semantics.findClassType(*handleSyntax);
  expect(valid.canGenerateCode() && valid.diagnostics.empty() &&
             handleSyntax != nullptr && handleSyntax->isForwardDeclaration() &&
             handle != nullptr && handle->cOpaqueHandle &&
             !handle->cAbiRecord && !handle->cAbiLayout,
         "an opaque native handle should retain one incomplete nominal "
         "identity without acquiring a record layout");
  if (valid.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(valid.hir,
                                         lang::OptimizationLevel::O1);
    const lang::BackendArtifact cpp =
        lang::CppBackend().generate({.program = valid.program,
                                     .semantics = valid.semantics,
                                     .hir = valid.hir,
                                     .mir = valid.mir,
                                     .sourceMir = &valid.mir,
                                     .optimizations = optimizations});
    expect(cpp.contents.find("struct NativeEngine;") != std::string::npos &&
               cpp.contents.find("struct NativeEngine {") ==
                   std::string::npos &&
               cpp.contents.find("::NativeEngine* engine_create") !=
                   std::string::npos,
           "the C++ backend should preserve the opaque handle as an "
           "incomplete type used only by pointer");
  }

  expectOpaqueHandleFailure(
      "opaque-body.gti",
      "[[c_opaque]] struct Bad { int32_t value; }; int main() { return 0; }",
      "must be incomplete");
  expectOpaqueHandleFailure("ordinary-forward.gti",
                            "struct Bad; int main() { return 0; }",
                            "requires the [[c_opaque]]");
  expectOpaqueHandleFailure("opaque-class.gti",
                            "[[c_opaque]] class Bad; int main() { return 0; }",
                            "only to an incomplete struct");
  expectOpaqueHandleFailure(
      "opaque-generic.gti",
      "[[c_opaque]] struct Bad<T>; int main() { return 0; }",
      "cannot declare generic");
  expectOpaqueHandleFailure(
      "opaque-base.gti",
      "struct Base {}; [[c_opaque]] struct Bad : public Base; "
      "int main() { return 0; }",
      "cannot declare base");
  expectOpaqueHandleFailure(
      "opaque-by-value.gti",
      "[[c_opaque]] struct Bad; Bad value; int main() { return 0; }",
      "may be used only behind one raw pointer");
  expectOpaqueHandleFailure(
      "opaque-record-conflict.gti",
      "[[c_abi, c_opaque]] struct Bad; int main() { return 0; }",
      "cannot also be a layout-stable");
  expectOpaqueHandleFailure(
      "opaque-capability-conflict.gti",
      "[[c_opaque, no_share]] struct Bad; int main() { return 0; }",
      "cannot be combined with concurrency");

  const lang::FrontendResult noPointeeLayout = analyze("opaque-layout.gti", R"(
[[c_opaque]] struct NativeEngine;
uint64_t invalid = sizeof(NativeEngine);
int main() { return 0; }
)");
  expect(!noPointeeLayout.canGenerateCode() &&
             countCode(noPointeeLayout, "GTI-S2065") == 1 &&
             countCode(noPointeeLayout, "GTI-S2063") == 0 &&
             countCode(noPointeeLayout, "GTI-B0001") == 0,
         "an opaque pointee should have no source-queryable layout while its "
         "pointer retains ordinary target layout");

  const lang::FrontendResult unsafeRequired =
      analyze("opaque-handle-unsafe.gti", R"(
[[c_opaque]] struct NativeEngine;
extern "C" { int32_t engine_read(const NativeEngine* engine); }
int read(const NativeEngine* engine) { return engine_read(engine); }
int main() { return 0; }
)");
  expect(!unsafeRequired.canGenerateCode() &&
             countCode(unsafeRequired, "GTI-S2055") == 1 &&
             countCode(unsafeRequired, "GTI-B0001") == 0,
         "opaque-handle calls should retain the lexical raw-pointer unsafe "
         "boundary");

  const auto expectOpaqueOperation = [](std::string_view name,
                                        std::string expression,
                                        std::string_view operation) {
    const lang::FrontendResult result = analyze(
        name, "[[c_opaque]] struct NativeEngine; "
              "int probe(mut NativeEngine* left, NativeEngine* right) { "
              "unsafe { " +
                  std::move(expression) +
                  "; } return 0; } int main() { return 0; }");
    const lang::Diagnostic *diagnostic = findCode(result, "GTI-S2065");
    const bool focused =
        !result.canGenerateCode() && diagnostic != nullptr &&
        diagnostic->message.find(operation) != std::string::npos &&
        !diagnostic->related.empty() && !diagnostic->hints.empty() &&
        diagnostic->fixes.empty() && countCode(result, "GTI-S2055") == 0 &&
        countCode(result, "GTI-B0001") == 0;
    if (!focused) {
      printDiagnostics(result);
    }
    expect(focused, "opaque handle operation '" + std::string(name) +
                        "' should fail as an incomplete-pointee use");
  };
  expectOpaqueOperation("opaque-dereference.gti", "*left",
                        "raw-pointer dereference");
  expectOpaqueOperation("opaque-index.gti", "left[0]", "raw-pointer indexing");
  expectOpaqueOperation("opaque-dereference-write.gti", "*left = *right",
                        "raw-pointer dereference");
  expectOpaqueOperation("opaque-index-write.gti", "left[0] = right[0]",
                        "raw-pointer indexing");
  expectOpaqueOperation("opaque-add.gti", "left + 1", "raw-pointer arithmetic");
  expectOpaqueOperation("opaque-difference.gti", "left - right",
                        "raw-pointer arithmetic");
  expectOpaqueOperation("opaque-prefix.gti", "++left",
                        "raw-pointer arithmetic");
  expectOpaqueOperation("opaque-postfix.gti", "left++",
                        "raw-pointer arithmetic");
  expectOpaqueOperation("opaque-compound.gti", "left += 1",
                        "raw-pointer arithmetic");
  expectOpaqueOperation("opaque-member.gti", "left->field",
                        "raw-pointer member access");

  const lang::FrontendResult genericOperation =
      analyze("opaque-generic-operation.gti", R"(
[[c_opaque]] struct NativeEngine;
T* advance<T>(mut T* pointer) {
  unsafe { pointer++; }
  return pointer;
}
int main() {
  mut NativeEngine* pointer = nullptr;
  unsafe { pointer = advance(pointer); }
  return pointer == nullptr ? 0 : 1;
}
)");
  const lang::Diagnostic *genericDiagnostic =
      findCode(genericOperation, "GTI-S2065");
  expect(!genericOperation.canGenerateCode() && genericDiagnostic != nullptr &&
             genericDiagnostic->message.find("raw-pointer arithmetic") !=
                 std::string::npos &&
             genericDiagnostic->related.size() >= 2,
         "concrete generic reanalysis should retain the opaque-pointee rule "
         "and both declaration and instantiation context");

  const lang::FrontendResult addressOnly =
      analyze("opaque-address-only.gti", R"(
[[c_opaque]] struct NativeEngine;
NativeEngine* preserve(mut NativeEngine* left, NativeEngine* right) {
  left = right;
  if (left == nullptr || left == right) { return left; }
  return right;
}
int main() { return 0; }
)");
  if (!addressOnly.canGenerateCode()) {
    printDiagnostics(addressOnly);
  }
  expect(addressOnly.canGenerateCode() && addressOnly.diagnostics.empty(),
         "opaque-handle pointers should retain address-only copy, assignment, "
         "comparison, null comparison, parameter, and return operations");
}

void testFormatting() {
  const std::string formatted = lang::Formatter().format(
      "[[ c_abi ]]struct Point{mut float x;mut float y;};");
  expect(formatted.find("[[c_abi]]") != std::string::npos &&
             formatted.find("struct Point") != std::string::npos &&
             formatted.find("mut float x;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve a canonical native-record attribute "
         "and remain idempotent");

  const std::string opaque =
      lang::Formatter().format("[[ c_opaque ]]struct NativeHandle;");
  expect(opaque.find("[[c_opaque]]") != std::string::npos &&
             opaque.find("struct NativeHandle;") != std::string::npos &&
             lang::Formatter().format(opaque) == opaque,
         "the formatter should preserve canonical opaque-handle syntax and "
         "remain idempotent");

  const std::string nativeArray =
      lang::Formatter().format("[[ c_opaque ]]struct NativeHandle;extern \"C\"{"
                               "[[ c_array ( count ) ]]NativeHandle ** "
                               "native_handles(int32_t * count);}");
  expect(
      nativeArray.find("[[c_array(count)]]") != std::string::npos &&
          nativeArray.find("NativeHandle** native_handles(int32_t* count);") !=
              std::string::npos &&
          lang::Formatter().format(nativeArray) == nativeArray,
      "the formatter should preserve the bounded native-array attribute "
      "and two-level return spelling idempotently");
}

} // namespace

int main() {
  testSemanticLayoutAndAbi();
  testTargetMatrix();
  testNativeRecordDiagnostics();
  testNativeIdentifierPortability();
  testUnsafeBoundary();
  testOpaqueHandles();
  testFormatting();
  if (failures != 0) {
    std::cerr << failures << " native-record test(s) failed\n";
    return 1;
  }
  return 0;
}
