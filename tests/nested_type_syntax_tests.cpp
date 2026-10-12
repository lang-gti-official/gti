#include "gti/formatter.h"
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

template <class T>
const T *findMember(const lang::StmtList &members, std::string_view name) {
  for (const auto &member : members) {
    if (const auto *declaration = lang::stmtAs<T>(member.get());
        declaration && declaration->name().lexeme == name) {
      return declaration;
    }
  }
  return nullptr;
}

void testDeclarationsAndContext() {
  const std::string source = R"(
class Config {
public:
  struct Network {
    int port;
    Network(int value) : port(value) {}
    ~Network() {}
    class Credentials {
    public:
      Credentials() {}
    };
    Network() : port(0) {}
  };
  [[no_transfer]] class Logging {
  public:
    Logging() {}
    ~Logging() {}
  };
  Config() {}
  ~Config() {}
  Network network;
};
struct Second {
  struct Child<T> { T value; };
  Second() {}
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "nested-types.gti"), source);
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "named nested types and enclosing constructors must parse");
  expect(program.declarations().size() == 2,
         "nested declarations must not become top-level declarations");
  const auto *config =
      findMember<lang::ClassDecl>(program.declarations(), "Config");
  expect(config && config->kind() == lang::ClassKind::Class,
         "outer declaration kind must be retained");
  const auto *network =
      findMember<lang::ClassDecl>(config->members(), "Network");
  const auto *logging =
      findMember<lang::ClassDecl>(config->members(), "Logging");
  expect(network && logging && network->kind() == lang::ClassKind::Struct &&
             logging->kind() == lang::ClassKind::Class,
         "consecutive named nested declarations must preserve their kinds");
  const auto *credentials =
      findMember<lang::ClassDecl>(network->members(), "Credentials");
  expect(credentials &&
             findMember<lang::ConstructorDecl>(credentials->members(),
                                               "Credentials") &&
             findMember<lang::ConstructorDecl>(network->members(), "Network") &&
             findMember<lang::DestructorDecl>(network->members(), "Network") &&
             findMember<lang::ConstructorDecl>(config->members(), "Config") &&
             findMember<lang::DestructorDecl>(config->members(), "Config"),
         "recursive class parsing must restore every enclosing special-member "
         "context");
  expect(network->extent() && network->extent()->source == "nested-types.gti" &&
             source
                 .substr(network->extent()->start,
                         network->extent()->end - network->extent()->start)
                 .starts_with("struct Network"),
         "nested declaration extent must retain its precise source identity");
  const auto *second =
      findMember<lang::ClassDecl>(program.declarations(), "Second");
  const auto *child =
      second ? findMember<lang::ClassDecl>(second->members(), "Child")
             : nullptr;
  expect(child && child->genericParameters().size() == 1,
         "nested declarations retain their own generic parameter syntax");
}

void testConditionalAndRecovery() {
  const std::string source = R"(
struct Outer {
#if defined(FEATURE)
  struct First { int value; };
#else
  class Second { public: Second() {} };
#endif
  Outer() {}
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source), source);
  const auto program = parser.parse();
  const auto *outer =
      findMember<lang::ClassDecl>(program.declarations(), "Outer");
  expect(
      !parser.hadError() && outer &&
          lang::stmtAs<lang::ConditionalStmt>(outer->members().front().get()) &&
          findMember<lang::ConstructorDecl>(outer->members(), "Outer"),
      "conditional class members must preserve nesting and enclosing "
      "constructor context");

  for (const std::string invalid : {
           "struct Outer { struct { int value; } child; Outer() {} };",
           "struct Outer { union Child { int value; }; Outer() {} };",
           "struct Outer { [[no_transfer]] interface Child {}; Outer() {} };",
           "struct Outer { class n::Box<int> {}; Outer() {} };",
           "interface Outer { struct Child {}; };",
           "union Outer { class Child {}; };",
       }) {
    lang::Lexer invalidLexer;
    lang::Parser invalidParser(invalidLexer.scan(invalid), invalid);
    const auto recovered = invalidParser.parse();
    expect(invalidParser.hadError() && !invalidParser.errors().empty() &&
               invalidParser.errors().front().code == "GTI-P0001",
           "unsupported anonymous types and owner/kind combinations must be "
           "diagnosed by syntax");
    expect(!recovered.declarations().empty(),
           "invalid nested declarations must preserve the enclosing recovered "
           "type");
    if (invalid.starts_with("struct Outer")) {
      const auto *recoveredOuter =
          findMember<lang::ClassDecl>(recovered.declarations(), "Outer");
      expect(recoveredOuter && findMember<lang::ConstructorDecl>(
                                   recoveredOuter->members(), "Outer"),
             "recovery from invalid nested types must restore enclosing "
             "constructor context");
    }
  }
}

void testNestingLimit() {
  std::string source;
  for (int index = 0; index != 70; ++index) {
    source += "struct N" + std::to_string(index) + " { ";
  }
  for (int index = 0; index != 70; ++index) {
    source += "}; ";
  }
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source), source);
  [[maybe_unused]] const auto program = parser.parse();
  bool bounded = false;
  for (const auto &diagnostic : parser.errors()) {
    bounded |= diagnostic.code == "GTI-P0005";
  }
  expect(
      parser.hadError() && bounded,
      "nested type declarations must not bypass the parser recursion budget");
}

void testFormatting() {
  const std::string source =
      "class Config{public:struct Network{int port;Network(int p):port(p){}"
      "class Credentials{public:Credentials(){}};Network():port(0){} };"
      "class Logging{public:Logging(){}};Config():network(80){}Network "
      "network;};";
  lang::FormatOptions options;
  options.packConstructorInitializers =
      lang::PackConstructorInitializersStyle::Never;
  options.breakConstructorInitializers =
      lang::BreakConstructorInitializersStyle::AfterColon;
  const lang::Formatter formatter(options);
  const auto formatted = formatter.format(source);
  const bool layout =
      formatted.find("  struct Network {\n    int port;") !=
          std::string::npos &&
      formatted.find("    class Credentials {\n    public:") !=
          std::string::npos &&
      formatted.find("  Config() :\n      network(80) {}") != std::string::npos;
  if (!layout) {
    std::cerr << formatted;
  }
  expect(layout, "formatter must indent nested declaration bodies and restore "
                 "outer constructor indentation");
  expect(formatter.format(formatted) == formatted,
         "nested declaration formatting must be idempotent");
  lang::Lexer beforeLexer;
  lang::Lexer afterLexer;
  const auto before = beforeLexer.scan(source);
  const auto after = afterLexer.scan(formatted);
  expect(before.size() == after.size(), "formatting must preserve token count");
  for (std::size_t index = 0; index != before.size(); ++index) {
    if (before[index].kind == lang::TokenKind::END_OF_FILE &&
        after[index].kind == lang::TokenKind::END_OF_FILE) {
      continue;
    }
    expect(before[index].kind == after[index].kind &&
               before[index].lexeme == after[index].lexeme,
           "formatting must preserve every nested declaration token");
  }
  lang::Parser parser(after, formatted);
  [[maybe_unused]] const auto program = parser.parse();
  expect(!parser.hadError(),
         "formatted nested declarations must remain parseable");
}
} // namespace

int main() {
  testDeclarationsAndContext();
  testConditionalAndRecovery();
  testNestingLimit();
  testFormatting();
}
