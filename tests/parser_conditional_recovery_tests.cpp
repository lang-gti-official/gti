#include "gti/lexer.h"
#include "gti/parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::FunctionDecl *findFunction(const lang::StmtList &statements,
                                       std::string_view name) {
  for (const lang::StmtPtr &statement : statements) {
    const auto *function = lang::stmtAs<lang::FunctionDecl>(statement.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

const lang::ConditionalStmt *findConditional(const lang::StmtList &statements) {
  for (const lang::StmtPtr &statement : statements) {
    if (const auto *conditional =
            lang::stmtAs<lang::ConditionalStmt>(statement.get())) {
      return conditional;
    }
  }
  return nullptr;
}

void expectSingleDiagnostic(const lang::Parser &parser,
                            std::string_view sourceName,
                            std::string_view source, std::string_view boundary,
                            std::string_view message) {
  expect(parser.errors().size() == 1,
         "recovery should preserve one owning parser diagnostic");
  if (parser.errors().size() != 1) {
    return;
  }

  const std::size_t start = source.find(boundary);
  const int line = start == std::string_view::npos
                       ? 0
                       : 1 + static_cast<int>(std::count(
                                 source.begin(), source.begin() + start, '\n'));
  const lang::Diagnostic &diagnostic = parser.errors().front();
  expect(start != std::string_view::npos && diagnostic.code == "GTI-P0001" &&
             diagnostic.phase == lang::DiagnosticPhase::Parsing &&
             diagnostic.severity == lang::DiagnosticSeverity::Error &&
             diagnostic.message == message &&
             diagnostic.primary.source == sourceName &&
             diagnostic.primary.start == start &&
             diagnostic.primary.end == start + boundary.size() &&
             diagnostic.primary.line == line,
         "recovery should retain the exact diagnostic and boundary span");
}

void testValidNestedConditionalSwitch() {
  constexpr std::string_view sourceName = "valid-nested-conditional-switch.gti";
  constexpr std::string_view source = R"(
#if target.os == "first"
void choose(int value) {
  switch (value) {
  case 0:
#if target.arch == "arm64"
    return;
#else
    break;
#endif
  default:
    break;
  }
}
#else
void choose(int value) {}
#endif
int recovered_after() { return 0; }
)";

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(std::string(source), std::string(sourceName)));
  const lang::Program program = parser.parse();
  const lang::ConditionalStmt *outer = findConditional(program.declarations());
  const lang::FunctionDecl *choose =
      outer == nullptr || outer->branches().empty()
          ? nullptr
          : findFunction(outer->branches().front().statements, "choose");
  const auto *selection = choose == nullptr || choose->body() == nullptr ||
                                  choose->body()->statements().empty()
                              ? nullptr
                              : lang::stmtAs<lang::SwitchStmt>(
                                    choose->body()->statements().front().get());
  const lang::ConditionalStmt *nested =
      selection == nullptr || selection->arms().empty()
          ? nullptr
          : findConditional(selection->arms().front().statements);

  expect(!lexer.hadError() && !parser.hadError() && outer != nullptr &&
             outer->branches().size() == 2 && nested != nullptr &&
             nested->branches().size() == 2 &&
             findFunction(program.declarations(), "recovered_after") != nullptr,
         "valid nested conditionals and switch arms should parse unchanged");
}

void testStrayContinuationRecovery() {
  constexpr std::array<std::string_view, 3> continuations = {"#elif", "#else",
                                                             "#endif"};
  for (const std::string_view continuation : continuations) {
    const std::string switchSource = "void malformed(int value) {\n"
                                     "  switch (value) {\n" +
                                     std::string(continuation) +
                                     "\n  }\n}\n"
                                     "int recovered_after() { return 0; }\n";
    constexpr std::string_view switchName = "stray-switch-continuation.gti";
    lang::Lexer switchLexer;
    lang::Parser switchParser(
        switchLexer.scan(switchSource, std::string(switchName)));
    const lang::Program switchProgram = switchParser.parse();
    expectSingleDiagnostic(switchParser, switchName, switchSource, continuation,
                           "Expect a 'case' or 'default' label inside switch.");
    expect(findFunction(switchProgram.declarations(), "malformed") != nullptr &&
               findFunction(switchProgram.declarations(), "recovered_after") !=
                   nullptr,
           "stray switch continuations should be consumed and preserve later "
           "declarations");

    const std::string externSource = "extern \"C\" {\n" +
                                     std::string(continuation) +
                                     "\n}\n"
                                     "int recovered_after() { return 0; }\n";
    constexpr std::string_view externName = "stray-extern-continuation.gti";
    lang::Lexer externLexer;
    lang::Parser externParser(
        externLexer.scan(externSource, std::string(externName)));
    const lang::Program externProgram = externParser.parse();
    expectSingleDiagnostic(externParser, externName, externSource, continuation,
                           "An extern \"C\" block may contain only function or "
                           "variable declarations.");
    expect(findFunction(externProgram.declarations(), "recovered_after") !=
               nullptr,
           "stray extern continuations should be consumed and preserve later "
           "declarations");
  }
}

struct ActiveBoundaryCase {
  std::string_view sourceName;
  std::string_view source;
  std::string_view boundary;
  std::size_t branchCount;
  std::size_t recoveredBranch;
  std::string_view recoveredFunction;
};

void testActiveBoundaryPropagation() {
  constexpr std::array<ActiveBoundaryCase, 3> cases = {{
      {"active-elif-switch-recovery.gti",
       R"(#if target.os == "first"
void malformed(int value) {
  switch (value) {
#elif target.os == "second"
int recovered_elif() { return 1; }
#else
int recovered_else() { return 2; }
#endif
int recovered_after() { return 0; }
)",
       "#elif", 3, 1, "recovered_elif"},
      {"active-else-switch-recovery.gti",
       R"(#if target.os == "first"
void malformed(int value) {
  switch (value) {
#else
int recovered_else() { return 2; }
#endif
int recovered_after() { return 0; }
)",
       "#else", 2, 1, "recovered_else"},
      {"active-endif-switch-recovery.gti",
       R"(#if target.os == "first"
void malformed(int value) {
  switch (value) {
#endif
int recovered_after() { return 0; }
)",
       "#endif", 1, 0, ""},
  }};

  for (const ActiveBoundaryCase &testCase : cases) {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(std::string(testCase.source),
                                   std::string(testCase.sourceName)));
    const lang::Program program = parser.parse();
    expectSingleDiagnostic(parser, testCase.sourceName, testCase.source,
                           testCase.boundary,
                           "Expect a 'case' or 'default' label inside switch.");

    const lang::ConditionalStmt *conditional =
        findConditional(program.declarations());
    const bool recoveredBranch =
        testCase.recoveredFunction.empty() ||
        (conditional != nullptr &&
         conditional->branches().size() > testCase.recoveredBranch &&
         findFunction(
             conditional->branches()[testCase.recoveredBranch].statements,
             testCase.recoveredFunction) != nullptr);
    expect(conditional != nullptr &&
               conditional->branches().size() == testCase.branchCount &&
               conditional->branches().front().statements.empty() &&
               recoveredBranch &&
               findFunction(program.declarations(), "recovered_after") !=
                   nullptr,
           "an active continuation should reach its conditional owner without "
           "being consumed by nested switch recovery");
  }
}

void testActiveExternBoundaryPropagation() {
  constexpr std::string_view sourceName = "active-extern-recovery.gti";
  constexpr std::string_view source = R"(#if target.os == "first"
extern "C" {
#endif
int recovered_after() { return 0; }
)";

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(std::string(source), std::string(sourceName)));
  const lang::Program program = parser.parse();
  expectSingleDiagnostic(parser, sourceName, source, "#endif",
                         "An extern \"C\" block may contain only function or "
                         "variable declarations.");
  const lang::ConditionalStmt *conditional =
      findConditional(program.declarations());
  expect(conditional != nullptr && conditional->branches().size() == 1 &&
             conditional->branches().front().statements.empty() &&
             findFunction(program.declarations(), "recovered_after") != nullptr,
         "active extern recovery should propagate the boundary to its "
         "conditional owner");
}

void testNestedBoundaryPropagation() {
  constexpr std::string_view sourceName =
      "nested-switch-conditional-recovery.gti";
  constexpr std::string_view source = R"(#if target.os == "outer"
#if target.arch == "inner"
void malformed(int value) {
  switch (value) {
  case 0:
    switch (value) {
#else
int recovered_inner() { return 1; }
#endif
int recovered_outer_first() { return 2; }
#else
int recovered_outer_else() { return 3; }
#endif
int recovered_after() { return 0; }
)";

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(std::string(source), std::string(sourceName)));
  const lang::Program program = parser.parse();
  expectSingleDiagnostic(parser, sourceName, source, "#else",
                         "Expect a 'case' or 'default' label inside switch.");

  const lang::ConditionalStmt *outer = findConditional(program.declarations());
  const lang::ConditionalStmt *inner =
      outer == nullptr || outer->branches().empty()
          ? nullptr
          : findConditional(outer->branches().front().statements);
  expect(outer != nullptr && outer->branches().size() == 2 &&
             inner != nullptr && inner->branches().size() == 2 &&
             inner->branches().front().statements.empty() &&
             findFunction(inner->branches()[1].statements, "recovered_inner") !=
                 nullptr &&
             findFunction(outer->branches()[0].statements,
                          "recovered_outer_first") != nullptr &&
             findFunction(outer->branches()[1].statements,
                          "recovered_outer_else") != nullptr &&
             findFunction(program.declarations(), "recovered_after") != nullptr,
         "nested switch recovery should stop at the nearest conditional owner "
         "and retain all later branches");
}

void testInvalidConditionRecovery() {
  for (const std::string source :
       {"#if FLAG\nint first() { return 1; }\n#endif\n",
        "#if defined(OK)\nint first() { return 1; }\n"
        "#elif FLAG\nint second() { return 2; }\n#endif\n",
        "#if FLAG\n#if defined(INNER)\nint inner() { return 1; }\n"
        "#else\nint other() { return 2; }\n#endif\n#endif\n"}) {
    const std::string complete =
        source + "int recovered_after() { return 0; }\n";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(complete, "bad-condition.gti"));
    const auto program = parser.parse();
    expect(parser.errors().size() == 1,
           "a failed condition should not orphan its branch boundaries");
    if (parser.errors().size() == 1) {
      const auto &diagnostic = parser.errors().front();
      expect(diagnostic.code == "GTI-P0003" &&
                 diagnostic.primary.source == "bad-condition.gti" &&
                 diagnostic.primary.start == complete.find("FLAG") &&
                 diagnostic.primary.end == complete.find("FLAG") + 4,
             "the original malformed-condition diagnostic retains its span");
    }
    expect(findConditional(program.declarations()) != nullptr &&
               findFunction(program.declarations(), "recovered_after") !=
                   nullptr,
           "condition recovery preserves nesting and subsequent declarations");
  }
}

} // namespace

int main() {
  testValidNestedConditionalSwitch();
  testStrayContinuationRecovery();
  testActiveBoundaryPropagation();
  testActiveExternBoundaryPropagation();
  testNestedBoundaryPropagation();
  testInvalidConditionRecovery();

  if (failures != 0) {
    std::cerr << failures << " parser conditional recovery test(s) failed\n";
    return 1;
  }
  std::cout << "Parser conditional recovery tests passed\n";
  return 0;
}
