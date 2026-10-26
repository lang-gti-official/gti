#include "gti/formatter.h"
#include "gti/lexer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"start( []() -> void {});", "start([]() -> void {}"},
      {"start(\n[]() -> void {}\n);", "start([]() -> void {}"},
      {"start([value]() -> int {return value;});", "start([value]()"},
      {"start([owned=std::move(value)]() -> void {});",
       "start([owned = std::move(value)]()"},
      {"start([&value]() -> void {});", "start([&value]()"},
      {"start([mut &value]() -> void {});", "start([mut &value]()"},
      {"outer(inner([]() -> void {}));", "inner([]()"},
      {"start(1,[]() -> void {});", "start(1, []()"},
      {"auto action=[]() -> void {};", "auto action = []()"},
      {"start(/* callback */ []() -> void {});", "/* callback */ []()"},
      {"start(// callback\n[]() -> void {});", "// callback\n"},
      {"start([]() -> void {", "start([]()"},
      {"items[index];", "items[index]"},
      {"[[discard]] start([]() -> void {});", "[[discard]] start([]()"},
      {"for(mut std::size_t i=0;i<N;i++){auto started="
       "std::thread::start( []() -> void {}\n);}",
       "std::thread::start([]() -> void {}"},
  };
  for (const auto &[source, expected] : cases) {
    const lang::Formatter formatter;
    const auto actual = formatter.format(source);
    if (actual.find(expected) == std::string::npos ||
        formatter.format(actual) != actual) {
      std::cerr << "Expected stable fragment: " << expected << "\nActual:\n"
                << actual;
      return EXIT_FAILURE;
    }
    lang::Lexer beforeLexer, afterLexer;
    const auto before = beforeLexer.scan(source);
    const auto after = afterLexer.scan(actual);
    if (before.size() != after.size()) {
      return EXIT_FAILURE;
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
      if (before[i].kind == lang::TokenKind::END_OF_FILE &&
          after[i].kind == lang::TokenKind::END_OF_FILE) {
        continue;
      }
      if (before[i].kind != after[i].kind ||
          before[i].lexeme != after[i].lexeme) {
        std::cerr << "Formatting changed a token in: " << source << '\n';
        return EXIT_FAILURE;
      }
    }
  }
  for (auto style : {lang::SpaceBeforeParensStyle::Never,
                     lang::SpaceBeforeParensStyle::ControlStatements,
                     lang::SpaceBeforeParensStyle::Always}) {
    lang::FormatOptions options;
    options.spaceBeforeParens = style;
    const lang::Formatter formatter(options);
    const auto actual = formatter.format("start( []() -> void {});");
    if (actual.find("([]") == std::string::npos ||
        formatter.format(actual) != actual) {
      std::cerr << "Parenthesis style changed lambda argument spacing:\n"
                << actual;
      return EXIT_FAILURE;
    }
  }
  std::cout << "Lambda spacing: " << cases.size()
            << " cases, token preservation and idempotence passed\n";
}
