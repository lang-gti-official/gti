#include "gti/cpp_emitter.h"
#include "gti/frontend.h"
#include "gti/optimization/effects.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void expectIntegerValue(const std::optional<lang::CheckedIntegerValue> &actual,
                        lang::CheckedIntegerValue expected,
                        std::string_view context) {
  expect(actual && *actual == lang::normalizeCheckedInteger(expected),
         std::string(context));
}

void testPrivateArithmeticAuthority() {
  for (const std::uint8_t width : {8, 16, 32, 64}) {
    const lang::CheckedIntegerDomain signedDomain{.width = width,
                                                  .signedValue = true};
    const std::uint64_t signedLimit = std::uint64_t{1} << (width - 1);
    const lang::CheckedIntegerValue signedMinimum{.negative = true,
                                                  .magnitude = signedLimit};
    const lang::CheckedIntegerValue signedMaximum{.magnitude = signedLimit - 1};
    const lang::CheckedIntegerValue one{.magnitude = 1};
    const lang::CheckedIntegerValue negativeOne{.negative = true,
                                                .magnitude = 1};
    const lang::CheckedIntegerValue two{.magnitude = 2};

    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMaximum,
                           one, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       signedMinimum, "signed wrapping add should reach min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMinimum, one, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       signedMaximum, "signed wrapping sub should reach max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMaximum, two, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       {.negative = true, .magnitude = 2},
                       "signed wrapping mul should preserve low bits");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMaximum,
                           one, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating add should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMinimum,
                           negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating add should clamp min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMinimum, one, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating sub should clamp min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMaximum, negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating sub should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMinimum, negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating mul should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMinimum, two, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating mul should clamp min");

    const lang::CheckedIntegerDomain unsignedDomain{.width = width};
    const lang::CheckedIntegerValue zero{};
    const lang::CheckedIntegerValue unsignedMaximum{
        .magnitude = lang::checkedIntegerMask(unsignedDomain)};
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, unsignedMaximum,
                           one, unsignedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       zero, "unsigned wrapping add should reach zero");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Subtract, zero, one, unsignedDomain,
            lang::IntegerArithmeticMode::Wrapping),
        unsignedMaximum, "unsigned wrapping sub should reach max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           unsignedMaximum, two, unsignedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       {.magnitude = unsignedMaximum.magnitude - 1},
                       "unsigned wrapping mul should preserve low bits");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Add, unsignedMaximum, one,
            unsignedDomain, lang::IntegerArithmeticMode::Saturating),
        unsignedMaximum, "unsigned saturating add should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract, zero, one,
                           unsignedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       zero, "unsigned saturating sub should clamp zero");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Multiply, unsignedMaximum, two,
            unsignedDomain, lang::IntegerArithmeticMode::Saturating),
        unsignedMaximum, "unsigned saturating mul should clamp max");

    for (const lang::IntegerArithmeticMode mode :
         {lang::IntegerArithmeticMode::Wrapping,
          lang::IntegerArithmeticMode::Saturating}) {
      expectIntegerValue(
          lang::evaluateDefinedIntegerBinary(lang::CheckedIntegerOperation::Add,
                                             {.magnitude = 2}, {.magnitude = 3},
                                             signedDomain, mode),
          {.magnitude = 5}, "in-range arithmetic should preserve exact value");
    }
  }

  expect(!lang::evaluateDefinedIntegerBinary(
             lang::CheckedIntegerOperation::Divide, {.magnitude = 4},
             {.magnitude = 2}, {.width = 8, .signedValue = true},
             lang::IntegerArithmeticMode::Wrapping),
         "defined arithmetic should reject unsupported operations");
  expect(!lang::evaluateDefinedIntegerBinary(
             lang::CheckedIntegerOperation::Add, {.magnitude = 1},
             {.magnitude = 1}, {}, lang::IntegerArithmeticMode::Wrapping),
         "defined arithmetic should reject invalid domains");
}

const lang::VariableDecl *findTopLevelVariable(const lang::Program &program,
                                               std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == name) {
      return variable;
    }
  }
  return nullptr;
}

std::optional<lang::ConstantInteger>
constantInteger(const lang::FrontendResult &result, std::string_view name) {
  const lang::VariableDecl *variable =
      findTopLevelVariable(result.program, name);
  if (variable == nullptr || !variable->initializer()) {
    return std::nullopt;
  }
  const std::optional<lang::ConstantValue> constant =
      result.semantics.findConstant(*variable->initializer());
  const auto *integer =
      constant ? std::get_if<lang::ConstantInteger>(&*constant) : nullptr;
  return integer == nullptr ? std::nullopt
                            : std::optional<lang::ConstantInteger>{*integer};
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void testPublicApiAndLowering() {
  const std::string source = R"(
#include <std/numeric>

constexpr int8_t wrap_i8 = std::wrapping_add(int8_t(127), int8_t(1));
constexpr int16_t wrap_i16 = std::wrapping_add(int16_t(32767), int16_t(1));
constexpr int32_t wrap_i32 = std::wrapping_add(int32_t(2147483647), int32_t(1));
constexpr int64_t wrap_i64 = std::wrapping_add(int64_t(9223372036854775807), int64_t(1));
constexpr uint8_t wrap_u8 = std::wrapping_add(uint8_t(255), uint8_t(1));
constexpr uint16_t wrap_u16 = std::wrapping_add(uint16_t(65535), uint16_t(1));
constexpr uint32_t wrap_u32 = std::wrapping_add(uint32_t(4294967295), uint32_t(1));
constexpr uint64_t wrap_u64 = std::wrapping_add(uint64_t(18446744073709551615), uint64_t(1));

constexpr int8_t wrap_sub = std::wrapping_sub(int8_t(-128), int8_t(1));
constexpr int8_t wrap_mul = std::wrapping_mul(int8_t(100), int8_t(2));
constexpr int8_t sat_add = std::saturating_add(int8_t(120), int8_t(20));
constexpr int8_t sat_sub = std::saturating_sub(int8_t(-120), int8_t(20));
constexpr int8_t sat_mul = std::saturating_mul(int8_t(-100), int8_t(2));

int8_t runtime_wrap(int8_t left, int8_t right) {
  int8_t added = std::wrapping_add(left, right);
  int8_t subtracted = std::wrapping_sub(added, right);
  return std::wrapping_mul(subtracted, int8_t(1));
}

int8_t runtime_saturate(int8_t left, int8_t right) {
  int8_t added = std::saturating_add(left, right);
  int8_t subtracted = std::saturating_sub(added, right);
  return std::saturating_mul(subtracted, int8_t(2));
}

int main() {
  return runtime_wrap(int8_t(127), int8_t(1)) == int8_t(-128) and
                 runtime_saturate(int8_t(100), int8_t(40)) == int8_t(127)
             ? 0
             : 1;
}
)";
  lang::FrontendResult result = analyze("defined-integer-valid.gti", source);
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode(),
         "public wrapping/saturating source should lower completely");

  const std::optional<lang::ConstantInteger> wrapI8 =
      constantInteger(result, "wrap_i8");
  const std::optional<lang::ConstantInteger> wrapI64 =
      constantInteger(result, "wrap_i64");
  const std::optional<lang::ConstantInteger> wrapU64 =
      constantInteger(result, "wrap_u64");
  const std::optional<lang::ConstantInteger> saturated =
      constantInteger(result, "sat_add");
  expect(wrapI8 && wrapI8->negative && wrapI8->magnitude == 128 &&
             wrapI8->domain ==
                 lang::CheckedIntegerDomain{.width = 8, .signedValue = true},
         "constexpr wrapping add should retain exact signed int8 bits");
  expect(wrapI64 && wrapI64->negative &&
             wrapI64->magnitude == (std::uint64_t{1} << 63),
         "constexpr wrapping add should retain exact signed int64 minimum");
  expect(wrapU64 && !wrapU64->negative && wrapU64->magnitude == 0 &&
             wrapU64->domain == lang::CheckedIntegerDomain{.width = 64},
         "constexpr wrapping add should retain exact uint64 zero");
  expect(saturated && !saturated->negative && saturated->magnitude == 127,
         "constexpr saturating add should retain its clamped value");

  std::unordered_set<lang::IntrinsicKind> hirIntrinsics;
  for (const lang::HirFunctionInstance &function :
       result.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (lang::integerArithmeticIntrinsic(value.intrinsic)) {
        hirIntrinsics.insert(value.intrinsic);
        expect(value.kind == lang::HirValueKind::Call &&
                   value.operands.size() == 3,
               "HIR should preserve each private arithmetic call and its "
               "callee plus two operands");
      }
    }
  }

  std::unordered_set<lang::IntrinsicKind> mirIntrinsics;
  for (const lang::MirFunctionInstance &function :
       result.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (!lang::integerArithmeticIntrinsic(instruction.intrinsic)) {
          continue;
        }
        mirIntrinsics.insert(instruction.intrinsic);
        const lang::MirEffectTraits effect = lang::effects(instruction);
        expect(instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.operands.size() == 2 && instruction.result &&
                   effect.speculatable && effect.removableWhenUnused &&
                   effect.reorderable && !effect.mayTrap &&
                   !effect.readsUnknownMemory && !effect.writesUnknownMemory,
               "MIR should retain non-failing, memory-free arithmetic "
               "effects");
      }
    }
  }
  expect(hirIntrinsics.size() == 6 && mirIntrinsics.size() == 6,
         "HIR and MIR should retain all six arithmetic intrinsic identities");
  expect(lang::effects(lang::MirOperation::Add).mayTrap,
         "ordinary integer addition should retain its checked failure effect");
  expect(lang::verifyMirProgram(result.mir).valid(),
         "defined integer arithmetic should produce valid MIR");

  if (result.canGenerateCode()) {
    const std::string generated =
        lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                         nullptr, &result.semantics, &result.hir)
            .emit(result.program);
    for (const std::string_view helper :
         {"wrapping_add", "wrapping_sub", "wrapping_mul", "saturating_add",
          "saturating_sub", "saturating_mul"}) {
      expect(generated.find("return gti_internal::backend::" +
                            std::string(helper) + "(") != std::string::npos,
             "ordinary std wrapper should lower through the selected private "
             "helper");
    }
  }
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

void testPublicApiRejectsUnsupportedTypes() {
  const lang::FrontendResult floating =
      analyze("defined-integer-float.gti",
              "#include <std/numeric>\n"
              "int main() { [[discard]] std::wrapping_add(float(1), float(2)); "
              "return 0; }\n");
  expect(!floating.canGenerateCode() && hasMessage(floating, "No overload"),
         "floating arguments should not enter the integer API");

  const lang::FrontendResult mixed = analyze(
      "defined-integer-mixed.gti",
      "#include <std/numeric>\n"
      "int main() { [[discard]] std::saturating_add(int8_t(1), int16_t(2)); "
      "return 0; }\n");
  expect(!mixed.canGenerateCode() && hasMessage(mixed, "No overload"),
         "mixed integer widths should require an explicit conversion");
}

} // namespace

int main() {
  testPrivateArithmeticAuthority();
  testPublicApiAndLowering();
  testPublicApiRejectsUnsupportedTypes();
  if (failures != 0) {
    std::cerr << failures << " defined integer arithmetic test(s) failed\n";
    return 1;
  }
  std::cout << "All defined integer arithmetic tests passed\n";
  return 0;
}
