#include "gti/ast_printer.h"
#include "gti/cpp_emitter.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir_printer.h"
#include "gti/optimization/rewrite.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
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

bool hasDiagnostic(const lang::FrontendResult &result, std::string_view code,
                   std::string_view text = {}) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code &&
                              (text.empty() || diagnostic.message.find(text) !=
                                                   std::string::npos);
                     });
}

const lang::Diagnostic *findDiagnostic(const lang::FrontendResult &result,
                                       std::string_view code,
                                       std::string_view text = {}) {
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == code &&
                            (text.empty() || diagnostic.message.find(text) !=
                                                 std::string::npos);
                   });
  return found == result.diagnostics.end() ? nullptr : &*found;
}

const lang::VariableDecl *findVariable(const lang::Program &program,
                                       std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    if (const auto *variable =
            dynamic_cast<const lang::VariableDecl *>(declaration.get());
        variable != nullptr && variable->name().lexeme == name) {
      return variable;
    }
  }
  return nullptr;
}

std::optional<lang::BinaryFloat>
constantFloat(const lang::FrontendResult &result, std::string_view name) {
  const lang::VariableDecl *variable = findVariable(result.program, name);
  if (variable == nullptr || variable->initializer() == nullptr) {
    return std::nullopt;
  }
  const lang::BindingInfo *binding = result.semantics.findBinding(*variable);
  const std::optional<lang::ConstantValue> constant =
      binding != nullptr && binding->constant
          ? binding->constant
          : result.semantics.findConstant(*variable->initializer());
  const auto *floating =
      constant ? std::get_if<lang::BinaryFloat>(&*constant) : nullptr;
  return floating == nullptr ? std::nullopt
                             : std::optional<lang::BinaryFloat>{*floating};
}

void testPrivateBinary64Authority() {
  const auto invalidFormat = static_cast<lang::BinaryFloatFormat>(0xff);
  expect(!lang::binaryFloatWidth(invalidFormat) &&
             !lang::validBinaryFloat({.bits = 0, .format = invalidFormat}) &&
             !lang::parseBinaryFloat("0.0", invalidFormat),
         "binary floating entry points should reject unknown format tags");
  const lang::BinaryFloatParseResult tenth =
      lang::parseBinaryFloat("0.1", lang::BinaryFloatFormat::Binary64);
  const lang::BinaryFloatParseResult fifth =
      lang::parseBinaryFloat("0.2", lang::BinaryFloatFormat::Binary64);
  expect(tenth && tenth.value->bits == 0x3fb999999999999aULL &&
             tenth.value->format == lang::BinaryFloatFormat::Binary64,
         "binary64 parsing should retain the exact nearest-even bit pattern");
  expect(fifth && fifth.value->bits == 0x3fc999999999999aULL,
         "binary64 parsing should not route through host double");

  const lang::BinaryFloat sum = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Add, *tenth.value, *fifth.value);
  expect(sum.bits == 0x3fd3333333333334ULL &&
             sum.format == lang::BinaryFloatFormat::Binary64,
         "binary64 arithmetic should round once in the selected format");

  const lang::BinaryFloat zero =
      *lang::parseBinaryFloat("0.0", lang::BinaryFloatFormat::Binary64).value;
  const lang::BinaryFloat one =
      *lang::parseBinaryFloat("1.0", lang::BinaryFloatFormat::Binary64).value;
  const lang::BinaryFloat negativeZero = lang::negateBinaryFloat(zero);
  const lang::BinaryFloat infinity =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Divide, one, zero);
  const lang::BinaryFloat nan =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Divide, zero, zero);
  expect(negativeZero.bits == 0x8000000000000000ULL &&
             infinity.bits == 0x7ff0000000000000ULL &&
             lang::compareBinaryFloat(nan, nan) ==
                 lang::BinaryFloatOrdering::Unordered,
         "binary64 should preserve signed zero, infinity, and unordered NaN");

  const std::optional<lang::BinaryFloat> roundedInteger =
      lang::integerToBinaryFloat({.magnitude = 9007199254740993ULL},
                                 {.width = 64, .signedValue = true},
                                 lang::BinaryFloatFormat::Binary64);
  expect(roundedInteger && roundedInteger->bits == 0x4340000000000000ULL,
         "integer-to-binary64 conversion should round ties to even");
  expect(
      lang::convertBinaryFloat(*tenth.value, lang::BinaryFloatFormat::Binary32)
              .bits == 0x3dcccccdU,
      "explicit binary64-to-binary32 conversion should round exactly");

  const std::string overflow(400, '9');
  expect(
      lang::parseBinaryFloat(overflow + ".0", lang::BinaryFloatFormat::Binary64)
              .failure == lang::BinaryFloatParseFailure::OutOfRange,
      "finite binary64 literal overflow should be reported");
}

void testLanguagePipeline() {
  const lang::FrontendResult result = analyze(
      "binary64-pipeline.gti",
      "T floating_identity<std::floating_point T>(T value) { return value; }\n"
      "extern \"C\" { double native_scale(double value); }\n"
      "constexpr double tenth = 0.1d;\n"
      "constexpr double sum = tenth + 0.2D;\n"
      "constexpr double mixed = 1.0 + 0.25d;\n"
      "constexpr double widened = 0.1;\n"
      "constexpr double grouped = ((0.5d));\n"
      "constexpr float grouped32 = ((0.25));\n"
      "constexpr float narrowed = float(sum);\n"
      "constexpr int32_t truncated = int32_t(3.9d);\n"
      "constexpr uint64_t double_size = sizeof(double);\n"
      "constexpr uint64_t double_alignment = alignof(double);\n"
      "int main() {\n"
      "  double generic = floating_identity<double>(mixed);\n"
      "  double runtime = generic + 1;\n"
      "  return runtime == 2.25d && narrowed > 0.0 && truncated == 3 && "
      "double_size == 8 && double_alignment == 8 ? 0 : 1;\n"
      "}\n");
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(result.canGenerateCode(),
         "binary64 should compile through syntax, semantics, HIR, and MIR");
  expect(
      constantFloat(result, "tenth") ==
              lang::BinaryFloat{.bits = 0x3fb999999999999aULL,
                                .format = lang::BinaryFloatFormat::Binary64} &&
          constantFloat(result, "sum") ==
              lang::BinaryFloat{.bits = 0x3fd3333333333334ULL,
                                .format = lang::BinaryFloatFormat::Binary64} &&
          constantFloat(result, "mixed") ==
              lang::BinaryFloat{.bits = 0x3ff4000000000000ULL,
                                .format = lang::BinaryFloatFormat::Binary64} &&
          constantFloat(result, "widened") ==
              lang::BinaryFloat{.bits = 0x3fb99999a0000000ULL,
                                .format = lang::BinaryFloatFormat::Binary64} &&
          constantFloat(result, "grouped") ==
              lang::BinaryFloat{.bits = 0x3fe0000000000000ULL,
                                .format = lang::BinaryFloatFormat::Binary64} &&
          constantFloat(result, "narrowed") ==
              lang::BinaryFloat{.bits = 0x3e99999aU,
                                .format = lang::BinaryFloatFormat::Binary32},
      "constexpr evaluation should retain exact binary64, exact binary32 "
      "widening, and explicit binary32 narrowing results");

  const std::string mir = lang::MirPrinter().print(result.mir);
  const bool validMir =
      mir.find("f64:0x3fb999999999999a") != std::string::npos &&
      mir.find("operation=convert") != std::string::npos &&
      lang::verifyMirProgram(result.mir).valid();
  expect(validMir, "MIR should retain width-tagged exact floating constants");

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = result.hir,
                                        .mir = result.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .compatibility = &compatibility});
  expect(optimized.valid() && optimized.report.passes.size() == 1 &&
             optimized.report.passes.front().shadowMismatches == 0 &&
             lang::MirPrinter()
                     .print(optimized.mir)
                     .find("f64:0x3fe0000000000000") != std::string::npos,
         "binary64 grouping should agree across HIR compatibility folding and "
         "verified MIR shadow rewriting");

  lang::MirProgram malformed = result.mir;
  lang::MirProgramEditor editor(malformed);
  bool queuedMalformedFloat = false;
  for (const lang::MirBodyAddress bodyAddress :
       lang::enumerateMirBodyAddresses(malformed)) {
    const lang::MirBody *body = lang::findMirBody(malformed, bodyAddress);
    if (body == nullptr || queuedMalformedFloat) {
      continue;
    }
    for (const lang::MirBlock &block : body->blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Literal &&
            !instruction.programConstantSubstitution &&
            instruction.info.type == lang::SemanticType::Float) {
          editor.queueLiteralReplacement(
              {.body = bodyAddress, .block = block.id, .index = index},
              instruction.id, lang::MirOperation::Literal,
              lang::Literal{lang::BinaryFloat{
                  .bits = 0x100000000ULL,
                  .format = lang::BinaryFloatFormat::Binary32}});
          queuedMalformedFloat = true;
          break;
        }
      }
      if (queuedMalformedFloat) {
        break;
      }
    }
  }
  const lang::MirEditResult malformedEdit = editor.apply();
  expect(queuedMalformedFloat && !malformedEdit.changed &&
             !malformedEdit.verification.valid() &&
             lang::verifyMirProgram(malformed).valid(),
         "MIR editing should atomically reject binary32 constants with high "
         "bits outside their declared representation");

  if (result.canGenerateCode()) {
    const std::string generated =
        lang::CppEmitter(result.semantics, result.hir).emit(result.program);
    expect(generated.find("std::bit_cast<double>(std::uint64_t{"
                          "0x3fb999999999999aULL})") != std::string::npos &&
               generated.find("static_assert(__gti_strict_ieee754 == 1") !=
                   std::string::npos &&
               generated.find("numeric_limits<double>::digits == 53") !=
                   std::string::npos,
           "the backend should emit retained binary64 bits under the strict "
           "IEEE policy");
  }
}

void testNarrowingAndOverflowDiagnostics() {
  const lang::FrontendResult narrowing =
      analyze("binary64-narrowing.gti",
              "int main() { float value = 0.1d; return 0; }\n");
  expect(!narrowing.canGenerateCode() &&
             hasDiagnostic(narrowing, "GTI-S2003", "type 'double'"),
         "binary64-to-binary32 assignment should require an explicit cast");

  const std::string compoundSource =
      "int main() { mut float value = 0.1; value += 0.2d; return 0; }\n";
  const lang::FrontendResult compound =
      analyze("binary64-compound.gti", compoundSource);
  const lang::Diagnostic *compoundDiagnostic =
      findDiagnostic(compound, "GTI-S2003", "implicitly narrow");
  const std::size_t operation = compoundSource.find("+=");
  expect(!compound.canGenerateCode() && compoundDiagnostic != nullptr &&
             compoundDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             compoundDiagnostic->primary.start == operation &&
             compoundDiagnostic->primary.end == operation + 2 &&
             !compoundDiagnostic->hints.empty() &&
             compoundDiagnostic->fixes.empty(),
         "compound assignment should not hide binary64 narrowing");

  const std::string overflow(400, '9');
  lang::Lexer lexer;
  (void)lexer.scan(overflow + ".0d");
  expect(lexer.hadError() && !lexer.errors().empty() &&
             lexer.errors().front().code == "GTI-L0006" &&
             lexer.errors().front().message.find("finite binary64 range") !=
                 std::string::npos,
         "binary64 literal overflow should identify the selected format");
}

void testFormatting() {
  const std::string formatted = lang::Formatter().format(
      "double compute(double value){return value+0.25d;}");
  const bool valid =
      formatted.find("double compute(double value)") != std::string::npos &&
      formatted.find("value + 0.25d") != std::string::npos &&
      lang::Formatter().format(formatted) == formatted;
  if (!valid) {
    std::cerr << formatted << '\n';
  }
  expect(valid,
         "the formatter should preserve binary64 type and literal spelling");
}

} // namespace

int main() {
  testPrivateBinary64Authority();
  testLanguagePipeline();
  testNarrowingAndOverflowDiagnostics();
  testFormatting();
  if (failures != 0) {
    std::cerr << failures << " binary64 test(s) failed\n";
    return 1;
  }
  std::cout << "All binary64 tests passed\n";
  return 0;
}
