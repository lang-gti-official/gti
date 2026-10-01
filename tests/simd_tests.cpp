#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
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
lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("simd-contract.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}

void testContract() {
  const auto frontend = analyze(R"gti(
#include <std/simd>
class UserVector {
  std::simd<float, 4> lanes;
public:
  UserVector(float value) : lanes(std::simd<float, 4>(value)) {}
  float sum(UserVector& other) {
    std::simd<float, 4> added = this.lanes + other.lanes;
    return added.reduce();
  }
};
int main() {
  UserVector a{2.0}; UserVector b{3.0};
  return int(a.sum(b));
}
)gti");
  expect(frontend.canGenerateCode(),
         "SIMD should compose inside an unrelated user class");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "SIMD should have verified MIR");
  lang::MirBodyAddress address{};
  lang::MirInstructionId id = 0;
  for (const auto candidate : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = lang::findMirBody(frontend.mir, candidate);
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::SimdAdd) {
          address = candidate;
          id = instruction.id;
        }
      }
    }
  }
  expect(id != 0, "SIMD must remain an explicit typed MIR operation");
  if (!id)
    return;
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto malformed = frontend.mir;
    auto *body =
        const_cast<lang::MirBody *>(lang::findMirBody(malformed, address));
    lang::MirInstruction *selected = nullptr;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id)
          selected = &instruction;
    if (!selected) {
      expect(false, "mutation must find the SIMD operation");
      continue;
    }
    switch (mutation) {
    case 0:
      selected->operands.pop_back();
      break;
    case 1:
      selected->operands[2].kind = lang::MirOperandKind::BorrowRead;
      break;
    case 2:
      selected->parameterTypes[2].referenceAccess = lang::AccessMode::ReadOnly;
      break;
    case 3:
      selected->intrinsic = lang::IntrinsicKind::SimdEqual;
      break;
    case 4:
      selected->parameterTypes[0].arguments[0].arrayLength = 65;
      break;
    case 5:
      selected->operands[2].place = selected->operands[0].place;
      break;
    case 6:
      selected->info.type = lang::SemanticType::Bool;
      break;
    case 7:
      selected->destination = selected->operands[2].place;
      break;
    }
    expect(
        !lang::verifyMirProgram(malformed).valid(),
        "MIR must reject malformed lane, extent, result, and access contracts");
  }
  const auto effect = lang::effects(lang::IntrinsicKind::SimdAdd);
  expect(effect.readsPlace && effect.writesPlace && !effect.mayTrap &&
             !effect.removableWhenUnused && !effect.reorderable,
         "SIMD output mutation must remain an optimizer barrier");
}

void testInvalidPrograms() {
  for (const std::string_view declaration :
       {"std::simd<int, 4> value{};", "std::simd<bool, 4> value{};",
        "std::simd<float, 0> value{};", "std::simd<float, 65> value{};",
        "std::simd_mask<0> value{};", "std::simd_mask<65> value{};"}) {
    const auto frontend = analyze("#include <std/simd>\nint main() { " +
                                  std::string(declaration) + " return 0; }");
    expect(!frontend.canGenerateCode(),
           "invalid lane types and extents must fail in semantics");
  }
  expect(!analyze(R"gti(
#include <std/simd>
int main() {
  std::simd<float, 4> a{}; std::simd<float, 3> b{};
  auto wrong = a + b; return 0;
}
)gti")
              .canGenerateCode(),
         "mixed extents must not silently truncate or convert");
  expect(!analyze(R"gti(
#include <std/simd>
int main() { std::simd_mask<4> mask{}; if (mask) { return 1; } return 0; }
)gti")
              .canGenerateCode(),
         "a lane mask must require explicit all/any reduction");
  expect(!analyze(R"gti(
#include <std/simd>
int main() {
  float a[4] = {}; mut float output[4] = {};
  gti_internal::simd_add(a, a, output); return 0;
}
)gti")
              .canGenerateCode(),
         "application spelling must not grant a private SIMD capability");
}

void testIncludeDiagnostics() {
  for (const std::string name : {"simd", "simd_mask"}) {
    const std::string source = "using lanes = std::" + name +
                               (name == "simd" ? "<float, 4>;\n" : "<4>;\n");
    const auto frontend = analyze(source);
    const lang::Diagnostic *missing = nullptr;
    for (const auto &diagnostic : frontend.diagnostics) {
      if (diagnostic.code == "GTI-S2089")
        missing = &diagnostic;
    }
    expect(!frontend.canGenerateCode() && missing != nullptr &&
               missing->phase == lang::DiagnosticPhase::Semantics &&
               missing->severity == lang::DiagnosticSeverity::Error &&
               missing->primary.start == source.find(name) &&
               missing->primary.end == source.find(name) + name.size() &&
               missing->message.find("<std/simd>") != std::string::npos &&
               missing->fixes.size() == 1 &&
               missing->fixes.front().span.source == missing->primary.source &&
               missing->fixes.front().span.start == 0 &&
               missing->fixes.front().span.end == 0 &&
               missing->fixes.front().replacement == "#include <std/simd>\n",
           "missing SIMD includes need an exact compiler-owned include fix");
  }
}

void testAccessDiagnostics() {
  const auto check = [](std::string statement, std::string code,
                        std::string selected, std::string message) {
    const std::string source =
        "#include <std/simd>\nint main() {\n" + statement + "\nreturn 0;\n}\n";
    const auto frontend = analyze(source);
    const lang::Diagnostic *found = nullptr;
    for (const auto &diagnostic : frontend.diagnostics) {
      if (diagnostic.code == code) {
        found = &diagnostic;
      }
    }
    expect(
        !frontend.canGenerateCode() && found != nullptr &&
            found->phase == lang::DiagnosticPhase::Semantics &&
            std::filesystem::path(found->primary.source).filename() ==
                "simd-contract.gti" &&
            found->primary.start < found->primary.end &&
            source.substr(found->primary.start,
                          found->primary.end - found->primary.start) ==
                selected &&
            found->message.find(message) != std::string::npos &&
            found->fixes.empty(),
        "SIMD must retain source-located ordinary mutation/alias diagnostics");
  };
  check("std::simd<float, 4> value{};\nvalue.set(std::size_t(0), 1.0);",
        "GTI-S2000", ")", "mutable receiver");
  check("mut std::simd<float, 4> value{1.0};\nvalue += value;", "GTI-S2017",
        "+=", "overlapping reference accesses");
}
} // namespace

int main() {
  testContract();
  testInvalidPrograms();
  testIncludeDiagnostics();
  testAccessDiagnostics();
  if (failures)
    return 1;
  std::cout << "SIMD semantic and MIR contracts passed\n";
}
