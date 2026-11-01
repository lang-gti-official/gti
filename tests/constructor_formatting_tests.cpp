#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/lexer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void check(std::string_view source, std::string_view expected,
           const lang::FormatOptions &options) {
  const lang::Formatter formatter(options);
  const std::string actual = formatter.format(source);
  if (actual != expected) {
    std::cerr << "Expected:\n" << expected << "Actual:\n" << actual;
    std::exit(1);
  }
  expect(formatter.format(actual) == actual,
         "constructor layout must be idempotent");
  lang::Lexer beforeLexer;
  lang::Lexer afterLexer;
  const auto before = beforeLexer.scan(std::string(source));
  const auto after = afterLexer.scan(actual);
  expect(!beforeLexer.hadError() && !afterLexer.hadError(),
         "fixtures must remain lexically valid");
  expect(before.size() == after.size(), "formatting must preserve token count");
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (before[i].kind == lang::TokenKind::END_OF_FILE &&
        after[i].kind == lang::TokenKind::END_OF_FILE) {
      continue;
    }
    if (before[i].kind != after[i].kind ||
        before[i].lexeme != after[i].lexeme) {
      std::cerr << "Token " << i << ": '" << before[i].lexeme << "' -> '"
                << after[i].lexeme << "'\nSource:\n"
                << source << "\nFormatted:\n"
                << actual;
    }
    expect(before[i].kind == after[i].kind &&
               before[i].lexeme == after[i].lexeme,
           "formatting must preserve token identity");
  }
}
} // namespace

int main() {
  const auto config =
      lang::parseFormatConfig("PackConstructorInitializers: Never\n"
                              "BreakConstructorInitializers: AfterColon\n"
                              "ConstructorInitializerIndentWidth: 4\n");
  expect(config.issues.empty(), "constructor configuration must parse");
  auto options = config.options;
  const std::string source =
      "http_service(std::http::client client,std::http::url base_url):"
      "m_client_(std::move(client)),m_base_url_(base_url.clone()){}";
  check(source,
        "http_service(std::http::client client, std::http::url base_url) :\n"
        "    m_client_(std::move(client)),\n"
        "    m_base_url_(base_url.clone()) {}\n",
        options);
  check(source,
        "http_service(std::http::client client, std::http::url base_url) : "
        "m_client_(std::move(client)), m_base_url_(base_url.clone()) {}\n",
        {});

  check("namespace app{[[track_caller]]#macro CHECK(bool condition,int value){"
        "if(!condition){report(value);}}void use(){CHECK(true,compute());}}",
        "namespace app {\n  [[track_caller]] #macro CHECK(bool condition, int "
        "value) {\n    if (!condition) {\n      report(value);\n    }\n  }\n"
        "  void use() {\n    CHECK(true, compute());\n  }\n}\n",
        {});
  check("#macro log_if<First,Args...>(bool condition,std::string_view "
        "pattern,First first,Args... args)requires std::formattable<First>{"
        "if(condition){print(pattern,first,args...);}}",
        "#macro log_if<First, Args...>(bool condition, std::string_view "
        "pattern, First first, Args... args)\n  requires "
        "std::formattable<First> {\n  if (condition) {\n    "
        "print(pattern, first, args...);\n  }\n}\n",
        {});
  check("#macro optional<T=int>(T value=1){consume(value);}\n#define "
        "ENABLED\nnamespace app{#macro IGNORE(bool condition){}}",
        "#macro optional<T = int>(T value = 1) {\n  consume(value);\n}\n"
        "#define ENABLED\nnamespace app {\n  #macro IGNORE(bool condition) "
        "{}\n}\n",
        {});
  auto macroAllman = lang::FormatOptions{};
  macroAllman.breakBeforeBraces = lang::BraceBreakingStyle::Allman;
  check("namespace app{#macro CHECK(bool condition){if(condition){use();}}}",
        "namespace app\n{\n  #macro CHECK(bool condition)\n  {\n    if "
        "(condition)\n    {\n      use();\n    }\n  }\n}\n",
        macroAllman);
  check("#macro int select(bool enabled,int first,int second){"
        "if(enabled){return first;}return second;}",
        "#macro int select(bool enabled, int first, int second) {\n"
        "  if (enabled) {\n    return first;\n  }\n  return second;\n}\n",
        {});
  check("[[track_caller]]#macro T choose<T>(bool enabled,T first,T second){"
        "return enabled?first:second;}",
        "[[track_caller]] #macro T choose<T>(bool enabled, T first, T second) "
        "{\n  return enabled ? first : second;\n}\n",
        {});
  check("#macro std::optional<T> maybe<T>(T value){return value;}",
        "#macro std::optional<T> maybe<T>(T value) {\n  return value;\n}\n",
        {});

  check("namespace tea{class Box{public:Box(int x):a(x),b(choose(1,2)){use();}"
        "Box():a(0),b(0){}int value(){return a;}};}",
        "namespace tea {\n  class Box {\n  public:\n"
        "    Box(int x) :\n        a(x),\n        b(choose(1, 2)) {\n"
        "      use();\n    }\n    Box() :\n        a(0),\n        b(0) {}\n"
        "    int value() {\n      return a;\n    }\n  };\n}\n",
        options);

  check("Box():first(make<int,int>(1,2)),second({1,2}),third(ready?1:2){}",
        "Box() :\n    first(make<int, int>(1, 2)),\n"
        "    second({1, 2}),\n    third(ready ? 1 : 2) {}\n",
        options);
  check("Box(): /* list */ first(1), // keep first\n/* next */ second(2){}",
        "Box() : /* list */\n    first(1), // keep first\n"
        "    /* next */ second(2) {}\n",
        options);
  check("class Box{\n#if "
        "enabled\nBox():a(1),b(2){}\n#else\nBox():a(3),b(4){}\n#endif\n};",
        "class Box {\n#if enabled\n  Box() :\n      a(1),\n      b(2) {}\n"
        "#else\n  Box() :\n      a(3),\n      b(4) {}\n#endif\n};\n",
        options);
  check(
      "class Box{constexpr Box<T>(T x)requires std::copyable<T>:value(x){};};",
      "class Box {\n  constexpr Box<T>(T x)\n    requires std::copyable<T> :\n"
      "      value(x) {};\n};\n",
      options);
  check("class Box{implicit Box(int x):a(x){}implicit constexpr Box(bool y):"
        "a(1){}};",
        "class Box {\n  implicit Box(int x) :\n      a(x) {}\n"
        "  implicit constexpr Box(bool y) :\n      a(1) {}\n};\n",
        options);
  check("class Port{implicit consteval Port(int number):value(number){}"
        "static consteval int checked(int number){return number;}};",
        "class Port {\n  implicit consteval Port(int number) :\n"
        "      value(number) {}\n  static consteval int checked(int number) {\n"
        "    return number;\n  }\n};\n",
        options);
  check("consteval T identity<T>(T value){return value;}"
        "struct Port{consteval operator int(){return this.value;}};",
        "consteval T identity<T>(T value) {\n  return value;\n}\n"
        "struct Port {\n  consteval operator int() {\n"
        "    return this.value;\n  }\n};\n",
        {});
  check("class Box:ns::Base<int>{Box():ns::Base<int>(1),field(2){};};",
        "class Box : ns::Base<int> {\n  Box() :\n    "
        "  ns::Base<int>(1),\n      field(2) {};\n};\n",
        options);

  options.breakConstructorInitializers =
      lang::BreakConstructorInitializersStyle::BeforeColon;
  check("class Box{Box():a(1),b(2){};};",
        "class Box {\n  Box()\n      : a(1),\n        b(2) {};\n};\n", options);
  options.breakBeforeBraces = lang::BraceBreakingStyle::Allman;
  check("class Box{Box():a(1),b(2){use();}int x;};",
        "class Box\n{\n  Box()\n      : a(1),\n        b(2)\n  {\n"
        "    use();\n  }\n  int x;\n};\n",
        options);
  options.breakConstructorInitializers =
      lang::BreakConstructorInitializersStyle::AfterColon;
  options.constructorInitializerIndentWidth = 3;
  options.insertSpaces = false;
  check("class Box{Box():a(1),b(2){};};",
        "class Box\n{\n\tBox() :\n\t\t a(1),\n\t\t b(2)\n\t{};\n};\n", options);

  const std::vector<std::string> nonConstructors{
      "class Box:Base{int x;};",
      "int main(){int value=ready?first():second();switch(value){case "
      "test():run();}}",
      "void use(){call(first(),second());}",
      "class Box{Box();Box()=default;Box(int x)=delete;};",
      "class Box{Box():a(",
      "class Box{Box():a(1),b(",
      "class Box{Box():a(1),}",
  };
  for (const auto &input : nonConstructors) {
    check(input, lang::Formatter().format(input), config.options);
  }
  const std::vector<std::string> nested{
      "Box():fn([]()->int{int x=1;if(x>0){return x;}return 0;}),n(1){}",
      "Box():fn([a=make(1,2),b=make(3,4)]()->int{return 1;}),n(1){}",
      "Box():data({\n{1,2},\n{3,4}\n}),n(1){}",
      "Box():text(\"comma, colon: braces{}\"),n(1){}",
      "class Box{Box() /* signature */ : // list\na(1), /* first */ b(2) // "
      "last\n{}};",
      "class Box{Box():a(1){}Box(int x):a(x){}};",
      "Box():fn([]()->void{for(int i=0;i<2;i++){call(i,2);}}),n(1){}",
      "Box():a(1),b(2){",
      "Box() /* signature */ : a(1), /* explanation\ncontinued */ b(2) {}",
      "class Box{Box():a(1),b(2){/* retained */}};",
      "Box():text(\"🙂\"),n(1){} // café\n",
      "Box():fn([]()->int{switch(1){case 1:return 2;default:return "
      "3;}}),n(1){}",
  };
  for (const auto &input : nested) {
    for (const auto brace :
         {lang::BraceBreakingStyle::Attach, lang::BraceBreakingStyle::Allman}) {
      for (const auto colon :
           {lang::BreakConstructorInitializersStyle::BeforeColon,
            lang::BreakConstructorInitializersStyle::AfterColon}) {
        for (const std::size_t width : {0, 3, 8}) {
          options = config.options;
          options.breakBeforeBraces = brace;
          options.breakConstructorInitializers = colon;
          options.constructorInitializerIndentWidth = width;
          check(input, lang::Formatter(options).format(input), options);
        }
      }
    }
  }

  const std::string switchBody =
      lang::Formatter(config.options)
          .format("Box():fn([]()->int{switch(1){case 1:return 2;default:return "
                  "3;}}),n(1){}");
  expect(switchBody.find("\n      case 1:\n        return 2;") !=
             std::string::npos,
         "nested switch labels retain the constructor continuation offset");

  const auto defaults = lang::parseFormatConfig(lang::defaultFormatConfig());
  expect(defaults.issues.empty() &&
             defaults.options.packConstructorInitializers ==
                 lang::PackConstructorInitializersStyle::BinPack &&
             defaults.options.breakConstructorInitializers ==
                 lang::BreakConstructorInitializersStyle::BeforeColon &&
             defaults.options.constructorInitializerIndentWidth == 4,
         "scaffold must preserve default constructor layout");
  const auto invalid = lang::parseFormatConfig(
      "PackConstructorInitializers: CurrentLine\nBreakConstructorInitializers: "
      "BeforeComma\n"
      "ConstructorInitializerIndentWidth: 17\n");
  expect(invalid.issues.size() == 3 &&
             invalid.options.constructorInitializerIndentWidth == 4,
         "unsupported values must report configuration issues without changing "
         "defaults");
  for (const auto value : {"-1", "x", "", "100"}) {
    expect(!lang::parseFormatConfig(
                std::string("ConstructorInitializerIndentWidth: ") + value)
                .issues.empty(),
           "invalid indentation must be diagnosed");
  }
  const auto overrides = lang::parseFormatConfig(
      "ConstructorInitializerIndentWidth: 0\nPackConstructorInitializers: "
      "Never\n"
      "BasedOnStyle: LLVM\nConstructorInitializerIndentWidth: 6\n");
  expect(overrides.issues.size() == 1 &&
             overrides.options.constructorInitializerIndentWidth == 6 &&
             overrides.options.packConstructorInitializers ==
                 lang::PackConstructorInitializersStyle::Never,
         "explicit constructor settings override the base style; later "
         "duplicates win");
  options = config.options;
  options.disableFormat = true;
  check(source, source, options);

  // Conversion operators are named by their target type, so the modifiers,
  // a qualified or generic target, and a consuming receiver keep their
  // declaration layout. `bool(value)` is an ordinary conversion expression.
  check("struct Id{operator UserId(){return UserId(this.raw);}implicit "
        "operator std::string()&&{return text;}implicit virtual operator "
        "Box<int>()mut;implicit constexpr operator bool(){return true;}};"
        "bool on=bool(id);",
        "struct Id {\n  operator UserId() {\n    return UserId(this.raw);\n"
        "  }\n  implicit operator std::string() && {\n    return text;\n  }\n"
        "  implicit virtual operator Box<int>() mut;\n"
        "  implicit constexpr operator bool() {\n    return true;\n  }\n};\n"
        "bool on = bool(id);\n",
        lang::FormatOptions{});
  std::cout << "Constructor formatting configuration, layout, token "
               "preservation and idempotence passed\n";
}
