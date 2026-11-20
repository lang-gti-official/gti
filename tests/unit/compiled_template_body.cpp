// Producer AST bodies round-trip independently of the source path and reject an
// incomplete recipe.
#include "gti/compiled_template_body.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "unit_test.h"
#include <iostream>
#include <stdexcept>

namespace {
lang::Program parse(std::string text, std::string path) {
  lang::Lexer lexer;
  auto tokens = lexer.scan(text, path);
  lang::Parser parser(std::move(tokens), text);
  auto result = parser.parse();
  if (lexer.hadError() || parser.hadError())
    throw std::runtime_error("invalid test input");
  return result;
}
} // namespace
GTI_UNIT_TEST(compiled_template_body) {
  const std::string source = "export class Sample<T, uint64_t N> { public: "
                             "constexpr uint64_t size() { return N + 13; } };";
  auto original = parse(source, "producer.gti");
  auto recipe = lang::compileTemplateModule(original.declarations(), source);
  if (recipe.bodies.size() != 1 ||
      recipe.declarations.find("return") != std::string::npos)
    return 1;
  auto bytes = lang::encodeTemplateModule(recipe);
  auto received = lang::decodeTemplateModule(bytes);
  auto consumer = parse(received.declarations, "consumer.gti");
  lang::restoreTemplateBodies(consumer.declarations(), received,
                              "consumer.gti");
  if (lang::encodeTemplateModule(lang::compileTemplateModule(
          consumer.declarations(), source)) != bytes)
    return 2;
  // The bounds and opcode checks apply even after the outer bundle checksum
  // passes.
  auto reject = [&](lang::CompiledTemplateModule broken) {
    auto program = parse(broken.declarations, "consumer.gti");
    try {
      lang::restoreTemplateBodies(program.declarations(), broken,
                                  "consumer.gti");
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  auto invalid = received;
  invalid.bodies.begin()->second.pop_back();
  if (!reject(invalid))
    return 3;
  invalid = received;
  invalid.bodies.begin()->second.replace(4, 4, 4, static_cast<char>(0xff));
  if (!reject(invalid))
    return 4;
  invalid = received;
  invalid.bodies.clear();
  if (!reject(invalid))
    return 5;
  return 0;
}
