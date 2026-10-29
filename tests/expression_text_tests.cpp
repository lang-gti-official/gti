#include "gti/ast_clone.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/coverage.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("expression-text.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}

std::unordered_set<std::string>
mainLiterals(const lang::FrontendResult &result) {
  std::unordered_set<std::string> literals;
  for (const auto &function : result.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "main")
      continue;
    for (const auto &value : function.body.values) {
      expect(value.intrinsic != lang::IntrinsicKind::SourceExpressionText,
             "expression metadata must never remain an executable intrinsic");
      if (value.kind == lang::HirValueKind::Literal && value.literal)
        if (const auto *text = std::get_if<std::string>(&*value.literal))
          literals.insert(*text);
    }
  }
  return literals;
}

void testWrittenOriginAndCloning() {
  const std::string source = "int main() { probe( (first /* keep λ */ + 2) , "
                             "[]() -> int { return 3; }); }";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "written-origin.gti"));
  const auto program = parser.parse();
  expect(parser.errors().empty(), "written-origin fixture should parse");
  const auto *function =
      lang::stmtAs<lang::FunctionDecl>(program.declarations().front().get());
  const auto *statement = lang::stmtAs<lang::ExpressionStmt>(
      function->body()->statements().front().get());
  const auto *call = lang::exprAs<lang::Call>(statement->expression().get());
  const auto text = [&](const lang::Expr &expression) {
    const auto &origin = expression.writtenOrigin();
    return origin ? source.substr(origin->start, origin->end - origin->start)
                  : std::string{};
  };
  expect(text(*call->arguments()[0]) == "(first /* keep λ */ + 2)",
         "extent should preserve grouping, UTF-8 and interior comment trivia");
  expect(text(*call->arguments()[1]) == "[]() -> int { return 3; }",
         "extent should include a lambda's closing body brace");
  const auto ordinary = lang::cloneExpr(*call->arguments()[0], 7);
  expect(text(*ordinary) == text(*call->arguments()[0]),
         "ordinary cloning should preserve the exact written origin");

  lang::AstCloneOptions replacement;
  replacement.expression = [&](const lang::Expr &expression) -> lang::ExprPtr {
    if (&expression == call->arguments()[0].get())
      return lang::cloneExpr(*call->arguments()[1]);
    return nullptr;
  };
  const auto forwarded = lang::cloneExpr(*call->arguments()[0], 0, replacement);
  expect(text(*forwarded) == text(*call->arguments()[1]),
         "whole-expression substitution should retain the replacement origin");
  const auto composed = lang::cloneExpr(*call, 0, replacement);
  expect(!composed->writtenOrigin(),
         "composing substituted children must invalidate a parent's origin");
  const auto *composedCall = lang::exprAs<lang::Call>(composed.get());
  expect(text(*composedCall->arguments()[0]) == text(*call->arguments()[1]),
         "a composed parent must still retain its forwarded child's origin");
}

void testLiteralNormalizationAndForwarding() {
  const auto result = analyze(R"gti(
import std.source_location;
import std.memory;
int effect() { return 19; }
#macro std::string_view named<T>(T argument) {
  return std::expression_text(argument);
}
#macro std::string_view nested<T>(T argument) { return named(argument); }
#macro std::string_view defaulted(int argument = 6 /* default */ + 1) {
  return nested(argument);
}
int main() {
  std::string_view direct = std::expression_text( effect( /* inside */ ) );
  std::string_view forwarded = nested((3 /* λ */ + 4));
  std::string_view unicode = std::expression_text("λβ");
  std::string_view from_default = defaulted();
  mut std::unique_ptr<int> owner = std::make_unique<int>(7);
  std::string_view named_owner = std::expression_text(owner);
  std::string_view moved = std::expression_text(std::move(owner));
  std::string_view borrowed = std::expression_text(*owner);
  owner.reset();
  return int32_t(direct.size() + forwarded.size() + unicode.size() +
                 from_default.size() + named_owner.size() + moved.size() +
                 borrowed.size());
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "forwarding, defaults and unevaluated owner expressions need verified "
         "MIR");
  const auto literals = mainLiterals(result);
  for (const std::string_view expected :
       {"effect( /* inside */ )", "(3 /* λ */ + 4)", "\"λβ\"",
        "6 /* default */ + 1", "owner", "std::move(owner)", "*owner"})
    expect(literals.contains(std::string(expected)),
           "expression-text result should be the exact original written text");
  for (const auto &function : result.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "main")
      for (const auto &value : function.body.values)
        if (value.functionTarget)
          if (const auto *target =
                  result.hir.findFunctionInstance(*value.functionTarget))
            expect(!target->source || target->source->name().lexeme != "effect",
                   "metadata inspection must not lower an argument call");
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        expect(instruction.intrinsic !=
                   lang::IntrinsicKind::SourceExpressionText,
               "MIR should consume only ordinary literal values");

  const auto crlf =
      analyze("import std.source_location;\r\n"
              "int main() { std::string_view text = std::expression_text("
              "(1 /* λ */ +\r\n 2)); return int32_t(text.size()); }\r\n");
  expect(
      crlf.canGenerateCode() &&
          mainLiterals(crlf).contains("(1 /* λ */ +\r\n 2)"),
      "expression text should preserve exact CRLF bytes in its source buffer");

  if (result.canGenerateCode()) {
    lang::HirProgram malformedHir = result.hir;
    auto &function = const_cast<lang::HirFunctionInstance &>(
        malformedHir.functionInstances().front());
    if (!function.body.values.empty()) {
      function.body.values.front().intrinsic =
          lang::IntrinsicKind::SourceExpressionText;
      expect(
          !lang::verifyHirProgramPlans(result.semantics, malformedHir).valid(),
          "HIR verification should reject unnormalized metadata calls");
    }
    lang::MirProgram malformedMir = result.mir;
    bool mutated = false;
    for (const auto &candidate : malformedMir.functionInstances()) {
      auto &body = const_cast<lang::MirFunctionInstance &>(candidate).body;
      for (auto &block : body.blocks)
        if (!block.instructions.empty()) {
          block.instructions.front().intrinsic =
              lang::IntrinsicKind::SourceExpressionText;
          mutated = true;
          expect(!lang::verifyMirOptimizationVocabulary(body).valid(),
                 "optimizer preflight must reject an executable metadata "
                 "intrinsic");
          break;
        }
      if (mutated)
        break;
    }
    expect(mutated && !lang::verifyMirProgram(malformedMir).valid(),
           "MIR verification must reject forged runtime metadata operations");
  }
}

void testRejections() {
  const auto composed = analyze(R"gti(
import std.source_location;
#macro std::string_view composed(int argument) {
  return std::expression_text(argument + 1);
}
int main() { std::string_view text = composed(2); return 0; }
)gti");
  expect(!composed.canGenerateCode(),
         "synthetic compound expression text should fail closed");
  bool originDiagnostic = false;
  for (const auto &diagnostic : composed.diagnostics)
    if (diagnostic.code == "GTI-S2019" &&
        diagnostic.message.find("complete written source expression") !=
            std::string::npos) {
      originDiagnostic = true;
      expect(std::filesystem::path(diagnostic.primary.source).filename() ==
                 "expression-text.gti",
             "provenance failure should retain its caller source identity");
    }
  expect(originDiagnostic, "synthetic source needs its semantic diagnostic");

  const std::string invalidSource = R"gti(
import std.source_location;
int main() {
  std::string_view text = std::expression_text(missing_expression + 1);
  return 0;
}
)gti";
  const auto invalid = analyze(invalidSource);
  expect(!invalid.canGenerateCode(),
         "unevaluated expression text must still reject an invalid expression");
  bool exactSpan = false;
  for (const auto &diagnostic : invalid.diagnostics)
    if (diagnostic.primary.start == invalidSource.find("missing_expression"))
      exactSpan = std::filesystem::path(diagnostic.primary.source).filename() ==
                      "expression-text.gti" &&
                  diagnostic.primary.end - diagnostic.primary.start == 18;
  expect(exactSpan, "invalid operands should retain their exact written span");

  const auto unsafe = analyze(R"gti(
import std.source_location;
int main() {
  int* pointer = nullptr;
  std::string_view text = std::expression_text(*pointer);
  return 0;
}
)gti");
  bool unsafeRequired = false;
  for (const auto &diagnostic : unsafe.diagnostics)
    unsafeRequired |= diagnostic.message.find("unsafe") != std::string::npos;
  expect(
      !unsafe.canGenerateCode() && unsafeRequired,
      "unevaluated source inspection must retain ordinary unsafe obligations");

  const auto privateCall = analyze(R"gti(
import std.source_location;
int main() {
  std::string_view text = gti_internal::source_expression_text<int>(1);
  return 0;
}
)gti");
  bool denied = false;
  for (const auto &diagnostic : privateCall.diagnostics)
    denied |= diagnostic.code == "GTI-S2058";
  expect(
      !privateCall.canGenerateCode() && denied,
      "the source-text capability must remain private by declaration identity");

  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Parsing;
  const auto parsed =
      lang::Frontend(options).analyze("unavailable-source.gti",
                                      "import std.source_location;\n"
                                      "int main() { std::string_view text = "
                                      "std::expression_text(2); return 0; }",
                                      {library / "prelude.gti"}, {}, {library});
  expect(parsed.sourceValid && parsed.syntaxValid,
         "unavailable-source fixture should retain valid loaded syntax");
  lang::SemanticVisitor withoutSource(lang::TargetInfo::host(),
                                      &parsed.sourceGraph);
  expect(!withoutSource.check(parsed.program),
         "expression text must fail closed without its owning source buffer");
  bool unavailable = false;
  for (const auto &diagnostic : withoutSource.errors())
    unavailable |=
        diagnostic.code == "GTI-S2019" &&
        diagnostic.message.find("complete written source expression") !=
            std::string::npos;
  expect(unavailable, "missing source storage needs the provenance diagnostic");
}

void testPublicSpellingHasNoCompilerMeaning() {
  const auto result = analyze(R"gti(
import std.string;
namespace std {
  std::string_view expression_text(int value) { return "ordinary"; }
}
int main() {
  mut int counter = 0;
  std::string_view text = std::expression_text(++counter);
  return counter;
}
)gti");
  expect(
      result.canGenerateCode(),
      "an unrelated ordinary declaration may use the public helper spelling");
  bool ordinaryCall = false;
  for (const auto &function : result.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "main")
      for (const auto &value : function.body.values)
        if (value.functionTarget)
          if (const auto *target =
                  result.hir.findFunctionInstance(*value.functionTarget))
            ordinaryCall |= target->source &&
                            target->source->name().lexeme == "expression_text";
  expect(
      ordinaryCall,
      "public std spelling must not acquire the trusted capability's meaning");
}

void testExplicitFallback() {
  const auto result = analyze(R"gti(
import std.source_location;
import std.memory;
import std.assert;
constexpr std::string_view fallback = "synthetic condition";
#macro std::string_view composed(int expression) {
  return std::expression_text(expression + 1, fallback);
}
#macro std::string_view forwarded(int expression) { return composed(expression); }
#macro std::string_view owner_text<T>(T expression) {
  return std::expression_text((expression), "owner fallback");
}
#macro checked(int expression) { std::assert(expression > 0); }
int main() {
  mut int counter = 0;
  std::string_view exact = std::expression_text(++counter, "unused fallback");
  std::string_view synthetic = forwarded(++counter);
  mut std::unique_ptr<int> owner = std::make_unique<int>(7);
  std::string_view named = owner_text(owner);
  std::string_view moved = owner_text(std::move(owner));
  std::string_view borrowed = owner_text(*owner);
  owner.reset();
  checked(3);
  return counter;
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "explicit constant fallback should admit synthesized source without "
         "effects");
  const auto literals = mainLiterals(result);
  for (const std::string_view expected :
       {"++counter", "synthetic condition", "owner fallback",
        "assertion condition"})
    expect(literals.contains(std::string(expected)),
           "available written text wins; synthetic source uses only explicit "
           "fallback");

  const auto dynamic = analyze(R"gti(
import std.source_location;
std::string_view dynamic() { return "dynamic"; }
int main() {
  std::string_view text = std::expression_text(1, dynamic());
  return 0;
}
)gti");
  bool rejected = false;
  for (const auto &diagnostic : dynamic.diagnostics)
    rejected |= diagnostic.code == "GTI-S2019" &&
                diagnostic.message.find("compile-time literal-backed") !=
                    std::string::npos;
  expect(!dynamic.canGenerateCode() && rejected,
         "dynamic fallback must fail even when exact expression text is "
         "available");

  const auto invalid = analyze(R"gti(
import std.source_location;
int main() {
  std::string_view text = std::expression_text(missing, "explicit fallback");
  return 0;
}
)gti");
  expect(!invalid.canGenerateCode(),
         "fallback must not hide a statically invalid operand");
}
} // namespace

int main() {
  testWrittenOriginAndCloning();
  testLiteralNormalizationAndForwarding();
  testRejections();
  testPublicSpellingHasNoCompilerMeaning();
  testExplicitFallback();
  return failures == 0 ? 0 : 1;
}
