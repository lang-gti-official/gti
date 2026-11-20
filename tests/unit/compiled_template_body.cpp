// Compiled declarations preserve bodies, parser-owned bindings, and lexical
// lifetime identity.
#include "gti/compiled_template_body.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "unit_test.h"
#include <iostream>
#include <stdexcept>

namespace {
lang::Program parse(std::string text, std::string path,
                    lang::LifetimeNames *names = nullptr) {
  lang::Lexer lexer;
  auto tokens = lexer.scan(text, path);
  lang::Parser parser(std::move(tokens), text);
  auto result = parser.parse();
  if (lexer.hadError() || parser.hadError())
    throw std::runtime_error(parser.errors().empty()
                                 ? "invalid lexical input"
                                 : parser.errors().front().message);
  if (names)
    *names = parser.lifetimeNames();
  return result;
}
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
lang::CompiledTemplateModule roundTrip(const std::string &source) {
  lang::LifetimeNames names;
  auto producer = parse(source, "producer.gti", &names);
  auto original =
      lang::compileTemplateModule(producer.declarations(), source, names);
  auto bytes = lang::encodeTemplateModule(original);
  auto module = lang::decodeTemplateModule(bytes);
  auto consumer = parse(module.declarations, "installed.gti");
  lang::restoreTemplateBodies(consumer.declarations(), module, "installed.gti");
  auto reencoded = lang::compileTemplateModule(
      consumer.declarations(), source,
      lang::compiledTemplateLifetimeNames(module, "installed.gti"));
  require(lang::encodeTemplateModule(reencoded) == bytes,
          "restored syntax changed");
  return module;
}
void checkDeclarationUnits() {
  const std::string declarations = R"gti(
enum class Side { left, right };
struct View { View() = delete; };
extern "C" { int native(int value); }
concept Reader<T> { int read(T& value); };
)gti";
  auto unit = roundTrip(declarations);
  require(unit.bodies.empty() && unit.declarations == declarations,
          "declaration-only unit changed");
  auto damaged = unit;
  damaged.bodies[declarations.find('{')] = "";
  try {
    auto declarationsOnly = parse(unit.declarations, "installed.gti");
    lang::restoreTemplateBodies(declarationsOnly.declarations(), damaged,
                                "installed.gti");
    throw std::runtime_error("extra recipe was accepted");
  } catch (const std::invalid_argument &) {
  }
  auto conformance = roundTrip(R"gti(
concept Reader<T> { int read(T& value); };
for concept Reader<int> { int read(int& value) { return value; } };
)gti");
  require(conformance.bodies.size() == 1,
          "conformance operation recorded twice");
}
void checkControlFlow() {
  const std::string source = R"gti(
int total<T>(T& values, int offset) {
  mut int answer = 0;
  auto add = [offset](int value) -> int { return value + offset; };
  for (auto& value : values) {
    switch (value) {
    case 0: continue;
    default: answer += add(value);
    }
  }
  while (answer < 1) { ++answer; }
  do { --answer; } while (answer > 8);
#if target.os == "windows"
  return chosen!(answer /* exact trivia */ + offset);
#else
  return answer;
#endif
}
)gti";
  auto unit = roundTrip(source);
  require(unit.bodies.size() == 1 &&
              unit.declarations.find("exact trivia") == std::string::npos,
          "body leaked into declarations");
  auto program = parse(unit.declarations, "installed.gti");
  auto spelling = lang::restoreTemplateBodies(program.declarations(), unit,
                                              "installed.gti");
  const std::string argument = "(answer /* exact trivia */ + offset)";
  require(spelling && spelling->substr(source.find(argument),
                                       argument.size()) == argument,
          "installed expression-text query would read a blank body");
  auto fn =
      lang::stmtAs<lang::FunctionDecl>(program.declarations().front().get());
  auto &statements = fn->body()->statements();
  auto loop = lang::stmtAs<lang::RangeForStmt>(statements.at(2).get());
  require(loop && loop->ownerBinding() &&
              loop->ownerBinding()->isRangeOwner() &&
              loop->rangeBinding()->isRangeBinding() &&
              loop->iteratorBinding()->isRangeIterator() &&
              loop->elementOwnerBinding() &&
              loop->elementOwnerBinding()->isRangeElementOwner() &&
              loop->elementBinding()->isRangeBinding() && loop->sourceBody(),
          "lost range loop bindings");
  auto cfg = lang::stmtAs<lang::ConditionalStmt>(statements.back().get());
  auto result = lang::stmtAs<lang::ReturnStmt>(
      cfg->branches().front().statements.front().get());
  auto call = lang::exprAs<lang::Call>(result->value().get());
  require(call && call->capturedArguments() &&
              call->capturedArguments()->text ==
                  "(answer /* exact trivia */ + offset)" &&
              call->capturedArguments()->origins.front().source ==
                  "installed.gti",
          "lost macro capture");
}
void checkInterfaceLambdas() {
  const std::string source = R"gti(
struct Box<T> {
  int value;
  Box() : value([]() -> int { return 34; }()) {}
};
int takes(int value = []() -> int { return 21; }()) { return value; }
)gti";
  auto unit = roundTrip(source);
  require(unit.bodies.size() == 4 &&
              unit.declarations.find("return 34") == std::string::npos &&
              unit.declarations.find("return 21") == std::string::npos,
          "unprojected initializer or default");
  auto program = parse(unit.declarations, "installed.gti");
  lang::restoreTemplateBodies(program.declarations(), unit, "installed.gti");
  auto cls = lang::stmtAs<lang::ClassDecl>(program.declarations().at(0).get());
  const lang::ConstructorDecl *constructor = nullptr;
  for (auto &member : cls->members()) {
    if (auto candidate = lang::stmtAs<lang::ConstructorDecl>(member.get()))
      constructor = candidate;
  }
  require(constructor, "missing constructor");
  auto operand = lang::exprAs<lang::Call>(
      constructor->initializers().front().arguments.front().get());
  auto closure = lang::exprAs<lang::Lambda>(operand->callee().get());
  auto result = lang::stmtAs<lang::ReturnStmt>(closure->body().at(0).get());
  require(
      lang::exprAs<lang::LiteralExpr>(result->value().get())->token().lexeme ==
          "34",
      "wrong initializer result");
}
void checkLifetimes() {
  const std::string source = R"gti(
int&<a> first<lifetime a>(int&<a> value) {
  int&<a> ref = value;
  auto size = sizeof(Callback<for<lifetime inner> int&<inner>(int&<inner>)>);
  return ref;
}
int&<a> second<lifetime a>(int&<a> value) { int&<a> ref = value; return ref; }
)gti";
  auto unit = roundTrip(source);
  lang::LifetimeNames original, skeleton;
  (void)parse(source, "producer.gti", &original);
  (void)parse(unit.declarations, "installed.gti", &skeleton);
  auto restored = lang::compiledTemplateLifetimeNames(unit, "installed.gti");
  require(original.binders.size() == restored.binders.size() &&
              original.uses.size() == restored.uses.size() &&
              skeleton.binders.size() < original.binders.size() &&
              skeleton.uses.size() < original.uses.size(),
          "test must distinguish a skeleton from complete lifetime mapping");
  for (std::size_t i = 0; i < original.binders.size(); ++i)
    require(original.binders[i].position == restored.binders[i].position &&
                original.binders[i].lexeme == restored.binders[i].lexeme &&
                restored.binders[i].source == "installed.gti",
            "lifetime binder identity changed");
  for (std::size_t i = 0; i < original.uses.size(); ++i)
    require(original.uses[i].name.position == restored.uses[i].name.position &&
                original.uses[i].binder.position ==
                    restored.uses[i].binder.position &&
                restored.uses[i].name.source == "installed.gti" &&
                restored.uses[i].binder.source == "installed.gti",
            "lifetime use rebound to a different declaration");
}
void checkUnsupported(const std::string &source, std::string_view reason) {
  auto parsed = parse(source, "producer.gti");
  try {
    (void)lang::compileTemplateModule(parsed.declarations(), source);
    throw std::runtime_error("accepted unrepresentable body");
  } catch (const std::invalid_argument &e) {
    require(std::string_view(e.what()).find(reason) != std::string_view::npos,
            "wrong unsupported construct");
  }
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
  checkDeclarationUnits();
  checkControlFlow();
  checkLifetimes();
  checkInterfaceLambdas();
  checkUnsupported("static_assert([]() -> bool { return true; }());",
                   "lambda in declaration context");
  checkUnsupported("void f() { auto [left, right] = pair(); }",
                   "structured binding");
  return 0;
}
