#include "compiler_test_fixture.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_lowering.h"

namespace {

void testFixedWidthIntegers() {
  lang::Lexer aliasLexer;
  const std::vector<lang::Token> aliases = aliasLexer.scan(
      "int8 int8_t int16 int16_t int32 int32_t int64 int64_t "
      "uint8 uint8_t uint16 uint16_t uint32 uint32_t uint64 uint64_t");
  expect(!aliasLexer.hadError() && aliases.size() == 17 &&
             aliases[0].kind == lang::TokenKind::INT8 &&
             aliases[0].kind == aliases[1].kind &&
             aliases[2].kind == lang::TokenKind::INT16 &&
             aliases[2].kind == aliases[3].kind &&
             aliases[4].kind == lang::TokenKind::INT32 &&
             aliases[4].kind == aliases[5].kind &&
             aliases[6].kind == lang::TokenKind::INT64 &&
             aliases[6].kind == aliases[7].kind &&
             aliases[8].kind == lang::TokenKind::UINT8 &&
             aliases[8].kind == aliases[9].kind &&
             aliases[10].kind == lang::TokenKind::UINT16 &&
             aliases[10].kind == aliases[11].kind &&
             aliases[12].kind == lang::TokenKind::UINT32 &&
             aliases[12].kind == aliases[13].kind &&
             aliases[14].kind == lang::TokenKind::UINT64 &&
             aliases[14].kind == aliases[15].kind,
         "canonical and suffix-less fixed-width spellings should lex to the "
         "same primitive token kinds");

  const lang::FrontendResult legacy =
      lang::Frontend().analyze("legacy-integer-aliases.gti", R"(
int main() {
  int8 signed8 = 1;
  int16 signed16 = 2;
  int32 signed32 = 3;
  int64 signed64 = 4;
  uint8 unsigned8 = 5;
  uint16 unsigned16 = 6;
  uint32 unsigned32 = 7;
  uint64 unsigned64 = 8;
  return 0;
}
)");
  expect(legacy.canGenerateCode() && legacy.diagnostics.empty(),
         "suffix-less fixed-width aliases should remain valid complete GTI "
         "programs without a prelude include");

  lang::Lexer lexer;
  const std::string source = R"(
int8_t minimum8 = -128;
int16_t widened16 = minimum8;
int32_t maximum32 = 2147483647;
int default32 = maximum32;
int64_t maximum64 = 9223372036854775807;
int64_t minimum64 = -9223372036854775808;
uint8_t maximum_u8 = 255;
uint16_t widened_u16 = maximum_u8;
uint32_t maximum_u32 = 4294967295;
uint default_u32 = maximum_u32;
uint64_t maximum_u64 = 18446744073709551615;
int64_t signed_widening = maximum_u32;
uint32_t hexadecimal_pattern = 0xA5A5;
uint8_t binary_pattern = 0b10100101;

int64_t add_wide(int16_t left, int64_t right) {
  return left + right;
}

uint64_t add_unsigned(uint16_t left, uint64_t right) {
  return left + right;
}

int main() {
  int8_t maximum8 = 127;
  int16_t minimum16 = -32768;
  int32_t promoted = maximum8 + minimum16;
  uint8_t unsigned_left = 1;
  uint8_t unsigned_right = 2;
  int32_t promoted_unsigned = unsigned_left + unsigned_right;
  uint8_t bytes[4] = {1, 2, 3, 4};
  uint32_t counter = 1;
  uint32_t next = counter + 1;
  bool has_next = next > 0;
  return promoted;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "fixed-width integer source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "fixed-width integer declarations should parse");

  lang::SemanticVisitor semantic;
  const bool valid = semantic.check(program);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
      std::cerr << "Unexpected fixed-width diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid, "in-range literals and widening conversions should be valid");

  const std::string generated = emitAnalyzedSource(source);
  expect(
      generated.find("#include <cstdint>") != std::string::npos &&
          generated.find("std::int8_t minimum8;") != std::string::npos &&
          generated.find("std::int16_t widened16;") != std::string::npos &&
          generated.find("std::int32_t default32;") != std::string::npos &&
          generated.find("std::int64_t maximum64;") != std::string::npos &&
          generated.find("std::int64_t minimum64;") != std::string::npos &&
          generated.find("std::uint8_t maximum_u8;") != std::string::npos &&
          generated.find("std::uint16_t widened_u16;") != std::string::npos &&
          generated.find("std::uint32_t default_u32;") != std::string::npos &&
          generated.find("std::uint64_t maximum_u64;") != std::string::npos &&
          generated.find("std::uint32_t hexadecimal_pattern;") !=
              std::string::npos &&
          generated.find("std::uint8_t binary_pattern;") != std::string::npos &&
          generated.find("std::array<std::uint8_t, 4>") != std::string::npos &&
          generated.find("static_cast<std::uint8_t>(1)") != std::string::npos &&
          generated.find("static_cast<std::uint8_t>(4)") != std::string::npos &&
          generated.find("program-initialization-v1 module-instance 0") !=
              std::string::npos &&
          generated.find("static_cast<std::int8_t>(-127 - 1)") !=
              std::string::npos &&
          generated.find("static_cast<std::int16_t>(-32767 - 1)") !=
              std::string::npos &&
          generated.find("static_cast<std::int64_t>("
                         "-9223372036854775807 - 1)") != std::string::npos &&
          generated.find("static_cast<std::uint64_t>("
                         "18446744073709551615ULL)") != std::string::npos &&
          generated.find("::__gti_program::hexadecimal_pattern = ") !=
              std::string::npos &&
          generated.find("::__gti_program::binary_pattern = ") !=
              std::string::npos &&
          generated.find("::__gti_program::__gti_program_initialize(") !=
              std::string::npos &&
          generated.find("int main()") != std::string::npos,
      "integer widths should lower to cstdint types while main stays valid");

  auto invalidTokens = lexer.scan(R"(
int8_t too_high = 128;
int8_t too_low = -129;
int16_t wide = 1;
int8_t narrowing = wide;
int alias_overflow = 2147483648;
int64_t signed_overflow = 9223372036854775808;
uint8_t unsigned_negative = -1;
uint8_t unsigned_overflow = 256;
uint16_t unsigned_wide = 1;
uint8_t unsigned_narrowing = unsigned_wide;
int32_t signed_value = 1;
uint32_t unsigned_value = 1;
uint32_t signed_to_unsigned = signed_value;
int32_t unsafe_sum = signed_value + unsigned_value;
bool unsafe_comparison = signed_value < unsigned_value;
uint32_t unsafe_negation = -unsigned_value;
uint alias_unsigned_overflow = 4294967296;
)");
  expect(!lexer.hadError(),
         "signed range errors should be diagnosed semantically");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "out-of-range source should still parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "out-of-range literals and narrowing should be rejected");
  expect(invalidSemantic.errors().size() == 13,
         "each invalid fixed-width integer conversion should be diagnosed");

  auto lexicalOverflow =
      lexer.scan("uint64_t too_large = 18446744073709551616;");
  (void)lexicalOverflow;
  expect(lexer.hadError() && lexer.errors().size() == 1,
         "integer literals larger than uint64_t should fail during lexing");

  auto invalidPrefixed =
      lexer.scan("uint64_t bad_hex = 0xGG; uint64_t bad_binary = 0b102; "
                 "uint64_t wide_hex = 0x10000000000000000;");
  (void)invalidPrefixed;
  expect(lexer.hadError() && lexer.errors().size() == 3 &&
             std::all_of(lexer.errors().begin(), lexer.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-L0007";
                         }),
         "malformed and overflowing hexadecimal or binary literals should "
         "fail during lexing");

  const std::string formatted =
      lang::Formatter().format("#include <std/uint64>\n"
                               "int8 small=1;int64 large=small;uint8 byte=0xFF;"
                               "uint64 wide=0b1010;");
  expect(formatted == "#include <std/uint64>\n"
                      "int8_t small = 1;\nint64_t large = small;\n"
                      "uint8_t byte = 0xFF;\nuint64_t wide = 0b1010;\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should canonicalize legacy fixed-width aliases, preserve "
         "include paths and prefixed integer literals, and remain idempotent");
}

void testContextualSignedIntegerLiterals() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("contextual-signed-integer-literals.gti", R"(
bool matches_boundaries(int8_t narrow, int32_t wide, int64_t widest) {
  return narrow == -1 and (-128) <= narrow and
         wide == -2147483648 and widest == -(9223372036854775808);
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected contextual signed literal diagnostic: "
                << diagnostic.message << '\n';
    }
  }

  bool sawInt8Operand = false;
  bool sawInt32Operand = false;
  bool sawInt64Operand = false;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      const auto *unary = dynamic_cast<const lang::Unary *>(value.source);
      if (unary == nullptr || unary->oper().kind != lang::TokenKind::MINUS) {
        continue;
      }
      sawInt8Operand =
          sawInt8Operand || value.info.type == lang::SemanticType::Int8;
      sawInt32Operand =
          sawInt32Operand || value.info.type == lang::SemanticType::Int32;
      sawInt64Operand =
          sawInt64Operand || value.info.type == lang::SemanticType::Int64;
    }
  }
  const std::string generated = gti_test::emitCppText(valid);
  expect(valid.canGenerateCode() && sawInt8Operand && sawInt32Operand &&
             sawInt64Operand,
         "signed literal operands should adopt the other integer operand's "
         "exact type through HIR");
  expect(generated.find("mir_checked_negate_v1<std::int8_t>") !=
                 std::string::npos &&
             generated.find("static_cast<std::int32_t>(-2147483647 - 1)") !=
                 std::string::npos &&
             generated.find(
                 "static_cast<std::int64_t>(-9223372036854775807 - 1)") !=
                 std::string::npos,
         "the C++ backend should cast the complete signed literal "
         "expression into its frontend-selected domain");

  const std::string invalidSource = R"(
int main() {
  uint8_t value = 5;
  bool first = value == -1;
  bool reversed = (-1) == value;
  bool ordered = value < (-1);
  bool positive = value == +256;
  return 0;
}
)";
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "contextual-negative-out-of-range.gti", invalidSource);
  const auto negativeDiagnostic =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2004" &&
                            diagnostic.message.find("Integer literal '-1'") !=
                                std::string::npos;
                   });
  const std::size_t firstNegative = invalidSource.find("-1");
  expect(!invalid.canGenerateCode() &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2004") == 4 &&
             countDiagnosticContaining(
                 invalid.diagnostics,
                 "Unary '-' cannot be applied to an unsigned integer") == 0 &&
             negativeDiagnostic != invalid.diagnostics.end() &&
             negativeDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             negativeDiagnostic->primary.source.ends_with(
                 "contextual-negative-out-of-range.gti") &&
             negativeDiagnostic->primary.start == firstNegative + 1 &&
             negativeDiagnostic->primary.end == firstNegative + 2 &&
             negativeDiagnostic->hints.size() == 1 &&
             negativeDiagnostic->hints.front().find("representable value") !=
                 std::string::npos,
         "out-of-range signed literal operands should produce one precise "
         "contextual range diagnostic without a unary cascade");
}

void testBinaryFloatSemantics() {
  lang::Lexer lexer;
  const std::vector<lang::Token> rounded =
      lexer.scan("0.1 1.000000059604644775390625 "
                 "1.0000000596046447753906250000000000000001 0.0");
  const auto floatBits = [&rounded](std::size_t index) {
    const auto *value =
        index < rounded.size()
            ? std::get_if<lang::BinaryFloat>(&rounded[index].literal)
            : nullptr;
    return value == nullptr ? std::optional<std::uint32_t>{}
                            : std::optional<std::uint32_t>{value->bits};
  };
  expect(!lexer.hadError() && floatBits(0) == 0x3dcccccdU &&
             floatBits(1) == 0x3f800000U && floatBits(2) == 0x3f800001U &&
             floatBits(3) == 0U,
         "floating literals should parse directly to binary32 with exact "
         "round-to-nearest, ties-to-even semantics");

  (void)lexer.scan("9999999999999999999999999999999999999999.0");
  expect(lexer.hadError() && lexer.errors().size() == 1 &&
             lexer.errors().front().code == "GTI-L0006" &&
             lexer.errors().front().message.find("finite binary32 range") !=
                 std::string::npos,
         "a decimal literal that overflows binary32 should receive a focused "
         "lexical diagnostic");

  const lang::BinaryFloat tenth = *lang::parseBinaryFloat("0.1").value;
  const lang::BinaryFloat fifth = *lang::parseBinaryFloat("0.2").value;
  const lang::BinaryFloat zero = *lang::parseBinaryFloat("0.0").value;
  const lang::BinaryFloat one = *lang::parseBinaryFloat("1.0").value;
  const lang::BinaryFloat sum =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Add, tenth, fifth);
  const lang::BinaryFloat negativeZero = lang::negateBinaryFloat(zero);
  const lang::BinaryFloat negativeInfinity = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Divide, one, negativeZero);
  const lang::BinaryFloat nan =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Divide, zero, zero);
  const lang::BinaryFloat overflow = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Multiply, {0x7f7fffffU}, {0x40000000U});
  const lang::BinaryFloat underflowTie = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Divide, {0x00000001U}, {0x40000000U});
  expect(sum.bits == 0x3e99999aU && negativeZero.bits == 0x80000000U &&
             negativeInfinity.bits == 0xff800000U &&
             overflow.bits == 0x7f800000U && underflowTie.bits == 0U &&
             lang::compareBinaryFloat(zero, negativeZero) ==
                 lang::BinaryFloatOrdering::Equal &&
             lang::compareBinaryFloat(nan, nan) ==
                 lang::BinaryFloatOrdering::Unordered,
         "APFloat-backed operations should preserve binary32 rounding, signed "
         "zero, infinities, and unordered NaN comparison");

  const lang::ConstantValue nanConstant{nan};
  const lang::ConstantEvaluation nanEqual = lang::evaluateConstantComparison(
      lang::TokenKind::EQUAL_EQUAL, nanConstant, nanConstant);
  const lang::ConstantEvaluation nanNotEqual = lang::evaluateConstantComparison(
      lang::TokenKind::BANG_EQUAL, nanConstant, nanConstant);
  const lang::ConstantEvaluation nanLess = lang::evaluateConstantComparison(
      lang::TokenKind::LESS, nanConstant, nanConstant);
  expect(nanEqual && !std::get<bool>(*nanEqual.value) && nanNotEqual &&
             std::get<bool>(*nanNotEqual.value) && nanLess &&
             !std::get<bool>(*nanLess.value),
         "constant NaN comparisons should follow IEEE unordered predicates");

  const std::optional<lang::BinaryFloat> roundedInteger =
      lang::integerToBinaryFloat({.magnitude = 16777217},
                                 {.width = 32, .signedValue = true});
  const lang::BinaryFloat threePointNine = *lang::parseBinaryFloat("3.9").value;
  const std::optional<lang::CheckedIntegerValue> truncated =
      lang::binaryFloatToInteger(threePointNine,
                                 {.width = 32, .signedValue = true});
  const std::optional<lang::CheckedIntegerValue> signed32Minimum =
      lang::binaryFloatToInteger({0xcf000000U},
                                 {.width = 32, .signedValue = true});
  const std::optional<lang::CheckedIntegerValue> unsigned32Value =
      lang::binaryFloatToInteger({0x4f000000U},
                                 {.width = 32, .signedValue = false});
  const std::optional<lang::CheckedIntegerValue> signed64Minimum =
      lang::binaryFloatToInteger({0xdf000000U},
                                 {.width = 64, .signedValue = true});
  expect(roundedInteger && roundedInteger->bits == 0x4b800000U && truncated &&
             !truncated->negative && truncated->magnitude == 3 &&
             signed32Minimum && signed32Minimum->negative &&
             signed32Minimum->magnitude == (std::uint64_t{1} << 31) &&
             unsigned32Value && !unsigned32Value->negative &&
             unsigned32Value->magnitude == (std::uint64_t{1} << 31) &&
             signed64Minimum && signed64Minimum->negative &&
             signed64Minimum->magnitude == (std::uint64_t{1} << 63) &&
             !lang::binaryFloatToInteger({0x4f000000U},
                                         {.width = 32, .signedValue = true}) &&
             !lang::binaryFloatToInteger({0x4f800000U},
                                         {.width = 32, .signedValue = false}) &&
             !lang::binaryFloatToInteger({0x5f000000U},
                                         {.width = 64, .signedValue = true}) &&
             !lang::binaryFloatToInteger({0x5f800000U},
                                         {.width = 64, .signedValue = false}) &&
             !lang::binaryFloatToInteger({0x7f800000U},
                                         {.width = 32, .signedValue = true}),
         "integer/float conversion should round through APFloat, preserve "
         "signed and unsigned boundaries, and reject out-of-range or "
         "non-finite conversions");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("constexpr-float.gti", R"(
constexpr float sum = 0.1 + 0.2;
constexpr float rounded = float(16777217);
constexpr int truncated = int(3.9);
constexpr float negative_zero = -0.0;
constexpr float nan = 0.0 / 0.0;
constexpr bool ordered = sum > 0.2;
constexpr bool zeros_equal = negative_zero == 0.0;
constexpr bool nan_equal = nan == nan;
constexpr bool nan_not_equal = nan != nan;
constexpr float half(float value) { return value / 2.0; }
constexpr float result = half(1.0);
constexpr float advance(float value) {
  mut float current = value;
  current += 1;
  current++;
  return current;
}
constexpr float advanced = advance(0.5);
int main() {
  if constexpr (ordered and zeros_equal and !nan_equal and nan_not_equal and
                truncated == 3 and result == 0.5 and advanced == 2.5) {
    return 0;
  } else {
    return discarded_float_branch_is_not_analyzed;
  }
}
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "binary32 literals, arithmetic, comparisons, conversions, and calls "
         "should participate in compiler-owned constexpr evaluation");

  const lang::VariableDecl *sumDeclaration = nullptr;
  const lang::VariableDecl *negativeZeroDeclaration = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "sum") {
      sumDeclaration = variable;
    } else if (variable != nullptr &&
               variable->name().lexeme == "negative_zero") {
      negativeZeroDeclaration = variable;
    }
  }
  const lang::BindingInfo *sumBinding =
      sumDeclaration == nullptr
          ? nullptr
          : frontend.semantics.findBinding(*sumDeclaration);
  const lang::BindingInfo *negativeZeroBinding =
      negativeZeroDeclaration == nullptr
          ? nullptr
          : frontend.semantics.findBinding(*negativeZeroDeclaration);
  const auto *sumValue =
      sumBinding == nullptr || !sumBinding->constant
          ? nullptr
          : std::get_if<lang::BinaryFloat>(&*sumBinding->constant);
  const auto *negativeZeroValue =
      negativeZeroBinding == nullptr || !negativeZeroBinding->constant
          ? nullptr
          : std::get_if<lang::BinaryFloat>(&*negativeZeroBinding->constant);
  expect(sumValue != nullptr && sumValue->bits == 0x3e99999aU &&
             negativeZeroValue != nullptr &&
             negativeZeroValue->bits == 0x80000000U,
         "semantic bindings should retain exact binary32 constexpr bits");

  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O1);
  const std::string generated = gti_test::emitCppText(
      frontend, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &optimized);
  expect(generated.find("std::bit_cast<float>(std::uint32_t{0x3e99999aU})") !=
                 std::string::npos &&
             generated.find("std::numeric_limits<float>::is_iec559") !=
                 std::string::npos &&
             generated.find("__gti_strict_ieee754 == 1") != std::string::npos &&
             generated.find("discarded_float_branch_is_not_analyzed") ==
                 std::string::npos,
         "the backend should emit exact binary32 constants, require an IEEE "
         "host, and erase unselected constexpr branches");

  const lang::FrontendResult ordinary = lang::Frontend().analyze(
      "fold-float.gti",
      "int main() { float rounded = float(16777217); "
      "float sum = 0.1 + 0.2; float mixed = 0.5 + 1; "
      "if (sum > rounded or mixed == 0.0) { return 1; } return 0; }");
  const lang::OptimizationResult ordinaryOptimized =
      lang::OptimizationPipeline().run(ordinary.hir,
                                       lang::OptimizationLevel::O1);
  const std::string ordinaryGenerated = gti_test::emitCppText(
      ordinary, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &ordinaryOptimized);
  expect(ordinary.canGenerateCode() &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x4b800000U})") !=
                 std::string::npos &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x3e99999aU})") !=
                 std::string::npos &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x3fc00000U})") !=
                 std::string::npos,
         "ordinary O1 folding should use APFloat for integer-to-float "
         "conversion and binary32 arithmetic, not only constexpr bindings");
}

void testCharactersAndStringViews() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("text-values.gti", R"(
char letter = 'G';
char newline = '\n';
std::string_view text = "GTI\0text";
bool chars_match = letter == 'G' and newline == '\n';
bool literal_chars_match = 'G' == 'G';
bool text_matches = text == "GTI\0text";
std::size_t text_size = text.size();
bool text_not_empty = !text.empty();
char first = text[0];
char embedded_zero = text[3];
uint8_t letter_code = uint8_t(letter);

int main() {
  if (chars_match and text_matches and text_size == 8 and text_not_empty and
      first == 'G' and embedded_zero == '\0' and letter_code == 71) {
    return 0;
  }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "character values and literal-backed string views should validate");

  const lang::VariableDecl *textDeclaration = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "text") {
      textDeclaration = variable;
      break;
    }
  }
  const lang::BindingInfo *textBinding =
      textDeclaration == nullptr
          ? nullptr
          : valid.semantics.findBinding(*textDeclaration);
  expect(textBinding != nullptr &&
             textBinding->type == lang::SemanticType::StringView &&
             textBinding->traits.drop == lang::DropKind::Trivial &&
             textBinding->traits.copyable,
         "string views should remain trivial copyable values in semantics");

  const lang::OptimizationResult optimized =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O1);
  const std::string generated = gti_test::emitCppText(
      valid, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &optimized);
  expect(generated.find("using string_view = std::string_view;") !=
                 std::string::npos &&
             generated.find("char8_t{71}") != std::string::npos &&
             generated.find("std::string_view{\"GTI\\000text\", 8}") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_string_view_at_v1") !=
                 std::string::npos &&
             generated.find("static_cast<std::uint8_t>(") !=
                 std::string::npos &&
             generated.find("program-initialization-v1 module-instance 0") !=
                 std::string::npos &&
             generated.find("::__gti_program::literal_chars_match = ") !=
                 std::string::npos &&
             generated.find("::__gti_program::__gti_program_initialize(") !=
                 std::string::npos,
         "the C++ backend should preserve counted text and exact code units");

  const lang::FrontendResult invalidView = lang::Frontend().analyze(
      "invalid-string-view.gti",
      "int main() { mut std::string_view view = \"abc\"; "
      "view[0] = 'x'; char outside = \"abc\"[3]; "
      "char wrong = view[false]; view.missing(); return 0; }",
      {standardLibraryPrelude()});
  expect(!invalidView.canGenerateCode() &&
             countDiagnosticCode(invalidView.diagnostics, "GTI-S2035") == 4 &&
             hasDiagnostic(invalidView.diagnostics,
                           "character access is read-only") &&
             hasDiagnostic(invalidView.diagnostics,
                           "outside the valid range [0, 3)") &&
             hasDiagnostic(invalidView.diagnostics,
                           "index must have an integer type") &&
             hasDiagnostic(invalidView.diagnostics,
                           "Unknown std::string_view member"),
         "string-view traversal should reject mutation, invalid literal "
         "indexes, non-integer indexes, and unknown members in semantics");

  lang::Lexer lexer;
  lang::Parser expressionParser(lexer.scan("'a'"));
  lang::ExprPtr expression = expressionParser.parseExpression();
  expect(expression != nullptr && !expressionParser.hadError() &&
             lang::AstPrinter().print(*expression) == "'a'",
         "character literals should retain their source-level AST spelling");

  (void)lexer.scan("char empty = ''; char many = 'ab'; "
                   "char escape = '\\q'; char open = 'x\n");
  expect(countDiagnosticCode(lexer.errors(), "GTI-L0010") == 2 &&
             countDiagnosticCode(lexer.errors(), "GTI-L0005") == 1 &&
             countDiagnosticCode(lexer.errors(), "GTI-L0009") == 1,
         "invalid character widths, escapes, and termination should have "
         "focused lexical diagnostics");

  const lang::FrontendResult numeric = lang::Frontend().analyze(
      "character-numeric.gti",
      "int main() { char value = 'A'; int number = value; "
      "return value + 1; }",
      {standardLibraryPrelude()});
  expect(!numeric.canGenerateCode() &&
             hasDiagnostic(numeric.diagnostics, "value of type 'char'") &&
             hasDiagnostic(numeric.diagnostics, "requires numeric operands"),
         "char should not inherit integer arithmetic or conversions");

  const lang::FrontendResult codeUnitBridge = lang::Frontend().analyze(
      "character-code-unit.gti",
      "static_assert(char(uint8_t(65)) == 'A'); "
      "static_assert(uint8_t('B') == uint8_t(66)); "
      "int main() { mut uint8_t byte = uint8_t(67); char value = char(byte); "
      "return value == 'C' ? 0 : 1; }",
      {standardLibraryPrelude()});
  expect(codeUnitBridge.canGenerateCode(),
         "char and uint8_t should support an explicit, lossless code-unit "
         "bridge in both directions");

  const lang::FrontendResult invalidCodeUnitBridge = lang::Frontend().analyze(
      "invalid-character-code-unit.gti",
      "int main() { uint8_t byte = uint8_t(65); char implicit = byte; "
      "char wide = char(uint16_t(65)); return 0; }",
      {standardLibraryPrelude()});
  expect(!invalidCodeUnitBridge.canGenerateCode() &&
             hasDiagnostic(invalidCodeUnitBridge.diagnostics,
                           "Cannot initialize 'implicit'") &&
             countDiagnosticCode(invalidCodeUnitBridge.diagnostics,
                                 "GTI-S2014") == 1,
         "the code-unit bridge should remain explicit and should not admit "
         "other integer widths");

  const lang::FrontendResult obsolete = lang::Frontend().analyze(
      "obsolete-string.gti", "int main() { string text = \"old\"; return 0; }",
      {standardLibraryPrelude()});
  const lang::Diagnostic *migration =
      findDiagnosticByCode(obsolete.diagnostics, "GTI-S2033");
  expect(!obsolete.canGenerateCode() && migration != nullptr &&
             migration->fixes.empty() && migration->hints.size() == 2 &&
             migration->message.find("std::string") != std::string::npos &&
             migration->message.find("std::string_view") != std::string::npos &&
             migration->hints.front().find("#include <std/string>") !=
                 std::string::npos,
         "obsolete string declarations should explain the owning and "
         "non-owning replacements without guessing ownership intent");

  const std::string formatted = lang::Formatter().format(
      "int main(){char quote='\\\'';char newline='\\n';return 0;}");
  expect(formatted.find("char quote = '\\\'';") != std::string::npos &&
             formatted.find("char newline = '\\n';") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve character literal escapes");
}

void testStandardString() {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / "gti-standard-string/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, R"(
#include <std/string>

int main() {
  std::string_view literal = "engine";
  mut std::string value = std::string(literal);
  value.push_back(' ');
  value.append("runtime");
  mut std::string copy = value.clone();
  std::string signed_text =
      std::to_string(int64_t(-9223372036854775807 - 1));
  std::string unsigned_text =
      std::to_string(uint64_t(18446744073709551615));
  bool cloned = value == copy;
  copy[0] = 'E';
  char first = copy.at(0);
  copy.clear();
  if (literal.size() == 6 and !literal.empty() and literal[0] == 'e' and
      cloned and value == "engine runtime" and first == 'E' and copy.empty() and
      signed_text == "-9223372036854775808" and
      unsigned_text == "18446744073709551615") {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.diagnostics.empty()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected standard-string diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "std::string should be an ordinary source-defined owner over char "
         "storage");

  const std::string generated = gti_test::emitCppText(valid);
  bool checkedMutableCharacters = false;
  for (const auto &function : valid.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        checkedMutableCharacters |=
            instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut &&
            instruction.info.type == lang::SemanticType::Char &&
            instruction.borrowAccess == lang::AccessMode::Mutable &&
            instruction.loan.has_value();
      }
    }
  }
  expect(generated.find("class string") != std::string::npos &&
             generated.find("class terminated_prefix_owner") !=
                 std::string::npos &&
             generated.find("prefix_owner<char8_t>") != std::string::npos &&
             generated.find("string(const string &other) = delete;") !=
                 std::string::npos &&
             generated.find("string(string &&) = default;") !=
                 std::string::npos &&
             checkedMutableCharacters &&
             generated.find("native_pointer_value(") != std::string::npos &&
             generated.find("::gti_internal::backend::prefix_storage<") ==
                 std::string::npos,
         "std::string lowering should retain nominal move-only lifecycle and "
         "checked mutable storage access");

  const lang::FrontendResult invalidCopy = lang::Frontend().analyze(
      entry, R"(
#include <std/string>
int main() {
  std::string original = std::string("text");
  std::string copied = original;
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidCopy.canGenerateCode() &&
             hasDiagnosticCode(invalidCopy.diagnostics, "GTI-S2003") &&
             hasDiagnostic(invalidCopy.diagnostics,
                           "Cannot initialize 'copied'") &&
             hasDiagnosticHint(invalidCopy.diagnostics,
                               "Move-only owners cannot be copied"),
         "std::string copies should require explicit allocating clone() rather "
         "than hidden allocation");

  const std::string constructorOwnershipSource = R"(
#include <std/string>
class Path {
public:
  Path(std::string value) {}
};
int main() {
  mut std::string source = std::string("/hello/world.txt");
  Path parsed = Path(source);
  return 0;
}
)";
  const lang::FrontendResult invalidConstructorCopy = lang::Frontend().analyze(
      entry, constructorOwnershipSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const lang::Diagnostic *constructorOwnership =
      findDiagnosticByCode(invalidConstructorCopy.diagnostics, "GTI-S2018");
  const std::size_t constructorArgument =
      constructorOwnershipSource.rfind("source);");
  const std::size_t constructorDeclaration =
      constructorOwnershipSource.find("Path(std::string");
  expect(!invalidConstructorCopy.canGenerateCode() &&
             constructorOwnership != nullptr &&
             countDiagnosticCode(invalidConstructorCopy.diagnostics,
                                 "GTI-S2012") == 0 &&
             constructorOwnership->message.find(
                 "Constructor argument 1 would call the deleted copy "
                 "constructor of 'std::string'") != std::string::npos &&
             constructorOwnership->primary.start == constructorArgument &&
             constructorOwnership->primary.end == constructorArgument + 6 &&
             constructorOwnership->related.size() == 1 &&
             constructorOwnership->related.front().span.start ==
                 constructorDeclaration &&
             constructorOwnership->related.front().span.end ==
                 constructorDeclaration + 4 &&
             constructorOwnership->related.front().message ==
                 "Exact-type candidate: Path(std::string)" &&
             constructorOwnership->hints.size() == 1 &&
             constructorOwnership->hints.front().find(
                 "std::move(...) when this call should transfer ownership") !=
                 std::string::npos &&
             constructorOwnership->fixes.empty(),
         "an exact-type constructor rejected only by copy viability should "
         "report the argument ownership failure rather than a type mismatch");

  const lang::FrontendResult integerOutput = lang::Frontend().analyze(
      entry, R"(
int main() {
  std::print(int8_t(-128));
  std::println(uint64_t(18446744073709551615));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(integerOutput.canGenerateCode() && integerOutput.diagnostics.empty(),
         "std::print and std::println should accept every type satisfying the "
         "fixed-width integral constraint");

  const lang::FrontendResult formattedOutput = lang::Frontend().analyze(
      entry, R"(
#include <std/format>
#include <std/string>
int main() {
  mut auto formatted =
      std::try_format("left={} right={} count={}", int8_t(-8), uint64_t(9), 3);
  if (!formatted) { return 1; }
  if (formatted.value() != "left=-8 right=9 count=3") { return 2; }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!formattedOutput.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : formattedOutput.diagnostics) {
      std::cerr << "Unexpected formatted-output diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  std::size_t concreteFoldValues = 0;
  std::size_t concreteFoldElements = 0;
  for (const lang::HirFunctionInstance &function :
       formattedOutput.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind != lang::HirValueKind::PackFold ||
          value.packFoldElements.empty()) {
        continue;
      }
      concreteFoldValues++;
      concreteFoldElements += value.packFoldElements.size();
    }
  }
  expect(formattedOutput.canGenerateCode() &&
             lang::verifyMirProgram(formattedOutput.mir).valid() &&
             concreteFoldValues == 2 && concreteFoldElements == 4,
         "source-defined formatting should use two verified ordered folds for "
         "counting and appending the concrete trailing integral pack after "
         "handling the first argument directly");
  const std::string formattedGenerated = gti_test::emitCppText(formattedOutput);
  expect(formattedGenerated.find("std::format(") == std::string::npos &&
             formattedGenerated.find("std::vformat") == std::string::npos &&
             formattedGenerated.find("count_format_argument") !=
                 std::string::npos &&
             formattedGenerated.find("append_selected_format_argument") !=
                 std::string::npos,
         "formatting should lower as ordinary GTI library calls rather than a "
         "native C++ formatting shortcut");

  const lang::FrontendResult invalidFloatOutput = lang::Frontend().analyze(
      entry, "int main() { std::print(1.5); return 0; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidFloatOutput.canGenerateCode() &&
             hasDiagnosticCode(invalidFloatOutput.diagnostics, "GTI-S2029") &&
             hasDiagnostic(invalidFloatOutput.diagnostics,
                           "does not satisfy concept 'std::integral<float>'"),
         "the bounded integer-output slice should not silently claim "
         "floating-point formatting");
}

void testLogicalOperatorSpellings() {
  lang::Lexer lexer;

  const std::vector<lang::Token> aliasTokens = lexer.scan("and && & or || |");
  expect(!lexer.hadError() && aliasTokens.size() == 7 &&
             aliasTokens[0].kind == lang::TokenKind::AND &&
             aliasTokens[1].kind == lang::TokenKind::AND &&
             aliasTokens[1].lexeme == "&&" &&
             aliasTokens[2].kind == lang::TokenKind::AMPERSAND &&
             aliasTokens[3].kind == lang::TokenKind::OR &&
             aliasTokens[4].kind == lang::TokenKind::OR &&
             aliasTokens[4].lexeme == "||" &&
             aliasTokens[5].kind == lang::TokenKind::PIPE,
         "logical words and symbols should normalize without consuming "
         "single-character bitwise operators");

  lang::Parser symbolicPrecedence(lexer.scan("true || false && false"));
  lang::ExprPtr symbolicExpression = symbolicPrecedence.parseExpression();
  expect(symbolicExpression != nullptr && !symbolicPrecedence.hadError() &&
             lang::AstPrinter().print(*symbolicExpression) ==
                 "(|| true (&& false false))",
         "symbolic logical operators should use C++ precedence");

  lang::Parser wordPrecedence(lexer.scan("true or false and false"));
  lang::ExprPtr wordExpression = wordPrecedence.parseExpression();
  expect(wordExpression != nullptr && !wordPrecedence.hadError() &&
             lang::AstPrinter().print(*wordExpression) ==
                 "(or true (and false false))",
         "word logical operators should retain their existing precedence");

  const std::string validSource = R"(
bool choose(bool left, bool right, bool fallback) {
  return left && right || fallback;
}

int main() {
  bool words = true and true or false;
  if (choose(words, true, false) && !false || false) {
    return 0;
  }
  return 1;
}
)";
  auto validTokens = lexer.scan(validSource);
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "mixed logical operator spellings should parse together");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mixed logical operator spellings should share boolean semantics");

  const lang::FrontendResult assignmentOperand =
      lang::Frontend().analyze("logical-assignment-operand.gti", R"(
int main() {
  mut int value = 0;
  if (value = 3 and value == 3) {
    return 0;
  }
  return 1;
}
)");
  expect(!assignmentOperand.canGenerateCode() &&
             hasDiagnostic(assignmentOperand.diagnostics,
                           "Logical operands must be bool"),
         "recursive analysis of an assignment operand should preserve its "
         "resolved symbol and produce the owning type diagnostic");

  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("&&") != std::string::npos &&
             generated.find("||") != std::string::npos,
         "both logical spellings should lower to short-circuit C++ operators");

  auto invalidTokens =
      lexer.scan("int main() { if (1 && true) { return 0; } return 1; }");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid symbolic logical operands should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram) &&
             hasDiagnostic(invalidSemantic, "Logical operands must be bool."),
         "symbolic logical operators should use existing operand diagnostics");

  const std::string formatted =
      lang::Formatter().format("int main(){bool value=true&&false||true;"
                               "if(value||false&&true){return 0;}return 1;}");
  expect(formatted.find("bool value = true && false || true;") !=
                 std::string::npos &&
             formatted.find("if (value || false && true)") != std::string::npos,
         "formatter should treat symbolic logical aliases as binary operators");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted symbolic logical aliases should be idempotent");
}

void testComparisonOperatorFormattingStability() {
  const std::string source = R"(
bool compare(uint64_t left, uint64_t right, uint64_t limit) {
  return left < right && right >= limit && limit > (0);
}
)";
  const std::string formatted = lang::Formatter().format(source);
  expect(
      formatted.find("return left < right && right >= limit && limit > (0);") !=
              std::string::npos &&
          formatted.find("> =") == std::string::npos,
      "formatter should not treat comparisons as generic angle lists or "
      "split '>='");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted comparison chains should be idempotent");

  const lang::FrontendResult analyzed =
      lang::Frontend().analyze("formatter-comparisons.gti", formatted);
  expect(analyzed.canGenerateCode(),
         "formatted comparison chains should remain valid GTI source");

  const std::string formattedConcept = lang::Formatter().format(
      "concept sortable<T>=std::totally_ordered<T>&&std::movable<T>;");
  expect(formattedConcept == "concept sortable<T> = std::totally_ordered<T> && "
                             "std::movable<T>;\n",
         "formatter should still disambiguate a compact concept generic close "
         "from its assignment token");

  // An incomplete directive marker followed by a word (recovered from invalid
  // input) must keep its separator: gluing '#' to 'case' would re-lex as the
  // single directive token '#case', flipping the following colon's spacing on
  // the next pass and breaking idempotence. Every such directive+word pair
  // must format to a fixed point.
  for (const std::string source :
       {"# case:", "# case :", "#\tcase:", "# default:", "# return :"}) {
    const std::string first = lang::Formatter().format(source);
    expect(lang::Formatter().format(first) == first,
           "formatting a directive marker glued to a word must be idempotent");
    expect(first.find("#case") == std::string::npos,
           "the formatter must not merge a lone '#' with a following word");
  }
}

void testIntegerBitwiseAndModuloOperators() {
  lang::Lexer lexer;

  lang::Parser bitwisePrecedence(lexer.scan("1 | 2 ^ 3 & 4"));
  lang::ExprPtr bitwiseExpression = bitwisePrecedence.parseExpression();
  expect(bitwiseExpression != nullptr &&
             lang::AstPrinter().print(*bitwiseExpression) ==
                 "(| 1 (^ 2 (& 3 4)))",
         "bitwise operators should follow C++ precedence");

  lang::Parser shiftPrecedence(lexer.scan("1 < 2 << 3 + 4 % 2"));
  lang::ExprPtr shiftExpression = shiftPrecedence.parseExpression();
  expect(shiftExpression != nullptr &&
             lang::AstPrinter().print(*shiftExpression) ==
                 "(< 1 (<< 2 (+ 3 (% 4 2))))",
         "modulo and shifts should integrate with arithmetic precedence");

  lang::Parser unaryPrecedence(lexer.scan("~1 * 2"));
  lang::ExprPtr unaryExpression = unaryPrecedence.parseExpression();
  expect(unaryExpression != nullptr &&
             lang::AstPrinter().print(*unaryExpression) == "(* (~ 1) 2)",
         "bitwise complement should bind as a unary operator");

  lang::Parser separatedShift(lexer.scan("1 > > 2"));
  expect(separatedShift.parseExpression() == nullptr &&
             separatedShift.hadError(),
         "spaced angle tokens should not become a shift operator");

  const std::string validSource = R"(
int combine(int left, int right) {
  return ((left & right) | (left ^ right)) % 17;
}

int shift_small(uint8_t value) { return (value << 3) >> 1; }
int64_t mix_widths(int64_t left, uint32_t right) { return left & right; }
uint64_t unsigned_bits(uint64_t left, uint64_t right) { return left | right; }

int main() {
  int8_t small = 3;
  int promoted = ~small;
  int flags = ((5 & 3) | 8) ^ 2;
  int shifted = (flags << 2) >> 1;
  int remainder = combine(shifted, 5);
  int wrapped = 1 << 31;
  if (promoted == -4 and remainder == 6 and wrapped == -2147483648) {
    return 0;
  }
  return 1;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "integer bitwise source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "integer bitwise and modulo operators should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected integer operator diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "valid integer bitwise and modulo operations should type-check");

  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("::gti_internal::backend::mir_checked_remainder_v1") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_shift_left_v1") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_shift_right_v1") !=
                 std::string::npos,
         "modulo and shifts should lower through checked integer helpers");
  expect(generated.find(" & __gti_mir_v_") != std::string::npos &&
             generated.find(" ^ __gti_mir_v_") != std::string::npos &&
             generated.find(" | __gti_mir_v_") != std::string::npos,
         "ordinary bitwise operators should lower directly");
  expect(generated.find("modulo by zero") != std::string::npos &&
             generated.find("std::bit_cast") != std::string::npos,
         "generated helpers should define invalid modulo and shift behavior");

  auto invalidTokens = lexer.scan(R"(
float decimal = 1.0;
bool condition = true;
int invalid_modulo = decimal % 2;
int invalid_and = condition & true;
int invalid_shift = 1 << decimal;
int invalid_complement = ~decimal;
int zero_modulo = 7 % 0;
int negative_shift = 1 << -1;
int wide_shift = 1 >> 32;
int32_t signed_value = 1;
uint32_t unsigned_value = 1;
int unsafe_bits = signed_value | unsigned_value;
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid integer operator types should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid integer operators should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "requires integer operands") &&
             hasDiagnostic(invalidSemantic,
                           "Bitwise complement requires an integer"),
         "floats and bools should not gain bitwise behavior");
  expect(hasDiagnostic(invalidSemantic, "Modulo divisor cannot be zero"),
         "literal modulo by zero should be rejected before lowering");
  expect(hasDiagnostic(invalidSemantic, "Shift count cannot be negative") &&
             hasDiagnostic(invalidSemantic, "Shift count must be less than 32"),
         "invalid literal shift counts should be diagnosed");
  expect(hasDiagnostic(invalidSemantic, "no safe common type"),
         "bitwise operations should preserve safe signed/unsigned rules");

  const std::string formatted = lang::Formatter().format(
      "int value=(mask&3)|((mask^1)<<2);int mod=value%7;"
      "int inv=~value;int shifted=value>>1;");
  expect(formatted == "int value = (mask & 3) | ((mask ^ 1) << 2);\n"
                      "int mod = value % 7;\n"
                      "int inv = ~value;\n"
                      "int shifted = value >> 1;\n",
         "formatter should use C++ spacing for integer operators");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted integer operators should be idempotent");
}

void testCheckedArithmeticAndCompoundAssignments() {
  lang::Lexer lexer;
  const std::vector<lang::Token> operators =
      lexer.scan("+= -= *= /= %= &= |= ^= <<= >>=");
  expect(!lexer.hadError() && operators.size() == 11 &&
             operators[0].kind == lang::TokenKind::PLUS_EQUAL &&
             operators[1].kind == lang::TokenKind::MINUS_EQUAL &&
             operators[2].kind == lang::TokenKind::STAR_EQUAL &&
             operators[3].kind == lang::TokenKind::SLASH_EQUAL &&
             operators[4].kind == lang::TokenKind::PERCENT_EQUAL &&
             operators[5].kind == lang::TokenKind::AMPERSAND_EQUAL &&
             operators[6].kind == lang::TokenKind::PIPE_EQUAL &&
             operators[7].kind == lang::TokenKind::CARET_EQUAL &&
             operators[8].kind == lang::TokenKind::SHIFT_LEFT_EQUAL &&
             operators[9].kind == lang::TokenKind::SHIFT_RIGHT_EQUAL,
         "the lexer should retain every compound assignment as one token");

  lang::Parser expressionParser(lexer.scan("value *= 2"));
  lang::ExprPtr expression = expressionParser.parseExpression();
  expect(expression != nullptr && !expressionParser.hadError() &&
             lang::AstPrinter().print(*expression) == "(*= value 2)",
         "compound assignment should parse as a right-associative assignment");

  const lang::FrontendResult valid =
      lang::Frontend().analyze("checked-arithmetic.gti", R"(
struct Meter {
  mut int value = 1;
};

int calculate(int left, int right) {
  return ((left + right) * (left - right)) / right;
}

int folded_arithmetic() { return ((40 + 2) * 3 - 42) / 2; }
bool checked_addition() { return 2147483647 + 1 == 0; }
bool checked_unsigned_subtraction() {
  return 1 - 18446744073709551615 == 0;
}
bool checked_negation() { return -(-9223372036854775808) == 0; }

int main() {
  mut int value = 6;
  value *= 7;
  value /= 6;
  value %= 5;
  value &= 3;
  value |= 8;
  value ^= 1;
  value <<= 2;
  value >>= 1;
  value += 2;
  value -= 2;

  mut int values[2] = {3, 4};
  mut int index = 0;
  values[index++] *= 2;
  mut Meter meter{};
  meter.value += 2;
  int negative = -1;
  int arithmetic = calculate(7, 2);
  if (value == 22 and index == 1 and values[0] == 6 and
      meter.value == 3 and negative == -1 and arithmetic == 22) {
    return 0;
  }
  return 1;
}
)");
  if (!valid.diagnostics.empty()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected checked-arithmetic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "checked arithmetic and every compound assignment should validate");

  const std::string generated = gti_test::emitCppText(valid);
  const std::vector<std::string_view> checkedHelpers = {
      "backend::mir_checked_add_v1<",
      "backend::mir_checked_subtract_v1<",
      "backend::mir_checked_multiply_v1<",
      "backend::mir_checked_divide_v1<",
      "backend::mir_checked_remainder_v1<",
      "backend::mir_checked_shift_left_v1<",
      "backend::mir_checked_shift_right_v1<",
      "backend::mir_checked_negate_v1<",
  };
  expect(std::all_of(checkedHelpers.begin(), checkedHelpers.end(),
                     [&](std::string_view helper) {
                       return generated.find(helper) != std::string::npos;
                     }) &&
             generated.find(" & __gti_mir_v_") != std::string::npos &&
             generated.find(" | __gti_mir_v_") != std::string::npos &&
             generated.find(" ^ __gti_mir_v_") != std::string::npos,
         "arithmetic and mutation should lower through checked GTI helpers");
  expect(generated.find("integer addition overflow") != std::string::npos &&
             generated.find("integer subtraction overflow") !=
                 std::string::npos &&
             generated.find("integer multiplication overflow") !=
                 std::string::npos &&
             generated.find("integer division overflow") != std::string::npos &&
             generated.find("division by zero") != std::string::npos,
         "generated arithmetic helpers should carry stable failure reasons");

  const lang::OptimizationResult optimized =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O1);
  const lang::HirFunctionInstance *negationInstance = nullptr;
  const lang::HirFunctionInstance *additionInstance = nullptr;
  const lang::HirFunctionInstance *unsignedSubtractionInstance = nullptr;
  const lang::HirFunctionInstance *foldedArithmeticInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "checked_negation") {
      negationInstance = &instance;
    } else if (instance.source->name().lexeme == "checked_addition") {
      additionInstance = &instance;
    } else if (instance.source->name().lexeme ==
               "checked_unsigned_subtraction") {
      unsignedSubtractionInstance = &instance;
    } else if (instance.source->name().lexeme == "folded_arithmetic") {
      foldedArithmeticInstance = &instance;
    }
  }
  const bool retainsCheckedNegation =
      negationInstance != nullptr &&
      std::any_of(negationInstance->body.values.begin(),
                  negationInstance->body.values.end(),
                  [&](const lang::HirValue &value) {
                    return value.kind == lang::HirValueKind::Unary &&
                           value.operation == lang::TokenKind::MINUS &&
                           optimized.replacement(value.id) == nullptr;
                  });
  expect(retainsCheckedNegation,
         "constant folding must not erase a signed negation overflow trap");
  const auto retainsCheckedBinary =
      [&](const lang::HirFunctionInstance *instance,
          lang::TokenKind operation) {
        return instance != nullptr &&
               std::any_of(instance->body.values.begin(),
                           instance->body.values.end(),
                           [&](const lang::HirValue &value) {
                             return value.kind == lang::HirValueKind::Binary &&
                                    value.operation == operation &&
                                    optimized.replacement(value.id) == nullptr;
                           });
      };
  expect(retainsCheckedBinary(additionInstance, lang::TokenKind::PLUS) &&
             retainsCheckedBinary(unsignedSubtractionInstance,
                                  lang::TokenKind::MINUS),
         "constant folding must preserve signed overflow and unsigned "
         "underflow operations");

  bool foldsInRangeArithmetic = false;
  if (foldedArithmeticInstance != nullptr) {
    for (const lang::HirValue &value : foldedArithmeticInstance->body.values) {
      if (value.kind != lang::HirValueKind::Binary ||
          value.operation != lang::TokenKind::SLASH) {
        continue;
      }
      const lang::ConstantValue *replacement = optimized.replacement(value.id);
      const auto *integer =
          replacement == nullptr
              ? nullptr
              : std::get_if<lang::IntegerConstant>(replacement);
      foldsInRangeArithmetic =
          integer != nullptr && !integer->negative && integer->magnitude == 42;
    }
  }
  expect(foldsInRangeArithmetic,
         "bounded constant evaluation should fold proven in-range arithmetic");

  const lang::MirBody *mainBody = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "main") {
      continue;
    }
    const lang::MirFunctionInstance *lowered =
        valid.mir.findFunctionInstance(instance.id);
    mainBody = lowered == nullptr ? nullptr : &lowered->body;
    break;
  }
  const auto hasOperation = [&](lang::MirOperation operation) {
    return mainBody != nullptr &&
           std::any_of(mainBody->blocks.begin(), mainBody->blocks.end(),
                       [&](const lang::MirBlock &block) {
                         return std::any_of(
                             block.instructions.begin(),
                             block.instructions.end(),
                             [&](const lang::MirInstruction &instruction) {
                               return instruction.operation == operation;
                             });
                       });
  };
  // Each same-domain compound mutation lowers to its ordered schedule, so the
  // body retains the exact arithmetic operation plus an assignment commit.
  expect(hasOperation(lang::MirOperation::Multiply) &&
             hasOperation(lang::MirOperation::Divide) &&
             hasOperation(lang::MirOperation::Remainder) &&
             hasOperation(lang::MirOperation::BitwiseAnd) &&
             hasOperation(lang::MirOperation::BitwiseOr) &&
             hasOperation(lang::MirOperation::BitwiseXor) &&
             hasOperation(lang::MirOperation::ShiftLeft) &&
             hasOperation(lang::MirOperation::ShiftRight) &&
             hasOperation(lang::MirOperation::Assign),
         "MIR should lower each compound mutation to its ordered "
         "read/operate/commit schedule");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-checked-arithmetic.gti", R"(
int main() {
  mut int integer = 1;
  mut float decimal = 1.0;
  mut bool condition = true;
  mut int32_t signed_value = 1;
  mut uint32_t unsigned_value = 1;
  integer += 1.5;
  integer /= 0;
  integer %= 0;
  integer <<= -1;
  integer >>= 32;
  decimal %= 2;
  condition &= true;
  signed_value *= unsigned_value;
  int zero = 7 / 0;
  return 0;
}
)");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot use a floating-point right operand") &&
          hasDiagnostic(invalid.diagnostics,
                        "Integer division by zero is not allowed") &&
          hasDiagnostic(invalid.diagnostics, "Modulo divisor cannot be zero") &&
          hasDiagnostic(invalid.diagnostics,
                        "Shift count cannot be negative") &&
          hasDiagnostic(invalid.diagnostics,
                        "Shift count must be less than 32") &&
          hasDiagnostic(invalid.diagnostics, "requires integer operands") &&
          hasDiagnostic(invalid.diagnostics, "no safe common type") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "Convert the right operand explicitly"),
      "invalid compound domains and statically known arithmetic failures "
      "should be rejected by semantics");

  const std::string formatted = lang::Formatter().format(
      "void update(mut int value){value*=2;value/=2;value%=3;value&=1;"
      "value|=2;value^=3;value<<=1;value>>=1;}");
  expect(formatted.find("value *= 2;") != std::string::npos &&
             formatted.find("value /= 2;") != std::string::npos &&
             formatted.find("value %= 3;") != std::string::npos &&
             formatted.find("value &= 1;") != std::string::npos &&
             formatted.find("value |= 2;") != std::string::npos &&
             formatted.find("value ^= 3;") != std::string::npos &&
             formatted.find("value <<= 1;") != std::string::npos &&
             formatted.find("value >>= 1;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should space every compound operator idempotently");
}

void testMirConstantEvaluation() {
  lang::FrontendWorkMetrics metrics;
  const auto frontend = lang::Frontend({.workMetrics = &metrics})
                            .analyze("mir-constexpr.gti", R"(
constexpr int32_t sum(int32_t count) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < count; index++) { total += index; }
  return total;
}
constexpr int32_t factorial(int32_t count) {
  if (count <= 1) { return 1; }
  return count * factorial(count - 1);
}
constexpr int32_t call_sum(int32_t count) { return sum(count); }
constexpr int table_sum(int seed) {
  mut int values[3] = {seed, seed + 1, seed + 2};
  values[1] += 4;
  mut int total = 0;
  for (mut uint64_t index = 0; index < 3; index++) { total += values[index]; }
  return total;
}
constexpr int array_defaults(int seed) {
  mut int values[3] = {};
  values[1] = seed;
  values[1]++;
  return values[0] + values[1] + values[2];
}
constexpr int array_bounds(int index) {
  int values[3] = {1, 2, 3};
  return values[index];
}
constexpr int array_write_bounds(int index) {
  mut int values[3] = {};
  values[index] = 4;
  return values[0];
}
constexpr int nested_bounds(int row) {
  int values[2][2] = {};
  return values[row][1 / (row - row)];
}
constexpr int large_array(int seed) {
  int values[10000] = {};
  return values[seed];
}
constexpr uint64_t checksum(std::string_view text) {
  mut uint64_t result = 0;
  for (mut uint64_t index = 0; index < text.size(); index++) {
    result += uint8_t(text[index]);
  }
  return result;
}
constexpr double half(double input) { return input / 2; }
constexpr bool folded(int32_t input) { return 2 == 2 || input > 0; }
constexpr T twice<std::numeric T>(T input) { return T(input + input); }
constexpr int32_t divide(int32_t input) { return 100 / input; }
constexpr int32_t endless(int32_t count) {
  while (count > 0) {}
  return count;
}
constexpr int32_t recursion(int32_t count) { return recursion(count); }
int32_t runtime(int32_t input) { return input; }
constexpr int32_t lazy(bool selected) {
  if (selected) { return 42; }
  return runtime(0);
}
constexpr int32_t sum_result = sum(5);
constexpr int32_t factorial_result = factorial(6);
constexpr uint64_t text_result = checksum("a\0é");
static_assert(sum_result == 10 && factorial_result == 720 && text_result == 461);
static_assert(table_sum(5) == 22 && array_defaults(4) == 5);
int main() { return twice<int32_t>(21) == 42 ? 0 : 1; }
)",
                                     {standardLibraryPrelude()});
  expect(frontend.canGenerateCode(), "MIR constexpr fixture should lower");
  expect(metrics.constexprMirExecutions >= 5,
         "semantic constant calls actually use staged MIR before whole-program "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  const auto functionId = [&](std::string_view name) {
    for (const auto &function : frontend.hir.functionInstances()) {
      if (function.source != nullptr && function.source->name().lexeme == name)
        return function.id;
    }
    return lang::HirFunctionInstanceId{0};
  };
  const auto integer = [](std::uint64_t value) -> lang::ConstantValue {
    return lang::makeConstantInteger({.magnitude = value},
                                     {.width = 32, .signedValue = true});
  };
  const auto evaluate = [&](std::string_view name, lang::ConstantValue argument,
                            lang::MirConstantLimits limits = {}) {
    const std::array arguments{argument};
    auto result = lang::evaluateMirConstantFunction(
        frontend.mir, functionId(name), arguments, limits);
    return result;
  };
  for (const auto &[name, input, expected] :
       {std::tuple{"sum", 5, 10}, std::tuple{"factorial", 6, 720},
        std::tuple{"table_sum", 5, 22}, std::tuple{"array_defaults", 4, 5}}) {
    const auto result = evaluate(name, integer(input));
    if (!result)
      std::cerr << name << ": " << result.detail << " at " << result.block
                << ':' << result.instruction << '\n';
    expect(result && result.value == integer(expected),
           "canonical MIR execution matches frontend scalar constants");
  }
  for (const auto name : {"array_bounds", "array_write_bounds"}) {
    const auto bounds = evaluate(name, integer(3));
    expect(!bounds &&
               bounds.failure ==
                   lang::ConstantEvaluationFailure::InvalidOperands &&
               bounds.instruction != 0,
           "MIR array reads and writes reject out-of-range indices at the "
           "operation");
    const auto negative = evaluate(
        name, lang::makeConstantInteger({.negative = true, .magnitude = 1},
                                        {.width = 32, .signedValue = true}));
    expect(!negative && negative.failure ==
                            lang::ConstantEvaluationFailure::InvalidOperands,
           "MIR array reads and writes reject negative indices");
  }
  const auto earlyBounds = evaluate("nested_bounds", integer(2));
  const auto laterDivision = evaluate("nested_bounds", integer(0));
  expect(!earlyBounds &&
             earlyBounds.failure ==
                 lang::ConstantEvaluationFailure::InvalidOperands &&
             !laterDivision &&
             laterDivision.failure ==
                 lang::ConstantEvaluationFailure::DivisionByZero,
         "constant array prefixes fail before later index effects using the "
         "same canonical checked-place schedule as runtime");
  const auto *nestedBody =
      frontend.mir.findFunctionInstance(functionId("nested_bounds"));
  bool checkedAddress = false;
  if (nestedBody != nullptr) {
    for (const auto &block : nestedBody->body.blocks)
      for (const auto &instruction : block.instructions)
        checkedAddress |=
            instruction.id == earlyBounds.instruction &&
            instruction.operation == lang::MirOperation::AddressOf &&
            instruction.result &&
            nestedBody->body.usesOf(*instruction.result).empty();
  }
  expect(checkedAddress,
         "constant prefix failures identify the unused checked address stage "
         "without creating a constant raw pointer");
  const auto large = evaluate("large_array", integer(0));
  expect(!large &&
             large.failure == lang::ConstantEvaluationFailure::ResourceLimit,
         "default array materialization is bounded before allocating storage");
  lang::FrontendWorkMetrics arrayMetrics;
  const auto arrayProgram =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &arrayMetrics})
          .analyze("early-array-constant.gti", R"(
constexpr int array_copy(int seed) {
  mut int matrix[2][3] = {};
  matrix[1][2] = seed;
  mut int copy[2][3] = matrix;
  copy[1][2]++;
  matrix[0] = {1, 2, 3};
  return matrix[1][2] + copy[1][2] + matrix[0][2];
}
constexpr int array_argument(int values[3]) {
  mut int copy[3] = values;
  copy[0]++;
  return copy[0] + values[0];
}
constexpr int table<uint64_t N>(int seed) {
  mut int values[N] = {};
  values[values.size() - 1] = seed;
  return values[values.size() - 1];
}
static_assert(array_copy(5) == 14);
static_assert(array_argument({2, 3, 4}) == 5);
static_assert(table<3>(7) == 7);
)");
  expect(arrayProgram.semanticValid &&
             arrayMetrics.constexprMirExecutions == 3 && !arrayProgram.hirValid,
         "nested array mutation, copy isolation, array arguments and generic "
         "extents execute early MIR");
  for (const bool write : {false, true}) {
    lang::FrontendWorkMetrics boundsMetrics;
    const std::string source =
        "constexpr int bounds(int index) { mut int values[3] = {}; " +
        std::string(write ? "values[index] = 7; return 0; }\n"
                          : "return values[index]; }\n") +
        "constexpr int bad = bounds(3);\n";
    const auto invalid = lang::Frontend({.workMetrics = &boundsMetrics})
                             .analyze("constant-array-bounds.gti", source);
    const auto *diagnostic =
        findDiagnosticByCode(invalid.diagnostics, "GTI-S2057");
    expect(!invalid.semanticValid &&
               boundsMetrics.constexprMirExecutions == 1 &&
               diagnostic != nullptr && diagnostic->primary.line == 1,
           "constant array bounds errors reject at the executing body, without "
           "AST fallback");
  }
  const auto text = evaluate("checksum", std::string("a\0é", 4));
  if (!text)
    std::cerr << "checksum: " << text.detail << " at " << text.block << ':'
              << text.instruction << '\n';
  expect(text && lang::constantUnsignedMagnitude(*text.value) == 461,
         "MIR text inspection shares byte semantics with the early kernel");
  const auto lazy = evaluate("lazy", true);
  const auto generic = evaluate("twice", integer(21));
  expect(generic && generic.value == integer(42),
         "MIR constant calls retain concrete generic substitution and "
         "conversions");
  const auto floating =
      lang::convertConstantFloat(integer(3), lang::BinaryFloatFormat::Binary64);
  const auto half = evaluate("half", *floating.value);
  expect(half && std::get<lang::BinaryFloat>(*half.value).bits ==
                     0x3ff8000000000000ULL,
         "MIR constant floating arithmetic uses the shared target binary64 "
         "kernel");
  const auto runtime = evaluate("lazy", false);
  expect(lazy && lazy.value == integer(42) && !runtime &&
             runtime.failure ==
                 lang::ConstantEvaluationFailure::UnsupportedExpression,
         "constant eligibility follows executed paths but never invokes "
         "runtime code");
  const auto zero = evaluate("divide", integer(0));
  expect(!zero &&
             zero.failure == lang::ConstantEvaluationFailure::DivisionByZero &&
             zero.function == functionId("divide") && zero.block != 0 &&
             zero.instruction != 0,
         "MIR constant arithmetic failure retains exact typed instruction "
         "provenance");
  const auto overflow = evaluate("factorial", integer(13));
  expect(!overflow && overflow.failure ==
                          lang::ConstantEvaluationFailure::IntegerOverflow,
         "MIR constant arithmetic preserves target integer overflow");
  const auto loop = evaluate("endless", integer(1), {.steps = 100});
  const auto recursion = evaluate("recursion", integer(1), {.callDepth = 4});
  expect(
      !loop && loop.failure == lang::ConstantEvaluationFailure::ResourceLimit &&
          !recursion &&
          recursion.failure == lang::ConstantEvaluationFailure::ResourceLimit,
      "all executed MIR blocks and nested calls share bounded resources");
  const auto badArgument = evaluate("sum", true);
  expect(!badArgument && badArgument.failure ==
                             lang::ConstantEvaluationFailure::InvalidOperands,
         "MIR evaluator requires exactly typed arguments");
  lang::MirProgram malformed = frontend.mir;
  const_cast<lang::MirFunctionInstance *>(
      malformed.findFunctionInstance(functionId("sum")))
      ->body.entry = 0;
  const std::array arguments{integer(5)};
  const auto rejected = lang::evaluateMirConstantFunction(
      malformed, functionId("sum"), arguments);
  expect(!rejected && rejected.detail.find("verified MIR") != std::string::npos,
         "ordinary MIR verification runs before const execution");
  const auto optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1});
  const auto transformed = lang::evaluateMirConstantFunction(
      optimized.mir, functionId("folded"), arguments);
  expect(optimized.valid() && !transformed &&
             transformed.detail.find("canonical unoptimized MIR") !=
                 std::string::npos,
         "const eligibility cannot depend on optimizer rewriting");

  lang::FrontendWorkMetrics earlyMetrics;
  const auto early =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &earlyMetrics})
          .analyze("early-constexpr.gti", R"(
constexpr uint64_t extent(uint64_t input) { return input + 1; }
constexpr int choose(int value) {
  switch (value) { case -4: return 11; default: return 22; }
}
constexpr int choose_char(char value) {
  switch (value) { case 'x': return 11; default: return 22; }
}
constexpr uint64_t capacity = extent(uint64_t(3));
int values[capacity] = {};
int selected() {
  if constexpr (extent(uint64_t(2)) == 3) { return 42; }
  else { return name_that_must_not_be_analyzed; }
}
static_assert(extent(uint64_t(41)) == 42);
static_assert(choose(-4) == 11 && choose(0) == 22);
static_assert(choose_char('x') == 11 && choose_char('y') == 22);
int later() { return selected(); }
)");
  expect(early.semanticValid && earlyMetrics.constexprMirExecutions >= 7 &&
             !early.hirValid,
         "staged execution forms types and selects branches before normal HIR "
         "exists");
  if (!early.semanticValid || earlyMetrics.constexprMirExecutions < 7) {
    std::cerr << "Early MIR executions: " << earlyMetrics.constexprMirExecutions
              << '\n';
    for (const auto &diagnostic : early.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }

  // A completed-body closure never publishes unfinished unrelated functions,
  // and its deliberately absent whole-program seal must remain non-publishable.
  lang::SemanticVisitor provider(lang::TargetInfo::host(),
                                 &frontend.sourceGraph, false, nullptr,
                                 &frontend.sources);
  expect(provider.check(frontend.program),
         "closure provider checks the fixture");
  std::vector<lang::FunctionId> completed;
  lang::FunctionId sumDeclaration = 0;
  lang::FunctionId callerDeclaration = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source == nullptr)
      continue;
    const auto *declaration = provider.model().findFunction(*function.source);
    if (declaration == nullptr)
      continue;
    if (function.source->name().lexeme == "sum")
      sumDeclaration = declaration->id;
    if (function.source->name().lexeme == "call_sum")
      callerDeclaration = declaration->id;
    if (function.source->isConstexpr())
      completed.push_back(declaration->id);
  }
  lang::HirLowerer stagedLowerer;
  auto staged = stagedLowerer.lowerConstantFunction(provider.model(), provider,
                                                    sumDeclaration, completed);
  expect(staged.valid() && staged.program.functionInstances().size() == 1 &&
             !lang::verifyHirProgramPlans(provider.model(), staged.program)
                  .valid(),
         "staged closure excludes unrelated bodies and cannot pass "
         "whole-program publication");
  const auto *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  const auto stagedMetadata = lang::FailureMetadataBuilder().build(
      frontend.sourceGraph, frontend.sources, staged.program, entry->path);
  const auto stagedMir =
      lang::MirLowerer().lower(staged.program, stagedMetadata.metadata);
  expect(stagedMetadata.valid() && stagedMir.valid() &&
             lang::verifyMirProgram(stagedMir.program).valid(),
         "staged bodies retain ordinary failure metadata and MIR verification");
  const auto publication = lang::LoweredProgramBuilder().build(
      frontend.program, provider.model(), staged.program, stagedMir.program,
      stagedMir.program, {}, lang::TargetInfo::host());
  expect(!publication.valid() && !publication.program.has_value(),
         "valid staged MIR cannot be sealed as a whole-program backend input");
  auto unavailable = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, sumDeclaration, {});
  expect(!unavailable.valid(),
         "a registered but incomplete root cannot be staged");
  auto incompleteCallee = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, callerDeclaration, {callerDeclaration});
  expect(!incompleteCallee.valid(), "a completed root cannot stage a callee "
                                    "absent from the completed inventory");
  auto completeClosure = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, callerDeclaration,
      {callerDeclaration, sumDeclaration});
  expect(completeClosure.valid() &&
             completeClosure.program.functionInstances().size() == 2,
         "completed callee discovery closes the ordinary staged function "
         "worklist");

  for (const auto &instance : frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.source->name().lexeme != "twice")
      continue;
    const auto *declaration = provider.model().findFunction(*instance.source);
    expect(declaration != nullptr && declaration->genericParameters.size() == 1,
           "generic stage fixture retains its semantic parameter identity");
    if (declaration == nullptr || declaration->genericParameters.size() != 1)
      continue;
    const auto id = declaration->id;
    const auto parameter = declaration->genericParameters.front().id;
    const lang::GenericSubstitution substitution{
        .types = {{parameter, lang::SemanticType::Int32}}};
    const auto genericStage = stagedLowerer.lowerConstantFunction(
        provider.model(), provider, id, completed, substitution);
    const auto genericMetadata = lang::FailureMetadataBuilder().build(
        frontend.sourceGraph, frontend.sources, genericStage.program,
        entry->path);
    const auto genericMir = lang::MirLowerer().lower(genericStage.program,
                                                     genericMetadata.metadata);
    const auto evaluated =
        lang::evaluateMirConstantFunction(genericMir.program, 1, arguments);
    expect(genericStage.valid() && genericMetadata.valid() &&
               genericMir.valid() && evaluated &&
               evaluated.value == integer(10),
           "generic HIR staging independently executes a concrete MIR body "
           "without frontend fallback");
    const auto missing = stagedLowerer.lowerConstantFunction(
        provider.model(), provider, id, completed);
    expect(!missing.valid(),
           "staged generic roots require exact concrete substitutions");
    break;
  }

  lang::FrontendWorkMetrics failureMetrics;
  const std::string failingSource =
      "constexpr int divide(int value) { return 100 / value; }\n"
      "constexpr int result = divide(0);\n";
  const auto failing =
      lang::Frontend({.workMetrics = &failureMetrics})
          .analyze("staged-constant-failure.gti", failingSource);
  const auto *failure = findDiagnosticByCode(failing.diagnostics, "GTI-S2057");
  expect(!failing.semanticValid && failureMetrics.constexprMirExecutions == 1 &&
             failure != nullptr &&
             failure->phase == lang::DiagnosticPhase::Semantics &&
             failure->primary.source == std::filesystem::weakly_canonical(
                                            "staged-constant-failure.gti")
                                            .string() &&
             failure->primary.start == failingSource.find('/') &&
             failure->primary.end == failingSource.find('/') + 1 &&
             failure->fixes.empty(),
         "MIR arithmetic failure is authoritative and retains its exact source "
         "operator");

  const auto compatibility =
      lang::Frontend().analyze("constant-profile-compatibility.gti", R"(
enum class Mode { First, Second };
constexpr bool is_first(Mode mode) { return mode == Mode::First; }
constexpr int select(int value) {
  switch (value) { case 1: return 11; default: return 22; }
}
static_assert(is_first(Mode::First));
static_assert(!is_first(Mode::Second));
static_assert(select(1) == 11 && select(0) == 22);
)");
  expect(compatibility.semanticValid,
         "unsupported MIR profile types and control flow preserve the existing "
         "constexpr subset");

  lang::FrontendWorkMetrics genericMetrics;
  const auto stagedGeneric =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &genericMetrics})
          .analyze("early-generic-constant.gti", R"(
constexpr T twice<std::numeric T>(T value) { return T(value + value); }
constexpr T scale<std::numeric T, uint64_t N>(T value) {
  mut T result = T(0);
  for (mut uint64_t index = 0; index < N; index++) { result = T(result + value); }
  return twice<T>(result);
}
constexpr int indirect(int value) { return scale<int, 3>(value); }
constexpr int first = scale<int, 3>(7);
constexpr double second = scale<double, 2>(double(1.5));
constexpr int third = indirect(2);
constexpr int zero = scale<int, 0>(5);
static_assert(first == 42 && second == double(6.0) && third == 12 && zero == 0);
int enclosing(int value) {
  mut int local = value;
  constexpr int bound = scale<int, 2>(2);
  local += bound;
  if constexpr (twice<int>(3) == 6) { return local; }
  else { return should_not_exist; }
}
)",
                   {standardLibraryPrelude()});
  if (!stagedGeneric.semanticValid ||
      genericMetrics.constexprMirExecutions < 6) {
    std::cerr << "Generic MIR executions: "
              << genericMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : stagedGeneric.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  expect(stagedGeneric.semanticValid &&
             genericMetrics.constexprMirExecutions >= 6,
         "concrete type/value generic closures execute MIR without corrupting "
         "enclosing scopes");

  lang::FrontendWorkMetrics loanMetrics;
  const auto loanIsolation = lang::Frontend({.workMetrics = &loanMetrics})
                                 .analyze("constexpr-loan-isolation.gti", R"(
constexpr T twice<std::numeric T>(T value) { return T(value + value); }
int invalid(int input) {
  mut int local = input;
  int& borrowed = local;
  constexpr int amount = twice<int>(1);
  local += amount;
  return borrowed;
}
)",
                                          {standardLibraryPrelude()});
  expect(!loanIsolation.semanticValid &&
             loanMetrics.constexprMirExecutions == 1 &&
             hasDiagnostic(loanIsolation.diagnostics, "borrow"),
         "staged concrete reanalysis cannot erase an enclosing live loan");

  const auto invalidInstance =
      lang::Frontend().analyze("constexpr-invalid-instance.gti", R"(
constexpr int invalid<uint64_t N>(int input) {
  static_assert(N == 0, "only zero is valid");
  return input;
}
constexpr int good = invalid<0>(7);
constexpr int bad = invalid<1>(7);
)");
  expect(!invalidInstance.semanticValid &&
             hasDiagnostic(invalidInstance.diagnostics, "only zero is valid"),
         "concrete semantic errors must not fall back to permissive AST "
         "execution");
}

void testDependentConstexprBranches() {
  const auto nestedSourceQuery =
      lang::Frontend().analyze("constexpr-source-query.gti", R"(
constexpr bool selected<uint64_t N>() {
  if constexpr (N == 0) { return true; }
  else { return selected<N - 1>(); }
}
int query<uint64_t N>() {
  if constexpr (selected<N>()) { return 7; }
  else { return 0; }
}
static_assert(selected<3>());
int main() { return query<3>() - 7; }
)");
  expect(nestedSourceQuery.canGenerateCode() &&
             nestedSourceQuery.diagnostics.empty(),
         "source access queries and nested staged constants share bounded "
         "request accounting without rejecting finite generic recursion");
  const auto cyclicSourceQuery =
      lang::Frontend().analyze("constexpr-source-query-cycle.gti", R"(
constexpr bool cycle<uint64_t N>() {
  if constexpr (cycle<N>()) { return true; }
  else { return false; }
}
static_assert(cycle<0>());
int main() { return 0; }
)");
  expect(!cyclicSourceQuery.canGenerateCode() &&
             hasDiagnosticCode(cyclicSourceQuery.diagnostics, "GTI-S2057") &&
             hasDiagnostic(cyclicSourceQuery.diagnostics, "limit"),
         "alternating source queries and constexpr staging reject cycles "
         "through the existing resource diagnostic rather than exhausting "
         "the compiler stack");
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constexpr_dependent_branches.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  lang::FrontendWorkMetrics metrics;
  const auto result = lang::Frontend({.workMetrics = &metrics})
                          .analyze(fixture, source, {standardLibraryPrelude()},
                                   {}, {standardLibraryRoot()});
  if (!result.canGenerateCode() || !result.diagnostics.empty()) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.primary.line << ": " << d.message << '\n';
  }
  expect(!source.empty() && result.canGenerateCode() &&
             result.diagnostics.empty() && metrics.constexprMirExecutions >= 10,
         "dependent constexpr selection instantiates only selected arms, "
         "including nested early calls, recursive generic calls, constructors "
         "and restored owners");

  // An enum named only by a generic call's type arguments still needs its
  // exact catalog row in the staged closure. Without one, MIR verification
  // rejected the closure and the AST fallback could not select the branch.
  lang::FrontendWorkMetrics enumMetrics;
  const auto enumArguments =
      lang::Frontend({.workMetrics = &enumMetrics})
          .analyze("constexpr-enum-type-arguments.gti", R"(
struct underlying_probe<E, U> {
  bool matches() requires std::enum_underlying<E, U> { return true; }
};
struct plain_probe<T> {
  bool matches() { return true; }
};
enum class small : uint8_t { low, high };
enum class wide : uint16_t { low, high };
enum envelope<T> { empty, held(T value) };
constexpr int width<E>() {
  if constexpr (requires(underlying_probe<E, uint8_t>& probe) {
                  { probe.matches() } -> bool;
                }) {
    return 8;
  } else if constexpr (requires(underlying_probe<E, uint16_t>& probe) {
                         { probe.matches() } -> bool;
                       }) {
    return 16;
  }
  return 0;
}
constexpr int plain<T>() {
  if constexpr (requires(plain_probe<T>& probe) {
                  { probe.matches() } -> bool;
                }) {
    return 1;
  }
  return 0;
}
static_assert(width<small>() == 8);
static_assert(width<wide>() == 16);
static_assert(width<int32_t>() == 0);
static_assert(width<envelope<small>>() == 0);
static_assert(plain<small>() == 1);
int main() {
  return width<small>() + width<wide>() + width<int32_t>() +
         width<envelope<small>>() + plain<small>() - 25;
}
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!enumArguments.canGenerateCode() || !enumArguments.diagnostics.empty())
    for (const auto &d : enumArguments.diagnostics)
      std::cerr << d.primary.line << ": " << d.message << '\n';
  expect(enumArguments.canGenerateCode() && enumArguments.diagnostics.empty() &&
             enumMetrics.constexprMirExecutions == 5,
         "satisfied and unsatisfied member constraints over identity-only "
         "enum type arguments select dependent branches through staged MIR");

  const auto selected = lang::Frontend().analyze(
      "selected-constexpr.gti",
      "constexpr int selected<uint64_t N>() { if constexpr(N == 0) { return 1; "
      "} else { "
      "return 2; } } int main() { return selected<0>(); }");
  lang::SemanticVisitor provider;
  expect(selected.canGenerateCode() && provider.check(selected.program),
         "branch-proof mutation fixture is valid before changing its semantic "
         "selection");
  const auto *declaration = lang::stmtAs<lang::FunctionDecl>(
      selected.program.declarations().front().get());
  const auto *branch =
      declaration && declaration->body()
          ? lang::stmtAs<lang::IfStmt>(
                declaration->body()->statements().front().get())
          : nullptr;
  auto model = provider.model();
  const auto *function =
      declaration ? model.findFunction(*declaration) : nullptr;
  expect(branch && function,
         "branch-proof fixture contains the selected function and branch");
  if (branch && function) {
    const std::vector<lang::FunctionId> completed{function->id};
    lang::GenericSubstitution substitution;
    substitution.values.emplace(function->genericParameters.front().id,
                                lang::CompileTimeValue::uint64(0));
    expect(lang::HirLowerer()
               .lowerConstantFunction(model, provider, function->id, completed,
                                      substitution)
               .valid(),
           "a concretely selected branch has a complete staged HIR body");
    struct UnselectedProvider final : lang::SemanticInstanceProvider {
      lang::SemanticVisitor &delegate;
      explicit UnselectedProvider(lang::SemanticVisitor &delegate)
          : delegate(delegate) {}
      lang::SemanticTypeTraits
      traitsFor(const lang::SemanticType &type) const override {
        return delegate.traitsFor(type);
      }
      bool
      requiresActiveCleanupFor(const lang::SemanticType &type) const override {
        return delegate.requiresActiveCleanupFor(type);
      }
      lang::SemanticInstanceAnalysis analyzeFunctionInstance(
          lang::FunctionId, const std::vector<lang::SemanticType> &,
          const std::vector<lang::CompileTimeValue> &,
          const std::vector<lang::SemanticType> &,
          const std::vector<lang::CompileTimeValue> &) override {
        // Fault injection: falsely offer the unresolved primary as an instance.
        return {.model = delegate.model()};
      }
      lang::SemanticInstanceAnalysis analyzeConstructorInstance(
          lang::ConstructorId id, const std::vector<lang::SemanticType> &a,
          const std::vector<lang::CompileTimeValue> &b,
          const std::vector<lang::SemanticType> &c,
          const std::vector<lang::CompileTimeValue> &d) override {
        return delegate.analyzeConstructorInstance(id, a, b, c, d);
      }
      lang::SemanticInstanceAnalysis analyzeDestructorInstance(
          lang::ClassId id, const std::vector<lang::SemanticType> &a,
          const std::vector<lang::CompileTimeValue> &b) override {
        return delegate.analyzeDestructorInstance(id, a, b);
      }
      lang::SemanticInstanceAnalysis analyzeClassFieldInitializers(
          lang::ClassId id, const std::vector<lang::SemanticType> &a,
          const std::vector<lang::CompileTimeValue> &b) override {
        return delegate.analyzeClassFieldInitializers(id, a, b);
      }
    } unselected(provider);
    expect(!lang::HirLowerer()
                .lowerConstantFunction(model, unselected, function->id,
                                       completed, substitution)
                .valid(),
           "an unresolved branch cannot silently become an empty executable "
           "HIR statement");
  }

  struct Rejection {
    std::string source;
    std::string code;
    std::string message;
  };
  const auto warnings = lang::Frontend().analyze(
      "constexpr-branch-warnings.gti",
      "constexpr int ordinary() { return 3; return 4; }\n"
      "constexpr int generic<uint64_t N>() { if constexpr(N == 0) { return 1; "
      "} else { return 2; } return 3; }\n"
      "static_assert(ordinary() == 3); static_assert(generic<0>() == 1);\n"
      "int main() { return generic<0>() - 1; }");
  if (!warnings.canGenerateCode())
    for (const auto &d : warnings.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  expect(
      warnings.canGenerateCode() &&
          !lang::hasErrorDiagnostics(warnings.diagnostics) &&
          std::any_of(warnings.diagnostics.begin(), warnings.diagnostics.end(),
                      [](const auto &d) {
                        return d.code == "GTI-S2095" &&
                               d.severity == lang::DiagnosticSeverity::Warning;
                      }),
      "warnings in primary and concrete constexpr bodies remain advisory "
      "through staged and ordinary HIR lowering");
  const std::string budgetHelper =
      "constexpr int cost<uint64_t N>() {\n"
      "  mut int total = 0;\n"
      "  for (mut int i = 0; i < 100; i++) { total += 1; }\n"
      "  return total;\n}\n";
  const auto independent = lang::Frontend().analyze(
      "independent-constexpr-budgets.gti",
      budgetHelper + "static_assert(cost<1>() == 100);\n"
                     "static_assert(cost<1>() == 100);\n"
                     "static_assert(cost<1>() == 100);\n"
                     "int main() { return 0; }");
  expect(independent.canGenerateCode() && independent.diagnostics.empty(),
         "independent constant requests each receive a fresh work budget");
  const std::string sharedSource =
      budgetHelper +
      "constexpr int nested<uint64_t N>() {\n"
      "  if constexpr (cost<N>() == 100) {\n"
      "    if constexpr (cost<N>() == 100) { return cost<N>(); }\n"
      "  }\n"
      "  return 0;\n}\n"
      "static_assert(nested<1>() == 100);\nint main() { return 0; }";
  const auto shared =
      lang::Frontend().analyze("shared-constexpr-budget.gti", sharedSource);
  expect(!shared.canGenerateCode() &&
             std::any_of(
                 shared.diagnostics.begin(), shared.diagnostics.end(),
                 [&](const auto &d) {
                   return d.code == "GTI-S2057" &&
                          d.severity == lang::DiagnosticSeverity::Error &&
                          d.phase == lang::DiagnosticPhase::Semantics &&
                          d.message.find("execution-step limit") !=
                              std::string::npos &&
                          d.primary.source == std::filesystem::weakly_canonical(
                                                  "shared-constexpr-budget.gti")
                                                  .string() &&
                          d.primary.start == sharedSource.find("++") &&
                          d.primary.end == d.primary.start + 2 &&
                          std::any_of(d.related.begin(), d.related.end(),
                                      [&](const auto &related) {
                                        return related.span.source ==
                                                   d.primary.source &&
                                               related.span.start ==
                                                   sharedSource.find(
                                                       "static_assert");
                                      }) &&
                          !d.hints.empty();
                 }),
         "nested semantic predicates and the enclosing MIR call share work, "
         "with exhaustion attributed to the executing source and requiring "
         "assertion rather than to advisory warnings");
  const std::vector<Rejection> rejections{
      {"int unused<uint64_t N>() { if constexpr(N == 0) { return 1; } "
       "static_assert(false, \"independent assertion\"); return 2; }\nint "
       "main() { return 0; }",
       "GTI-S2079", "independent assertion"},
      {"int unused<uint64_t N>() { if constexpr(N == 0) { return 1; } "
       "return missing_outside_branch; }\nint main() { return 0; }",
       "GTI-S2001", "missing_outside_branch"},
      {"int bad<uint64_t N>() { if constexpr(N == 0) { return 1; } }\n"
       "int main() { return bad<1>(); }",
       "GTI-S2031", "without returning"},
      {"int bad<uint64_t N>(bool runtime) { if constexpr(N == 0 && runtime) { "
       "return 1; } else { return 2; } }\n"
       "int main() { return bad<0>(true); }",
       "GTI-S2057", "not constexpr"},
      {"int bad<uint64_t N>() { mut std::string text = std::string(\"text\"); "
       "if constexpr(N == 0) { std::string old = std::move(text); } return "
       "int(text.size()); }\n"
       "int main() { return bad<0>(); }",
       "GTI-S2018", "moved"},
      {"int bad<uint64_t N>() { mut std::string text = std::string(\"text\"); "
       "std::string_view view = text.view(); "
       "if constexpr(N == 0) { text.clear(); } return int(view.size()); }\n"
       "int main() { return bad<0>(); }",
       "GTI-S2017", "borrow"},
      {"int& bad<uint64_t N>(int& input) { if constexpr(N == 0) { int local = "
       "3; return local; } else { return input; } }\n"
       "int main() { int value = 1; return bad<0>(value); }",
       "GTI-S2017", "reference must derive"},
      {"constexpr bool cycle<uint64_t N>() { if constexpr(cycle<N>()) { return "
       "true; } else { return false; } }\n"
       "static_assert(cycle<0>()); int main() { return 0; }",
       "GTI-S2057", "limit"}};
  for (const auto &test : rejections) {
    const auto invalid = lang::Frontend().analyze(
        "dependent-constexpr-invalid.gti",
        "#include <std/string>\n" + test.source, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    const bool found =
        std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                    [&](const auto &d) {
                      return d.code == test.code &&
                             d.phase == lang::DiagnosticPhase::Semantics &&
                             d.message.find(test.message) != std::string::npos;
                    });
    if (!found)
      for (const auto &d : invalid.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    expect(!invalid.canGenerateCode() && found,
           "dependent branch deferral retains concrete flow, declaration "
           "checks and resource limits: " +
               test.source);
  }
}

void testConstexprOperators() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constexpr_operators.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  lang::FrontendWorkMetrics metrics;
  const auto result = lang::Frontend({.workMetrics = &metrics})
                          .analyze(fixture, source, {standardLibraryPrelude()},
                                   {}, {standardLibraryRoot()});
  if (!result.canGenerateCode() || metrics.constexprMirExecutions != 19) {
    std::cerr << "Operator MIR executions: " << metrics.constexprMirExecutions
              << '\n';
    for (const auto &d : result.diagnostics)
      std::cerr << d.primary.line << ": " << d.message << '\n';
  }
  expect(!source.empty() && result.canGenerateCode() &&
             metrics.constexprMirExecutions == 19,
         "direct operator roots, generic owners, fresh receivers and array "
         "indexing execute canonical MIR");
  lang::HirFunctionInstanceId work = 0;
  for (const auto &function : result.hir.functionInstances()) {
    if (function.source && function.source->name().lexeme == "compute")
      work = function.id;
  }
  const auto evaluated =
      lang::evaluateMirConstantFunction(result.mir, work, {});
  if (!evaluated)
    std::cerr << "Operator closure " << work << " fn=" << evaluated.function
              << " instruction=" << evaluated.instruction << ": "
              << evaluated.detail << '\n';
  expect(evaluated &&
             std::get<lang::ConstantInteger>(*evaluated.value).magnitude == 48,
         "a nested operator closure executes independently of AST fallback");
  auto nonConstexpr = result.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           nonConstexpr.functionInstances())) {
    if (function.overloadedOperator == lang::OverloadedOperator::Plus) {
      function.constexprFunction = false;
      changed = true;
    }
  }
  expect(changed && !lang::evaluateMirConstantFunction(nonConstexpr, work, {}),
         "a selected operator cannot execute after its constexpr eligibility "
         "is erased");
  auto wrongCapability = result.mir;
  auto *workFunction = const_cast<lang::MirFunctionInstance *>(
      wrongCapability.findFunctionInstance(work));
  changed = false;
  if (workFunction) {
    for (auto &block : workFunction->body.blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.callableInvocation ==
            lang::CallableInvocationCapability::Read) {
          instruction.callableInvocation =
              lang::CallableInvocationCapability::Mutable;
          changed = true;
        }
      }
    }
  }
  const auto invalidCapability =
      lang::evaluateMirConstantFunction(wrongCapability, work, {});
  expect(
      changed && !lang::verifyMirProgram(wrongCapability).valid() &&
          !invalidCapability &&
          invalidCapability.detail.find("verified MIR") != std::string::npos,
      "forged mutable callable access is rejected before constant execution");

  struct Rejection {
    std::string source;
    std::string message;
    std::string marker;
    std::size_t width;
  };
  const std::vector<Rejection> rejected{
      {"struct Item { int* x = nullptr; constexpr int operator()() mut { "
       "return 0; } "
       "};",
       "structural aggregate", "operator()", 8},
      {"struct Item { int x; constexpr int operator()() && { return this.x; } "
       "};",
       "cannot consume", "operator()", 8},
      {"struct Item { int x; constexpr Item(int x) : x(x) {} int operator+(int "
       "n) { return this.x + n; } }; "
       "constexpr Item item = Item(1); constexpr int bad = item + 2;",
       "does not name a constexpr function", "+ 2", 1},
      {"struct Item { int x; constexpr Item(int x) : x(x) {} constexpr int "
       "operator+(int n) { return this.x + "
       "n; } }; "
       "int work() { Item item = Item(1); constexpr int bad = item + 2; "
       "return bad; }",
       "not constexpr", "item + 2", 4},
      {"struct Item { int x; constexpr Item(int x) : x(x) {} constexpr int "
       "operator+(int n) { return this.x + "
       "n; } }; "
       "constexpr Item item = Item(2147483647); constexpr int bad = item + 1;",
       "overflow", "+ n", 1}};
  for (const auto &test : rejected) {
    const auto invalid =
        lang::Frontend().analyze("constexpr-operator-invalid.gti",
                                 test.source + "\nint main() { return 0; }");
    const bool found = std::any_of(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [&](const auto &d) {
          return d.code == "GTI-S2057" &&
                 d.phase == lang::DiagnosticPhase::Semantics &&
                 d.primary.source == std::filesystem::weakly_canonical(
                                         "constexpr-operator-invalid.gti")
                                         .string() &&
                 d.primary.start == test.source.find(test.marker) &&
                 d.primary.end == d.primary.start + test.width &&
                 d.message.find(test.message) != std::string::npos;
        });
    if (!found) {
      for (const auto &d : invalid.diagnostics)
        std::cerr << d.primary.start << '-' << d.primary.end << ": "
                  << d.message << '\n';
    }
    expect(!invalid.canGenerateCode() && found,
           "operator eligibility and runtime-input/arithmetic failures retain "
           "exact source diagnostics: " +
               test.source);
  }
  const auto bounds = lang::Frontend().analyze(
      "constexpr-operator-bounds.gti",
      "#include <std/array>\nconstexpr std::array<int, 2> a = std::array<int, "
      "2>({1, 2});\n"
      "constexpr int invalid = a[2];\nint main() { return 0; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  std::ifstream arrayInput(standardLibraryRoot() / "std/array.gti");
  const std::string arraySource((std::istreambuf_iterator<char>(arrayInput)),
                                std::istreambuf_iterator<char>());
  const auto access = arraySource.find(
      "[index]", arraySource.find("constexpr T& operator[](int"));
  expect(
      !bounds.canGenerateCode() && access != std::string::npos &&
          std::any_of(
              bounds.diagnostics.begin(), bounds.diagnostics.end(),
              [&](const auto &d) {
                return d.code == "GTI-S2057" &&
                       d.primary.source ==
                           (standardLibraryRoot() / "std/array.gti").string() &&
                       d.primary.start == access && d.primary.end == access + 1;
              }),
      "constexpr operator bounds failures retain the original library index "
      "location");
}

void testConstexprMutation() {
  const auto scalar =
      lang::Frontend().analyze("constexpr-mutation-scalar.gti", R"(
constexpr void add(mut int& value, int amount) { value += amount; }
constexpr mut int& forward_mut(mut int& value) { return value; }
constexpr int scalar_work(int seed) {
  mut int value = seed;
  add(value, 4);
  { mut int& same = forward_mut(value); add(same, 2); }
  return value;
}
struct Flag {
  mut bool enabled;
  constexpr Flag() : enabled(true) {}
};
constexpr void flip(mut bool& flag) {
  if (flag) { flag = false; } else { flag = true; }
}
constexpr bool conditional_work() {
  mut Flag flag = Flag();
  flip(flag.enabled);
  return !flag.enabled;
}
int main() { return scalar_work(3) - 9; }
)");
  lang::HirFunctionInstanceId scalarId = 0;
  for (const auto &function : scalar.hir.functionInstances()) {
    if (function.source && function.source->name().lexeme == "scalar_work")
      scalarId = function.id;
  }
  const std::array<lang::ConstantValue, 1> argument{lang::makeConstantInteger(
      {.magnitude = 3}, {.width = 32, .signedValue = true})};
  const auto computed =
      lang::evaluateMirConstantFunction(scalar.mir, scalarId, argument);
  if (!computed)
    std::cerr << "Mutable scalar MIR failure: " << int(computed.failure)
              << " body " << computed.body.owner << " instruction "
              << computed.instruction << ": " << computed.detail << '\n';
  expect(scalar.canGenerateCode() && computed &&
             std::get<lang::ConstantInteger>(*computed.value).magnitude == 9,
         "direct canonical MIR shares mutable referents across nested calls");
  for (std::size_t budget = 0; computed && budget < computed.steps; ++budget) {
    const auto limited = lang::evaluateMirConstantFunction(
        scalar.mir, scalarId, argument, {.steps = budget});
    expect(!limited && limited.failure ==
                           lang::ConstantEvaluationFailure::ResourceLimit,
           "every interrupted mutable call step preserves the shared resource "
           "limit failure");
  }
  const auto shallow = lang::evaluateMirConstantFunction(
      scalar.mir, scalarId, argument, {.callDepth = 1});
  expect(!shallow &&
             shallow.failure == lang::ConstantEvaluationFailure::ResourceLimit,
         "mutable calls share the ordinary constexpr call-depth limit");
  for (const auto &function : scalar.hir.functionInstances()) {
    if (!function.source ||
        function.source->name().lexeme != "conditional_work")
      continue;
    const auto conditional =
        lang::evaluateMirConstantFunction(scalar.mir, function.id, {});
    expect(conditional && conditional.value == lang::ConstantValue{true},
           "a branch reads the live mutable field referent");
    for (std::size_t budget = 0; conditional && budget < conditional.steps;
         ++budget) {
      const auto limited = lang::evaluateMirConstantFunction(
          scalar.mir, function.id, {}, {.steps = budget});
      expect(!limited && limited.failure ==
                             lang::ConstantEvaluationFailure::ResourceLimit,
             "an interrupted mutable-reference branch retains its resource "
             "failure instead of being relabeled as invalid input");
    }
  }
  for (const auto &function : scalar.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "add")
      continue;
    const std::array<lang::ConstantValue, 2> arguments{argument[0],
                                                       argument[0]};
    const auto completed =
        lang::evaluateMirConstantFunction(scalar.mir, function.id, arguments);
    expect(completed && completed.voidResult && !completed.value,
           "a void root completes without manufacturing a constant value or "
           "persisting its mutable input reference");
  }
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constexpr_mutation.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  const auto constants = source.find("\nconstexpr Table baked");
  const auto runtimeSource = source.substr(0, constants) + R"(
int main() { Table table = build(4); return table.total() + array_work(4) - 68; }
)";
  const auto runtime = lang::Frontend().analyze(fixture, runtimeSource,
                                                {standardLibraryPrelude()}, {},
                                                {standardLibraryRoot()});
  for (const auto &name : {"build", "array_work", "alias_work", "callable_work",
                           "temporary_work"}) {
    lang::HirFunctionInstanceId id = 0;
    for (const auto &function : runtime.hir.functionInstances()) {
      if (function.source && function.source->name().lexeme == name)
        id = function.id;
    }
    const std::array<lang::ConstantValue, 1> seed{lang::makeConstantInteger(
        {.magnitude = 4}, {.width = 32, .signedValue = true})};
    const auto direct = lang::evaluateMirConstantFunction(
        runtime.mir, id,
        std::string_view(name) == "build" ||
                std::string_view(name) == "array_work"
            ? std::span<const lang::ConstantValue>(seed)
            : std::span<const lang::ConstantValue>());
    if (!direct) {
      std::cerr << name << " MIR failure " << int(direct.failure) << " body "
                << int(direct.body.kind) << ':' << direct.body.owner
                << " instruction " << direct.instruction << ": "
                << direct.detail << '\n';
      if (const auto *body = lang::findMirBody(runtime.mir, direct.body)) {
        if (const auto *block = body->findBlock(direct.block)) {
          for (const auto &instruction : block->instructions) {
            if (instruction.id == direct.instruction)
              std::cerr << "kind " << int(instruction.kind) << " operation "
                        << int(instruction.operation) << " type "
                        << int(instruction.info.type.kind) << '\n';
          }
        }
      }
    }
    expect(runtime.canGenerateCode() && direct,
           "direct constant execution supports the mutable builder: " +
               std::string(name));
    if (direct && std::string_view(name) != "build")
      expect(std::get<lang::ConstantInteger>(*direct.value).magnitude ==
                 (std::string_view(name) == "array_work"      ? 29
                  : std::string_view(name) == "alias_work"    ? 18
                  : std::string_view(name) == "callable_work" ? 12
                                                              : 6),
             "mutable calls, once-formed indexed references, independent "
             "copies and reflection agree on the exact result: " +
                 std::string(name));
  }
  lang::FrontendWorkMetrics metrics;
  const auto result = lang::Frontend({.workMetrics = &metrics})
                          .analyze(fixture, source, {standardLibraryPrelude()},
                                   {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  }
  expect(!source.empty() && result.canGenerateCode() &&
             metrics.constexprMirExecutions >= 5,
         "mutable references, void helpers, mutable methods and constructor "
         "bodies build constants through canonical MIR");

  lang::FrontendWorkMetrics earlyMetrics;
  const auto early =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &earlyMetrics})
          .analyze(fixture, source, {standardLibraryPrelude()}, {},
                   {standardLibraryRoot()});
  expect(early.semanticValid && !early.canGenerateCode() &&
             earlyMetrics.constexprMirExecutions ==
                 metrics.constexprMirExecutions &&
             early.hir.functionInstances().empty(),
         "required constants and reflection-driven builders stage before "
         "whole-program HIR, including the array extent");

  const std::string prefix =
      "constexpr void add(mut int& value, int amount) { value += amount; }\n";
  struct Rejection {
    std::string body;
    std::string code;
  };
  const std::vector<Rejection> rejections{
      {"constexpr int bad() { mut int value = 2147483647; add(value, 1); "
       "return value; } constexpr int constant = bad();",
       "GTI-S2057"},
      {"constexpr void divide(mut int& value) { value /= 0; } "
       "constexpr int bad() { mut int value = 4; divide(value); return value; "
       "} "
       "constexpr int constant = bad();",
       "GTI-S2057"},
      {"constexpr int bad() { mut int value = 0; "
       "while (value < 5000) { add(value, 1); } return value; } "
       "constexpr int constant = bad();",
       "GTI-S2057"},
      {"constexpr void recurse(mut int& value) { "
       "if (value > 0) { value--; recurse(value); } } "
       "constexpr int bad() { mut int value = 100; recurse(value); return "
       "value; } "
       "constexpr int constant = bad();",
       "GTI-S2057"},
      {"constexpr mut int& bad() { mut int local = 4; return local; }",
       "GTI-S2017"},
      {"constexpr int bad() { mut int value = 4; int& view = value; "
       "add(value, 1); return view; }",
       "GTI-S2017"},
      {"constexpr int bad() { add(4, 1); return 0; }", "GTI-S2017"},
      {"int runtime(mut int& value) { value++; return value; } "
       "constexpr int bad() { mut int value = 4; return runtime(value); } "
       "constexpr int constant = bad();",
       "GTI-S2057"},
      {"int runtime = 4; constexpr int inspect(mut int value) { "
       "add(value, 1); return value; } constexpr int constant = "
       "inspect(runtime);",
       "GTI-S2057"},
      {"constexpr void bad(mut int* value) {}", "GTI-S2057"},
      {"constexpr int& retained = bad(); "
       "constexpr int& bad() { int value = 4; return value; }",
       "GTI-S2057"}};
  for (const auto &test : rejections) {
    const auto rejected = lang::Frontend().analyze(
        "constexpr-mutation-invalid.gti",
        prefix + test.body + "\nint main() { return 0; }");
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, test.code) &&
               std::none_of(rejected.diagnostics.begin(),
                            rejected.diagnostics.end(),
                            [](const auto &d) {
                              return d.phase == lang::DiagnosticPhase::Backend;
                            }),
           "mutable constexpr execution preserves frontend rejection: " +
               test.body);
  }

  for (const bool removeOrigin : {false, true}) {
    auto malformed = scalar.mir;
    for (const auto &instance : scalar.hir.functionInstances()) {
      if (instance.source && instance.source->name().lexeme == "forward_mut") {
        auto *function = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(instance.id));
        for (auto &loan : function->body.loans) {
          if (loan.kind != lang::MirLoanKind::Return)
            continue;
          if (removeOrigin)
            loan.source = 0;
          else
            loan.access = lang::AccessMode::ReadOnly;
        }
      }
    }
    const auto rejected =
        lang::evaluateMirConstantFunction(malformed, scalarId, argument);
    expect(!lang::verifyMirProgram(malformed).valid() && !rejected &&
               rejected.detail.find("verified MIR") != std::string::npos,
           "ordinary verification rejects a lost origin or forged read-only "
           "return loan before mutable constant execution");
  }
  auto runtimeOnly = scalar.mir;
  for (const auto &instance : scalar.hir.functionInstances()) {
    if (instance.source && instance.source->name().lexeme == "add")
      const_cast<lang::MirFunctionInstance *>(
          runtimeOnly.findFunctionInstance(instance.id))
          ->constexprFunction = false;
  }
  const auto unavailable =
      lang::evaluateMirConstantFunction(runtimeOnly, scalarId, argument);
  expect(lang::verifyMirProgram(runtimeOnly).valid() && !unavailable &&
             unavailable.failure ==
                 lang::ConstantEvaluationFailure::UnsupportedExpression,
         "runtime validity is not proof of constant eligibility for a mutable "
         "selected call");

  const auto constructorUnsupported = lang::Frontend().analyze(
      "constexpr-mutation-constructor-invalid.gti", R"(
#include <std/numeric>
struct Item {
  mut int value;
  constexpr Item(int seed) : value(seed) {
    this.value++;
    this.value = std::wrapping_add(this.value, 1);
  }
};
constexpr Item constant = Item(3);
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !constructorUnsupported.canGenerateCode() &&
          hasDiagnosticCode(constructorUnsupported.diagnostics, "GTI-S2057") &&
          hasDiagnostic(constructorUnsupported.diagnostics,
                        "requires the verified MIR execution profile"),
      "constructor-body mutation cannot fall back to a value-copy AST "
      "interpretation after an unsupported operation");

  const std::string enclosing = R"(
constexpr int write(mut int& value) { value++; return value; }
constexpr int build() {
  mut int value = 1;
  [[discard]] write(value);
  return value;
}
constexpr int result = build();
static_assert(result == 2);
int main() { return result - 2; }
)";
  const auto staged =
      lang::Frontend().analyze("constexpr-mutation-enclosing.gti", enclosing);
  expect(staged.canGenerateCode(),
         "an enclosing verified-MIR builder retains an ignored mutable "
         "callee's writes");
  // Retain real source-unit/binding identities but withhold the source manager
  // that the staged service needs. This selects the compatibility interpreter.
  lang::SemanticVisitor withoutStagingSource(lang::TargetInfo::host(),
                                             &staged.sourceGraph);
  const bool compatible = withoutStagingSource.check(staged.program);
  if (!hasDiagnostic(withoutStagingSource, "AST compatibility frame"))
    for (const auto &diagnostic : withoutStagingSource.errors())
      std::cerr << diagnostic.message << '\n';
  expect(!compatible &&
             hasDiagnostic(withoutStagingSource, "AST compatibility frame"),
         "a source-less compatibility caller cannot stage an isolated mutable "
         "callee and silently lose its writes");
}

void testConstexprReadOnlyReferences() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constexpr_readonly_references.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  lang::FrontendWorkMetrics metrics;
  const auto result = lang::Frontend({.workMetrics = &metrics})
                          .analyze(fixture, source, {standardLibraryPrelude()},
                                   {}, {standardLibraryRoot()});
  if (!result.canGenerateCode() || metrics.constexprMirExecutions != 14) {
    std::cerr << "Read-only MIR executions: " << metrics.constexprMirExecutions
              << '\n';
    for (const auto &d : result.diagnostics)
      std::cerr << d.primary.line << ": " << d.message << '\n';
  }
  expect(!source.empty() && result.canGenerateCode() &&
             metrics.constexprMirExecutions == 14,
         "read-only reference calls, returns, rebinding across loop iterations "
         "and std::array/string-view observers execute canonical MIR");

  const auto functionId = [&](std::string_view name) {
    for (const auto &function : result.hir.functionInstances()) {
      if (function.source && function.source->name().lexeme == name)
        return function.id;
    }
    return lang::HirFunctionInstanceId{0};
  };
  const auto work = functionId("work");
  const auto evaluated =
      lang::evaluateMirConstantFunction(result.mir, work, {});
  expect(
      work != 0 && evaluated &&
          std::get<lang::ConstantInteger>(*evaluated.value).magnitude == 24,
      "reference observation executes independently of the legacy AST driver");

  const std::array<lang::ConstantValue, 1> arguments{lang::makeConstantInteger(
      {.magnitude = 4}, {.width = 32, .signedValue = true})};
  const auto forwarded = lang::evaluateMirConstantFunction(
      result.mir, functionId("forward"), arguments);
  expect(forwarded && forwarded.value == arguments.front(),
         "a returned reference exposes the exact referent value, not a host "
         "address or a persistable constant reference");
  for (std::size_t budget = 0; evaluated && budget < evaluated.steps;
       ++budget) {
    const auto limited = lang::evaluateMirConstantFunction(result.mir, work, {},
                                                           {.steps = budget});
    expect(!limited && limited.failure ==
                           lang::ConstantEvaluationFailure::ResourceLimit,
           "every interrupted reference-observation step retains the resource "
           "limit failure instead of being relabeled as invalid input");
  }
  for (const auto &wrong :
       {lang::ConstantValue{true},
        lang::ConstantValue{lang::makeConstantInteger(
            {.magnitude = 4}, {.width = 64, .signedValue = true})}}) {
    const std::array wrongArguments{wrong};
    const auto rejected = lang::evaluateMirConstantFunction(
        result.mir, functionId("inspect"), wrongArguments);
    expect(!rejected && rejected.failure ==
                            lang::ConstantEvaluationFailure::InvalidOperands,
           "read-only reference arguments require the exact referent type");
  }
  for (const bool changeAccess : {false, true}) {
    auto malformed = result.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(functionId("forward")));
    expect(function != nullptr, "reference forwarding has a MIR function");
    if (!function)
      continue;
    const auto loan =
        std::find_if(function->body.loans.begin(), function->body.loans.end(),
                     [](const auto &item) {
                       return item.kind == lang::MirLoanKind::Return;
                     });
    expect(loan != function->body.loans.end(),
           "returned-reference mutation fixture has a represented return loan");
    if (loan == function->body.loans.end())
      continue;
    if (changeAccess)
      loan->access = lang::AccessMode::Mutable;
    else
      loan->source = 0;
    const auto rejected = lang::evaluateMirConstantFunction(
        malformed, functionId("forward"), arguments);
    expect(!lang::verifyMirProgram(malformed).valid() && !rejected &&
               rejected.detail.find("verified MIR") != std::string::npos,
           "ordinary MIR verification rejects forged reference access and "
           "missing origin before constexpr execution");
  }

  struct Rejection {
    std::string body;
    std::string code;
    std::string message;
    std::string marker;
    std::size_t width;
  };
  const std::vector<Rejection> rejections{
      {"constexpr int& bad() { int value = 4; return value; }", "GTI-S2017",
       "borrowed return requires", "bad()", 3},
      {"constexpr int bad(mut int* value) { return 0; }", "GTI-S2057",
       "parameters must be", "value)", 5},
      {"constexpr int bad() { mut int value = 4; int& ref = value; "
       "value = 5; return ref; }",
       "GTI-S2017", "overlapping borrow", "value = 5", 5},
      {"int runtime = 4; constexpr int value = inspect(runtime);", "GTI-S2057",
       "not constexpr", "runtime)", 7},
      {"constexpr int& retained = values.front();", "GTI-S2057",
       "not supported yet", "constexpr int&", 9},
      {"constexpr int bad(std::string& owner) { return int(owner.size()); }",
       "GTI-S2057", "parameters must be", "owner)", 5}};
  const std::string prefix =
      "#include <std/array>\n#include <std/string>\n"
      "constexpr int inspect(int& value) { return value + 1; }\n"
      "constexpr std::array<int, 3> values = std::array<int, 3>({1,2,3});\n";
  for (const auto &test : rejections) {
    const std::string invalidSource =
        prefix + test.body + "\nint main() { return 0; }";
    const auto invalid = lang::Frontend().analyze(
        "constexpr-reference-invalid.gti", invalidSource,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!invalid.semanticValid && !invalid.canGenerateCode() &&
               std::any_of(
                   invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [&](const auto &d) {
                     return d.code == test.code &&
                            d.phase == lang::DiagnosticPhase::Semantics &&
                            d.primary.source ==
                                std::filesystem::weakly_canonical(
                                    "constexpr-reference-invalid.gti")
                                    .string() &&
                            d.primary.start ==
                                invalidSource.rfind(test.marker) &&
                            d.primary.end == d.primary.start + test.width &&
                            d.message.find(test.message) != std::string::npos;
                   }),
           "read-only constant observations retain lifetime/type/bounds "
           "diagnostics at their exact source span: " +
               test.body);
  }

  const auto bounds = lang::Frontend().analyze(
      "constexpr-reference-bounds.gti",
      prefix + "constexpr int value = values.at(3);\nint main() { return 0; }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  std::ifstream arrayInput(standardLibraryRoot() / "std/array.gti");
  const std::string arraySource((std::istreambuf_iterator<char>(arrayInput)),
                                std::istreambuf_iterator<char>());
  const auto access =
      arraySource.find("[index]", arraySource.find("constexpr T& at(int"));
  expect(!bounds.canGenerateCode() && access != std::string::npos &&
             std::any_of(bounds.diagnostics.begin(), bounds.diagnostics.end(),
                         [&](const auto &d) {
                           return d.code == "GTI-S2057" &&
                                  d.primary.source ==
                                      std::filesystem::weakly_canonical(
                                          standardLibraryRoot() /
                                          "std/array.gti")
                                          .string() &&
                                  d.primary.start == access &&
                                  d.primary.end == access + 1;
                         }),
         "out-of-bounds returned reference points at the original library "
         "index expression, not an unrelated caller token");

  const auto assetFixture = fixture.parent_path() / "constexpr_asset_table.gti";
  std::ifstream assetInput(assetFixture);
  const std::string assetSource((std::istreambuf_iterator<char>(assetInput)),
                                std::istreambuf_iterator<char>());
  lang::FrontendWorkMetrics assetMetrics;
  const auto assetTable =
      lang::Frontend({.workMetrics = &assetMetrics})
          .analyze(assetFixture, assetSource, {standardLibraryPrelude()}, {},
                   {standardLibraryRoot()});
  // The passive record-array constructor now has an explicit field-write
  // stage and executes through MIR too, instead of the AST fallback.
  if (!assetTable.canGenerateCode() ||
      assetMetrics.constexprMirExecutions != 10) {
    std::cerr << "Asset table MIR executions: "
              << assetMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : assetTable.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  expect(!assetSource.empty() && assetTable.canGenerateCode() &&
             assetMetrics.constexprMirExecutions == 10,
         "compile-time asset metadata composes text inspection, record "
         "construction, fixed-array storage and checked reference traversal");
}

void testConstexprLibrary() {
  const auto chronoPath = std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/constexpr_chrono.gti";
  std::ifstream chronoInput(chronoPath);
  const std::string chronoSource((std::istreambuf_iterator<char>(chronoInput)),
                                 std::istreambuf_iterator<char>());
  lang::FrontendWorkMetrics chronoMetrics;
  const auto chrono =
      lang::Frontend({.workMetrics = &chronoMetrics})
          .analyze(chronoPath, chronoSource, {standardLibraryPrelude()}, {},
                   {standardLibraryRoot()});
  if (!chrono.canGenerateCode() || chronoMetrics.constexprMirExecutions != 12) {
    std::cerr << "Chrono MIR executions: "
              << chronoMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : chrono.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  expect(!chronoSource.empty() && chrono.canGenerateCode() &&
             chronoMetrics.constexprMirExecutions == 12 &&
             lang::verifyMirProgram(chrono.mir).valid(),
         "chrono constants, signed boundary decomposition and exact distinct "
         "time-point observers execute staged MIR, not native constexpr");
  for (const std::string source :
       {"constexpr auto value = std::chrono::system_clock::now();",
        "constexpr int budget() { mut int total = 0; "
        "for (mut int i = 0; i < 5000; i++) { "
        "std::chrono::nanoseconds n{int64_t(i)}; total += int(n.count()); } "
        "return total; } constexpr int value = budget();",
        "constexpr int depth(int i) { std::chrono::nanoseconds n{int64_t(i)}; "
        "if (n.count() == int64_t(0)) { return 0; } return depth(i - 1); } "
        "constexpr int value = depth(100);"}) {
    const auto invalid = lang::Frontend().analyze(
        "constexpr-chrono-invalid.gti", "#include <std/chrono>\n" + source,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2057"),
           "chrono adoption preserves runtime-only clocks and shared constant "
           "evaluation resource/depth limits: " +
               source);
    if (source.find("budget()") != std::string::npos)
      expect(hasDiagnostic(invalid.diagnostics, "execution-step limit"),
             "the chrono loop really reaches the shared MIR step limit");
    if (source.find("depth(100)") != std::string::npos)
      expect(hasDiagnostic(invalid.diagnostics, "call depth exceeded"),
             "the chrono recursion really reaches the shared call-depth limit");
  }
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constexpr_library_positive.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  expect(!source.empty(), "constexpr library fixture must be readable");
  lang::FrontendWorkMetrics metrics;
  const auto library = lang::Frontend({.workMetrics = &metrics})
                           .analyze(fixture, source, {standardLibraryPrelude()},
                                    {}, {standardLibraryRoot()});
  if (!library.canGenerateCode() || metrics.constexprMirExecutions != 20) {
    std::cerr << "Library MIR executions: " << metrics.constexprMirExecutions
              << '\n';
    for (const auto &diagnostic : library.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  }
  expect(library.canGenerateCode() && metrics.constexprMirExecutions == 20,
         "scalar algorithms and pair construction execute staged MIR while "
         "owning generic instances remain ordinary runtime calls");

  const auto runtime = lang::Frontend().analyze(
      "constexpr-runtime-only-instance.gti", R"(
#include <std/string>
struct Holder<T> {
  T value;
  constexpr Holder(T input) : value(std::move(input)) {}
  constexpr int tag() { return 7; }
};
constexpr T relay<T>(T value) { return std::move(value); }
int main() {
  Holder<std::string> owner = Holder<std::string>(relay(std::string("asset")));
  return owner.tag() == 7 && owner.value.size() == 5 ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!runtime.canGenerateCode())
    for (const auto &diagnostic : runtime.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(runtime.canGenerateCode(),
         "unrelated generic constructor and read-only method declarations "
         "admit runtime-only owning substitutions");

  for (const std::string body :
       {"constexpr std::string invalid() { return std::string(\"asset\"); }",
        "constexpr int invalid(std::string value) { return 7; }",
        "struct Owner { std::string value; constexpr Owner() : "
        "value(std::string(\"asset\")) {} };",
        "constexpr T invalid<T>(mut T* value) { return 0; }",
        "constexpr int value = std::lcm(2147483647, 2);",
        "constexpr int8_t invert(int8_t value) { return int8_t(-value); } "
        "constexpr int8_t value = invert(int8_t(-128));",
        "constexpr std::string value = std::make_pair(std::string(\"a\"), "
        "2).first;",
        "constexpr int inspect<T>(T value) { return 7; } "
        "constexpr int value = inspect(std::string(\"a\"));"}) {
    const auto invalid = lang::Frontend().analyze(
        "constexpr-library-invalid.gti",
        "#include <std/string>\n#include <std/numeric>\n#include "
        "<std/utility>\n" +
            body + "\nint main() { return 0; }\n",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &d) { return d.code == "GTI-S2057"; }),
           "runtime generic admission must not permit invalid constexpr "
           "declarations or required constant calls: " +
               body);
  }

  const std::string invalidSignature =
      "#include <std/string>\nconstexpr std::string impossible() { "
      "return std::string(\"asset\"); }\nint main() { return 0; }";
  const auto signature = lang::Frontend().analyze(
      "constexpr-invalid-signature.gti", invalidSignature,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!signature.canGenerateCode() &&
             std::any_of(
                 signature.diagnostics.begin(), signature.diagnostics.end(),
                 [&](const auto &d) {
                   return d.code == "GTI-S2057" &&
                          d.message.find(
                              "must return void or a supported value") !=
                              std::string::npos &&
                          d.primary.start ==
                              invalidSignature.find("string impossible") &&
                          d.primary.end == d.primary.start + 6;
                 }),
         "non-generic owning constexpr results retain their declaration-time "
         "diagnostic and exact return-type span");

  for (const std::string body :
       {"std::string value = std::string(\"a\"); "
        "std::string copy = relay(value); return 0;",
        "std::string value = std::string(\"a\"); "
        "std::string moved = relay(std::move(value)); return "
        "int(value.size());",
        "std::string value = std::string(\"a\"); "
        "std::string& borrowed = value; "
        "std::string moved = relay(std::move(value)); return "
        "int(borrowed.size());"}) {
    const auto invalid = lang::Frontend().analyze(
        "constexpr-runtime-ownership.gti",
        "#include <std/string>\nconstexpr T relay<T>(T value) { "
        "return std::move(value); }\nint main() { " +
            body + " }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(
        !invalid.semanticValid && !invalid.diagnostics.empty() &&
            std::none_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2057"; }),
        "runtime-only constexpr instantiations retain ordinary ownership "
        "errors rather than signature eligibility errors: " +
            body);
  }
}

void testMirConstantRecords() {
  lang::FrontendWorkMetrics observerMetrics;
  const auto observers =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &observerMetrics})
          .analyze("early-constexpr-methods.gti", R"(
struct Sized<T, uint64_t N> {
  T value;
  constexpr Sized(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
  constexpr uint64_t size() { return N; }
  constexpr T recurse(int depth) {
    if (depth == 0) { return this.get(); }
    return this.recurse(depth - 1);
  }
};
struct Utility {
  static constexpr int twice(int value) { return value * 2; }
};
constexpr Sized<int, 3> first = Sized<int, 3>(9);
constexpr Sized<int, 5> second = Sized<int, 5>(17);
static_assert(first.get() == 9 && second.get() == 17);
static_assert(first.size() == 3 && second.size() == 5);
static_assert(Utility::twice(3) == 6);
constexpr uint64_t extent = first.size();
int values[extent] = {};
int selected() {
  if constexpr (first.recurse(2) == 9) { return 0; }
  else { return discarded_invalid_name; }
}
)");
  if (!observers.semanticValid || observerMetrics.constexprMirExecutions != 9) {
    std::cerr << "Direct method MIR executions: "
              << observerMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : observers.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  }
  expect(observers.semanticValid && !observers.hirValid &&
             observerMetrics.constexprMirExecutions == 9,
         "direct generic instance/static method roots use staged MIR for "
         "assertions, array extents and branch conditions");
  lang::FrontendWorkMetrics libraryMetrics;
  const auto library =
      lang::Frontend({.workMetrics = &libraryMetrics})
          .analyze("constexpr-library-array.gti", R"(
#include <std/array>
constexpr std::array<int, 3> make_values(int seed) {
  return std::array<int, 3>({seed, seed + 1, seed + 2});
}
constexpr std::array<int, 3> make_empty() { return std::array<int, 3>(); }
constexpr bool inspect_values(int seed) {
  std::array<int, 3> value = make_values(seed);
  return value.size() == 3 && !value.empty();
}
constexpr std::array<int, 3> values = make_values(4);
constexpr std::array<int, 3> empty = make_empty();
static_assert(inspect_values(9));
static_assert(values.size() == 3 && empty.size() == 3);
int main() { return make_values(4).size() == 3 ? 0 : 1; }
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!library.canGenerateCode())
    for (const auto &diagnostic : library.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(library.canGenerateCode() &&
             libraryMetrics.constexprMirExecutions >= 3,
         "ordinary std::array explicit and default constructors execute "
         "and read-only observers execute through staged MIR");
  if (library.canGenerateCode()) {
    auto malformed = library.mir;
    bool changedStage = false;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            malformed.constructorInstances());
    for (auto &constructor : constructors) {
      for (auto &block : constructor.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (!changedStage &&
              instruction.kind == lang::MirInstructionKind::Initialize &&
              instruction.info.type.kind == lang::SemanticType::Array &&
              instruction.constructorInitializer != 0) {
            instruction.constructorInitializer = 0;
            changedStage = true;
          }
        }
      }
    }
    expect(changedStage && !lang::verifyMirProgram(malformed).valid(),
           "MIR rejects a passive array field write detached from its exact "
           "constructor stage");
  }
  lang::FrontendWorkMetrics metrics;
  const auto frontend = lang::Frontend({.workMetrics = &metrics})
                            .analyze("constexpr-record.gti", R"(
struct Point {
  mut int x;
  mut int y;
  constexpr Point(int a, int b) : x(a), y(b) {}
  constexpr int squared() { return this.x * this.x + this.y * this.y; }
  constexpr int scaled(int factor) { return this.squared() * factor; }
};
struct Box<T> {
  mut T value;
  constexpr Box(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
};
struct Mesh {
  mut Point point;
  mut int indices[3];
  constexpr Mesh(Point initial, int values[3]) : point(initial), indices(values) {}
  constexpr int inspect() { return this.point.scaled(this.indices[1]); }
};
constexpr Point translate(Point value, int delta) {
  mut Point result = value;
  result.x += delta;
  result.y++;
  return result;
}
constexpr Point create_point(int x, int y) { return Point(x, y); }
constexpr Box<T> create_box<T>(T value) { return Box<T>(value); }
constexpr Mesh create_mesh(int x) { return Mesh(Point(x, 4), {1, 2, 3}); }
constexpr int inspect_mesh(int x) { return create_mesh(x).inspect(); }
constexpr T inspect_box<T>(T value) { return create_box<T>(value).get(); }
constexpr int distance(Point value) { return value.x * value.x + value.y * value.y; }
constexpr Box<T> replace<T>(Box<T> value, T next) {
  mut Box<T> result = value;
  result.value = next;
  return result;
}
constexpr Mesh edit(Mesh input) {
  mut Mesh result = input;
  result.point.x++;
  result.indices[1] += 7;
  return result;
}
constexpr Point point = Point(3, 4);
constexpr Point moved = translate(point, 2);
constexpr Mesh mesh = Mesh(point, {1, 2, 3});
constexpr Mesh changed = edit(mesh);
constexpr Box<int> first = Box<int>(2);
constexpr Box<int> second = replace<int>(first, 7);
constexpr Point built = create_point(6, 8);
constexpr Box<int> built_box = create_box<int>(12);
constexpr Mesh built_mesh = create_mesh(3);
static_assert(built.x == 6 && built.y == 8);
static_assert(built_box.value == 12 && built_mesh.point.x == 3);
static_assert(inspect_mesh(3) == 50 && inspect_box<int>(17) == 17);
static_assert(point.x == 3 && point.y == 4);
static_assert(moved.x == 5 && moved.y == 5 && distance(moved) == 50);
static_assert(mesh.point.x == 3 && mesh.indices[1] == 2);
static_assert(changed.point.x == 4 && changed.indices[1] == 9);
static_assert(first.value == 2 && second.value == 7);
int main() { return distance(translate(Point(3, 4), 2)) == 50 ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  expect(frontend.canGenerateCode() && metrics.constexprMirExecutions >= 9,
         "passive record fields, nested arrays, mutation, generic identity and "
         "return execute staged MIR");
  if (!frontend.canGenerateCode())
    return;
  lang::FrontendWorkMetrics failureMetrics;
  const std::string failingSource =
      "struct Fraction { int value; constexpr Fraction(int divisor) : value(10 "
      "/ divisor) {} };\n"
      "constexpr int build(int divisor) { Fraction item = Fraction(divisor); "
      "return item.value; }\n"
      "constexpr int invalid = build(0);\n";
  const auto failed =
      lang::Frontend({.workMetrics = &failureMetrics})
          .analyze("constant-constructor-failure.gti", failingSource);
  const auto *failure = findDiagnosticByCode(failed.diagnostics, "GTI-S2057");
  expect(!failed.semanticValid && failureMetrics.constexprMirExecutions == 1 &&
             failure != nullptr &&
             failure->primary.start == failingSource.find('/') &&
             failure->primary.end == failingSource.find('/') + 1,
         "MIR constructor arithmetic failures retain the original initializer "
         "operator");
  const auto point = std::find_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(),
      [](const lang::MirClassInstance &item) {
        return item.declaredFields.size() == 2 &&
               item.declaredFields[0].type == lang::SemanticType::Int32 &&
               item.declaredFields[1].type == lang::SemanticType::Int32;
      });
  expect(point != frontend.mir.classInstances().end(),
         "record execution retains exact MIR field identities");
  if (point == frontend.mir.classInstances().end())
    return;
  const auto integer = [](std::uint64_t value) -> lang::ConstantValue {
    return lang::makeConstantInteger({.magnitude = value},
                                     {.width = 32, .signedValue = true});
  };
  lang::ConstantAggregate aggregate{
      .kind = lang::ConstantAggregateKind::Object,
      .identity = point->declaration,
      .exactType = std::make_shared<const lang::SemanticType>(point->type),
      .fields = {{point->declaredFields[0].symbol, integer(3)},
                 {point->declaredFields[1].symbol, integer(4)}}};
  lang::HirFunctionInstanceId distance = 0;
  lang::HirConstructorInstanceId pointConstructor = 0;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.owner == point->id)
      pointConstructor = constructor.id;
  const std::array pointArguments{integer(3), integer(4)};
  const auto constructedPoint = lang::evaluateMirConstantConstructor(
      frontend.mir, pointConstructor, pointArguments);
  expect(constructedPoint &&
             constructedPoint.value ==
                 lang::ConstantValue{lang::makeConstantAggregate(aggregate)},
         "direct constructor roots publish exact fully initialized fields");
  lang::HirFunctionInstanceId inspectMesh = 0;
  lang::HirFunctionInstanceId observePoint = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source && function.source->name().lexeme == "distance")
      distance = function.id;
    if (function.source && function.source->name().lexeme == "inspect_mesh")
      inspectMesh = function.id;
    if (function.source && function.source->name().lexeme == "squared")
      observePoint = function.id;
  }
  const std::array meshArguments{integer(3)};
  const auto observed = lang::evaluateMirConstantFunction(
      frontend.mir, inspectMesh, meshArguments);
  if (!observed || observed.value != integer(50))
    std::cerr << "Direct observer root=" << inspectMesh
              << " failure=" << static_cast<int>(observed.failure)
              << " body=" << static_cast<int>(observed.body.kind) << ':'
              << observed.body.owner << " instruction=" << observed.instruction
              << " detail=" << observed.detail << '\n';
  expect(observed && observed.value == integer(50),
         "direct MIR executes nested constructors, projected receivers and "
         "read-only method calls without compatibility fallback");
  expect(!lang::evaluateMirConstantFunction(frontend.mir, observePoint, {}),
         "an instance method cannot execute without its exact receiver");
  const auto directMethod = lang::evaluateMirConstantFunction(
      frontend.mir, observePoint, {}, {},
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)});
  expect(directMethod && directMethod.value == integer(25),
         "direct method roots receive exact fully initialized constant state");
  expect(!lang::evaluateMirConstantFunction(frontend.mir, observePoint, {}, {},
                                            integer(3)),
         "a scalar cannot impersonate the method's structural receiver");
  auto nonConstantMethod = frontend.mir;
  auto &methodInstances = const_cast<std::vector<lang::MirFunctionInstance> &>(
      nonConstantMethod.functionInstances());
  for (auto &function : methodInstances)
    if (function.id == observePoint)
      function.constexprFunction = false;
  expect(!lang::evaluateMirConstantFunction(nonConstantMethod, inspectMesh,
                                            meshArguments),
         "a called read-only method requires explicit constexpr eligibility");
  auto nonConstantConstructor = frontend.mir;
  auto &constructorInstances =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          nonConstantConstructor.constructorInstances());
  for (auto &constructor : constructorInstances)
    constructor.constexprConstructor = false;
  expect(
      !lang::evaluateMirConstantFunction(nonConstantConstructor, inspectMesh,
                                         meshArguments),
      "native constructibility cannot replace semantic constexpr eligibility");
  lang::SemanticVisitor provider(lang::TargetInfo::host(),
                                 &frontend.sourceGraph, false, nullptr,
                                 &frontend.sources);
  expect(provider.check(frontend.program),
         "constructor closure provider checks the fixture");
  std::vector<lang::FunctionId> completedFunctions;
  std::vector<lang::ConstructorId> completedConstructors;
  lang::FunctionId createPoint = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source || !function.source->isConstexpr())
      continue;
    const auto *declaration = provider.model().findFunction(*function.source);
    if (!declaration)
      continue;
    completedFunctions.push_back(declaration->id);
    if (function.source->name().lexeme == "create_point")
      createPoint = declaration->id;
  }
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    if (!constructor.source)
      continue;
    if (const auto *declaration =
            provider.model().findConstructor(*constructor.source))
      completedConstructors.push_back(declaration->id);
  }
  lang::HirLowerer constructorLowerer;
  const auto missingConstructor = constructorLowerer.lowerConstantFunction(
      provider.model(), provider, createPoint, completedFunctions);
  expect(!missingConstructor.valid(),
         "a registered constexpr constructor cannot be staged before its "
         "completed-definition inventory admits it");
  const auto completedConstructor = constructorLowerer.lowerConstantFunction(
      provider.model(), provider, createPoint, completedFunctions, {},
      completedConstructors);
  expect(completedConstructor.valid() &&
             completedConstructor.program.constructorInstances().size() == 1,
         "completed constructor staging reuses the ordinary exact worklist");
  const std::array arguments{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  const auto evaluated =
      lang::evaluateMirConstantFunction(frontend.mir, distance, arguments);
  expect(evaluated && evaluated.value == integer(25),
         "direct MIR record evaluation consumes ordinary verified field "
         "projections and passive drops");
  aggregate.fields.pop_back();
  const std::array incomplete{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  expect(!lang::evaluateMirConstantFunction(frontend.mir, distance, incomplete),
         "an incomplete constant record cannot supply an initialized field");
  aggregate.fields.push_back({point->declaredFields[1].symbol, true});
  const std::array mistyped{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  const auto invalid =
      lang::evaluateMirConstantFunction(frontend.mir, distance, mistyped);
  expect(!invalid && invalid.failure ==
                         lang::ConstantEvaluationFailure::InvalidOperands,
         "record identity alone does not authorize a mistyped constant field");
  aggregate.fields[1] = aggregate.fields[0];
  const std::array duplicated{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  expect(
      !lang::evaluateMirConstantFunction(frontend.mir, distance, duplicated),
      "duplicated field identities cannot stand in for complete record state");
}

void testConstexprStringViewInspection() {
  // Exercise the service independently of host char signedness and the lexer.
  std::string bytes;
  for (unsigned value = 0; value != 256; ++value) {
    bytes.push_back(static_cast<char>(value));
  }
  const lang::ConstantValue text = bytes;
  const auto size = lang::evaluateConstantTextSize(text);
  expect(size && lang::constantUnsignedMagnitude(*size.value) == 256,
         "constant text size includes embedded NUL bytes");
  for (unsigned value = 0; value != 256; ++value) {
    const auto index = lang::makeConstantInteger(
        {.magnitude = value}, {.width = 64, .signedValue = false});
    const auto element = lang::evaluateConstantTextIndex(text, index);
    expect(element &&
               std::get<lang::CharacterLiteral>(*element.value).value == value,
           "constant text indexing preserves every unsigned byte");
  }
  expect(!lang::evaluateConstantTextIndex(
             text,
             lang::makeConstantInteger({.negative = true, .magnitude = 1},
                                       {.width = 32, .signedValue = true})) &&
             !lang::evaluateConstantTextSize(lang::ConstantValue{true}),
         "constant text kernels reject negative indices and non-text values");

  const auto valid = lang::Frontend().analyze("constexpr-text.gti", R"(
constexpr uint64_t count(std::string_view text) { return text.size(); }
constexpr char at(std::string_view text, int32_t offset) { return text[offset]; }
constexpr std::string_view identity(std::string_view text) { return text; }
struct Label {
  std::string_view text;
  constexpr Label(std::string_view initial) : text(initial) {}
  constexpr uint64_t size() { return this.text.size(); }
};
struct Capacity<uint64_t N> { int values[N] = {}; };
constexpr std::string_view text = identity("a\0z");
constexpr Label label = Label(text);
constexpr uint64_t length = count(text);
static_assert(length == 3 && !text.empty() && "".empty());
static_assert(at(text, 0) == 'a' && at(text, 1) == '\0' && at(text, 2) == 'z');
static_assert(label.size() == length);
static_assert(identity("z").size() == 1);
int buffer[length] = {};
Capacity<length> capacity{};
int main() {
  if constexpr (text.size() == 3) { return 0; }
  else { return unselected_invalid_name; }
}
)",
                                              {standardLibraryPrelude()});
  for (const auto &diagnostic : valid.diagnostics) {
    std::cerr << diagnostic.primary.line << ": " << diagnostic.message << '\n';
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "text inspection composes with constant calls, fields, extents, "
         "generic values, and early branch selection");

  for (const std::string index : {"-1", "3", "18446744073709551615"}) {
    const std::string source = "constexpr std::string_view text = \"abc\";\n"
                               "constexpr char value = text[" +
                               index + "];";
    const auto invalid = lang::Frontend().analyze(
        "constexpr-text-bounds.gti", source, {standardLibraryPrelude()});
    const auto *diagnostic =
        findDiagnosticByCode(invalid.diagnostics, "GTI-S2057");
    expect(
        !invalid.semanticValid && diagnostic != nullptr &&
            diagnostic->message.find("outside the valid range [0, 3)") !=
                std::string::npos &&
            diagnostic->phase == lang::DiagnosticPhase::Semantics &&
            diagnostic->primary.source ==
                std::filesystem::weakly_canonical("constexpr-text-bounds.gti")
                    .string() &&
            diagnostic->primary.start == source.find(index) &&
            diagnostic->primary.end ==
                source.find(index) + (index == "-1" ? 1 : index.size()) &&
            diagnostic->fixes.empty(),
        "constexpr out-of-bounds text access diagnoses its index expression");
  }
  const auto runtime =
      lang::Frontend().analyze("constexpr-runtime-text.gti", R"(
uint64_t inspect(std::string_view text) {
  constexpr uint64_t size = text.size();
  return size;
}
)",
                               {standardLibraryPrelude()});
  expect(!runtime.semanticValid &&
             findDiagnosticByCode(runtime.diagnostics, "GTI-S2057") != nullptr,
         "runtime text cannot be inspected in a required constant context");

  const auto limit = lang::Frontend().analyze("constexpr-text-budget.gti", R"(
constexpr uint64_t spin(std::string_view text) {
  mut uint64_t result = 0;
  while (!text.empty()) { result += text.size(); }
  return result;
}
constexpr uint64_t result = spin("x");
)",
                                              {standardLibraryPrelude()});
  expect(!limit.semanticValid && hasDiagnostic(limit.diagnostics, "limit"),
         "constant text operations share the deterministic execution budget");
}

void testConstexprBindings() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keyword =
      lexer.scan("constexpr int32_t value = 1;");
  expect(keyword.size() >= 2 &&
             keyword.front().kind == lang::TokenKind::CONSTEXPR,
         "constexpr should be a dedicated declaration keyword");

  const std::string formattedConstructor = lang::Formatter().format(
      "struct Point{constexpr Point(int32_t value):coordinate(value){}"
      "int32_t coordinate;};");
  expect(formattedConstructor.find(
             "constexpr Point(int32_t value) : coordinate(value) {}") !=
                 std::string::npos &&
             lang::Formatter().format(formattedConstructor) ==
                 formattedConstructor,
         "the formatter should preserve constexpr constructors and remain "
         "idempotent");

  const std::string source = R"(
constexpr uint64_t factorial(mut uint64_t value) {
  mut uint64_t result = 1;
  while (value > 1) {
    result *= value;
    value--;
  }
  return result;
}

constexpr uint64_t fibonacci(uint64_t value) {
  if (value < uint64_t(2)) {
    return value;
  }
  return fibonacci(value - 1) + fibonacci(value - 2);
}

constexpr int32_t classify(int32_t value) {
  switch (value) {
  case 0:
    return 10;
  case 1:
    return 20;
  default:
    return 30;
  }
}

enum class BuildMode : uint8_t {
  debug = 1,
  release = 2,
};

class ConstantBox<T> {
  T value;

public:
  constexpr ConstantBox(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
};

constexpr T preserve<T>(T value) { return value; }

constexpr uint64_t extent = uint64_t(2 + 2);
constexpr int32_t base = 6 * 7;
constexpr bool selected = base == 42 and true;
constexpr uint64_t factorial_value = factorial(uint64_t(5));
constexpr uint64_t fibonacci_value = fibonacci(uint64_t(10));
constexpr int32_t raw_values[3] = {2, 4, 6};
constexpr ConstantBox<int32_t> signed_box = ConstantBox<int32_t>(7);
constexpr ConstantBox<uint64_t> wide_box =
    ConstantBox<uint64_t>(uint64_t(9));
constexpr BuildMode selected_mode = BuildMode::release;
constexpr int32_t preserved = preserve<int32_t>(11);

static_assert(raw_values[1] == 4);
static_assert(signed_box.get() == 7);
static_assert(wide_box.get() == uint64_t(9));
static_assert(selected_mode == BuildMode::release);
static_assert(preserved == 11);

namespace values {
constexpr char marker = 'G';
}

class Limits {
public:
  static constexpr uint64_t count = extent;
  static constexpr uint64_t twice(uint64_t value) { return value * 2; }
};

constexpr uint64_t doubled = Limits::twice(uint64_t(21));

class Block<T, uint64_t N> {
public:
  T values[N] = {};
};

int main() {
  constexpr auto local = selected ? base : 0;
  int32_t data[extent] = {1, 2, 3, 4};
  Block<int32_t, extent> block{};
  if constexpr (factorial_value == uint64_t(120) and
                fibonacci_value == uint64_t(55) and
                doubled == uint64_t(42)) {
    if (local == 42 and Limits::count == 4 and data[3] == 4 and
        values::marker == 'G' and raw_values[2] == 6 and
        signed_box.get() == 7 and wide_box.get() == uint64_t(9) and
        selected_mode == BuildMode::release and preserved == 11) {
      return classify(0) - 10;
    }
    return 1;
  } else {
    return inactive_branch_is_not_analyzed;
  }
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("constexpr.gti", source);
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "bounded scalar constexpr bindings should compile and provide array "
         "and value-generic extents");

  const lang::VariableDecl *extent = nullptr;
  const lang::VariableDecl *signedBox = nullptr;
  const lang::VariableDecl *wideBox = nullptr;
  const lang::ClassDecl *limits = findTopLevelClass(frontend.program, "Limits");
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "extent") {
      extent = variable;
    } else if (variable != nullptr && variable->name().lexeme == "signed_box") {
      signedBox = variable;
    } else if (variable != nullptr && variable->name().lexeme == "wide_box") {
      wideBox = variable;
    }
  }
  const lang::BindingInfo *extentBinding =
      extent == nullptr ? nullptr : frontend.semantics.findBinding(*extent);
  const auto *extentValue =
      extentBinding == nullptr || !extentBinding->constant
          ? nullptr
          : std::get_if<lang::ConstantInteger>(&*extentBinding->constant);
  expect(extent != nullptr && extent->isConstexpr() && extentValue != nullptr &&
             extentValue->magnitude == 4 && !extentValue->negative &&
             extentValue->domain == lang::CheckedIntegerDomain{.width = 64},
         "semantic bindings should retain the typed computed constexpr value");

  const lang::BindingInfo *signedBoxBinding =
      signedBox == nullptr ? nullptr
                           : frontend.semantics.findBinding(*signedBox);
  const lang::BindingInfo *wideBoxBinding =
      wideBox == nullptr ? nullptr : frontend.semantics.findBinding(*wideBox);
  const auto *signedBoxValue =
      signedBoxBinding == nullptr || !signedBoxBinding->constant
          ? nullptr
          : std::get_if<lang::ConstantAggregateValue>(
                &*signedBoxBinding->constant);
  const auto *wideBoxValue =
      wideBoxBinding == nullptr || !wideBoxBinding->constant
          ? nullptr
          : std::get_if<lang::ConstantAggregateValue>(
                &*wideBoxBinding->constant);
  expect(signedBoxValue != nullptr && signedBoxValue->value != nullptr &&
             signedBoxValue->value->exactType != nullptr &&
             signedBoxBinding != nullptr &&
             *signedBoxValue->value->exactType == signedBoxBinding->type &&
             wideBoxValue != nullptr && wideBoxValue->value != nullptr &&
             wideBoxValue->value->exactType != nullptr &&
             wideBoxBinding != nullptr &&
             *wideBoxValue->value->exactType == wideBoxBinding->type &&
             *signedBoxValue->value->exactType !=
                 *wideBoxValue->value->exactType,
         "aggregate constants should retain exact concrete generic type "
         "identity rather than only their class declaration");

  const lang::VariableDecl *count = nullptr;
  if (limits != nullptr) {
    for (const lang::StmtPtr &member : limits->members()) {
      const auto *field =
          dynamic_cast<const lang::VariableDecl *>(member.get());
      if (field != nullptr && field->name().lexeme == "count") {
        count = field;
      }
    }
  }
  const lang::BindingInfo *countBinding =
      count == nullptr ? nullptr : frontend.semantics.findBinding(*count);
  expect(count != nullptr && count->isStatic() && count->isConstexpr() &&
             countBinding != nullptr && countBinding->constant.has_value(),
         "static constexpr class fields should retain constant metadata");

  bool hirRetainsConstant = false;
  bool hirRetainsConstexprFunction = false;
  bool hirDiscardsInactiveBranch = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "factorial") {
      hirRetainsConstexprFunction = function.constexprFunction;
    }
    if (function.source == nullptr ||
        function.source->name().lexeme != "main") {
      continue;
    }
    hirRetainsConstant = std::any_of(
        function.body.bindings.begin(), function.body.bindings.end(),
        [](const lang::HirBinding &binding) {
          return binding.variable != nullptr &&
                 binding.variable->name().lexeme == "local" &&
                 binding.info.constant.has_value();
        });
    hirDiscardsInactiveBranch = std::any_of(
        function.body.statements.begin(), function.body.statements.end(),
        [](const lang::HirStatement &statement) {
          return statement.kind == lang::HirStatementKind::CompileTimeBranch;
        });
  }
  expect(hirRetainsConstant,
         "typed HIR should retain constexpr values independently of the C++ "
         "backend");
  expect(hirRetainsConstexprFunction,
         "typed HIR should retain constexpr function metadata");
  expect(hirDiscardsInactiveBranch,
         "typed HIR should represent if constexpr as a selected compile-time "
         "branch");

  lang::MirProgram forgedAggregate = frontend.mir;
  auto &forgedSteps =
      const_cast<std::vector<lang::MirProgramInitializationStep> &>(
          forgedAggregate.programInitializationPlan().steps);
  bool corruptedAggregateField = false;
  for (lang::MirProgramInitializationStep &step : forgedSteps) {
    auto *aggregate =
        step.dataConstant
            ? std::get_if<lang::ConstantAggregateValue>(&*step.dataConstant)
            : nullptr;
    if (aggregate == nullptr || aggregate->value == nullptr ||
        aggregate->value->kind != lang::ConstantAggregateKind::Object ||
        aggregate->value->fields.empty()) {
      continue;
    }
    lang::ConstantAggregate corrupted = *aggregate->value;
    corrupted.fields.front().symbol = 0;
    step.dataConstant =
        lang::ConstantValue{lang::makeConstantAggregate(std::move(corrupted))};
    corruptedAggregateField = true;
    break;
  }
  expect(corruptedAggregateField &&
             !lang::verifyMirProgram(forgedAggregate).valid(),
         "MIR verification should reject an aggregate constant whose exact "
         "field inventory was forged after semantic analysis");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      generated.find(
          "constexpr std::uint64_t extent = static_cast<std::uint64_t>(4)") !=
              std::string::npos &&
          generated.find("static constexpr std::uint64_t count = "
                         "static_cast<std::uint64_t>(4)") !=
              std::string::npos &&
          generated.find("constexpr std::uint64_t factorial_value = "
                         "static_cast<std::uint64_t>(120)") !=
              std::string::npos &&
          generated.find("// GTI verified-MIR body:") != std::string::npos &&
          generated.find("inactive_branch_is_not_analyzed") ==
              std::string::npos,
      "the C++ backend should emit computed constexpr values and keep "
      "static constexpr fields inline while erasing inactive branches");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-constexpr.gti", R"(
constexpr mut int32_t mutable_value = 1;
constexpr int32_t missing;
int32_t runtime_value() { return 1; }
constexpr int32_t called = runtime_value();
constexpr int32_t generic_values<Args...>(Args... values) { return 0; }
class InvalidMethod {
  constexpr int32_t method() && { return 1; }
};
class InvalidMember {
  constexpr int32_t value = 1;
};
struct RuntimePoint {
  int32_t value;
  RuntimePoint(int32_t initial) : value(initial) {}
};
constexpr RuntimePoint runtime_point = RuntimePoint(1);
)");
  expect(
      !invalid.semanticValid &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2057") >= 6 &&
          hasDiagnostic(invalid.diagnostics, "cannot be declared mut") &&
          hasDiagnostic(invalid.diagnostics, "requires an initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "does not name a constexpr function") &&
          hasDiagnostic(invalid.diagnostics,
                        "Constexpr generic parameter packs") &&
          hasDiagnostic(invalid.diagnostics, "cannot consume its receiver") &&
          hasDiagnostic(invalid.diagnostics, "must be static") &&
          hasDiagnostic(invalid.diagnostics,
                        "not an available constexpr constructor"),
      "unsupported constexpr forms should fail with focused bounded "
      "diagnostics");

  const lang::FrontendResult invalidBranch =
      lang::Frontend().analyze("invalid-if-constexpr.gti", R"(
bool runtime_condition() { return true; }
int main() {
  if constexpr (runtime_condition()) {
    return 0;
  }
  return 1;
}
)");
  expect(!invalidBranch.semanticValid &&
             hasDiagnostic(invalidBranch.diagnostics,
                           "does not name a constexpr function"),
         "if constexpr should require a frontend-evaluated constant bool");

  const lang::FrontendResult exhausted =
      lang::Frontend().analyze("exhausted-constexpr.gti", R"(
constexpr uint64_t recurse(uint64_t value) {
  return recurse(value);
}
constexpr uint64_t result = recurse(uint64_t(0));
)");
  expect(
      !exhausted.semanticValid &&
          hasDiagnostic(exhausted.diagnostics, "constexpr call depth exceeded"),
      "constexpr recursion should stop at a deterministic frontend "
      "resource limit");

  const lang::FrontendResult exhaustedAggregate =
      lang::Frontend().analyze("exhausted-constexpr-aggregate.gti", R"(
constexpr int32_t values[4096] = {};
)");
  expect(!exhaustedAggregate.semanticValid &&
             hasDiagnostic(exhaustedAggregate.diagnostics,
                           "bounded evaluation limit"),
         "default aggregate synthesis should consume the same deterministic "
         "constexpr step budget as explicit expressions");
}

void testStaticAssertions() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keyword = lexer.scan("static_assert(true);");
  expect(keyword.size() >= 2 &&
             keyword.front().kind == lang::TokenKind::STATIC_ASSERT &&
             lang::isKeywordToken(keyword.front().kind) &&
             !lang::isCppReservedIdentifier("static_assert"),
         "static_assert should be a dedicated GTI declaration keyword");

  const std::string validSource = R"(
#define ASSERTIONS_ENABLED

constexpr uint64_t expected_width = uint64_t(4);
static_assert(true);

namespace checks {
static_assert(expected_width == uint64_t(4),
              "must never reach native C++");
}

struct Packet {
  static_assert(sizeof(int32_t) == uint64_t(4), "packet field width");
  int32_t value = 0;
};

interface Contract {
  static_assert(true);
};

union Choice {
  static_assert(true);
  int32_t value;
};

int main() {
  constexpr bool ready = true;
  static_assert(ready, "local assertion");
#ifdef ASSERTIONS_ENABLED
  static_assert(true);
#else
  static_assert(false, "inactive configuration assertion");
#endif
  if constexpr (false) {
    static_assert(false, "discarded constexpr assertion");
  } else {
    static_assert(true);
  }
  return 0;
}
)";
  const lang::FrontendResult valid =
      lang::Frontend().analyze("static-assertions.gti", validSource);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "active exact-bool static assertions should pass in namespace, "
         "class-like, and block scopes");

  const lang::StaticAssertDecl *topLevelAssertion = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    if (const auto *assertion =
            dynamic_cast<const lang::StaticAssertDecl *>(declaration.get())) {
      topLevelAssertion = assertion;
      break;
    }
  }
  expect(topLevelAssertion != nullptr &&
             valid.semantics.findStaticAssertResult(*topLevelAssertion) ==
                 lang::StaticAssertResult::Passed,
         "SemanticModel should retain the passed assertion result without "
         "creating a binding");

  bool assertionReachedHir = false;
  const auto inspectBody = [&](const lang::HirBody &body) {
    for (const lang::HirStatement &statement : body.statements) {
      assertionReachedHir =
          assertionReachedHir || dynamic_cast<const lang::StaticAssertDecl *>(
                                     statement.source) != nullptr;
    }
  };
  inspectBody(valid.hir.module());
  for (const lang::HirFunctionInstance &function :
       valid.hir.functionInstances()) {
    inspectBody(function.body);
  }
  for (const lang::HirClassInstance &classType : valid.hir.classInstances()) {
    inspectBody(classType.fieldInitializers);
    inspectBody(classType.staticFieldInitializers);
  }
  expect(!assertionReachedHir,
         "successful static assertions should be erased before HIR");

  const std::string generated = gti_test::emitCppText(valid);
  expect(generated.find("must never reach native C++") == std::string::npos &&
             generated.find("discarded constexpr assertion") ==
                 std::string::npos,
         "source static assertions and their messages must not be emitted to "
         "the C++ backend");

  const std::string formatted = lang::Formatter().format(
      "static_assert(sizeof(int32_t)==uint64_t(4),\"layout\");");
  expect(
      formatted ==
              "static_assert(sizeof(int32_t) == uint64_t(4), \"layout\");\n" &&
          lang::Formatter().format(formatted) == formatted,
      "the formatter should preserve both static_assert forms and space "
      "the optional message comma");

  const lang::FrontendResult falseAssertion =
      lang::Frontend().analyze("false-static-assert.gti", R"(
static_assert(false, "declared invariant");
int main() { return 0; }
)");
  const lang::Diagnostic *failed = nullptr;
  for (const lang::Diagnostic &diagnostic : falseAssertion.diagnostics) {
    if (diagnostic.code == "GTI-S2079") {
      failed = &diagnostic;
      break;
    }
  }
  expect(!falseAssertion.semanticValid && failed != nullptr &&
             failed->message == "Static assertion failed. declared invariant" &&
             failed->primary.start == 15 && failed->primary.end == 20,
         "a false static assertion should use its full condition span and "
         "dedicated diagnostic code");

  const lang::FrontendResult wrongType =
      lang::Frontend().analyze("typed-static-assert.gti", R"(
static_assert(1);
int main() { return 0; }
)");
  expect(!wrongType.semanticValid &&
             hasDiagnosticCode(wrongType.diagnostics, "GTI-S2080") &&
             hasDiagnostic(wrongType.diagnostics, "exact type 'bool'") &&
             hasDiagnostic(wrongType.diagnostics, "int32_t"),
         "static assertions should reject non-bool conditions without "
         "contextual conversion");

  const lang::FrontendResult runtimeDependency =
      lang::Frontend().analyze("runtime-static-assert.gti", R"(
int verify(int value) {
  static_assert(value == 4);
  return value;
}
)");
  expect(!runtimeDependency.semanticValid &&
             hasDiagnosticCode(runtimeDependency.diagnostics, "GTI-S2057") &&
             hasDiagnostic(runtimeDependency.diagnostics,
                           "reads a binding that is not constexpr"),
         "runtime values should remain invalid in static assertions");

  const lang::FrontendResult unusedDependent =
      lang::Frontend().analyze("unused-dependent-static-assert.gti", R"(
class Unused<T, uint64_t N> {
  static_assert(sizeof(T) == N, "instance-only invariant");
};
int main() { return 0; }
)");
  const lang::ClassDecl *unusedClass =
      findTopLevelClass(unusedDependent.program, "Unused");
  const lang::StaticAssertDecl *deferred = nullptr;
  if (unusedClass != nullptr) {
    for (const lang::StmtPtr &member : unusedClass->members()) {
      if (const auto *assertion =
              dynamic_cast<const lang::StaticAssertDecl *>(member.get())) {
        deferred = assertion;
      }
    }
  }
  expect(unusedDependent.canGenerateCode() && deferred != nullptr &&
             unusedDependent.semantics.findStaticAssertResult(*deferred) ==
                 lang::StaticAssertResult::Deferred,
         "an unused genuinely dependent assertion should remain a deferred "
         "semantic obligation");

  const lang::FrontendResult repeatedFailure =
      lang::Frontend().analyze("dependent-static-assert.gti", R"(
class Bound<T, uint64_t N> {
  static_assert(N <= uint64_t(4), "capacity too large");
};

int main() {
  Bound<int32_t, 5> first{};
  Bound<int32_t, 5> second{};
  return 0;
}
)");
  expect(!repeatedFailure.hirValid &&
             countDiagnosticCode(repeatedFailure.diagnostics, "GTI-S2079") ==
                 1 &&
             hasRelatedDiagnostic(repeatedFailure.diagnostics,
                                  "Concrete substitution: 'T' = 'int32_t'") &&
             hasRelatedDiagnostic(repeatedFailure.diagnostics,
                                  "Concrete substitution: 'N' = '5'") &&
             hasRelatedDiagnostic(repeatedFailure.diagnostics,
                                  "Concrete generic instance requested here"),
         "a dependent class assertion should run once per canonical instance "
         "and identify substitutions plus the first requiring use");

  const lang::FrontendResult functionFailure =
      lang::Frontend().analyze("function-static-assert.gti", R"(
int bounded<uint64_t N>(int values[N]) {
  static_assert(N <= uint64_t(2), "too many values");
  return values[0];
}
int main() { return bounded({1, 2, 3}); }
)");
  expect(!functionFailure.hirValid &&
             countDiagnosticCode(functionFailure.diagnostics, "GTI-S2079") ==
                 1 &&
             hasRelatedDiagnostic(functionFailure.diagnostics,
                                  "Concrete substitution: 'N' = '3'") &&
             hasRelatedDiagnostic(functionFailure.diagnostics,
                                  "Concrete generic instance requested here"),
         "dependent assertions in generic function bodies should evaluate "
         "during canonical function-instance reanalysis");

  const lang::FrontendResult constructorFailure =
      lang::Frontend().analyze("constructor-static-assert.gti", R"(
class Extent {
public:
  Extent<uint64_t N>(int values[N]) {
    static_assert(N <= uint64_t(2), "too many constructor values");
  }
};
int main() {
  Extent value = Extent({1, 2, 3});
  return 0;
}
)");
  expect(!constructorFailure.hirValid &&
             countDiagnosticCode(constructorFailure.diagnostics, "GTI-S2079") ==
                 1 &&
             hasRelatedDiagnostic(constructorFailure.diagnostics,
                                  "Concrete substitution: 'N' = '3'") &&
             hasRelatedDiagnostic(constructorFailure.diagnostics,
                                  "Concrete generic instance requested here"),
         "dependent assertions in generic constructors should evaluate "
         "during canonical constructor-instance reanalysis");

  const lang::FrontendResult methodFailure =
      lang::Frontend().analyze("method-static-assert.gti", R"(
class Validator<T> {
public:
  int validate<uint64_t N>(int values[N]) {
    static_assert(sizeof(T) == uint64_t(4) && N <= uint64_t(2),
                  "unsupported method instance");
    return values[0];
  }
};
int main() {
  Validator<int64_t> validator{};
  return validator.validate({1, 2, 3});
}
)");
  expect(!methodFailure.hirValid &&
             countDiagnosticCode(methodFailure.diagnostics, "GTI-S2079") == 1 &&
             hasRelatedDiagnostic(methodFailure.diagnostics,
                                  "Concrete substitution: 'T' = 'int64_t'") &&
             hasRelatedDiagnostic(methodFailure.diagnostics,
                                  "Concrete substitution: 'N' = '3'") &&
             hasRelatedDiagnostic(methodFailure.diagnostics,
                                  "Concrete generic instance requested here"),
         "dependent assertions in generic methods should evaluate with the "
         "combined owner and method substitution");

  lang::FrontendOptions noToolingOptions;
  noToolingOptions.toolingOccurrences = false;
  const lang::FrontendResult noToolingFailure =
      lang::Frontend(noToolingOptions)
          .analyze("no-tooling-static-assert.gti", R"(
class Limited<uint64_t N> {
  static_assert(N < uint64_t(2), "extent is too large");
};
int main() {
  Limited<3> value{};
  return 0;
}
)");
  expect(!noToolingFailure.hirValid &&
             countDiagnosticCode(noToolingFailure.diagnostics, "GTI-S2079") ==
                 1,
         "dependent assertion detection must not depend on semantic tooling "
         "occurrence collection");

  const lang::FrontendResult immediateGenericFailure =
      lang::Frontend().analyze("immediate-generic-static-assert.gti", R"(
class Broken<T> {
  static_assert(false, "not dependent");
};
int main() { return 0; }
)");
  expect(!immediateGenericFailure.semanticValid &&
             countDiagnosticCode(immediateGenericFailure.diagnostics,
                                 "GTI-S2079") == 1,
         "a non-dependent false assertion in an unused generic declaration "
         "should fail immediately");

  const std::array<std::pair<std::string, std::string>, 7> malformed{{
      {"static_assert; int recovered() { return 0; }", "Expect '('"},
      {"static_assert(); int recovered() { return 0; }", "Expect a condition"},
      {"static_assert(true,); int recovered() { return 0; }", "trailing comma"},
      {"static_assert(true, 1); int recovered() { return 0; }",
       "quoted string message"},
      {"static_assert(true, \"one\", \"two\"); "
       "int recovered() { return 0; }",
       "only a condition and one optional message"},
      {"static_assert(true) int recovered() { return 0; }", "Expect ';'"},
      {"extern \"C\" { static_assert(true); int ok(); }",
       "may contain only function declarations"},
  }};
  for (const auto &[source, expected] : malformed) {
    lang::Parser parser(lexer.scan(source, "malformed-static-assert.gti"));
    const lang::Program recovered = parser.parse();
    expect(parser.hadError() && hasDiagnostic(parser.errors(), expected) &&
               !recovered.declarations().empty(),
           "malformed static assertions should diagnose precisely and retain "
           "a recoverable declaration boundary");
  }
}

void testStaticStorageAndMembers() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keyword = lexer.scan("static int value = 1;");
  expect(keyword.size() >= 2 && keyword.front().kind == lang::TokenKind::STATIC,
         "static should be a dedicated declaration keyword");

  const std::string source = R"(
static int file_value = 7;
static int add_file_value(int value) { return value + file_value; }

namespace detail {
static int namespace_value = 5;
static int read_namespace_value() { return namespace_value; }
}

class Counter {
public:
  static int answer = 42;
  static mut int count = 0;
  static int current() { return count; }
  int value = 3;
};

class Empty {
public:
  static Empty value = Empty();
};

class Managed {
public:
  static mut int live = 0;
  ~Managed() {}
};

int main() {
  Counter::count = add_file_value(2);
  int result = Counter::current() + Counter::answer +
               detail::read_namespace_value() + detail::namespace_value;
  if (result == 61) { return 0; }
  return 1;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("static-members.gti", source);
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "namespace and class static declarations should compile");
  const std::size_t assignmentOwner = source.find("Counter::count =");
  const std::vector<lang::SemanticOccurrence> &entryOccurrences =
      frontend.semantics.database().occurrences(
          frontend.sourceGraph.entryUnit());
  const auto ownerOccurrence = std::find_if(
      entryOccurrences.begin(), entryOccurrences.end(),
      [assignmentOwner](const lang::SemanticOccurrence &occurrence) {
        return occurrence.span.start == assignmentOwner &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::TypeUse);
      });
  expect(ownerOccurrence != entryOccurrences.end(),
         "qualified static assignments should retain their class occurrence");

  const lang::ClassTypeInfo *counter = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Counter") {
      counter = frontend.semantics.findClassType(instance.declaration);
      expect(instance.fields.size() == 1 && instance.staticFields.size() == 2,
             "HIR should keep static fields separate from object fields");
      break;
    }
  }
  expect(counter != nullptr && counter->fields.size() == 1 &&
             counter->staticFields.size() == 2,
         "semantic class layout should exclude static data members");

  bool foundStaticMethod = false;
  bool foundStaticFunction = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source == nullptr) {
      continue;
    }
    if (function.source->name().lexeme == "current") {
      foundStaticMethod = function.staticMember && !function.internalLinkage;
    } else if (function.source->name().lexeme == "add_file_value") {
      foundStaticFunction = !function.staticMember && function.internalLinkage;
    }
  }
  expect(foundStaticMethod && foundStaticFunction,
         "HIR functions should distinguish static methods from internal "
         "namespace functions");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      generated.find("static const std::int32_t answer;") !=
              std::string::npos &&
          generated.find("inline const std::int32_t Counter::answer = "
                         "static_cast<std::int32_t>(42)") !=
              std::string::npos &&
          generated.find("static std::int32_t count;") != std::string::npos &&
          generated.find("inline std::int32_t Counter::count = "
                         "static_cast<std::int32_t>(0)") != std::string::npos &&
          generated.find("inline const ::__gti_program::Empty Empty::value = "
                         "::__gti_program::Empty{}") != std::string::npos &&
          generated.find("inline std::int32_t Managed::live = "
                         "static_cast<std::int32_t>(0)") != std::string::npos &&
          generated.find("live(std::move(other.live))") == std::string::npos &&
          generated.find("static std::int32_t __gti_fn_") !=
              std::string::npos &&
          generated.find("detail::__gti_static_") != std::string::npos &&
          generated.find("__gti_static_") != std::string::npos,
      "the C++ backend should emit inline class statics and uniquely named "
      "qualified internal-linkage declarations");

  const lang::FrontendResult localStatic = lang::Frontend().analyze(
      "local-static.gti", "int main() { static int value = 1; return value; }");
  expect(!localStatic.syntaxValid &&
             hasDiagnostic(localStatic.diagnostics,
                           "Block-scope static declarations are not supported"),
         "block-scope static should fail with a focused parser diagnostic");

  const lang::FrontendResult invalidMembers =
      lang::Frontend().analyze("invalid-static-members.gti", R"(
class Bad {
public:
  static int missing;
  int value = 1;
  static int read() { return this.value; }
};
)");
  expect(!invalidMembers.semanticValid &&
             hasDiagnostic(invalidMembers.diagnostics,
                           "require an in-class initializer") &&
             hasDiagnostic(invalidMembers.diagnostics,
                           "Static methods do not have a 'this' object"),
         "static fields should require definitions and static methods should "
         "have no receiver");

  const lang::FrontendResult invalidAccess =
      lang::Frontend().analyze("invalid-static-access.gti", R"(
class Registry {
public:
  static int value = 1;
};

int main() {
  Registry registry = Registry();
  return registry.value;
}
)");
  expect(!invalidAccess.semanticValid &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "must be accessed through its class or struct name"),
         "static members should reject object-qualified access");

  const lang::FrontendResult genericStatic =
      lang::Frontend().analyze("generic-static.gti", R"(
class Registry<T> {
public:
  static int value = 1;
};
)");
  expect(!genericStatic.semanticValid &&
             hasDiagnostic(genericStatic.diagnostics,
                           "qualified generic member paths"),
         "generic class statics should remain rejected until qualified "
         "generic paths have a complete representation");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-static-storage";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path left = root / "left.gti";
  const std::filesystem::path right = root / "right.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const lang::FrontendResult separateUnits = lang::Frontend().analyze(
      entry,
      "#include \"left.gti\"\n#include \"right.gti\"\n"
      "int main() { return left_value() + right_value() - 3; }\n",
      {},
      {{canonical(left),
        "using SharedName = int; static int value = 1; "
        "static int helper() { return value; } "
        "int left_value() { SharedName local = helper(); return local; }\n"},
       {canonical(right), "static int SharedName = 2; static int value = 2; "
                          "static int helper() { return value; } "
                          "int right_value() { return helper(); }\n"}});
  expect(separateUnits.canGenerateCode() && separateUnits.diagnostics.empty(),
         "different source units should isolate static names from sibling "
         "symbols and aliases");

  const lang::FrontendResult hiddenStatic = lang::Frontend().analyze(
      entry, "#include \"left.gti\"\nint main() { return value; }\n", {},
      {{canonical(left), "static int value = 1;\n"}});
  expect(!hiddenStatic.semanticValid &&
             hasDiagnostic(hiddenStatic.diagnostics, "Undefined name 'value'"),
         "namespace static declarations should not leak into includers");
}

void testConcurrentGlobalPolicy() {
  lang::FrontendOptions concurrentOptions;
  concurrentOptions.target = lang::TargetInfo::host();
  concurrentOptions.target.executionProfile =
      lang::ExecutionProfile::Concurrent;

  const std::string mutableSource = R"(
mut int namespace_count = 0;
static mut int internal_count = 0;
class Registry {
public:
  static mut int current = 0;
};
int main() { return namespace_count + internal_count + Registry::current; }
)";
  const lang::FrontendResult singleThreaded =
      lang::Frontend().analyze("single-threaded-globals.gti", mutableSource);
  expect(singleThreaded.canGenerateCode() &&
             singleThreaded.semantics.executionProfile() ==
                 lang::ExecutionProfile::SingleThreaded,
         "the default execution profile should preserve existing mutable "
         "namespace and static-field storage");

  const lang::FrontendResult mutableConcurrent =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-mutable-globals.gti", mutableSource);
  const lang::Diagnostic *mutableDiagnostic =
      findDiagnosticByCode(mutableConcurrent.diagnostics, "GTI-S2060");
  expect(!mutableConcurrent.canGenerateCode() &&
             countDiagnosticCode(mutableConcurrent.diagnostics, "GTI-S2060") ==
                 3 &&
             mutableDiagnostic != nullptr &&
             mutableDiagnostic->primary.start ==
                 mutableSource.find("namespace_count") &&
             mutableDiagnostic->message.find("requires namespace global") !=
                 std::string::npos &&
             hasDiagnostic(mutableConcurrent.diagnostics,
                           "requires static field 'current' to be immutable") &&
             hasDiagnosticHint(mutableConcurrent.diagnostics,
                               "Remove 'mut' from the binding") &&
             mutableDiagnostic->fixes.empty(),
         "the concurrent profile should reject mutable external, internal, "
         "and class-static storage with one migration diagnostic per "
         "declaration and no unsafe mechanical fix");

  const lang::FrontendResult genericStatic =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-generic-static.gti", R"(
class Registry<T> {
public:
  static mut int current = 0;
};
)");
  expect(!genericStatic.canGenerateCode() &&
             countDiagnosticCode(genericStatic.diagnostics, "GTI-S2039") == 1 &&
             countDiagnosticCode(genericStatic.diagnostics, "GTI-S2060") == 0 &&
             hasDiagnostic(genericStatic.diagnostics,
                           "qualified generic member paths"),
         "generic static fields should remain unconditionally rejected under "
         "the concurrent profile while the deliberate GTI-S2060 suppression "
         "avoids a redundant second diagnostic");

  const std::string validSource = R"(
using Count = int;
class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
};
[[unsafe_share]] class ReviewedSharedState {
  mut int value = 0;
};
Count published = 1;
static Count internal = 2;
Box<int> boxed = Box<int>(3);
ReviewedSharedState synchronized = ReviewedSharedState();
class Registry {
public:
  static int value = 4;
};
int main() { return published + internal + Registry::value - 7; }
)";
  const lang::FrontendResult valid =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-immutable-globals.gti", validSource);
  expect(valid.canGenerateCode() && valid.diagnostics.empty() &&
             valid.semantics.executionProfile() ==
                 lang::ExecutionProfile::Concurrent &&
             valid.hir.executionProfile() ==
                 lang::ExecutionProfile::Concurrent &&
             valid.mir.executionProfile() == lang::ExecutionProfile::Concurrent,
         "immutable share-capable aliases, concrete generics, internal "
         "storage, static fields, and reviewed sharing assertions should "
         "compile while one selected profile fact reaches semantics, HIR, "
         "and MIR");

  const std::string cleanupGlobalSource = R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() {}
};
class Wrapper {
public:
  Cleanup nested = Cleanup();
};
class CleanupBase {
public:
  CleanupBase() {}
  ~CleanupBase() {}
};
class DerivedCleanup : public CleanupBase {
public:
  DerivedCleanup() : CleanupBase() {}
};
using Wrapped = Wrapper;
Wrapped global_cleanup{};
Wrapped cleanup_array[2] = {Wrapped(), Wrapped()};
DerivedCleanup base_cleanup{};
class Registry {
public:
  static Wrapped static_cleanup{};
};
int main() { return 0; }
)";
  const lang::FrontendResult cleanupGlobals =
      lang::Frontend().analyze("cleanup-globals.gti", cleanupGlobalSource);
  const lang::Diagnostic *cleanupDiagnostic =
      findDiagnosticByCode(cleanupGlobals.diagnostics, "GTI-S2061");
  if (countDiagnosticCode(cleanupGlobals.diagnostics, "GTI-S2061") != 4) {
    for (const lang::Diagnostic &diagnostic : cleanupGlobals.diagnostics) {
      std::cerr << "Unexpected cleanup-global diagnostic [" << diagnostic.code
                << "]: " << diagnostic.message << '\n';
    }
  }
  expect(
      !cleanupGlobals.canGenerateCode() &&
          countDiagnosticCode(cleanupGlobals.diagnostics, "GTI-S2061") == 4 &&
          cleanupDiagnostic != nullptr &&
          cleanupDiagnostic->primary.start ==
              cleanupGlobalSource.find("global_cleanup") &&
          countDiagnosticCode(cleanupGlobals.diagnostics, "GTI-S2029") == 0 &&
          countDiagnosticCode(cleanupGlobals.diagnostics, "GTI-S2003") == 0 &&
          hasDiagnostic(cleanupGlobals.diagnostics,
                        "Static field 'static_cleanup'") &&
          hasRelatedDiagnostic(cleanupGlobals.diagnostics,
                               "Field 'nested' has cleanup-owning type") &&
          hasRelatedDiagnostic(cleanupGlobals.diagnostics,
                               "This base type requires active cleanup") &&
          hasDiagnosticHint(cleanupGlobals.diagnostics,
                            "Move the value into a local binding") &&
          cleanupDiagnostic->fixes.empty(),
      "single-threaded namespace globals and static fields should reject "
      "cleanup-owning aliases, arrays, fields, and bases without unrelated "
      "generic or assignment diagnostics until global shutdown is defined");

  lang::Lexer cleanupQueryLexer;
  lang::Parser cleanupQueryParser(cleanupQueryLexer.scan(R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() {}
};
class Box<T> {
public:
  T nested;
};
class Expand<T> {
public:
  expected<Expand<Expand<T>>, int> nested = unexpected(0);
};
class Branch<T> {
public:
  T left;
  T right;
};
int main() { return 0; }
)",
                                                         "cleanup-query.gti"));
  const lang::Program cleanupQueryProgram = cleanupQueryParser.parse();
  lang::SemanticVisitor cleanupQuerySemantic;
  (void)cleanupQuerySemantic.check(cleanupQueryProgram);
  const lang::ClassDecl *cleanupClass =
      findTopLevelClass(cleanupQueryProgram, "Cleanup");
  const lang::ClassDecl *boxClass =
      findTopLevelClass(cleanupQueryProgram, "Box");
  const lang::ClassDecl *expandClass =
      findTopLevelClass(cleanupQueryProgram, "Expand");
  const lang::ClassDecl *branchClass =
      findTopLevelClass(cleanupQueryProgram, "Branch");
  const lang::ClassTypeInfo *cleanupInfo =
      cleanupClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*cleanupClass);
  const lang::ClassTypeInfo *boxInfo =
      boxClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*boxClass);
  const lang::ClassTypeInfo *expandInfo =
      expandClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*expandClass);
  const lang::ClassTypeInfo *branchInfo =
      branchClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*branchClass);
  lang::SemanticType nestedCleanup = lang::SemanticType::Unknown;
  lang::SemanticType expandingRecurrence = lang::SemanticType::Unknown;
  lang::SemanticType branchingCleanupFree = lang::SemanticType::Int32;
  if (cleanupInfo != nullptr && boxInfo != nullptr) {
    const lang::SemanticType cleanup =
        lang::SemanticType::classType(cleanupInfo->id);
    nestedCleanup = lang::SemanticType::classType(
        boxInfo->id, {lang::SemanticType::classType(boxInfo->id, {cleanup})});
  }
  if (expandInfo != nullptr) {
    expandingRecurrence = lang::SemanticType::classType(
        expandInfo->id, {lang::SemanticType::Int32});
  }
  if (branchInfo != nullptr) {
    for (std::size_t depth = 0; depth < 18; ++depth) {
      branchingCleanupFree = lang::SemanticType::classType(
          branchInfo->id, {std::move(branchingCleanupFree)});
    }
  }
  expect(
      !cleanupQueryLexer.hadError() && !cleanupQueryParser.hadError() &&
          cleanupInfo != nullptr && boxInfo != nullptr &&
          expandInfo != nullptr && branchInfo != nullptr &&
          cleanupQuerySemantic.requiresActiveCleanupFor(nestedCleanup) &&
          cleanupQuerySemantic.requiresActiveCleanupFor(expandingRecurrence) &&
          !cleanupQuerySemantic.requiresActiveCleanupFor(branchingCleanupFree),
      "active-cleanup queries should preserve decreasing concrete generic "
      "detection, terminate non-decreasing recurrence, and memoize repeated "
      "cleanup-free generic branches");

  const std::string nonShareableSource = R"(
class RawState { int* address = nullptr; };
using RawAlias = RawState;
class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
};
[[no_share]] class NativeLikeHandle { int descriptor = -1; };
class Cleanup {
  int value = 0;
public:
  ~Cleanup() {}
};
int* raw_address = nullptr;
std::string_view view = "view";
RawAlias aliased = RawAlias();
Box<std::string_view> generic = Box<std::string_view>("view");
NativeLikeHandle native = NativeLikeHandle();
Cleanup cleanup = Cleanup();
class StaticRaw {
public:
  static int* address = nullptr;
};
int main() { return 0; }
)";
  const lang::FrontendResult nonShareable =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-non-shareable-globals.gti", nonShareableSource,
                   {standardLibraryPrelude()});
  if (countDiagnosticCode(nonShareable.diagnostics, "GTI-S2060") != 6 ||
      countDiagnosticCode(nonShareable.diagnostics, "GTI-S2061") != 1) {
    for (const lang::Diagnostic &diagnostic : nonShareable.diagnostics) {
      std::cerr << "Unexpected concurrent-global diagnostic ["
                << diagnostic.code << "]: " << diagnostic.message << '\n';
    }
  }
  expect(!nonShareable.canGenerateCode() &&
             countDiagnosticCode(nonShareable.diagnostics, "GTI-S2060") == 6 &&
             countDiagnosticCode(nonShareable.diagnostics, "GTI-S2061") == 1 &&
             hasDiagnostic(nonShareable.diagnostics,
                           "'raw_address' to have a share-capable type") &&
             hasDiagnostic(nonShareable.diagnostics,
                           "'generic' to have a share-capable type") &&
             hasDiagnostic(nonShareable.diagnostics,
                           "'native' to have a share-capable type") &&
             hasDiagnostic(nonShareable.diagnostics,
                           "Namespace global 'cleanup' has type 'Cleanup'") &&
             hasRelatedDiagnostic(nonShareable.diagnostics,
                                  "explicitly opts out of sharing") &&
             hasRelatedDiagnostic(nonShareable.diagnostics,
                                  "Declared cleanup makes this type require") &&
             hasRelatedDiagnostic(nonShareable.diagnostics,
                                  "field has non-share-capable type") &&
             hasDiagnosticHint(nonShareable.diagnostics,
                               "declare [[unsafe_share]] only after"),
         "the concurrent profile should reject direct pointers, borrowed "
         "views, aliases, concrete generic state, native-like opt-outs, "
         "cleanup owners, and static fields with deterministic causes");

  const lang::FrontendResult trustedNative =
      analyzeTrustedPreludeFixture("concurrent-native-wrapper.gti",
                                   R"(
namespace std {
[[no_transfer, no_share]] class native_resource {
  int descriptor;
public:
  native_resource(int value) : descriptor(value) {}
};
}
std::native_resource resource = std::native_resource(1);
int main() { return 0; }
)",
                                   concurrentOptions);
  expect(!trustedNative.canGenerateCode() &&
             countDiagnosticCode(trustedNative.diagnostics, "GTI-S2060") == 1 &&
             hasDiagnostic(trustedNative.diagnostics,
                           "'resource' to have a share-capable type") &&
             hasRelatedDiagnostic(trustedNative.diagnostics,
                                  "explicitly opts out of sharing"),
         "trusted standard-library native wrappers should obey the same "
         "concurrent global policy without a privileged exemption");
}

void testCompileTimeConditionals() {
  lang::Lexer lexer;
  const std::string source = R"(
#if target.vendor == "apple"
int platform_value() { return 101; }
#if target.arch == "arm64"
int nested_value() { return 64; }
#else
int nested_value() { return 32; }
#endif
#elif target.os == "windows"
int platform_value() { return 202; }
#else
int platform_value() { return 303; }
#endif

#if target.os == "never"
expected<int, int> inactive_error() { return missing_name; }
@runtime("stdout.write")
void inactive_runtime(gti_internal::text_view value);
#error "This target is deliberately unsupported."
#endif

class PlatformInfo {
#if target.arch == "arm64"
  int bits = 64;
#else
  int bits = 32;
#endif
public:
  int selected_bits() { return this.bits; }
};

int main() {
  PlatformInfo info{};
#if target.os != "windows"
  int value = platform_value();
#else
  int value = platform_value();
#endif
  return value + info.selected_bits();
}
)";

  {
    lang::Lexer lexer;
    const std::vector<lang::Token> directiveTokens = lexer.scan(
        "#define DEBUG\n#undef DEBUG\n#ifdef DEBUG\n#endif\n#ifndef DEBUG\n"
        "#endif\n");
    expect(!lexer.hadError() && directiveTokens.size() >= 8 &&
               directiveTokens[0].kind == lang::TokenKind::HASH_DEFINE &&
               directiveTokens[2].kind == lang::TokenKind::HASH_UNDEF &&
               directiveTokens[4].kind == lang::TokenKind::HASH_IFDEF,
           "configuration directives should have dedicated lexer tokens");

    const std::string selectionSource = R"(
#define LOCAL
#define LOCAL
#if target.os == "never"
#define LEAKED
#define ALTERNATE
#define EXTERNAL
#endif

#if defined(EXTERNAL) &&
    (target.os != "never" || defined(ALTERNATE))
int selected_value() { return 11; }
#else
int selected_value() { return 22; }
#endif

#undef EXTERNAL
#ifdef EXTERNAL
#error "a source undef did not remove an externally seeded flag"
#endif

#ifdef LEAKED
#error "an inactive definition leaked"
#endif

#undef LOCAL
#ifndef LOCAL
int local_state() { return 7; }
#else
int local_state() { return 9; }
#endif

int main() { return selected_value() + local_state(); }
)";
    lang::FrontendOptions configuredOptions;
    configuredOptions.configurationFlags = {"EXTERNAL"};
    const lang::FrontendResult configured =
        lang::Frontend(configuredOptions)
            .analyze("configuration-selection.gti", selectionSource);
    const lang::SourceUnit *configurationUnit =
        configured.sourceGraph.findUnit(configured.sourceGraph.entryUnit());
    const auto hasConfigurationRoleAt = [&](std::size_t position,
                                            lang::ConfigurationTokenKind kind) {
      return configurationUnit != nullptr &&
             std::any_of(configurationUnit->configurationTokens.begin(),
                         configurationUnit->configurationTokens.end(),
                         [&](const lang::ConfigurationToken &configuration) {
                           return configuration.kind == kind &&
                                  configuration.span.start == position;
                         });
    };
    const std::size_t defineLocal =
        selectionSource.find("LOCAL", selectionSource.find("#define LOCAL"));
    const std::size_t definedOperator = selectionSource.find("defined");
    const std::size_t definedExternal =
        selectionSource.find("EXTERNAL", definedOperator);
    const std::size_t undefExternal =
        selectionSource.find("EXTERNAL", selectionSource.find("#undef"));
    expect(hasConfigurationRoleAt(defineLocal,
                                  lang::ConfigurationTokenKind::Flag) &&
               hasConfigurationRoleAt(definedOperator,
                                      lang::ConfigurationTokenKind::Operator) &&
               hasConfigurationRoleAt(definedExternal,
                                      lang::ConfigurationTokenKind::Flag) &&
               hasConfigurationRoleAt(undefExternal,
                                      lang::ConfigurationTokenKind::Flag),
           "the source graph should retain exact configuration operator and "
           "flag-name roles for tooling");
    expect(configured.canGenerateCode() && configured.diagnostics.empty(),
           "boolean target/flag conditions and source-order define changes "
           "should compile without diagnostics");
    const std::string configuredCpp = gti_test::emitCppText(configured);
    expect(configuredCpp.find("static_cast<std::int32_t>(11)") !=
                   std::string::npos &&
               configuredCpp.find("static_cast<std::int32_t>(7)") !=
                   std::string::npos &&
               configuredCpp.find("static_cast<std::int32_t>(22)") ==
                   std::string::npos &&
               configuredCpp.find("inactive definition leaked") ==
                   std::string::npos &&
               configuredCpp.find("#define LOCAL") == std::string::npos &&
               configuredCpp.find("#define EXTERNAL") == std::string::npos,
           "only selected configuration branches should reach the backend");

    const lang::FrontendResult unconfigured = lang::Frontend().analyze(
        "configuration-selection.gti", selectionSource);
    expect(unconfigured.canGenerateCode() && unconfigured.diagnostics.empty(),
           "the same source should compile under an empty external define set");
    const std::string unconfiguredCpp = gti_test::emitCppText(unconfigured);
    expect(unconfiguredCpp.find("static_cast<std::int32_t>(22)") !=
                   std::string::npos &&
               unconfiguredCpp.find("static_cast<std::int32_t>(11)") ==
                   std::string::npos,
           "external define sets should deterministically change selection");

    const lang::FrontendResult invalidInactiveBranch = lang::Frontend().analyze(
        "configuration-inactive-syntax.gti",
        "#define KNOWN\n#undef KNOWN\n#if defined(KNOWN)\n"
        "int broken( { return 0; }\n#endif\n"
        "int main() { return 0; }\n");
    expect(!invalidInactiveBranch.syntaxValid,
           "inactive configuration branches should remain syntactically valid "
           "GTI rather than being skipped as text");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "gti-configuration-includes";
    const std::filesystem::path entry = root / "main.gti";
    const std::filesystem::path dependency = root / "flags.gti";
    const std::string dependencyKey =
        std::filesystem::weakly_canonical(dependency).string();
    const lang::FrontendResult includeOrdered = lang::Frontend().analyze(
        entry,
        "#define BEFORE_INCLUDE\n"
        "#include \"flags.gti\"\n"
        "#ifdef FROM_INCLUDE\n"
        "int main() { return included_value(); }\n"
        "#else\n#error \"include flags were not visible\"\n#endif\n",
        {},
        {{dependencyKey, "#ifdef BEFORE_INCLUDE\n#define FROM_INCLUDE\n#endif\n"
                         "int included_value() { return 0; }\n"}});
    expect(includeOrdered.canGenerateCode() &&
               includeOrdered.diagnostics.empty(),
           "defines before an include should enter it and active definitions "
           "from the included unit should remain visible afterwards");

    const lang::FrontendResult replacement = lang::Frontend().analyze(
        "configuration-replacement.gti",
        "#define BUFFER_SIZE 4096\nint main() { return 0; }\n");
    expect(!replacement.sourceValid &&
               hasDiagnosticCode(replacement.diagnostics, "GTI-I0011") &&
               hasDiagnostic(replacement.diagnostics, "constexpr"),
           "replacement-list macros should be rejected with the typed-constant "
           "alternative");

    const lang::FrontendResult missingName =
        lang::Frontend().analyze("configuration-missing-name.gti",
                                 "#define\nint main() { return 0; }\n");
    expect(!missingName.sourceValid &&
               hasDiagnosticCode(missingName.diagnostics, "GTI-L0012"),
           "configuration directives should require an ordinary identifier");

    const lang::FrontendResult malformedCondition = lang::Frontend().analyze(
        "configuration-malformed.gti",
        "#if defined(DEBUG) &&\nint main() { return 0; }\n#endif\n");
    expect(!malformedCondition.syntaxValid &&
               hasDiagnosticCode(malformedCondition.diagnostics, "GTI-P0003"),
           "malformed boolean compile conditions should use the focused parser "
           "diagnostic");

    const lang::FrontendResult definedExpression = lang::Frontend().analyze(
        "configuration-defined-expression.gti",
        "bool enabled = defined(DEBUG);\nint main() { return 0; }\n");
    expect(!definedExpression.syntaxValid &&
               hasDiagnosticCode(definedExpression.diagnostics, "GTI-P0004"),
           "defined(...) should remain confined to compile-time conditions");

    const lang::FrontendResult unknownFlag = lang::Frontend().analyze(
        "configuration-unknown.gti",
        "#ifdef DEBGU\nint unused() { return 1; }\n#endif\n"
        "#undef NOT_DEFINED\nint main() { return 0; }\n");
    const auto warning = std::find_if(
        unknownFlag.diagnostics.begin(), unknownFlag.diagnostics.end(),
        [](const lang::Diagnostic &diagnostic) {
          return diagnostic.code == "GTI-S2075" &&
                 diagnostic.severity == lang::DiagnosticSeverity::Warning;
        });
    expect(unknownFlag.canGenerateCode() &&
               warning != unknownFlag.diagnostics.end() &&
               countDiagnosticCode(unknownFlag.diagnostics, "GTI-S2075") == 1,
           "unknown flag references should warn without failing compilation, "
           "while undefining an absent flag remains a no-op");

    const std::string formatted = lang::Formatter().format(
        "#define DEBUG\n#if defined(DEBUG)&&!(target.os==\"never\")\n"
        "int value=1;\n#undef DEBUG\n#endif");
    expect(formatted == "#define DEBUG\n#if defined(DEBUG) && !(target.os == "
                        "\"never\")\nint value = 1;\n#undef DEBUG\n#endif\n",
           "the formatter should preserve and normalize configuration "
           "directives and boolean conditions");
  }
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "compile-time directives should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "compile-time branches should parse");

  const lang::TargetInfo apple{"macos", "apple", "arm64"};
  lang::SemanticVisitor appleSemantic(apple);
  expect(appleSemantic.check(program),
         "inactive branches should not participate in Apple semantics");
  const std::string appleCpp = emitAnalyzedSource(source, apple);
  const auto appleProgramStart = appleCpp.find("namespace __gti_program {");
  expect(
      appleCpp.find("static_cast<std::int32_t>(101)") != std::string::npos &&
          appleCpp.find("static_cast<std::int32_t>(64)") != std::string::npos &&
          appleCpp.find("std::int32_t bits;") != std::string::npos &&
          appleCpp.find("PlatformInfo::PlatformInfo()") != std::string::npos &&
          appleCpp.find("missing_name") == std::string::npos &&
          appleCpp.find("#include <expected>") == std::string::npos &&
          appleCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
          appleProgramStart != std::string::npos &&
          appleCpp.find("#if", appleProgramStart) == std::string::npos,
      "Apple lowering should emit only active branches without C++ macros");

  const lang::TargetInfo windows{"windows", "pc", "x86_64"};
  lang::SemanticVisitor windowsSemantic(windows);
  expect(windowsSemantic.check(program),
         "Windows should select the elif and else branches");
  const std::string windowsCpp = emitAnalyzedSource(source, windows);
  expect(windowsCpp.find("static_cast<std::int32_t>(202)") !=
                 std::string::npos &&
             windowsCpp.find("std::int32_t bits;") != std::string::npos &&
             windowsCpp.find("PlatformInfo::PlatformInfo()") !=
                 std::string::npos &&
             windowsCpp.find("static_cast<std::int32_t>(32)") !=
                 std::string::npos &&
             windowsCpp.find("nested_value") == std::string::npos &&
             windowsCpp.find("static_cast<std::int32_t>(101)") ==
                 std::string::npos,
         "target selection should distinguish vendor, OS, and architecture");

  auto malformedTokens = lexer.scan(R"(
#if target.os == "never"
int broken = ;
#else
int valid = 1;
#endif
)");
  lang::Parser malformedParser(std::move(malformedTokens));
  malformedParser.parse();
  expect(malformedParser.errors().size() == 1,
         "inactive branches must still be syntactically valid");

  auto invalidConditionTokens = lexer.scan(R"(
#if target.platform == "macos"
int value = 1;
#endif
)");
  lang::Parser invalidConditionParser(std::move(invalidConditionTokens));
  invalidConditionParser.parse();
  expect(invalidConditionParser.hadError(),
         "unknown target properties should be diagnosed");

  const std::vector<lang::Token> errorKeyword =
      lexer.scan("#error \"unsupported configuration\"");
  expect(errorKeyword.size() >= 3 &&
             errorKeyword.front().kind == lang::TokenKind::HASH_ERROR,
         "#error should have a dedicated directive token");

  const std::string errorSource = R"(
#if target.os == "blocked"
#error "This target is not supported."
#endif

class PlatformState {
#if target.os == "blocked"
#error "PlatformState is unavailable on this target."
#endif
  int value = 1;
};

int main() {
#if target.os == "blocked"
#error "No entry point is available for this target."
#endif
  return 0;
}
)";
  auto errorTokens = lexer.scan(errorSource);
  lang::Parser errorParser(std::move(errorTokens));
  lang::Program errorProgram = errorParser.parse();
  expect(!errorParser.hadError(),
         "#error should parse in declaration, member, and block contexts");

  const lang::TargetInfo allowed{"macos", "apple", "arm64"};
  lang::SemanticVisitor allowedSemantic(allowed);
  expect(allowedSemantic.check(errorProgram),
         "inactive #error directives should not affect semantics");
  const std::string allowedCpp = emitAnalyzedSource(errorSource, allowed);
  const auto allowedProgramStart = allowedCpp.find("namespace __gti_program {");
  // Backend-private ABI guards are independent of folded source directives.
  expect(allowedProgramStart != std::string::npos &&
             allowedCpp.find("#error", allowedProgramStart) ==
                 std::string::npos,
         "source #error directives should never leak into program output");

  const lang::TargetInfo blocked{"blocked", "test", "test"};
  lang::SemanticVisitor blockedSemantic(blocked);
  expect(!blockedSemantic.check(errorProgram) &&
             blockedSemantic.errors().size() == 3 &&
             std::all_of(blockedSemantic.errors().begin(),
                         blockedSemantic.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-S2047";
                         }) &&
             hasDiagnostic(blockedSemantic, "This target is not supported."),
         "active #error directives should report their source messages once");

  auto missingMessageTokens = lexer.scan("#error\nint value = 1;");
  lang::Parser missingMessageParser(std::move(missingMessageTokens));
  missingMessageParser.parse();
  expect(missingMessageParser.hadError() &&
             hasDiagnostic(missingMessageParser.errors(),
                           "Expect a string message after '#error'"),
         "#error should require an explicit string message");

  const std::string formattedError = lang::Formatter().format(
      "#if target.os==\"never\"\n#error   \"unsupported\"\n#endif");
  expect(formattedError ==
             "#if target.os == \"never\"\n#error \"unsupported\"\n#endif\n",
         "the formatter should preserve and normalize compile-time errors");
}

constexpr NamedTest kTests[] = {
    {"testFixedWidthIntegers", testFixedWidthIntegers},
    {"testContextualSignedIntegerLiterals",
     testContextualSignedIntegerLiterals},
    {"testBinaryFloatSemantics", testBinaryFloatSemantics},
    {"testCharactersAndStringViews", testCharactersAndStringViews},
    {"testStandardString", testStandardString},
    {"testLogicalOperatorSpellings", testLogicalOperatorSpellings},
    {"testComparisonOperatorFormattingStability",
     testComparisonOperatorFormattingStability},
    {"testIntegerBitwiseAndModuloOperators",
     testIntegerBitwiseAndModuloOperators},
    {"testCheckedArithmeticAndCompoundAssignments",
     testCheckedArithmeticAndCompoundAssignments},
    {"testConstexprBindings", testConstexprBindings},
    {"testConstexprStringViewInspection", testConstexprStringViewInspection},
    {"testMirConstantEvaluation", testMirConstantEvaluation},
    {"testMirConstantRecords", testMirConstantRecords},
    {"testConstexprLibrary", testConstexprLibrary},
    {"testConstexprOperators", testConstexprOperators},
    {"testDependentConstexprBranches", testDependentConstexprBranches},
    {"testConstexprReadOnlyReferences", testConstexprReadOnlyReferences},
    {"testConstexprMutation", testConstexprMutation},
    {"testStaticAssertions", testStaticAssertions},
    {"testStaticStorageAndMembers", testStaticStorageAndMembers},
    {"testConcurrentGlobalPolicy", testConcurrentGlobalPolicy},
    {"testCompileTimeConditionals", testCompileTimeConditionals},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_constexpr_tests");
  return runNamedTests(argc, argv, kTests, "constexpr");
}
