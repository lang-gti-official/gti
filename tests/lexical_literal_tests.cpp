// Digit separators, numeric literal suffixes and raw string literals through
// the lexer, the frontend's literal typing and the formatter.
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Lexed {
  std::vector<lang::Token> tokens;
  std::vector<lang::Diagnostic> errors;
};

Lexed lex(std::string_view source) {
  lang::Lexer lexer;
  Lexed result;
  result.tokens = lexer.scan(std::string(source), "literals.gti");
  result.errors = lexer.errors();
  return result;
}

const lang::Token &first(const Lexed &lexed) { return lexed.tokens.front(); }

std::uint64_t integerValue(const lang::Token &token) {
  const auto *value = std::get_if<std::uint64_t>(&token.literal);
  return value == nullptr ? ~std::uint64_t{0} : *value;
}

void testIntegerSeparatorsAndSuffixes() {
  struct Case {
    std::string_view source;
    std::uint64_t value;
    lang::NumericSuffix suffix;
  };
  const Case cases[] = {
      {"1_000_000", 1000000, lang::NumericSuffix::None},
      {"0xFF_FF", 0xFFFF, lang::NumericSuffix::None},
      {"0b1010_0101", 0xA5, lang::NumericSuffix::None},
      {"10u8", 10, lang::NumericSuffix::U8},
      {"10_u8", 10, lang::NumericSuffix::U8},
      {"65535u16", 65535, lang::NumericSuffix::U16},
      {"7u32", 7, lang::NumericSuffix::U32},
      {"18_446_744_073_709_551_615u64", 18446744073709551615ULL,
       lang::NumericSuffix::U64},
      {"128i8", 128, lang::NumericSuffix::I8},
      {"3i16", 3, lang::NumericSuffix::I16},
      {"3i32", 3, lang::NumericSuffix::I32},
      {"3i64", 3, lang::NumericSuffix::I64},
      {"0xffu8", 0xff, lang::NumericSuffix::U8},
      {"0x1f_i32", 0x1f, lang::NumericSuffix::I32},
      {"0b1_u8", 1, lang::NumericSuffix::U8},
      // In a hexadecimal literal `f` is a digit, never a floating suffix.
      {"0x1f32", 0x1f32, lang::NumericSuffix::None},
  };
  for (const Case &entry : cases) {
    const Lexed lexed = lex(entry.source);
    const lang::Token &token = first(lexed);
    expect(lexed.errors.empty() && token.kind == lang::TokenKind::INT_LITERAL &&
               token.lexeme == entry.source &&
               integerValue(token) == entry.value &&
               token.numericSuffix == entry.suffix,
           "integer literal '" + std::string(entry.source) +
               "' should keep its spelling, value and suffix");
  }
}

void testFloatingSeparatorsAndSuffixes() {
  struct Case {
    std::string_view source;
    lang::BinaryFloatFormat format;
    std::string_view equivalent;
    lang::NumericSuffix suffix;
  };
  const Case cases[] = {
      {"1_000.000_5", lang::BinaryFloatFormat::Binary32, "1000.0005",
       lang::NumericSuffix::None},
      {"2.5f32", lang::BinaryFloatFormat::Binary32, "2.5",
       lang::NumericSuffix::F32},
      {"2.5f64", lang::BinaryFloatFormat::Binary64, "2.5",
       lang::NumericSuffix::F64},
      {"2.5_f64", lang::BinaryFloatFormat::Binary64, "2.5",
       lang::NumericSuffix::F64},
      {"1f32", lang::BinaryFloatFormat::Binary32, "1.0",
       lang::NumericSuffix::F32},
      {"1_000f64", lang::BinaryFloatFormat::Binary64, "1000.0",
       lang::NumericSuffix::F64},
      {"0.1d", lang::BinaryFloatFormat::Binary64, "0.1",
       lang::NumericSuffix::None},
  };
  for (const Case &entry : cases) {
    const Lexed lexed = lex(entry.source);
    const lang::Token &token = first(lexed);
    const auto *value = std::get_if<lang::BinaryFloat>(&token.literal);
    const lang::BinaryFloatParseResult expected =
        lang::parseBinaryFloat(entry.equivalent, entry.format);
    expect(lexed.errors.empty() &&
               token.kind == lang::TokenKind::FLOAT_LITERAL &&
               token.lexeme == entry.source && value != nullptr && expected &&
               *value == *expected.value && token.numericSuffix == entry.suffix,
           "floating literal '" + std::string(entry.source) +
               "' should denote the same value as '" +
               std::string(entry.equivalent) + "' in its format");
  }
}

void testMalformedNumericLiterals() {
  struct Case {
    std::string_view source;
    std::string_view code;
    std::size_t start;
    std::size_t end;
    std::string_view message;
  };
  const Case cases[] = {
      {"0x_ff", "GTI-L0007", 2, 3, "cannot follow the radix prefix '0x'"},
      {"0b_1", "GTI-L0007", 2, 3, "cannot follow the radix prefix '0b'"},
      {"1_", "GTI-L0007", 1, 2, "must be followed by a digit"},
      {"1__000", "GTI-L0007", 1, 3, "Consecutive digit separators"},
      {"1___0", "GTI-L0007", 1, 4, "Consecutive digit separators"},
      {"10__u8", "GTI-L0007", 2, 4, "Consecutive digit separators"},
      {"1_.5", "GTI-L0006", 1, 2, "next to '.'"},
      {"1._5", "GTI-L0006", 2, 3, "next to '.'"},
      {"1.5_", "GTI-L0006", 3, 4, "must be followed by a digit"},
      {"1.5u8", "GTI-L0006", 3, 5, "cannot be applied to a floating-point"},
      {"0b1f32", "GTI-L0007", 3, 6, "requires a decimal literal"},
      {"10usize", "GTI-L0007", 2, 7, "Unknown numeric literal suffix"},
      {"10ull", "GTI-L0007", 2, 5, "Unknown numeric literal suffix"},
      {"1.5f", "GTI-L0006", 3, 4, "Unknown numeric literal suffix"},
      {"1e5", "GTI-L0007", 1, 3, "no exponent form"},
      {"1.5e3", "GTI-L0006", 3, 5, "no exponent form"},
      {"1d", "GTI-L0007", 1, 2, "literal with a fraction"},
      {"0b102", "GTI-L0007", 0, 5, "Invalid binary integer literal."},
      {"0x", "GTI-L0007", 0, 2, "Invalid hexadecimal integer literal."},
  };
  for (const Case &entry : cases) {
    const Lexed lexed = lex(entry.source);
    const bool one = lexed.errors.size() == 1;
    const lang::Diagnostic *diagnostic = one ? &lexed.errors.front() : nullptr;
    expect(diagnostic != nullptr && diagnostic->code == entry.code &&
               diagnostic->phase == lang::DiagnosticPhase::Lexing &&
               diagnostic->primary.start == entry.start &&
               diagnostic->primary.end == entry.end &&
               diagnostic->message.find(entry.message) != std::string::npos,
           "malformed literal '" + std::string(entry.source) +
               "' should report one exact lexical diagnostic");
    expect(lexed.tokens.size() == 2,
           "malformed literal '" + std::string(entry.source) +
               "' should recover as one literal token");
  }
}

void testRawStrings() {
  const std::string source = "R\"(a\\nb \"q\" // c)\" "
                             "R\"re(x)\"y)re\" "
                             "R\"()\" "
                             "R\"json(\n{\"k\": 1}\n)json\" "
                             "R xR\"s\" R+1";
  const Lexed lexed = lex(source);
  const auto stringAt = [&](std::size_t index) {
    const auto *value =
        index < lexed.tokens.size()
            ? std::get_if<std::string>(&lexed.tokens[index].literal)
            : nullptr;
    return value == nullptr ? std::string("<none>") : *value;
  };
  const auto kindAt = [&](std::size_t index) {
    return index < lexed.tokens.size() ? lexed.tokens[index].kind
                                       : lang::TokenKind::END_OF_FILE;
  };
  expect(lexed.errors.empty(), "valid raw strings should lex cleanly");
  expect(kindAt(0) == lang::TokenKind::STRING_LITERAL &&
             stringAt(0) == "a\\nb \"q\" // c" &&
             lexed.tokens[0].lexeme == "R\"(a\\nb \"q\" // c)\"",
         "a raw string takes its body verbatim, without escapes or comments");
  expect(kindAt(1) == lang::TokenKind::STRING_LITERAL && stringAt(1) == "x)\"y",
         "a delimiter lets the body contain ')\"'");
  expect(kindAt(2) == lang::TokenKind::STRING_LITERAL && stringAt(2).empty(),
         "an empty raw string has an empty value");
  expect(kindAt(3) == lang::TokenKind::STRING_LITERAL &&
             stringAt(3) == "\n{\"k\": 1}\n" && lexed.tokens[3].line == 1,
         "a raw string keeps its newlines and starts on its opening line");
  expect(kindAt(4) == lang::TokenKind::IDENTIFIER &&
             lexed.tokens[4].lexeme == "R" &&
             kindAt(5) == lang::TokenKind::IDENTIFIER &&
             lexed.tokens[5].lexeme == "xR" &&
             kindAt(6) == lang::TokenKind::STRING_LITERAL &&
             stringAt(6) == "s" && kindAt(7) == lang::TokenKind::IDENTIFIER &&
             lexed.tokens[7].lexeme == "R" && lexed.tokens[7].line == 3,
         "'R' not followed by '\"' and 'R\"' after an identifier character "
         "stay identifier syntax, and lines after a raw string are exact");

  struct Case {
    std::string_view source;
    std::string_view code;
    std::size_t end;
    std::string_view message;
  };
  const Case cases[] = {
      {"R\"(abc", "GTI-L0004", 2, "Unterminated raw string literal."},
      {"R\"abc", "GTI-L0016", 5, "missing '('"},
      {"R\"", "GTI-L0016", 2, "missing '('"},
      {"R\"a b(x)a b\"", "GTI-L0016", 4, "Invalid character"},
      {"R\"a\\(x)a\\\"", "GTI-L0016", 4, "Invalid character"},
      {"R\"12345678901234567(x)12345678901234567\"", "GTI-L0016", 19,
       "at most 16 characters"},
  };
  for (const Case &entry : cases) {
    const Lexed bad = lex(entry.source);
    const lang::Diagnostic *diagnostic =
        bad.errors.size() == 1 ? &bad.errors.front() : nullptr;
    expect(diagnostic != nullptr && diagnostic->code == entry.code &&
               diagnostic->primary.start == 0 &&
               diagnostic->primary.end == entry.end &&
               diagnostic->message.find(entry.message) != std::string::npos,
           "raw string '" + std::string(entry.source) +
               "' should report exactly one diagnostic at its opening");
  }
  const std::string sixteen = "R\"1234567890123456(x)1234567890123456\"";
  expect(lex(sixteen).errors.empty(),
         "a sixteen-character delimiter is accepted");
  expect(lang::scanRawStringLiteral(source, 0).end ==
                 lexed.tokens[0].lexeme.size() &&
             lang::scanRawStringLiteral("R\"a b(x)a b\"; int", 0).end == 12,
         "the shared raw-string extent matches the lexer's token and its "
         "recovery");
}

const auto library =
    std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";

lang::FrontendResult analyze(std::string_view name, std::string_view source) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Mir;
  return lang::Frontend(options).analyze(std::string(name), std::string(source),
                                         {library / "prelude.gti"}, {},
                                         {library});
}

std::vector<std::string> codes(const lang::FrontendResult &result) {
  std::vector<std::string> found;
  for (const lang::Diagnostic &diagnostic : result.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      found.push_back(diagnostic.code);
  return found;
}

void testSuffixedLiteralSemantics() {
  const std::string valid = R"gti(
enum class Small : int8_t { low = -128i8, high = 127i8 };
enum class Wide : uint64_t { top = 18446744073709551615u64 };
constexpr uint64_t million = 1_000_000u64;
static_assert(million == 1000000u64);
static_assert(-128i8 == int8_t(-128));
static_assert(-32768i16 == int16_t(-32768));
static_assert(-2147483648i32 == int32_t(-2147483648));
static_assert(-9223372036854775808i64 + 1i64 == -9223372036854775807i64);
static_assert(255u8 == uint8_t(255) && 65535u16 == uint16_t(65535));
static_assert(4294967295u32 == uint32_t(4294967295));
static_assert(0x1f32 == 7986 && 0xff_u8 == 255u8 && 0b1111_0000 == 240);
static_assert(1_000 + 1 == 1001);
constexpr std::string_view pattern = R"re(\d+"x")re";
static_assert(pattern.size() == 6u64 && pattern == "\\d+\"x\"");
static_assert(R"(a\nb)" == "a\\nb");
uint64_t extent<uint64_t N>() { return N; }
int main() {
  int8_t smallest = -128i8;
  int64_t widened = 5i8;
  uint8_t bytes[4u64] = {1u8, 2u8, 3u8, 4u8};
  switch (bytes[0]) {
  case 1u8:
    break;
  default:
    break;
  }
  int32_t sum = 1i32 + 2i8;
  float single = 2.5f32;
  double whole = 1f64;
  return int32_t(extent<4u64>()) + sum + int32_t(smallest) + int32_t(widened) -
         int32_t(single) - int32_t(whole) + 128 - 13;
}
)gti";
  const lang::FrontendResult accepted = analyze("suffixed-literals.gti", valid);
  if (!accepted.canGenerateCode())
    for (const lang::Diagnostic &diagnostic : accepted.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(accepted.canGenerateCode(),
         "suffixed literals at every range edge, in enums, constexpr, "
         "static_assert, array extents, generic arguments and switch labels "
         "should be accepted");

  struct Case {
    std::string_view body;
    std::string_view code;
    std::string_view message;
    std::string_view span;
  };
  const Case cases[] = {
      {"uint8_t value = 256u8;", "GTI-S2004",
       "Integer literal '256' is out of range for its suffix type 'uint8_t' "
       "(0 to 255).",
       "256u8"},
      {"int8_t value = 128i8;", "GTI-S2004", "(-128 to 127)", "128i8"},
      {"int8_t value = -129i8;", "GTI-S2004", "Integer literal '-129'",
       "-129i8"},
      {"uint8_t value = -1u8;", "GTI-S2004", "Integer literal '-1'", "-1u8"},
      {"uint16_t value = 65536u16;", "GTI-S2004", "'uint16_t'", "65536u16"},
      {"int16_t value = 32768i16;", "GTI-S2004", "'int16_t'", "32768i16"},
      {"uint32_t value = 4294967296u32;", "GTI-S2004", "'uint32_t'",
       "4294967296u32"},
      {"int32_t value = 2147483648i32;", "GTI-S2004", "'int32_t'",
       "2147483648i32"},
      {"int64_t value = 9223372036854775808i64;", "GTI-S2004", "'int64_t'",
       "9223372036854775808i64"},
      {"uint8_t value = 5i32;", "GTI-S2003", "", "5i32"},
      {"float value = 2.5f64;", "GTI-S2003", "", "2.5f64"},
      {"int64_t subject = 1; switch (subject) { case 1i32: break; default: "
       "break; }",
       "GTI-S2037", "does not exactly match", "1i32"},
  };
  for (const Case &entry : cases) {
    const std::string source =
        "int main() {\n  " + std::string(entry.body) + "\n  return 0;\n}\n";
    const lang::FrontendResult rejected =
        analyze("suffixed-rejection.gti", source);
    const std::vector<std::string> found = codes(rejected);
    const auto diagnostic =
        std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [&](const lang::Diagnostic &candidate) {
                       return candidate.code == entry.code;
                     });
    const std::size_t spanStart = source.find(entry.span);
    expect(found.size() == 1 && diagnostic != rejected.diagnostics.end() &&
               diagnostic->message.find(entry.message) != std::string::npos &&
               diagnostic->primary.start == spanStart &&
               diagnostic->primary.end == spanStart + entry.span.size(),
           "'" + std::string(entry.body) + "' should be rejected once with " +
               std::string(entry.code) + " at its literal");
  }

  const lang::FrontendResult call =
      analyze("suffixed-call.gti", "void take(uint8_t value) {}\n"
                                   "int main() { take(7i32); return 0; }\n");
  expect(codes(call) == std::vector<std::string>{"GTI-S2003"},
         "a suffixed argument keeps its type instead of adopting a parameter");
  const lang::FrontendResult enumeration =
      analyze("suffixed-enum.gti", "enum class E : uint8_t { a = 5i8 };\n"
                                   "int main() { return 0; }\n");
  expect(codes(enumeration) == std::vector<std::string>{"GTI-S2036"},
         "a suffixed enum value must have the enum's backing type");
}

void testFormatting() {
  const std::string source =
      "int main(){uint64_t big=1_000_000u64;float f=2.5_f32;"
      "uint32_t h=0xFF_FFu32;std::string_view r=R\"x(  keep  \"this\" // "
      "text\n   exactly )x\";return 0;}\n";
  const std::string expected = "int main() {\n"
                               "  uint64_t big = 1_000_000u64;\n"
                               "  float f = 2.5_f32;\n"
                               "  uint32_t h = 0xFF_FFu32;\n"
                               "  std::string_view r = R\"x(  keep  \"this\" "
                               "// text\n   exactly )x\";\n"
                               "  return 0;\n"
                               "}\n";
  const std::string formatted = lang::Formatter().format(source);
  if (formatted != expected)
    std::cerr << "Formatted literals were:\n" << formatted;
  expect(formatted == expected,
         "the formatter should keep separators, suffixes and raw string text "
         "exactly as written");
  expect(lang::Formatter().format(formatted) == formatted,
         "literal formatting should be idempotent");
}

} // namespace

int main() {
  testIntegerSeparatorsAndSuffixes();
  testFloatingSeparatorsAndSuffixes();
  testMalformedNumericLiterals();
  testRawStrings();
  testSuffixedLiteralSemantics();
  testFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "Lexical literal tests passed\n";
  return 0;
}
