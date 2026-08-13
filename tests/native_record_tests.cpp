#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = result.program,
                                   .semantics = result.semantics,
                                   .hir = result.hir,
                                   .mir = result.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("static_assert(sizeof(Packet) == 32") !=
                 std::string::npos &&
             artifact.contents.find("static_assert(alignof(Packet) == 8") !=
                 std::string::npos &&
             artifact.contents.find("offsetof(Packet, point) == 20") !=
                 std::string::npos &&
             artifact.contents.find("::Packet packet_roundtrip(::Packet") !=
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
  expectNativeRecordFailure("array-field.gti",
                            "[[c_abi]] struct Bad { int32_t values[4]; }; "
                            "int main() { return 0; }",
                            "outside the bounded");
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

void testFormatting() {
  const std::string formatted = lang::Formatter().format(
      "[[ c_abi ]]struct Point{mut float x;mut float y;};");
  expect(formatted.find("[[c_abi]]") != std::string::npos &&
             formatted.find("struct Point") != std::string::npos &&
             formatted.find("mut float x;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve a canonical native-record attribute "
         "and remain idempotent");
}

} // namespace

int main() {
  testSemanticLayoutAndAbi();
  testTargetMatrix();
  testNativeRecordDiagnostics();
  testUnsafeBoundary();
  testFormatting();
  if (failures != 0) {
    std::cerr << failures << " native-record test(s) failed\n";
    return 1;
  }
  return 0;
}
