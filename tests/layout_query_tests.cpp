#include "gti/ast_printer.h"
#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

static_assert(!std::is_default_constructible_v<lang::CppEmitter>);
static_assert(!std::is_constructible_v<lang::CppEmitter, lang::SemanticModel &&,
                                       const lang::HirProgram &>);
static_assert(
    !std::is_constructible_v<lang::CppEmitter, const lang::SemanticModel &,
                             lang::HirProgram &&>);
static_assert(!std::is_constructible_v<lang::CppEmitter, lang::SemanticModel &&,
                                       lang::HirProgram &&>);

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

const lang::Diagnostic *findCode(const lang::FrontendResult &result,
                                 std::string_view code) {
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == code;
                   });
  return found == result.diagnostics.end() ? nullptr : &*found;
}

const lang::FunctionDecl *findFunction(const lang::Program &program,
                                       std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

const lang::HirFunctionInstance *
findHirFunction(const lang::FrontendResult &result, std::string_view name) {
  const auto found =
      std::find_if(result.hir.functionInstances().begin(),
                   result.hir.functionInstances().end(),
                   [&](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == name;
                   });
  return found == result.hir.functionInstances().end() ? nullptr : &*found;
}

const lang::VariableDecl *findVariable(const lang::Program &program,
                                       std::string_view functionName,
                                       std::string_view variableName) {
  const lang::FunctionDecl *function = findFunction(program, functionName);
  if (function == nullptr || function->body() == nullptr) {
    return nullptr;
  }
  for (const lang::StmtPtr &statement : function->body()->statements()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(statement.get());
    if (variable != nullptr && variable->name().lexeme == variableName) {
      return variable;
    }
  }
  return nullptr;
}

const lang::LayoutQuery *findQuery(const lang::Program &program,
                                   std::string_view variableName) {
  const lang::VariableDecl *variable =
      findVariable(program, "main", variableName);
  return variable == nullptr ? nullptr
                             : dynamic_cast<const lang::LayoutQuery *>(
                                   variable->initializer().get());
}

std::optional<std::uint64_t> queryValue(const lang::FrontendResult &result,
                                        std::string_view variableName) {
  const lang::LayoutQuery *query = findQuery(result.program, variableName);
  if (query == nullptr ||
      result.semantics.typeOf(*query) != lang::SemanticType::UInt64) {
    return std::nullopt;
  }
  const std::optional<lang::ConstantValue> constant =
      result.semantics.findConstant(*query);
  const auto *integer = constant == std::nullopt
                            ? nullptr
                            : std::get_if<lang::ConstantInteger>(&*constant);
  if (integer == nullptr || integer->negative || integer->domain.width != 64 ||
      integer->domain.signedValue) {
    return std::nullopt;
  }
  return integer->magnitude;
}

std::unordered_map<std::string, std::uint64_t>
queryValues(const lang::FrontendResult &result,
            const std::vector<std::string> &names) {
  std::unordered_map<std::string, std::uint64_t> values;
  for (const std::string &name : names) {
    if (const std::optional<std::uint64_t> value = queryValue(result, name)) {
      values.emplace(name, *value);
    }
  }
  return values;
}

lang::FrontendResult
analyze(std::string_view name, std::string source,
        std::optional<lang::TargetInfo> target = std::nullopt) {
  lang::FrontendOptions options;
  if (target) {
    options.target = std::move(*target);
  }
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source));
}

void testParserAndAst() {
  lang::Lexer lexer;
  lang::Parser sizeParser(lexer.scan("sizeof(int16_t[3][4])"));
  const lang::ExprPtr size = sizeParser.parseExpression();
  const auto *sizeQuery = dynamic_cast<const lang::LayoutQuery *>(size.get());
  expect(sizeQuery != nullptr && !sizeParser.hadError() &&
             sizeQuery->kind() == lang::LayoutQueryKind::Size &&
             sizeQuery->keyword().kind == lang::TokenKind::SIZEOF &&
             sizeQuery->type().arrayExtents.size() == 2 &&
             lang::AstPrinter().print(*size) == "(sizeof int16_t[3][4])",
         "sizeof(type) should parse as a dedicated type-only AST node");

  lang::Parser alignmentParser(lexer.scan("alignof(const uint32_t*)"));
  const lang::ExprPtr alignment = alignmentParser.parseExpression();
  const auto *alignmentQuery =
      dynamic_cast<const lang::LayoutQuery *>(alignment.get());
  expect(alignmentQuery != nullptr && !alignmentParser.hadError() &&
             alignmentQuery->kind() == lang::LayoutQueryKind::Alignment &&
             alignmentQuery->keyword().kind == lang::TokenKind::ALIGNOF &&
             alignmentQuery->type().pointeeConst.has_value() &&
             alignmentQuery->type().pointer.has_value() &&
             lang::AstPrinter().print(*alignment) ==
                 "(alignof const uint32_t*)",
         "alignof(type) should retain pointee const and raw-pointer syntax");

  lang::Parser missingParentheses(lexer.scan("sizeof int32_t"));
  expect(missingParentheses.parseExpression() == nullptr &&
             missingParentheses.hadError(),
         "layout queries should require a parenthesized type operand");

  lang::Parser expressionOperand(lexer.scan("sizeof(1)"));
  expect(expressionOperand.parseExpression() == nullptr &&
             expressionOperand.hadError(),
         "layout queries should reject expression operands");

  lang::Parser recovery(lexer.scan(
      "uint64_t broken = alignof(int32_t; int intact() { return 0; }"));
  const lang::Program recovered = recovery.parse();
  expect(recovery.hadError() && recovered.declarations().size() == 1 &&
             findFunction(recovered, "intact") != nullptr,
         "a missing layout-query parenthesis should preserve the following "
         "declaration");

  const std::vector<lang::Token> operatorTokens = lexer.scan("sizeof alignof");
  expect(operatorTokens.size() >= 3 &&
             operatorTokens[0].kind == lang::TokenKind::SIZEOF &&
             operatorTokens[1].kind == lang::TokenKind::ALIGNOF &&
             lang::isOperatorToken(operatorTokens[0].kind) &&
             lang::isOperatorToken(operatorTokens[1].kind) &&
             !lang::isKeywordToken(operatorTokens[0].kind) &&
             !lang::isKeywordToken(operatorTokens[1].kind),
         "sizeof and alignof should use the word-operator token class");
}

std::string validLayoutSource() {
  return R"(
using Word = uint16_t;
using Matrix = int16_t[3][4];
enum class State : uint8_t { Ready };
union WordBits { uint32_t integer; float real; };
constexpr uint64_t stride = sizeof(uint32_t);

int main() {
  uint64_t bool_size = sizeof(bool);
  uint64_t char_alignment = alignof(char);
  uint64_t default_int_size = sizeof(int);
  uint64_t default_uint_alignment = alignof(uint);
  uint64_t i8_size = sizeof(int8_t);
  uint64_t i16_alignment = alignof(int16_t);
  uint64_t i32_size = sizeof(int32_t);
  uint64_t i64_alignment = alignof(int64_t);
  uint64_t u8_size = sizeof(uint8_t);
  uint64_t u16_alignment = alignof(uint16_t);
  uint64_t u32_size = sizeof(uint32_t);
  uint64_t u64_alignment = alignof(uint64_t);
  uint64_t float_size = sizeof(float);
  uint64_t double_size = sizeof(double);
  uint64_t double_alignment = alignof(double);
  uint64_t pointer_alignment = alignof(const int32_t*);
  uint64_t void_pointer_size = sizeof(void*);
  uint64_t alias_size = sizeof(Word);
  uint64_t enum_size = sizeof(State);
  uint64_t union_size = sizeof(WordBits);
  uint64_t union_alignment = alignof(WordBits);
  uint64_t matrix_size = sizeof(Matrix);
  uint64_t matrix_alignment = alignof(Matrix);
  uint64_t indirect_array_size = sizeof(int16_t[stride]);
  int32_t values[stride] = {};
  return values[0];
}
)";
}

void testSemanticLayoutConstants() {
  const lang::FrontendResult result =
      analyze("layout-query-valid.gti", validLayoutSource());
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode() && result.diagnostics.empty(),
         "supported scalar, pointer, enum, union, alias, and array layout "
         "queries should complete the frontend pipeline");

  const std::unordered_map<std::string, std::uint64_t> expected = {
      {"bool_size", 1},         {"char_alignment", 1},
      {"default_int_size", 4},  {"default_uint_alignment", 4},
      {"i8_size", 1},           {"i16_alignment", 2},
      {"i32_size", 4},          {"i64_alignment", 8},
      {"u8_size", 1},           {"u16_alignment", 2},
      {"u32_size", 4},          {"u64_alignment", 8},
      {"float_size", 4},        {"double_size", 8},
      {"double_alignment", 8},  {"pointer_alignment", 8},
      {"void_pointer_size", 8}, {"alias_size", 2},
      {"enum_size", 1},         {"union_size", 4},
      {"union_alignment", 4},   {"matrix_size", 24},
      {"matrix_alignment", 2},  {"indirect_array_size", 8}};
  for (const auto &[name, value] : expected) {
    expect(queryValue(result, name) == value,
           "semantic model should retain the uint64_t layout constant for '" +
               name + "'");
  }

  const lang::VariableDecl *values =
      findVariable(result.program, "main", "values");
  const lang::CompileTimeValue *extent =
      values == nullptr || values->type().arrayExtents.empty()
          ? nullptr
          : result.semantics.findArrayExtent(
                *values->type().arrayExtents.front());
  expect(extent != nullptr && extent->value == 4,
         "a constexpr initialized by sizeof should remain usable as a later "
         "fixed-array extent");
}

void testPointerLayoutDoesNotRequirePointeeLayout() {
  const lang::FrontendResult result = analyze("layout-query-pointees.gti", R"(
class Opaque {};

uint64_t pointer_layout<T>() { return sizeof(T*); }

int main() {
  uint64_t class_pointer_size = sizeof(Opaque*);
  return int32_t(pointer_layout<Opaque>() - class_pointer_size);
}
)");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode() &&
             queryValue(result, "class_pointer_size") == 8,
         "one-level pointer layout should be known without laying out its "
         "ordinary or generic pointee");
}

void testTargetDeterminism() {
  const std::array<std::string_view, 6> triples = {
      "aarch64-apple-darwin",      "x86_64-apple-darwin",
      "aarch64-unknown-linux-gnu", "x86_64-unknown-linux-gnu",
      "aarch64-pc-windows-msvc",   "x86_64-pc-windows-msvc"};
  std::vector<lang::TargetInfo> targets;
  for (const std::string_view triple : triples) {
    if (std::optional<lang::TargetInfo> target =
            lang::parseTargetTriple(triple)) {
      targets.push_back(std::move(*target));
    }
  }
  expect(targets.size() == triples.size(),
         "the supported synthetic target matrix should be available");
  if (targets.size() != triples.size()) {
    return;
  }

  const std::vector<std::string> names = {
      "bool_size",         "char_alignment",
      "default_int_size",  "default_uint_alignment",
      "i8_size",           "i16_alignment",
      "i32_size",          "i64_alignment",
      "u8_size",           "u16_alignment",
      "u32_size",          "u64_alignment",
      "float_size",        "double_size",
      "double_alignment",  "pointer_alignment",
      "void_pointer_size", "alias_size",
      "enum_size",         "union_size",
      "union_alignment",   "matrix_size",
      "matrix_alignment",  "indirect_array_size"};
  std::vector<std::unordered_map<std::string, std::uint64_t>> selections;
  bool allValid = true;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const lang::FrontendResult result =
        analyze("layout-query-target-" + std::to_string(index) + ".gti",
                validLayoutSource(), targets[index]);
    allValid = allValid && result.canGenerateCode();
    selections.push_back(queryValues(result, names));
  }
  expect(allValid,
         "layout queries should compile for every accepted synthetic target");
  expect(std::all_of(selections.begin(), selections.end(),
                     [&](const auto &selection) {
                       return selection == selections.front() &&
                              selection.size() == names.size();
                     }),
         "layout constants should be deterministic across all six supported "
         "OS and architecture selections");
}

void expectLayoutFailure(std::string_view name, std::string source,
                         std::string_view messageFragment,
                         std::string_view primarySpelling) {
  const std::size_t primaryStart = source.rfind(primarySpelling);
  const lang::FrontendResult result = analyze(name, std::move(source));
  const lang::Diagnostic *diagnostic = findCode(result, "GTI-S2063");
  const bool focused =
      !result.canGenerateCode() && countCode(result, "GTI-S2063") == 1 &&
      diagnostic != nullptr &&
      diagnostic->message.find(messageFragment) != std::string::npos &&
      primaryStart != std::string::npos &&
      diagnostic->primary.start == primaryStart &&
      diagnostic->primary.end == primaryStart + primarySpelling.size() &&
      !diagnostic->hints.empty() && diagnostic->fixes.empty();
  if (!focused) {
    printDiagnostics(result);
  }
  expect(focused,
         "invalid layout query '" + std::string(name) +
             "' should produce one focused GTI-S2063 without a fix-it");
}

void testLayoutDiagnostics() {
  expectLayoutFailure("layout-query-class.gti", R"(
class Box {};
uint64_t query() { return sizeof(Box); }
)",
                      "no GTI layout contract", "Box");
  expectLayoutFailure("layout-query-reference.gti", R"(
uint64_t query(int32_t& value) { return alignof(int32_t&); }
)",
                      "no GTI layout contract", "&");
  expectLayoutFailure("layout-query-void.gti", R"(
uint64_t query() { return sizeof(void); }
)",
                      "no GTI layout contract", "void");
  expectLayoutFailure("layout-query-payload-enum.gti", R"(
enum class State { Ready, Value(uint8_t value) };
uint64_t query() { return sizeof(State); }
)",
                      "no GTI layout contract", "State");
  expectLayoutFailure("layout-query-nullptr-type.gti", R"(
uint64_t query() { return alignof(nullptr_t); }
)",
                      "no GTI layout contract", "nullptr_t");
  expectLayoutFailure("layout-query-expected.gti", R"(
uint64_t query() { return sizeof(expected<int32_t, int32_t>); }
)",
                      "no GTI layout contract", "expected");
  expectLayoutFailure("layout-query-zero.gti", R"(
uint64_t query() { return sizeof(int32_t[0]); }
)",
                      "zero-length fixed array", "0");
  expectLayoutFailure("layout-query-symbolic-type.gti", R"(
uint64_t query<T>() { return sizeof(T); }
)",
                      "requires a concrete type", "T");
  expectLayoutFailure("layout-query-symbolic-extent.gti", R"(
class ArrayShape<uint64_t N> {
public:
  uint64_t query() { return alignof(int32_t[N]); }
};
)",
                      "concrete fixed-array extent", "N");
  expectLayoutFailure("layout-query-overflow.gti", R"(
uint64_t query() {
  return sizeof(int16_t[18446744073709551615]);
}
)",
                      "exceeds uint64_t", "18446744073709551615");

  const lang::FrontendResult unknown = analyze("layout-query-unknown.gti", R"(
uint64_t query() { return sizeof(MissingType); }
)");
  expect(!unknown.canGenerateCode() && !unknown.diagnostics.empty() &&
             countCode(unknown, "GTI-S2063") == 0,
         "ordinary unknown-type diagnostics should suppress the dependent "
         "layout-contract diagnostic");
}

void testHirMirAndBackend() {
  const lang::FrontendResult result =
      analyze("layout-query-lowering.gti", validLayoutSource());
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  const lang::HirFunctionInstance *main = findHirFunction(result, "main");
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : result.mir.findFunctionInstance(main->id);
  expect(result.canGenerateCode() && main != nullptr && mirMain != nullptr,
         "valid layout queries should have matching HIR and MIR function "
         "instances");
  if (!result.canGenerateCode() || main == nullptr || mirMain == nullptr) {
    return;
  }

  std::unordered_set<lang::HirValueId> layoutValues;
  for (const lang::HirValue &value : main->body.values) {
    if (value.kind != lang::HirValueKind::LayoutQuery) {
      continue;
    }
    layoutValues.insert(value.id);
    const auto *integer =
        value.constant == std::nullopt
            ? nullptr
            : std::get_if<lang::ConstantInteger>(&*value.constant);
    const auto *literal = value.literal == std::nullopt
                              ? nullptr
                              : std::get_if<std::uint64_t>(&*value.literal);
    expect(
        value.info.type == lang::SemanticType::UInt64 && integer != nullptr &&
            !integer->negative &&
            integer->domain ==
                lang::CheckedIntegerDomain{.width = 64, .signedValue = false} &&
            literal != nullptr && *literal == integer->magnitude,
        "HIR should preserve layout-query provenance and its exact uint64 "
        "literal");
  }
  expect(layoutValues.size() == 24,
         "HIR should retain every local source layout query");

  std::unordered_set<lang::HirValueId> literalOperations;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (layoutValues.contains(instruction.hirValue) &&
          instruction.operation == lang::MirOperation::Literal &&
          instruction.literal.has_value()) {
        literalOperations.insert(instruction.hirValue);
      }
    }
  }
  expect(literalOperations == layoutValues &&
             lang::verifyMirProgram(result.mir).valid(),
         "MIR should lower every layout query to a verified literal "
         "operation");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  expect(std::all_of(layoutValues.begin(), layoutValues.end(),
                     [&](lang::HirValueId id) {
                       return optimizations.replacement(id) != nullptr;
                     }),
         "constant folding should consume retained layout-query constants");

  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = result.program,
                                   .semantics = result.semantics,
                                   .hir = result.hir,
                                   .mir = result.mir,
                                   .optimizations = optimizations});
  const auto emittedInitializer = [&](std::string_view name,
                                      std::uint64_t expected) {
    const std::size_t binding =
        artifact.contents.find(std::string(name) + " =");
    const std::size_t equal = artifact.contents.find('=', binding);
    const std::size_t semicolon = artifact.contents.find(';', equal);
    if (binding == std::string::npos || equal == std::string::npos ||
        semicolon == std::string::npos) {
      return false;
    }
    const std::string_view initializer(artifact.contents.data() + equal,
                                       semicolon - equal);
    return initializer.find("sizeof") == std::string_view::npos &&
           initializer.find("alignof") == std::string_view::npos &&
           initializer.find(std::to_string(expected)) != std::string_view::npos;
  };
  expect(emittedInitializer("bool_size", 1) &&
             emittedInitializer("matrix_size", 24) &&
             emittedInitializer("pointer_alignment", 8),
         "the backend should emit frontend-computed numeric constants, not "
         "native C++ layout queries for source expressions");

  expect(!std::is_default_constructible_v<lang::CppEmitter>,
         "backend emission should require semantic and HIR facts at its API "
         "boundary");
}

void testConstexprControlFlow() {
  const lang::FrontendResult result = analyze("layout-query-control.gti", R"(
constexpr uint64_t word_bytes = sizeof(uint32_t);

int main() {
  if constexpr (word_bytes == 4) {
    switch (alignof(uint32_t)) {
    case uint64_t(4):
      return 0;
    default:
      return 1;
    }
  } else {
    return 2;
  }
}
)");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode(),
         "layout constants should participate in constexpr branching and "
         "switch-case constant matching");
}

void testFormatting() {
  const std::string source =
      "uint64_t query(){return sizeof ( int32_t )+alignof(uint64_t);}";
  const std::string formatted = lang::Formatter().format(source);
  expect(formatted.find("sizeof(int32_t) + alignof(uint64_t)") !=
                 std::string::npos &&
             formatted.find("sizeof (") == std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should treat layout queries as word operators and be "
         "idempotent");

  lang::FormatOptions always;
  always.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always;
  const std::string alwaysFormatted = lang::Formatter(always).format(source);
  expect(alwaysFormatted.find("sizeof(int32_t)") != std::string::npos &&
             alwaysFormatted.find("alignof(uint64_t)") != std::string::npos,
         "SpaceBeforeParens=Always should not separate a layout operator from "
         "its required type operand");
}

} // namespace

int main() {
  testParserAndAst();
  testSemanticLayoutConstants();
  testPointerLayoutDoesNotRequirePointeeLayout();
  testTargetDeterminism();
  testLayoutDiagnostics();
  testHirMirAndBackend();
  testConstexprControlFlow();
  testFormatting();
  if (failures != 0) {
    std::cerr << failures << " layout-query test(s) failed\n";
    return 1;
  }
  return 0;
}
