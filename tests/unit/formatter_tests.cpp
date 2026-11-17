// Formatter library: layout, idempotence, token preservation on broken input,
// style options and .gti-format parsing over source fragments. Library API
// over fragments and options a .gti test cannot set. (Moved unchanged from
// compiler_diagnostics_tests.cpp.)
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/lexer.h"
#include "unit_test.h"

#include <algorithm>
#include <iostream>
#include <iterator>
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

void testFormatting() {
  const std::string source = R"(import "math.gti";
import std.array;

namespace engine{class Counter{mut int value=0;
#if target.arch=="arm64"
int word_bits=64;
#endif
int tick(int amount)mut{if(amount>0){this.value+=amount;}else{this.value-=1;}return this.value;}};}
#if target.vendor=="apple"
int main(){for(mut int i=0;i<3;i++){if(i==1){continue ;}std::println("frame"); // keep this comment
if(i>1){break ;}
}mut int attempts=0;do{attempts++;}while(attempts<1);return -1;}
#else
int main(){[[discard]] engine::run();return 0;}
#endif
)";

  const std::string expected = R"(import "math.gti";
import std.array;

namespace engine {
  class Counter {
    mut int value = 0;
#if target.arch == "arm64"
    int word_bits = 64;
#endif
    int tick(int amount) mut {
      if (amount > 0) {
        this.value += amount;
      } else {
        this.value -= 1;
      }
      return this.value;
    }
  };
}
#if target.vendor == "apple"
int main() {
  for (mut int i = 0; i < 3; i++) {
    if (i == 1) {
      continue;
    }
    std::println("frame"); // keep this comment
    if (i > 1) {
      break;
    }
  }
  mut int attempts = 0;
  do {
    attempts++;
  } while (attempts < 1);
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

  for (const std::string name : {"expected", "outcome"}) {
    const std::string input = name + "<int,bool>& inspect(" + name +
                              "<int,bool>& result){return result;}";
    const std::string want = name + "<int, bool>& inspect(" + name +
                             "<int, bool>& result) {\n  return result;\n}\n";
    const auto actual = lang::Formatter().format(input);
    expect(actual == want && lang::Formatter().format(actual) == actual,
           "imported source-defined generic reference types format without "
           "recognizing a library name");
  }
  // Generic parameters are type arguments too: an imported generic over them
  // is a reference type, not a comparison, in functions and conformance
  // blocks alike.
  for (const auto &[input, want] :
       std::vector<std::pair<std::string, std::string>>{
           {"bool count<T, E>(expected<T,E>& value){return value.has_value();}",
            "bool count<T, E>(expected<T, E>& value) {\n"
            "  return value.has_value();\n}\n"},
           {"concept sized<C> { bool count(C& value); };\n"
            "for <T, E> concept sized<Pair<T,E>> {\n"
            "bool count(Pair<T,E>& value){return true;}\n};\n",
            "concept sized<C> {\n  bool count(C& value);\n};\n"
            "for <T, E> concept sized<Pair<T, E>> {\n"
            "  bool count(Pair<T, E>& value) {\n    return true;\n  "
            "}\n};\n"}}) {
    const auto actual = lang::Formatter().format(input);
    if (actual != want)
      std::cerr << "Formatted output was:\n" << actual;
    expect(actual == want && lang::Formatter().format(actual) == actual,
           "an imported generic over generic parameters is a reference type");
  }
  expect(lang::Formatter().format("bool test(){return expected<limit;}") ==
             "bool test() {\n  return expected < limit;\n}\n",
         "a source-defined result name is not an unconditional generic prefix");
  expect(lang::Formatter()
                 .format("bool test(){return left<int(2)>&right;}")
                 .find("left < int(2) > &right") != std::string::npos,
         "numeric conversions do not turn comparison expressions into generic "
         "reference types");

  const std::string nestedLambda = lang::Formatter().format(
      "void invoke<Callback>(Callback callback){callback();}"
      "void run(){invoke([]()->void{mut auto result=login();if(!result){"
      "report();}for(mut int i=0;i<3;i++){tick();}});}");
  expect(nestedLambda.find("login();\n    if (!result)") != std::string::npos &&
             nestedLambda.find("for (mut int i = 0; i < 3; i++)") !=
                 std::string::npos &&
             lang::Formatter().format(nestedLambda) == nestedLambda,
         "statements in a lambda argument should break at the lambda block "
         "depth without splitting for-loop header semicolons");

  const auto initializerLayout = [](const std::string &input,
                                    const std::string &expected) {
    const std::string output = lang::Formatter().format(input);
    if (output != expected)
      std::cerr << "Initializer formatting was:\n" << output;
    expect(output == expected, "initializer layout should retain nesting");
    expect(lang::Formatter().format(output) == output,
           "initializer layout should be idempotent");
    lang::Lexer beforeLexer;
    lang::Lexer afterLexer;
    const auto before = beforeLexer.scan(input);
    const auto after = afterLexer.scan(output);
    expect(!before.empty() && before.size() == after.size() &&
               std::equal(before.begin(), std::prev(before.end()),
                          after.begin(),
                          [](const auto &a, const auto &b) {
                            return a.kind == b.kind && a.lexeme == b.lexeme;
                          }),
           "initializer formatting must preserve tokens, even on broken input");
  };
  const std::string schemaSource =
      "void setup(){Command schema={.name=std::string{\"tea\"},"
      ".subcommands={{.name=std::string{\"auth\"},"
      ".subcommands={{.name=std::string{\"login\"}}}}}};}";
  initializerLayout(schemaSource, R"(void setup() {
  Command schema = {
    .name = std::string{"tea"},
    .subcommands = {
      {
        .name = std::string{"auth"},
        .subcommands = {
          {
            .name = std::string{"login"}
          }
        }
      }
    }
  };
}
)");
  initializerLayout("C c={.a=call(1,2),.b=make<int,int>(3,4),"
                    ".c=[left,right](int a,int b){return a+b;},.d={}};",
                    R"(C c = {
  .a = call(1, 2),
  .b = make<int, int>(3, 4),
  .c = [left, right](int a, int b) {
    return a + b;
  },
  .d = {}
};
)");
  initializerLayout("int values[3]={\n1,2,3,\n};make({1,2},{3,4});",
                    "int values[3] = {\n  1,\n  2,\n  3,\n};\n"
                    "make({1, 2}, {3, 4});\n");
  initializerLayout("C c={ // header\n.name=\"{,} 😀\", // name\n"
                    ".count=call(1,2), /* count */ .empty={\n}};",
                    "C c = { // header\n  .name = \"{,} 😀\", // name\n"
                    "  .count = call(1, 2), /* count */\n  .empty = {}\n};\n");
  initializerLayout("C root={.name=\"tea\",.children={.name=\"auth\"}}",
                    "C root = {\n  .name = \"tea\",\n  .children = {\n"
                    "    .name = \"auth\"\n  }\n}\n");
  initializerLayout("C root={.name=\"tea\",.children={\n{.name=",
                    "C root = {\n  .name = \"tea\",\n  .children = {\n"
                    "    {\n      .name =\n");
  const auto tabs =
      lang::Formatter({.indentWidth = 4,
                       .insertSpaces = false,
                       .breakBeforeBraces = lang::BraceBreakingStyle::Allman})
          .format("void f(){C c={.n=1,.child={.n=2}};}");
  expect(tabs == "void f()\n{\n\tC c = {\n\t\t.n = 1,\n"
                 "\t\t.child = {\n\t\t\t.n = 2\n\t\t}\n\t};\n}\n" &&
             lang::Formatter(
                 {.indentWidth = 4,
                  .insertSpaces = false,
                  .breakBeforeBraces = lang::BraceBreakingStyle::Allman})
                     .format(tabs) == tabs,
         "initializer indentation honors tabs without Allman block wrapping");

  const std::string separatedAmpersands = lang::Formatter().format("& &m");
  expect(separatedAmpersands == "& &m\n" &&
             lang::Formatter().format(separatedAmpersands) ==
                 separatedAmpersands,
         "formatting must preserve a source token boundary between adjacent "
         "ampersands");
  for (const std::string_view boundary :
       {"+ +value", "- -value", "/ *p", "/ /p", ". . .", "1 . 2"}) {
    const std::string boundaryFormatted = lang::Formatter().format(boundary);
    lang::Lexer sourceLexer;
    lang::Lexer formattedLexer;
    const std::vector<lang::Token> sourceTokens = sourceLexer.scan(
        std::string(boundary), "formatter-boundary-source.gti");
    const std::vector<lang::Token> formattedTokens = formattedLexer.scan(
        boundaryFormatted, "formatter-boundary-formatted.gti");
    const bool tokenEquivalent =
        sourceTokens.size() == formattedTokens.size() &&
        !sourceTokens.empty() &&
        sourceTokens.back().kind == lang::TokenKind::END_OF_FILE &&
        formattedTokens.back().kind == lang::TokenKind::END_OF_FILE &&
        std::equal(sourceTokens.begin(), std::prev(sourceTokens.end()),
                   formattedTokens.begin(),
                   [](const lang::Token &source, const lang::Token &formatted) {
                     return source.kind == formatted.kind &&
                            source.lexeme == formatted.lexeme;
                   });
    expect(tokenEquivalent &&
               lang::Formatter().format(boundaryFormatted) == boundaryFormatted,
           "maximal-munch token boundaries should remain formatter-"
           "equivalent and idempotent for '" +
               std::string(boundary) + "'");
  }

  const std::string staticFormatted = lang::Formatter().format(
      "static mut int count=0;class Registry{public:static int value=1;"
      "static int current(){return value;} public: Registry() = default; };");
  expect(staticFormatted == R"(static mut int count = 0;
class Registry {
public:
  static int value = 1;
  static int current() {
    return value;
  }
public:
  Registry() = default;
};
)" && lang::Formatter().format(staticFormatted) == staticFormatted,
         "formatter should preserve static declaration ownership and remain "
         "idempotent");

  const std::string constexprFormatted = lang::Formatter().format(
      "constexpr uint64_t extent=uint64_t(2+2);class Limits{public:static "
      "constexpr uint64_t count=extent; public: Limits() = default; };int "
      "choice(){if constexpr(true){"
      "return 1;}else{return 0;}}");
  expect(constexprFormatted == R"(constexpr uint64_t extent = uint64_t(2 + 2);
class Limits {
public:
  static constexpr uint64_t count = extent;
public:
  Limits() = default;
};
int choice() {
  if constexpr (true) {
    return 1;
  } else {
    return 0;
  }
}
)" && lang::Formatter().format(constexprFormatted) == constexprFormatted,
         "formatter should preserve constexpr declarations and branches and "
         "remain idempotent");

  const std::string tabIndented =
      lang::Formatter({.indentWidth = 4, .insertSpaces = false})
          .format("int main(){if(true){return 0;}}");
  expect(tabIndented.find("\n\tif (true) {\n\t\treturn 0;") !=
             std::string::npos,
         "formatter should honor tab indentation requested by an editor");

  const std::string referenceSource =
      "class Box<T>{ public: Box() = default; };"
      "void use(int& value,Box<int>& box){int A=value;int bits=A&value;}";
  const std::string leftReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Left})
          .format(referenceSource);
  const std::string rightReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Right})
          .format(referenceSource);
  const std::string defaultReferences = lang::Formatter().format(
      "class Box{};void use(int& value,Box& box){int bits=value&value;}"
      "int min(int left,int right){if(left> right){return right;}return "
      "left;}int invoke(){return 1;}int expression(){mut int A=1;"
      "A&invoke();return A;}");
  const std::string middleReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Middle})
          .format(referenceSource);
  expect(leftReferences.find("int& value") != std::string::npos &&
             leftReferences.find("Box<int>& box") != std::string::npos &&
             rightReferences.find("int &value") != std::string::npos &&
             rightReferences.find("Box<int> &box") != std::string::npos &&
             leftReferences.find("A & value") != std::string::npos &&
             rightReferences.find("A & value") != std::string::npos &&
             defaultReferences.find("int& value") != std::string::npos &&
             defaultReferences.find("Box& box") != std::string::npos &&
             defaultReferences.find("A & invoke();") != std::string::npos &&
             middleReferences.find("int & value") != std::string::npos,
         "reference alignment should default left, preserve explicit styles, "
         "and recognize declared generic types without changing bitwise-and "
         "spacing for capitalized values or call expressions");
  expect(defaultReferences.find("if (left > right)") != std::string::npos,
         "comparison operators should retain binary spacing");

  const std::string importedReturnReferences = lang::Formatter().format(
      "class Application{public:mut LayerStack & GetLayerStack()mut{"
      "return this.m_LayerStack;}LayerStack & PeekLayerStack(){return "
      "this.m_LayerStack;}static mut Application & GetApplication(){unsafe{"
      "return *Application::app;}} public: Application() = default; };");
  expect(importedReturnReferences.find(
             "mut LayerStack& GetLayerStack() mut {") != std::string::npos &&
             importedReturnReferences.find("LayerStack& PeekLayerStack() {") !=
                 std::string::npos &&
             importedReturnReferences.find(
                 "static mut Application& GetApplication() {") !=
                 std::string::npos &&
             lang::Formatter().format(importedReturnReferences) ==
                 importedReturnReferences,
         "default left alignment should cover included nominal return types "
         "and remain idempotent");

  const std::string dependentDeclarators = lang::Formatter().format(
      "T & borrow<T>(T & value){return value;}"
      "T * address<T>(T * value){return value;}"
      "virtual void observe(External & reference,External * pointer);"
      "int expressions(int left,int right){mut External * handle=nullptr;"
      "int product=left*right;return left&right;}");
  const std::string expectedDependentDeclarators = R"(T& borrow<T>(T& value) {
  return value;
}
T* address<T>(T* value) {
  return value;
}
virtual void observe(External& reference, External* pointer);
int expressions(int left, int right) {
  mut External* handle = nullptr;
  int product = left * right;
  return left & right;
}
)";
  if (dependentDeclarators != expectedDependentDeclarators) {
    std::cerr << "Dependent declarator formatting was:\n"
              << dependentDeclarators;
  }
  expect(dependentDeclarators == expectedDependentDeclarators &&
             lang::Formatter().format(dependentDeclarators) ==
                 dependentDeclarators,
         "default declarator alignment should attach generic and imported "
         "pointer/reference markers to their types without changing binary "
         "operator spacing");

  const std::string pointerFormatted = lang::Formatter().format(
      "class Byte{};void use(mut uint8_t * "
      "data,const uint8_t*read_only){"
      "mut uint8_t*alias=& data[0];* data=read_only [0];"
      "uint8_t product=data[0]*read_only[0];"
      "uint8_t bits=data[0]&read_only[0];return * alias;}"
      "class Link{public:uint8_t value=0;};"
      "uint8_t member(Link*link){return link -> value;}"
      "class RawBox<T>{T * value;const T * read(){return value;}};");
  const std::string expectedPointerFormatting =
      R"(class Byte {};
void use(mut uint8_t* data, const uint8_t* read_only) {
  mut uint8_t* alias = &data[0];
  *data = read_only[0];
  uint8_t product = data[0] * read_only[0];
  uint8_t bits = data[0] & read_only[0];
  return *alias;
}
class Link {
public:
  uint8_t value = 0;
};
uint8_t member(Link* link) {
  return link->value;
}
class RawBox<T> {
  T* value;
  const T* read() {
    return value;
  }
};
)";
  expect(pointerFormatted == expectedPointerFormatting &&
             lang::Formatter().format(pointerFormatted) == pointerFormatted,
         "formatter should distinguish pointer declarators and unary raw "
         "pointer operators from multiplication and bitwise-and");

  const std::string commentedGenericReturn = lang::Formatter().format(
      "namespace detail{class Iterator<T>{ public: Iterator() = default; "
      "};}class Range<T>{public:"
      "// Iterator access remains read-only.\n"
      "detail::Iterator<T> begin();detail::Iterator<Range<T>> nested();"
      "bool ordered(int left,int right){return left < right;} public: Range() "
      "= default; };");
  expect(commentedGenericReturn.find("// Iterator access remains read-only.\n"
                                     "  detail::Iterator<T> begin();") !=
                 std::string::npos &&
             commentedGenericReturn.find(
                 "detail::Iterator<Range<T>> nested();") != std::string::npos,
         "comments should not make generic return types format as relational "
         "expressions");
  expect(commentedGenericReturn.find("return left < right;") !=
                 std::string::npos &&
             lang::Formatter().format(commentedGenericReturn) ==
                 commentedGenericReturn,
         "generic return formatting should preserve comparisons and remain "
         "idempotent");

  const auto qualifiedReturnLayout = [](const std::string &input,
                                        const std::string &signature) {
    const std::string output = lang::Formatter().format(input);
    if (output.find(signature) == std::string::npos)
      std::cerr << "Qualified return formatting was:\n" << output;
    expect(output.find(signature) != std::string::npos,
           "qualified method return types should retain generic spacing");
    expect(lang::Formatter().format(output) == output,
           "qualified return formatting should be idempotent");
    lang::Lexer beforeLexer;
    lang::Lexer afterLexer;
    const auto before = beforeLexer.scan(input);
    const auto after = afterLexer.scan(output);
    expect(!beforeLexer.hadError() && !afterLexer.hadError() &&
               !before.empty() && before.size() == after.size() &&
               std::equal(before.begin(), std::prev(before.end()),
                          after.begin(),
                          [](const auto &a, const auto &b) {
                            return a.kind == b.kind && a.lexeme == b.lexeme;
                          }),
           "qualified return formatting must preserve tokens");
  };
  qualifiedReturnLayout(
      "namespace psych{std::unique_ptr < Window > Window::Create(){}}",
      "std::unique_ptr<Window> Window::Create() {");
  qualifiedReturnLayout("Parcel < Item > app::Factory::create(){}",
                        "Parcel<Item> app::Factory::create() {");
  qualifiedReturnLayout(
      "Result < Parcel<Item>, Error > app::Factory::create(){}",
      "Result<Parcel<Item>, Error> app::Factory::create() {");
  qualifiedReturnLayout("Parcel<Parcel<Item>> app::Factory::create(){}",
                        "Parcel<Parcel<Item>> app::Factory::create() {");
  qualifiedReturnLayout("Parcel < Item > app:: /* owner */ Factory::create(){}",
                        "Parcel<Item> app:: /* owner */ Factory::create() {");
  qualifiedReturnLayout("constexpr Parcel < int > Factory::create(){}",
                        "constexpr Parcel<int> Factory::create() {");
  qualifiedReturnLayout("void compare(){left<middle>limits::value();}",
                        "left < middle > limits::value();");
  qualifiedReturnLayout("bool compare(){return left<middle>limits::value();}",
                        "return left < middle > limits::value();");
  qualifiedReturnLayout("Parcel<Item> Factory::", "Factory::");
  // A declaration after an attribute list keeps its generic return type.
  qualifiedReturnLayout(
      "[[discardable]] expected < void, std::filesystem::errc > walk(mut "
      "Node& node){return;}",
      "[[discardable]] expected<void, std::filesystem::errc> walk(");
  qualifiedReturnLayout("[[discardable]] Parcel < Item > Factory::create(){}",
                        "[[discardable]] Parcel<Item> Factory::create() {");
  // A subscript closing `]]` is not an attribute list.
  qualifiedReturnLayout(
      "bool compare(){return grid[rows[i]]<middle>limits::value();}",
      "return grid[rows[i]] < middle > limits::value();");

  const lang::FormatConfigResult parsedStyle =
      lang::parseFormatConfig("IndentWidth: 4\n"
                              "BasedOnStyle: LLVM\n"
                              "UseTab: Never\n"
                              "BreakBeforeBraces: Allman\n"
                              "SpaceBeforeParens: Never\n"
                              "IndentCaseLabels: true\n"
                              "AccessModifierOffset: -4\n"
                              "MaxEmptyLinesToKeep: 0\n"
                              "SpacesBeforeTrailingComments: 3\n"
                              "SpaceBeforeAssignmentOperators: false\n"
                              "ReferenceAlignment: Left\n",
                              {.indentWidth = 7});
  expect(parsedStyle.issues.empty() && parsedStyle.options.indentWidth == 4 &&
             parsedStyle.options.insertSpaces &&
             parsedStyle.options.breakBeforeBraces ==
                 lang::BraceBreakingStyle::Allman &&
             parsedStyle.options.spaceBeforeParens ==
                 lang::SpaceBeforeParensStyle::Never &&
             parsedStyle.options.indentCaseLabels &&
             parsedStyle.options.accessModifierOffset == -4 &&
             parsedStyle.options.maxEmptyLinesToKeep == 0 &&
             parsedStyle.options.spacesBeforeTrailingComments == 3 &&
             !parsedStyle.options.spaceBeforeAssignmentOperators &&
             parsedStyle.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "format configuration should apply a base style before explicit "
         "clang-format-compatible overrides");

  const lang::FormatConfigResult defaultStyle =
      lang::parseFormatConfig(lang::defaultFormatConfig());
  expect(defaultStyle.issues.empty() &&
             defaultStyle.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "the generated default format configuration should parse back to "
         "the GTI default style");

  const std::string configuredSource =
      "class Example{public:int run(int value){if(value>0){value+=1;"
      "// retain\n}else{return 0;}switch(value){case 1:value+=1;break;"
      "default:return value;}}};";
  const std::string configuredExpected = R"(class Example
{
public:
    int run(int value)
    {
        if(value > 0)
        {
            value+= 1;   // retain
        }
        else
        {
            return 0;
        }
        switch(value)
        {
            case 1:
                value+= 1;
                break;
            default:
                return value;
        }
    }
};
)";
  const std::string configured =
      lang::Formatter(parsedStyle.options).format(configuredSource);
  if (configured != configuredExpected) {
    std::cerr << "Configured formatted output was:\n" << configured;
  }
  expect(configured == configuredExpected &&
             lang::Formatter(parsedStyle.options).format(configured) ==
                 configured,
         "configured Allman, spacing, access, case-label, and comment styles "
         "should be stable and idempotent");

  const lang::FormatConfigResult tabStyle =
      lang::parseFormatConfig("IndentWidth: 4\nUseTab: ForIndentation\n");
  expect(!tabStyle.options.insertSpaces &&
             lang::Formatter(tabStyle.options)
                     .format("int main(){if(true){return 0;}}")
                     .find("\n\tif (true) {") != std::string::npos,
         "UseTab: ForIndentation should use tabs for structural indentation");

  const std::string parenthesizedReturn = lang::Formatter().format(
      "T midpoint<std::numeric T>(T left,T right){return(left+right)/2;}");
  const std::string expectedParenthesizedReturn =
      "T midpoint<std::numeric T>(T left, T right) {\n"
      "  return (left + right) / 2;\n"
      "}\n";
  expect(parenthesizedReturn == expectedParenthesizedReturn &&
             lang::Formatter().format(parenthesizedReturn) ==
                 parenthesizedReturn,
         "default formatting should separate return from a parenthesized "
         "expression idempotently");

  const std::string parenthesizedControlSource =
      "int select(bool ready){if(ready){return(1);}return(0);}";
  const lang::Formatter neverParentheses(
      {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Never});
  const std::string neverParenthesizedReturn =
      neverParentheses.format(parenthesizedControlSource);
  expect(
      neverParenthesizedReturn.find("if(ready)") != std::string::npos &&
          neverParenthesizedReturn.find("return (1);") != std::string::npos &&
          neverParenthesizedReturn.find("return (0);") != std::string::npos &&
          neverParentheses.format(neverParenthesizedReturn) ==
              neverParenthesizedReturn,
      "SpaceBeforeParens: Never should not remove the separator after "
      "return");

  const std::string spacedParentheses =
      lang::Formatter(
          {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always})
          .format("int invoke<T>(T value){if(true){return invoke<T>(value);}}");
  expect(spacedParentheses.find("int invoke<T> (T value)") !=
                 std::string::npos &&
             spacedParentheses.find("if (true)") != std::string::npos &&
             spacedParentheses.find("invoke<T> (value)") != std::string::npos,
         "SpaceBeforeParens: Always should cover declarations, controls, and "
         "generic calls");
  const lang::Formatter alwaysParentheses(
      {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always});
  const std::string alwaysParenthesizedReturn =
      alwaysParentheses.format(parenthesizedControlSource);
  expect(
      alwaysParenthesizedReturn.find("int select (bool ready)") !=
              std::string::npos &&
          alwaysParenthesizedReturn.find("if (ready)") != std::string::npos &&
          alwaysParenthesizedReturn.find("return (1);") != std::string::npos &&
          alwaysParentheses.format(alwaysParenthesizedReturn) ==
              alwaysParenthesizedReturn,
      "SpaceBeforeParens: Always should retain the separator after return "
      "idempotently");

  const lang::FormatConfigResult invalidStyle =
      lang::parseFormatConfig("IndentWidth: 0\n"
                              "UseTab: Sometimes\n"
                              "UnknownOption: value\n"
                              "not-an-entry\n");
  expect(invalidStyle.issues.size() == 4,
         "invalid and unsupported format entries should remain observable");

  const std::string emptyLines = lang::Formatter({.maxEmptyLinesToKeep = 2})
                                     .format("int first=1;\n\n\nint second=2;");
  expect(emptyLines == "int first = 1;\n\n\nint second = 2;\n" &&
             lang::Formatter({.maxEmptyLinesToKeep = 2}).format(emptyLines) ==
                 emptyLines,
         "MaxEmptyLinesToKeep should cap and preserve the requested number "
         "of empty lines");

  const std::string disabledSource = "int main(){return 0;}";
  expect(lang::Formatter({.disableFormat = true}).format(disabledSource) ==
             disabledSource,
         "DisableFormat should leave source byte-for-byte unchanged");
}

// An incomplete directive marker followed by a word (recovered from invalid
// input) must keep its separator: gluing '#' to 'case' would re-lex as the
// single directive token '#case', flipping the following colon's spacing on
// the next pass and breaking idempotence. Every such directive+word pair
// must format to a fixed point. (Moved unchanged from the former
// compiler_constexpr_tests.cpp testComparisonOperatorFormattingStability.)
void testDirectiveMarkerSeparation() {
  for (const std::string source :
       {"# case:", "# case :", "#\tcase:", "# default:", "# return :"}) {
    const std::string first = lang::Formatter().format(source);
    expect(lang::Formatter().format(first) == first,
           "formatting a directive marker glued to a word must be idempotent");
    expect(first.find("#case") == std::string::npos,
           "the formatter must not merge a lone '#' with a following word");
  }
}

} // namespace

GTI_UNIT_TEST(formatter) {
  testFormatting();
  testDirectiveMarkerSeparation();
  return failures == 0 ? 0 : 1;
}
