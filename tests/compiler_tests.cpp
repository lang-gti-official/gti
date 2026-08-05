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
  expect(generated.find("int twice(const int value)") != std::string::npos,
         "emitter should lower function signatures");
  expect(generated.find("const int result = twice(4)") != std::string::npos,
         "emitter should make variables const by default");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
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
  expect(!semantic.errors().empty() && semantic.errors().front().token.line == 3,
         "semantic diagnostics should preserve literal source lines");
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
  expect(generated.find("int identity(const int value)") != std::string::npos,
         "parameters should be const by default");
  expect(generated.find("const int fixed = 1") != std::string::npos,
         "immutable variables should lower to const");
  expect(generated.find("int moving = 1") != std::string::npos,
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
             cpp23.find("std::expected<int, std::string>") !=
                 std::string::npos &&
             cpp23.find("std::unexpected(") != std::string::npos &&
             cpp23.find("return {};") != std::string::npos,
         "C++23 should lower expected values to the standard library");

  const std::string cpp20 =
      lang::CppEmitter(lang::CppStandard::Cpp20).emit(program);
  expect(cpp20.find("#include <nonstd/expected.hpp>") != std::string::npos &&
             cpp20.find("nonstd::expected<int, std::string>") !=
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
  expect(generated.find("int print(const int value)") != std::string::npos,
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
             appleCpp.find("const int bits = 64") != std::string::npos &&
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
             windowsCpp.find("const int bits = 32") != std::string::npos &&
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
int tick(int amount){if(amount>0){self.value+=amount;}else{self.value-=1;}return self.value;}};}
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
    int tick(int amount) {
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
  testParserRecovery();
  testSemanticDiagnostics();
  testDefaultImmutability();
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
