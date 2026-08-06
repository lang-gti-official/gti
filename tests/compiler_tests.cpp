#include "gti/ast_printer.h"
#include "gti/cpp_emitter.h"
#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"

#include <iostream>
#include <string>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                   const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void testCompletePipeline() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int twice(int value) {
  return value * 2;
}

int main() {
  int result = twice(4);
  if (result == 8) {
    return 0;
  }
  return 1;
}
)");
  expect(!lexer.hadError(), "valid source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "valid source should parse");
  expect(program.declarations().size() == 2,
         "program should contain both functions");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program), "valid program should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t twice(const std::int32_t value)") !=
             std::string::npos,
         "emitter should lower function signatures");
  expect(generated.find("const std::int32_t result = twice(4)") !=
             std::string::npos,
         "emitter should make variables const by default");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
}

void testFixedWidthIntegers() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int8 minimum8 = -128;
int16 widened16 = minimum8;
int32 maximum32 = 2147483647;
int default32 = maximum32;
int64 maximum64 = 9223372036854775807;
int64 minimum64 = -9223372036854775808;
uint8 maximum_u8 = 255;
uint16 widened_u16 = maximum_u8;
uint32 maximum_u32 = 4294967295;
uint default_u32 = maximum_u32;
uint64 maximum_u64 = 18446744073709551615;
int64 signed_widening = maximum_u32;

int64 add_wide(int16 left, int64 right) {
  return left + right;
}

uint64 add_unsigned(uint16 left, uint64 right) {
  return left + right;
}

int main() {
  int8 maximum8 = 127;
  int16 minimum16 = -32768;
  int32 promoted = maximum8 + minimum16;
  uint8 unsigned_left = 1;
  uint8 unsigned_right = 2;
  int32 promoted_unsigned = unsigned_left + unsigned_right;
  uint32 counter = 1;
  uint32 next = counter + 1;
  bool has_next = next > 0;
  return promoted;
}
)");
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
  expect(valid,
         "in-range literals and widening conversions should be valid");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <cstdint>") != std::string::npos &&
             generated.find("const std::int8_t minimum8 = (-128)") !=
                 std::string::npos &&
             generated.find("const std::int16_t widened16 = minimum8") !=
                 std::string::npos &&
             generated.find("const std::int32_t default32 = maximum32") !=
                 std::string::npos &&
             generated.find("const std::int64_t maximum64 = "
                            "9223372036854775807") != std::string::npos &&
             generated.find("const std::int64_t minimum64 = "
                            "(-9223372036854775807LL - 1)") !=
                 std::string::npos &&
             generated.find("const std::uint8_t maximum_u8 = 255") !=
                 std::string::npos &&
             generated.find("const std::uint16_t widened_u16 = maximum_u8") !=
                 std::string::npos &&
             generated.find("const std::uint32_t default_u32 = maximum_u32") !=
                 std::string::npos &&
             generated.find("const std::uint64_t maximum_u64 = "
                            "18446744073709551615ULL") != std::string::npos &&
             generated.find("int main()") != std::string::npos,
         "integer widths should lower to cstdint types while main stays valid");

  auto invalidTokens = lexer.scan(R"(
int8 too_high = 128;
int8 too_low = -129;
int16 wide = 1;
int8 narrowing = wide;
int alias_overflow = 2147483648;
int64 signed_overflow = 9223372036854775808;
uint8 unsigned_negative = -1;
uint8 unsigned_overflow = 256;
uint16 unsigned_wide = 1;
uint8 unsigned_narrowing = unsigned_wide;
int32 signed_value = 1;
uint32 unsigned_value = 1;
uint32 signed_to_unsigned = signed_value;
int32 unsafe_sum = signed_value + unsigned_value;
bool unsafe_comparison = signed_value < unsigned_value;
uint32 unsafe_negation = -unsigned_value;
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
      lexer.scan("uint64 too_large = 18446744073709551616;");
  (void)lexicalOverflow;
  expect(lexer.hadError() && lexer.errors().size() == 1,
         "integer literals larger than uint64 should fail during lexing");

  const std::string formatted = lang::Formatter().format(
      "int8 small=1;int64 large=small;uint8 byte=255;uint64 wide=byte;");
  expect(formatted == "int8 small = 1;\nint64 large = small;\n"
                      "uint8 byte = 255;\nuint64 wide = byte;\n",
         "formatter should preserve fixed-width type keywords");
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

  auto validTokens = lexer.scan(R"(
int combine(int left, int right) {
  return ((left & right) | (left ^ right)) % 17;
}

int shift_small(uint8 value) { return (value << 3) >> 1; }
int64 mix_widths(int64 left, uint32 right) { return left & right; }
uint64 unsigned_bits(uint64 left, uint64 right) { return left | right; }

int main() {
  int8 small = 3;
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
)");
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

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("gti_internal::backend::modulo(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_left(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_right(") !=
                 std::string::npos,
         "modulo and shifts should lower through checked integer helpers");
  expect(generated.find("(left & right)") != std::string::npos &&
             generated.find("(left ^ right)") != std::string::npos,
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
int32 signed_value = 1;
uint32 unsigned_value = 1;
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

void testParserRecovery() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int first = ;
int second = ;
class Broken {
  return 1;
  int value = 2;
};
int main() { return 0; }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();

  expect(parser.errors().size() == 3,
         "parser should report independent declaration errors");
  expect(program.declarations().size() == 2,
         "parser should recover and keep later declarations");
}

void testSemanticDiagnostics() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int main() {
  if (1) { missing = 3; }
  return 0;
}
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "semantic test source should parse");

  lang::SemanticVisitor semantic;
  expect(!semantic.check(program), "invalid semantics should be rejected");
  expect(semantic.errors().size() == 2,
         "condition type and undefined variable should both be reported");
  expect(!semantic.errors().empty() &&
             semantic.errors().front().primary.line == 3,
         "semantic diagnostics should preserve literal source lines");
}

void testDiagnosticFoundation() {
  lang::SourceManager sources;
  const std::string unicodePrefix = "\xF0\x9F\x99\x82value";
  sources.set("unicode.gti", unicodePrefix);
  const lang::SourceLocation unicodeLocation =
      sources.locate(lang::SourceSpan{"unicode.gti", 4, 9, 1});
  expect(unicodeLocation.line == 1 && unicodeLocation.column == 2,
         "source locations should count a UTF-8 scalar as one CLI column");

  lang::Lexer lexer;
  const std::string invalidEscape = "string value = \"first\nbad\\q\";";
  lexer.scan(invalidEscape, "escape.gti");
  expect(
      lexer.errors().size() == 1 &&
          lexer.errors().front().code == "GTI-L0005" &&
          lexer.errors().front().primary.start == invalidEscape.find("\\q") &&
          lexer.errors().front().primary.end == invalidEscape.find("\\q") + 2 &&
          lexer.errors().front().primary.line == 2,
      "lexical diagnostics should identify the exact invalid escape span");

  const std::string missingSemicolon = "int first = 1\nint second = 2;\n";
  lang::Parser parser(lexer.scan(missingSemicolon, "parse.gti"));
  parser.parse();
  expect(parser.errors().size() == 1 &&
             parser.errors().front().code == "GTI-P0001" &&
             parser.errors().front().fixes.size() == 1 &&
             parser.errors().front().fixes.front().replacement == ";" &&
             parser.errors().front().fixes.front().span.start ==
                 missingSemicolon.find("int second"),
         "missing punctuation should carry an insertion fix-it");

  auto semanticTokens = lexer.scan(R"(
int duplicate = 1;
int duplicate = 2;
int main() {
  int fixed = 1;
  fixed = 2;
  int value = "text";
  return 0;
}
)",
                                   "semantic.gti");
  lang::Parser semanticParser(std::move(semanticTokens));
  lang::Program program = semanticParser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program),
         "rich semantic diagnostic source should fail");

  const lang::Diagnostic *duplicate = nullptr;
  const lang::Diagnostic *immutable = nullptr;
  const lang::Diagnostic *mismatch = nullptr;
  for (const lang::Diagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == "GTI-S2006") {
      duplicate = &diagnostic;
    } else if (diagnostic.code == "GTI-S2002") {
      immutable = &diagnostic;
    } else if (diagnostic.code == "GTI-S2003") {
      mismatch = &diagnostic;
    }
  }
  expect(duplicate != nullptr && duplicate->related.size() == 1,
         "duplicate declarations should reference the original declaration");
  expect(immutable != nullptr && immutable->related.size() == 1 &&
             !immutable->hints.empty(),
         "immutability diagnostics should explain the declaration and remedy");
  expect(mismatch != nullptr &&
             mismatch->message.find("int32") != std::string::npos &&
             mismatch->message.find("string") != std::string::npos,
         "type mismatches should name expected and actual GTI types");
}

void testDefaultImmutability() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int identity(int value) { return value; }
int main() {
  int fixed = 1;
  mut int moving = 1;
  moving++;
  return identity(fixed);
}
)");
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "mutability syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mutable bindings should permit mutation");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find(
             "std::int32_t identity(const std::int32_t value)") !=
             std::string::npos,
         "parameters should be const by default");
  expect(generated.find("const std::int32_t fixed = 1") !=
             std::string::npos,
         "immutable variables should lower to const");
  expect(generated.find("std::int32_t moving = 1") != std::string::npos,
         "mut variables should lower without const");

  auto invalidTokens = lexer.scan(R"(
class Box {
  int value = 1;
  int change() { self.value = 2; return self.value; }
};
int changeParameter(int value) { value = 2; return value; }
int main() {
  int fixed = 1;
  fixed = 2;
  int missingInitializer;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "immutability error source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "immutable bindings should reject mutation");
  expect(invalidSemantic.errors().size() == 4,
         "members, parameters, locals, and missing initializers should fail");
}

void testClassesStructsAndAccess() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return self.secret; }
  int reveal_other(Vault other) { return other.secret; }
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return self.value + self.hidden; }
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
)");
  expect(!lexer.hadError(), "class and struct access syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "class and struct access syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "public members and same-class private access should resolve");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("class Vault;") != std::string::npos &&
             generated.find("struct Reading;") != std::string::npos &&
             generated.find("class Vault {") != std::string::npos &&
             generated.find("struct Reading {") != std::string::npos &&
             generated.find("public:\n  std::int32_t reveal()") !=
                 std::string::npos &&
             generated.find("private:\n  const std::int32_t hidden = 2") !=
                 std::string::npos,
         "emitter should preserve declaration kinds and access labels");

  auto invalidTokens = lexer.scan(R"(
class A {
  int private_value = 1;
public:
  int visible = 2;
  int inspect(A other) { return other.private_value; }
};

struct B {
  int public_value = 1;
private:
  int hidden = 2;
};

A wrong_type(B value) { return value; }
int read_class_private(A value) { return value.private_value; }
int read_struct_private(B value) { return value.hidden; }
int read_missing(B value) { return value.missing; }

class Duplicate {
  int value = 1;
public:
  int value = 2;
};

class InvalidFields {
  mut int missing_initializer;
  int invalid_reference = missing_initializer;
  int invalid_self = self.private_value;
};

int invalid_global_self = self.private_value;
MissingType unresolved();
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid class semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "nominal, access, member, field, and self errors should be rejected");
  expect(hasDiagnostic(invalidSemantic, "Cannot return a value of type"),
         "different nominal class types should not be assignable");
  expect(hasDiagnostic(invalidSemantic, "of 'A' is private") &&
             hasDiagnostic(invalidSemantic, "of 'B' is private"),
         "class defaults and explicit private labels should be enforced");
  expect(hasDiagnostic(invalidSemantic, "Unknown member 'missing'"),
         "unknown members should be diagnosed on their nominal type");
  expect(hasDiagnostic(invalidSemantic, "Duplicate member declaration"),
         "duplicate fields and methods should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "mutable fields should require initialization until constructors exist");
  expect(hasDiagnostic(invalidSemantic, "referenced from field initializers"),
         "field initializers should not depend on member initialization order");
  expect(hasDiagnostic(invalidSemantic, "outside a class or struct method"),
         "self should be rejected in fields and outside methods");
  expect(hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'"),
         "unknown nominal types should be diagnosed");

  auto conditionalTokens = lexer.scan(R"(
class PlatformValue {
#if target.os == "public-os"
public:
#else
private:
#endif
  int value = 1;
};
int read_platform(PlatformValue value) { return value.value; }
)");
  lang::Parser conditionalParser(std::move(conditionalTokens));
  lang::Program conditionalProgram = conditionalParser.parse();
  expect(!conditionalParser.hadError(),
         "conditional access labels should parse in class bodies");

  lang::SemanticVisitor publicTarget(
      lang::TargetInfo{.os = "public-os", .vendor = "test", .arch = "test"});
  lang::SemanticVisitor privateTarget(
      lang::TargetInfo{.os = "private-os", .vendor = "test", .arch = "test"});
  expect(publicTarget.check(conditionalProgram),
         "the active public access branch should expose following members");
  expect(!privateTarget.check(conditionalProgram) &&
             hasDiagnostic(privateTarget, "is private"),
         "the active private access branch should hide following members");

  auto recoveryTokens = lexer.scan(R"(
struct Recovered {
  public int value = 1;
private:
  int hidden = 2;
};
int main() { return 0; }
)");
  lang::Parser recoveryParser(std::move(recoveryTokens));
  lang::Program recoveryProgram = recoveryParser.parse();
  expect(recoveryParser.errors().size() == 1 &&
             recoveryProgram.declarations().size() == 2,
         "parser recovery should resume at access labels and later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Box{public:int value=1;private:int hidden=2;};"
      "struct Point{int x=0;};");
  expect(formatted == "class Box {\npublic:\n  int value = 1;\nprivate:\n"
                      "  int hidden = 2;\n};\nstruct Point {\n"
                      "  int x = 0;\n};\n",
         "formatter should outdent C++-style access labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted class access labels should be idempotent");
}

void testConstructorsAndReceiverMutability() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter(int initial) : value(initial) {}
  int read() { return self.value; }
  int advance(int amount) mut {
    self.value += amount;
    return self.value;
  }
};

struct Origin {
  int x = 0;
};

int inspect(Counter counter) { return counter.read(); }
int main() {
  Counter fixed = Counter(1);
  mut int observed = fixed.read();
  mut Counter moving = Counter(observed);
  observed = moving.advance(2);
  Origin origin = Origin();
  return observed + origin.x;
}
)");
  expect(!lexer.hadError(), "constructor and receiver syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "constructor and receiver syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit construction and mutable receiver calls should validate");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find(
             "explicit Counter(const std::int32_t initial) : value(initial)") !=
             std::string::npos,
         "constructors should lower explicitly with field initialization");
  expect(generated.find("std::int32_t read() const") != std::string::npos,
         "methods should lower as read-only by default");
  expect(
      generated.find("std::int32_t advance(const std::int32_t amount) const") ==
              std::string::npos &&
          generated.find("std::int32_t advance(const std::int32_t amount)") !=
              std::string::npos,
      "mutable receiver methods should lower without C++ const");
  expect(generated.find("Origin origin = Origin()") != std::string::npos,
         "types with defaulted fields should receive default construction");

  auto invalidTokens = lexer.scan(R"(
class MissingInitialization {
  int value;
};

class InvalidConstructor {
  int first;
  int second;

public:
  InvalidConstructor(int value)
      : second(value), first(self.second), second(value) { return; }
  InvalidConstructor() : first(0), second(0) {}
};

class PrivateValue {
  int value;
  PrivateValue(int initial) : value(initial) {}
};

class MutableValue {
  mut int value = 0;

public:
  void mutate() { self.value = 1; }
  void mutate_other(MutableValue other) mut { other.value = 1; }
  void bump() mut { self.value += 1; }
};

int main() {
  PrivateValue hidden = PrivateValue(1);
  MutableValue fixed = MutableValue();
  fixed.bump();
  mut MutableValue moving = MutableValue();
  moving.bump();
  mut MutableValue uninitialized;
  InvalidConstructor mismatch = InvalidConstructor(true);
  MutableValue implicit_value = 1;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid constructor semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid construction and receiver use should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "a class without a constructor should still initialize every field");
  expect(hasDiagnostic(invalidSemantic, "more than one constructor"),
         "constructor overloading should remain unavailable in this layer");
  expect(hasDiagnostic(invalidSemantic, "field declaration order") &&
             hasDiagnostic(invalidSemantic, "initialized more than once"),
         "constructor initializer order and uniqueness should be enforced");
  expect(hasDiagnostic(invalidSemantic,
                       "Cannot use 'self' in a constructor initializer"),
         "constructor initializers should not observe a partial object");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructors cannot contain return statements"),
         "constructor bodies should reject return statements");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructor of 'PrivateValue' is private"),
         "constructor access should follow class access labels");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot mutate through a read-only receiver"),
         "mutable methods and field writes should require mutable receivers");
  expect(
      hasDiagnostic(invalidSemantic, "Constructor argument 1 has type") &&
          hasDiagnostic(invalidSemantic, "Cannot initialize 'implicit_value'"),
      "constructor calls should reject mismatched and implicit conversions");
  expect(hasDiagnostic(invalidSemantic, "require explicit construction"),
         "class variables should never invoke construction implicitly");

  const std::string formatted = lang::Formatter().format(
      "class Counter{mut int value;public:Counter(int initial):value(initial){}"
      "int read(){return self.value;}void reset()mut{self.value=0;}};");
  expect(formatted.find("Counter(int initial) : value(initial) {}") !=
                 std::string::npos &&
             formatted.find("void reset() mut {") != std::string::npos,
         "formatter should distinguish initializer and access-label colons");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted constructors and receiver qualifiers should be idempotent");

  lang::Parser globalQualifierParser(lexer.scan("void invalid() mut {}"));
  globalQualifierParser.parse();
  expect(globalQualifierParser.hadError() &&
             globalQualifierParser.errors().front().message.find(
                 "Only class and struct methods") != std::string::npos,
         "free functions should reject receiver mutability qualifiers");
}

void testNamedGenerics() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return self.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { self.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  string text = identity<string>("generic");
  return relayed;
}
)");
  expect(!lexer.hadError(), "named generic source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "generic classes, functions, and applications should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected generic diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid, "generic substitution and exact inference should validate");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("template <typename T>\nclass Box;") !=
                 std::string::npos &&
             generated.find("template <typename T>\nclass Box {") !=
                 std::string::npos,
         "generic classes should lower with matching C++ forward declarations");
  expect(generated.find("template <typename T>\nT identity(const T value)") !=
                 std::string::npos &&
             generated.find("template <typename T>\nT unbox(") !=
                 std::string::npos,
         "generic functions should lower as C++ function templates");
  expect(generated.find("Box<std::int32_t> box = "
                        "Box<std::int32_t>(identity(7))") !=
                 std::string::npos &&
             generated.find("identity<std::int32_t>(9)") != std::string::npos,
         "applied types and explicit generic calls should lower recursively");
  expect(generated.find(".template echo<std::int32_t>(") != std::string::npos,
         "the backend should hide C++ dependent-template disambiguation");

  auto invalidTokens = lexer.scan(R"(
class Duplicate<T, T> {};
class SameName<SameName> {};
class Shadow<T> {
public:
  T replace<T>(T value) { return value; }
};
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return self.value; }
};

T identity<T>(T value) { return value; }
T choose<T>(T left, T right) { return left; }
T unsupported_add<T>(T left, T right) { return left + right; }
bool unsupported_equal<T>(T left, T right) { return left == right; }
T make<T>();
int ordinary(int value) { return value; }
int main<T>() { return 0; }

int use() {
  Box missing = Box(1);
  Box<int, string> excessive = Box<int, string>(1);
  Box<void> impossible = Box<void>(1);
  int mismatch = identity<int>(true);
  int conflict = choose(1, true);
  int unknown = make();
  int excessive_types = identity<int, string>(1);
  int not_generic = ordinary<int>(1);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid generic semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid generic applications should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "Duplicate generic type parameter"),
         "generic parameter names should be unique");
  expect(hasDiagnostic(invalidSemantic, "same name as its declaration") &&
             hasDiagnostic(invalidSemantic, "cannot shadow"),
         "generic parameters should not collide with enclosing declarations");
  expect(hasDiagnostic(invalidSemantic, "requires 1 generic type argument"),
         "generic class applications should enforce arity");
  expect(hasDiagnostic(invalidSemantic, "cannot be void"),
         "void should not be accepted as a user generic argument");
  expect(hasDiagnostic(invalidSemantic, "Conflicting types inferred"),
         "repeated generic parameters should infer one exact type");
  expect(
      hasDiagnostic(invalidSemantic, "numeric operands"),
      "unconstrained type parameters should not gain operators by duck typing");
  expect(hasDiagnostic(invalidSemantic, "Equality operands"),
         "generic equality should wait for an explicit contract model");
  expect(hasDiagnostic(invalidSemantic, "Cannot infer generic type parameter"),
         "return-only generic parameters should require explicit arguments");
  expect(
      hasDiagnostic(invalidSemantic, "wrong number of type arguments") &&
          hasDiagnostic(invalidSemantic, "Non-generic functions do not take"),
      "explicit function type arguments should enforce generic arity");
  expect(hasDiagnostic(invalidSemantic, "main entry point cannot be generic"),
         "the native entry point should remain non-generic");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{T value;public:Box(T value):value(value){}T get(){return "
      "self.value;}};T identity<T>(T value){return value;}int main(){Box<"
      "Box<int>> nested=Box<Box<int>>(Box<int>(1));int value=identity<int>(1);"
      "return value;}");
  expect(formatted.find("class Box<T> {") != std::string::npos &&
             formatted.find("T identity<T>(T value) {") != std::string::npos &&
             formatted.find("Box<Box<int>> nested = Box<Box<int>>(") !=
                 std::string::npos &&
             formatted.find("identity<int>(1)") != std::string::npos,
         "formatter should preserve compact generic angle brackets");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted generic syntax should be idempotent");

  const std::string comparison =
      lang::Formatter().format("bool result=a < b > c;");
  expect(comparison == "bool result = a < b > c;\n",
         "formatter should not treat relational expressions as generic types");

  lang::Parser malformedParser(lexer.scan("class Broken<> {}; int okay = 1;"));
  const lang::Program recovered = malformedParser.parse();
  expect(malformedParser.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after malformed generic parameters");
}

void testDefaultNodiscard() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  [[discard]] calculate();
  perform();
  mut int count = 0;
  count++;
  return 0;
}
)");
  expect(!lexer.hadError(), "discard attribute syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "discard attribute syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit discard and void calls should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("calculate();") != std::string::npos &&
             generated.find("[[discard]]") == std::string::npos,
         "discard should be a GTI-only call-site attribute");

  auto invalidTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  calculate();
  [[discard]] perform();
  [[discard]] 1 + 2;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid discard uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "ignored function results and invalid discard attributes should fail");
  expect(invalidSemantic.errors().size() == 3,
         "nodiscard should produce focused diagnostics for all invalid uses");
}

void testExpectedValues() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
expected<int, string> calculate(bool fail) {
  if (fail) { return unexpected("calculation failed"); }
  return 42;
}
expected<void, string> render(bool fail) {
  if (fail) { return unexpected("render failed"); }
  return;
}
int main() {
  expected<int, string> result = calculate(false);
  if (!result.has_value()) { return 1; }
  int value = result.value_or(0);
  expected<void, string> rendered = render(false);
  if (!rendered) { return 2; }
  rendered.value();
  [[discard]] calculate(false);
  return value - 42;
}
)");
  expect(!lexer.hadError(), "expected source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "expected types and values should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "expected construction and observers should pass semantic checks");

  const std::string cpp23 = lang::CppEmitter().emit(program);
  expect(cpp23.find("#include <expected>") != std::string::npos &&
             cpp23.find("std::expected<std::int32_t, std::string>") !=
                 std::string::npos &&
             cpp23.find("std::unexpected(") != std::string::npos &&
             cpp23.find("return {};") != std::string::npos,
         "C++23 should lower expected values to the standard library");

  const std::string cpp20 =
      lang::CppEmitter(lang::CppStandard::Cpp20).emit(program);
  expect(cpp20.find("#include <nonstd/expected.hpp>") != std::string::npos &&
             cpp20.find("nonstd::expected<std::int32_t, std::string>") !=
                 std::string::npos &&
             cpp20.find("nonstd::make_unexpected(") != std::string::npos,
         "C++20 should lower expected values to the vendored implementation");

  auto invalidTokens = lexer.scan(R"(
expected<int, void> invalid_error() { return 1; }
expected<int, int> bad_success() { return "wrong"; }
expected<int, int> bad_error() { return unexpected("wrong"); }
expected<int, int> valid_result() { return 1; }
expected<void, int> complete() { return; }
int main() {
  valid_result();
  expected<int, int> result = bad_success();
  result.has_value();
  expected<void, int> completion = complete();
  completion.value_or(0);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid expected uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid expected types, states, and observers should fail");
  expect(invalidSemantic.errors().size() == 6,
         "expected validation should produce focused diagnostics");
}

void testPrintIsAnIdentifier() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int print(int value) { return value; }
int main() { return print(0); }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "print should parse as an ordinary identifier");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "user code should be able to declare a function named print");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t print(const std::int32_t value)") !=
             std::string::npos,
         "print should lower as a normal function");
}

void testNamespacesAndAliases() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace engine {
namespace graphics {
class Renderer {};
void render() {}
void renderTwice() {
  render();
  render();
}
}
}

namespace gfx = engine::graphics;
gfx::Renderer createRenderer();

int main() {
  engine::graphics::render();
  gfx::renderTwice();
  return 0;
}
)");
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("namespace engine {") != std::string::npos &&
             generated.find("namespace graphics {") != std::string::npos,
         "emitter should preserve nested namespaces");
  expect(generated.find("namespace gfx = engine::graphics;") !=
             std::string::npos,
         "emitter should preserve namespace aliases");
  expect(generated.find("gfx::Renderer createRenderer();") !=
             std::string::npos,
         "qualified types should parse and emit through namespace aliases");
  expect(generated.find("engine::graphics::render();") != std::string::npos &&
             generated.find("gfx::renderTwice();") != std::string::npos,
         "emitter should preserve qualified calls");

  auto invalidTokens = lexer.scan(R"(
namespace engine {}
namespace gfx = engine::missing;
int main() {
  engine::missing::render();
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid namespace source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "unknown namespace paths should be rejected");
  expect(invalidSemantic.errors().size() == 2,
         "alias targets and qualified calls should both be diagnosed");
}

void testCompileTimeConditionals() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
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
void inactive_runtime(string value);
#endif

class PlatformInfo {
#if target.arch == "arm64"
  int bits = 64;
#else
  int bits = 32;
#endif
};

int main() {
#if target.os != "windows"
  int value = platform_value();
#else
  int value = platform_value();
#endif
  return value;
}
)");
  expect(!lexer.hadError(), "compile-time directives should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "compile-time branches should parse");

  const lang::TargetInfo apple{"macos", "apple", "arm64"};
  lang::SemanticVisitor appleSemantic(apple);
  expect(appleSemantic.check(program),
         "inactive branches should not participate in Apple semantics");
  const std::string appleCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, apple).emit(program);
  expect(appleCpp.find("return 101;") != std::string::npos &&
             appleCpp.find("return 64;") != std::string::npos &&
             appleCpp.find("const std::int32_t bits = 64") !=
                 std::string::npos &&
             appleCpp.find("missing_name") == std::string::npos &&
             appleCpp.find("#include <expected>") == std::string::npos &&
             appleCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
             appleCpp.find("#if") == std::string::npos,
         "Apple lowering should emit only active branches without C++ macros");

  const lang::TargetInfo windows{"windows", "pc", "x86_64"};
  lang::SemanticVisitor windowsSemantic(windows);
  expect(windowsSemantic.check(program),
         "Windows should select the elif and else branches");
  const std::string windowsCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, windows).emit(program);
  expect(windowsCpp.find("return 202;") != std::string::npos &&
             windowsCpp.find("const std::int32_t bits = 32") !=
                 std::string::npos &&
             windowsCpp.find("nested_value") == std::string::npos &&
             windowsCpp.find("return 101;") == std::string::npos,
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
}

void testRuntimeBackedStdlibSurface() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace gti_internal {
namespace runtime {
@runtime("stdout.write")
void write_stdout(string value);
}
}

namespace std {
void print(string value) {
  gti_internal::runtime::write_stdout(value);
}
}

int main() {
  std::print("hello");
  return 0;
}
)");
  expect(!lexer.hadError(), "runtime-backed stdlib source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "runtime-backed stdlib source should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "runtime binding and string call signatures should validate");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <gti/runtime.hpp>") != std::string::npos,
         "runtime-backed programs should include the native adapter");
  expect(generated.find("namespace gti_std") != std::string::npos &&
             generated.find("gti_std::print(std::string{\"hello\", 5})") !=
                 std::string::npos,
         "GTI std should lower outside the reserved C++ std namespace");
  expect(generated.find("const std::string &value") != std::string::npos,
         "immutable string parameters should lower by const reference");

  auto invalidTokens = lexer.scan(R"(
@runtime("stdout.write")
void fake_write(string value);
int main() { fake_write("hello"); return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid runtime declaration should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "runtime bindings outside the compiler-owned symbol should fail");
  expect(invalidSemantic.errors().size() == 1,
         "invalid runtime binding should produce one focused diagnostic");
}

void testFormatting() {
  const std::string source = R"(include   "math.gti"

namespace engine{class Counter{mut int value=0;
#if target.arch=="arm64"
int word_bits=64;
#endif
int tick(int amount)mut{if(amount>0){self.value+=amount;}else{self.value-=1;}return self.value;}};}
#if target.vendor=="apple"
int main(){for(mut int i=0;i<3;i++){std::println("frame"); // keep this comment
}return -1;}
#else
int main(){[[discard]] engine::run();return 0;}
#endif
)";

  const std::string expected = R"(include "math.gti"

namespace engine {
  class Counter {
    mut int value = 0;
#if target.arch == "arm64"
    int word_bits = 64;
#endif
    int tick(int amount) mut {
      if (amount > 0) {
        self.value += amount;
      } else {
        self.value -= 1;
      }
      return self.value;
    }
  };
}
#if target.vendor == "apple"
int main() {
  for (mut int i = 0; i < 3; i++) {
    std::println("frame"); // keep this comment
  }
  return -1;
}
#else
int main() {
  [[discard]] engine::run();
  return 0;
}
#endif
)";

  const std::string formatted = lang::Formatter().format(source);
  if (formatted != expected) {
    std::cerr << "Formatted output was:\n" << formatted;
  }
  expect(formatted == expected,
         "formatter should produce stable C++-style GTI layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatting should be idempotent");

  const std::string tabIndented =
      lang::Formatter({.indentWidth = 4, .insertSpaces = false})
          .format("int main(){if(true){return 0;}}");
  expect(tabIndented.find("\n\tif (true) {\n\t\treturn 0;") !=
             std::string::npos,
         "formatter should honor tab indentation requested by an editor");
}

} // namespace

int main() {
  testCompletePipeline();
  testFixedWidthIntegers();
  testIntegerBitwiseAndModuloOperators();
  testParserRecovery();
  testSemanticDiagnostics();
  testDiagnosticFoundation();
  testDefaultImmutability();
  testClassesStructsAndAccess();
  testConstructorsAndReceiverMutability();
  testNamedGenerics();
  testDefaultNodiscard();
  testExpectedValues();
  testPrintIsAnIdentifier();
  testNamespacesAndAliases();
  testCompileTimeConditionals();
  testRuntimeBackedStdlibSurface();
  testFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All compiler tests passed\n";
  return 0;
}
