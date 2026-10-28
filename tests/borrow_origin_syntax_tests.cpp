#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testAttributes() {
  const std::string source = R"(
[[track_caller, borrowed_from(owner)]] int& borrow(int& owner) { return owner; }
[[borrowed_from(owner)]] [[track_caller]] int& relay(int& owner) { return owner; }
struct Holder {
  int value;
  [[borrowed_from(this)]] int& get() { return this.value; }
};
[[noreturn, track_caller]] void stop() { while (true) {} }
[[c_abi]] struct Native { int value; };
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "borrow-origin-syntax.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "borrow attributes and existing attribute families must parse");
  expect(program.declarations().size() == 5, "declarations must be preserved");
  for (std::size_t index = 0; index < 2; ++index) {
    const auto *function =
        lang::stmtAs<lang::FunctionDecl>(program.declarations()[index].get());
    expect(function != nullptr, "annotated declaration must remain a function");
    const auto *attribute = function->findFunctionAttribute("borrowed_from");
    expect(attribute && attribute->argument &&
               attribute->argument->lexeme == "owner" &&
               attribute->argument->kind == lang::TokenKind::IDENTIFIER &&
               attribute->argument->source == "borrow-origin-syntax.gti" &&
               source.substr(attribute->argument->position, 5) == "owner",
           "owner argument must retain its source token");
    expect(function->hasFunctionAttribute("track_caller") &&
               !function->findFunctionAttribute("missing"),
           "existing presence queries must remain compatible");
  }
  const auto *holder =
      lang::stmtAs<lang::ClassDecl>(program.declarations()[2].get());
  expect(holder != nullptr && holder->members().size() == 2,
         "member attribute must preserve class layout");
  const auto *method =
      lang::stmtAs<lang::FunctionDecl>(holder->members()[1].get());
  const auto *attribute =
      method ? method->findFunctionAttribute("borrowed_from") : nullptr;
  expect(attribute && attribute->argument &&
             attribute->argument->kind == lang::TokenKind::THIS,
         "receiver origin must retain the this keyword token");
}

void testMalformedAttributes() {
  struct Fixture {
    const char *attribute;
    const char *message;
  };
  for (const Fixture fixture : {
           Fixture{"[[borrowed_from]]", "Expect '(' after 'borrowed_from'."},
           Fixture{"[[borrowed_from()]]",
                   "Expect an owner parameter name or 'this'"},
           Fixture{"[[borrowed_from(1)]]",
                   "Expect an owner parameter name or 'this'"},
           Fixture{"[[borrowed_from(owner, other)]]",
                   "Expect ')' after the borrowed_from owner."},
           Fixture{"[[borrowed_from(owner]]",
                   "Expect ')' after the borrowed_from owner."},
           Fixture{"[[borrowed_from(owner)]",
                   "Expect ']]' after declaration attributes."},
           Fixture{"[[noreturn(owner)]]", "Unknown function attribute"},
           Fixture{"[[unknown(owner)]]", "Unknown function attribute"},
           Fixture{"[[track_caller, unknown(owner)]]",
                   "Unknown function attribute"},
       }) {
    lang::Lexer lexer;
    lang::Parser parser(
        lexer.scan(std::string(fixture.attribute) +
                   " int& borrow(int& owner) { return owner; }"));
    [[maybe_unused]] const auto program = parser.parse();
    expect(parser.hadError() && !parser.errors().empty(),
           "malformed borrow attributes must not be accepted");
    const auto &diagnostic = parser.errors().front();
    expect(diagnostic.code == "GTI-P0001" &&
               diagnostic.phase == lang::DiagnosticPhase::Parsing &&
               diagnostic.message.find(fixture.message) != std::string::npos,
           "malformed attribute must have its owning parser diagnostic");
  }
}

void testFormatting() {
  const std::string source = "[[track_caller,borrowed_from(owner)]] int& "
                             "borrow(int& owner){return owner;}\n"
                             "struct Holder{int value;[[borrowed_from(this)]] "
                             "int& get(){return this.value;}};";
  const lang::Formatter formatter;
  const auto formatted = formatter.format(source);
  expect(formatted.find("borrowed_from(owner)") != std::string::npos &&
             formatted.find("borrowed_from(this)") != std::string::npos,
         "formatter must preserve the attribute invocation");
  expect(formatter.format(formatted) == formatted,
         "borrow attribute formatting must be idempotent");
  lang::Lexer beforeLexer, afterLexer;
  const auto before = beforeLexer.scan(source);
  const auto after = afterLexer.scan(formatted);
  expect(before.size() == after.size(), "formatter must preserve token count");
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (before[i].kind == lang::TokenKind::END_OF_FILE &&
        after[i].kind == lang::TokenKind::END_OF_FILE) {
      continue;
    }
    if (before[i].kind != after[i].kind ||
        before[i].lexeme != after[i].lexeme) {
      std::cerr << "Token " << i << ": '" << before[i].lexeme << "' -> '"
                << after[i].lexeme << "'\nFormatted:\n"
                << formatted;
    }
    expect(before[i].kind == after[i].kind &&
               before[i].lexeme == after[i].lexeme,
           "formatter must preserve every attribute token");
  }
  lang::Parser parser(after);
  [[maybe_unused]] const auto program = parser.parse();
  expect(!parser.hadError(), "formatted attributes must remain parseable");
}

void testResolvedSignatures() {
  const std::string source = R"(
[[borrowed_from(right)]] int& choose(int& left, int& right) { return right; }
[[borrowed_from(owner)]] T& relay<T>(T& owner) { return owner; }
struct Holder {
  int value = 7;
  [[borrowed_from(this)]] int& get() { return this.value; }
public:
  Holder() = default;
};
int& implicit(int& owner) { return owner; }
int main() {
  int left = 3;
  int right = 4;
  Holder holder{};
  return choose(left, right) + holder.get() + relay(right) + implicit(left);
}
)";
  const auto frontend =
      lang::Frontend().analyze("borrow-origin-hover.gti", source);
  expect(frontend.canGenerateCode(),
         "borrow-origin hover fixture must analyze");
  const lang::LanguageQueries queries;
  const auto unit = frontend.sourceGraph.entryUnit();
  const auto check = [&](std::size_t offset, std::string_view contract) {
    const auto hover = queries.hover(frontend, unit, offset + 1);
    if (!hover || !hover->signature.starts_with(contract)) {
      std::cerr << "Expected contract: " << contract
                << "\nActual: " << (hover ? hover->signature : "<missing>")
                << '\n';
      expect(false, "hover must render the resolved borrow origin");
    }
  };
  check(source.find("choose("), "[[borrowed_from(right)]]");
  check(source.rfind("choose("), "[[borrowed_from(right)]]");
  check(source.find("get()"), "[[borrowed_from(this)]]");
  check(source.rfind("get()"), "[[borrowed_from(this)]]");
  check(source.find("relay<T>"), "[[borrowed_from(owner)]]");
  check(source.rfind("relay(right)"), "[[borrowed_from(owner)]]");
  const auto implicit =
      queries.hover(frontend, unit, source.find("implicit(") + 1);
  expect(implicit &&
             implicit->signature.find("borrowed_from") == std::string::npos,
         "inferred origins must not invent an explicit source annotation");
  const auto origin = source.find("right)");
  const auto declaration = source.find("right) { return");
  const auto returned =
      source.find("return right") + std::string("return ").size();
  const auto definition = queries.definition(frontend, unit, origin + 1);
  expect(definition && definition->target.start == declaration,
         "attribute owner must resolve to its exact parameter declaration");
  const auto references = queries.references(frontend, unit, origin + 1, true);
  expect(references && references->sites.size() == 3 &&
             references->sites[0].span.start == origin &&
             references->sites[1].span.start == declaration &&
             references->sites[2].span.start == returned,
         "attribute, parameter, and body use must share one symbol identity");
  const auto rename =
      queries.rename(frontend, unit, origin + 1, "selected_owner");
  expect(rename.edits && rename.edits->spans.size() == 3,
         "parameter rename must include its borrow-origin annotation");
}
} // namespace

int main() {
  testAttributes();
  testMalformedAttributes();
  testFormatting();
  testResolvedSignatures();
}
