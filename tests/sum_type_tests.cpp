#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/optimizer.h"
#include "gti/parser.h"

#include <algorithm>
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

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::string(name), std::move(source));
}

const lang::ClassDecl *findClass(const lang::Program &program,
                                 std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *candidate =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (candidate != nullptr && candidate->name().lexeme == name) {
      return candidate;
    }
  }
  return nullptr;
}

const lang::EnumDecl *findEnum(const lang::Program &program,
                               std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *candidate =
        dynamic_cast<const lang::EnumDecl *>(declaration.get());
    if (candidate != nullptr && candidate->name().lexeme == name) {
      return candidate;
    }
  }
  return nullptr;
}

std::string validSource() {
  return R"(
enum class BitKind : uint8_t { integer, real };

union Bits {
  mut uint32_t integer;
  mut float real;
  mut BitKind kind;
};

enum class Message {
  quit,
  move(int32_t x, int32_t y),
  code(int32_t value),
};

int32_t inspect(Message message) {
  switch (message) {
  case Message::quit:
    return 1;
  case Message::move(x, y):
    return x + y;
  case Message::code(value):
    return value;
  }
}

int main() {
  mut Bits bits{uint32_t(0)};
  unsafe {
    bits.real = 42.0;
  }
  Message message = Message::move(20, 22);
  return inspect(message) - 42;
}
)";
}

void testParserSurface() {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(R"(
union Bits { uint32_t integer; float real; };
enum class Event { idle, point(int32_t x, int32_t y), };
)");
  expect(!tokens.empty() && tokens.front().kind == lang::TokenKind::UNION &&
             lang::isKeywordToken(tokens.front().kind),
         "union should be a first-class declaration keyword");

  lang::Parser parser(tokens);
  const lang::Program program = parser.parse();
  const lang::ClassDecl *bits = findClass(program, "Bits");
  const lang::EnumDecl *event = findEnum(program, "Event");
  expect(!parser.hadError() && bits != nullptr &&
             bits->kind() == lang::ClassKind::Union &&
             bits->members().size() == 2,
         "native union syntax should produce a union class declaration");
  expect(event != nullptr && event->enumerators().size() == 2 &&
             event->enumerators()[0].payload.empty() &&
             event->enumerators()[1].payload.size() == 2 &&
             event->enumerators()[1].payload[0].name.lexeme == "x" &&
             event->enumerators()[1].payload[1].name.lexeme == "y",
         "payload enum alternatives should retain named typed fields");
}

void testSemanticAndIrModel() {
  const lang::FrontendResult result = analyze("sum-types.gti", validSource());
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode() && result.diagnostics.empty(),
         "valid unions and payload enums should complete the frontend");

  const lang::ClassDecl *bits = findClass(result.program, "Bits");
  const lang::EnumDecl *message = findEnum(result.program, "Message");
  const lang::ClassTypeInfo *bitsInfo =
      bits == nullptr ? nullptr : result.semantics.findClassType(*bits);
  const lang::EnumTypeInfo *messageInfo =
      message == nullptr ? nullptr : result.semantics.findEnumType(*message);
  expect(bitsInfo != nullptr && bitsInfo->kind == lang::ClassKind::Union &&
             bitsInfo->unionLayout.has_value() &&
             bitsInfo->unionLayout->sizeBytes == 4 &&
             bitsInfo->unionLayout->abiAlignmentBytes == 4 &&
             bitsInfo->unionLayout->fields.size() == 3,
         "semantic union types should retain target-checked max-field layout "
         "including internal integral-enum facts");
  expect(messageInfo != nullptr && messageInfo->payload &&
             messageInfo->enumerators.size() == 3 &&
             messageInfo->enumerators[1].variantIndex == 1 &&
             messageInfo->enumerators[1].payloadTypes.size() == 2 &&
             messageInfo->enumerators[1].payloadTypes[0] ==
                 lang::SemanticType::Int32,
         "semantic payload enums should retain ordered variant metadata");

  bool hirConstruction = false;
  bool hirExtraction = false;
  for (const lang::HirFunctionInstance &function :
       result.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      hirConstruction = hirConstruction ||
                        value.kind == lang::HirValueKind::PayloadConstruction;
      hirExtraction =
          hirExtraction || value.kind == lang::HirValueKind::PayloadExtraction;
    }
  }
  expect(hirConstruction && hirExtraction,
         "HIR should distinguish payload construction from case extraction");

  bool mirConstruction = false;
  bool mirExtraction = false;
  for (const lang::MirFunctionInstance &function :
       result.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        mirConstruction =
            mirConstruction ||
            instruction.operation == lang::MirOperation::PayloadConstruct;
        mirExtraction = mirExtraction || instruction.operation ==
                                             lang::MirOperation::PayloadExtract;
      }
    }
  }
  expect(mirConstruction && mirExtraction,
         "MIR should preserve payload construction and extraction effects");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = result.program,
                                   .semantics = result.semantics,
                                   .hir = result.hir,
                                   .mir = result.mir,
                                   .sourceMir = &result.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("union Bits {") != std::string::npos &&
          artifact.contents.find("std::variant<") != std::string::npos &&
          artifact.contents.find(".__gti_value.index()") != std::string::npos &&
          artifact.contents.find("std::get<1>(") != std::string::npos &&
          artifact.contents.find("std::is_union_v<Bits>") != std::string::npos,
      "the C++ backend should use native unions and tagged variant storage");
}

void testUnionSafetyBoundary() {
  const lang::FrontendResult safeAccess = analyze("safe-union-access.gti", R"(
union Bits { uint32_t integer; float real; };
int main() {
  Bits bits{uint32_t(7)};
  return int32_t(bits.integer);
}
)");
  expect(hasCode(safeAccess, "GTI-S2055") &&
             hasMessage(safeAccess, "Union member access"),
         "union member reads should require an unsafe block");

  const lang::FrontendResult generic =
      analyze("generic-union.gti",
              "union Slot<T> { T value; }; int main() { return 0; }");
  expect(hasCode(generic, "GTI-S2066") &&
             hasMessage(generic, "cannot declare generic parameters"),
         "the passive native-union baseline should reject generic storage");

  const lang::FrontendResult behavior = analyze("behavioral-union.gti", R"(
union Bad {
  int value;
  int read() { unsafe { return this.value; } }
};
int main() { return 0; }
)");
  expect(hasCode(behavior, "GTI-S2066") &&
             hasMessage(behavior, "cannot declare methods"),
         "native unions should not acquire class behavior or hidden lifecycle");

  const lang::FrontendResult initializedField =
      analyze("initialized-union-field.gti",
              "union Bad { int value = 1; }; int main() { return 0; }");
  expect(hasCode(initializedField, "GTI-S2066") &&
             hasMessage(initializedField, "cannot have an initializer"),
         "union declarations should not hide an active-field choice");
}

void testPayloadEnumDiagnostics() {
  const lang::FrontendResult backing = analyze("payload-backing.gti", R"(
enum class Result : uint8_t { ok(int32_t value), error };
int main() { return 0; }
)");
  expect(hasCode(backing, "GTI-S2067") &&
             hasMessage(backing, "cannot declare an integral backing type"),
         "payload enums should own their tag representation");

  const lang::FrontendResult construction =
      analyze("payload-construction.gti", R"(
enum class Result { ok(int32_t value), error };
int main() {
  Result first = Result::ok();
  Result second = Result::ok("wrong");
  return 0;
}
)");
  expect(hasCode(construction, "GTI-S2067") &&
             hasMessage(construction, "expects 1 argument") &&
             hasMessage(construction, "requires exact type 'int32_t'"),
         "payload construction should enforce exact arity and types");

  const lang::FrontendResult nonExhaustive =
      analyze("payload-exhaustiveness.gti", R"(
enum class Result { ok(int32_t value), error };
int main() {
  Result result = Result::ok(1);
  switch (result) {
  case Result::ok(value):
    return value;
  }
}
)");
  expect(hasCode(nonExhaustive, "GTI-S2067") &&
             hasMessage(nonExhaustive, "not exhaustive") &&
             hasMessage(nonExhaustive, "Result::error"),
         "payload-enum switches should require every variant or default");

  const lang::FrontendResult badPattern = analyze("payload-pattern.gti", R"(
enum class Pair { values(int32_t left, int32_t right) };
int main() {
  Pair pair = Pair::values(1, 2);
  switch (pair) {
  case Pair::values(value, value):
    return value;
  }
}
)");
  expect(hasCode(badPattern, "GTI-S2067") &&
             hasMessage(badPattern, "Duplicate payload binding"),
         "payload case bindings should be unique within an alternative");
}

void testOrdinaryEnumsAndFormatting() {
  const lang::FrontendResult ordinary = analyze("ordinary-enum.gti", R"(
enum class Stage : uint8_t { ready, running = 4 };
int main() {
  Stage stage = Stage::ready;
  switch (stage) {
  case Stage::ready:
    return 0;
  case Stage::running:
    return 1;
  }
}
)");
  expect(ordinary.canGenerateCode() && ordinary.diagnostics.empty(),
         "payload support must not change integral scoped enums");

  const std::string formatted = lang::Formatter().format(
      "union Bits{mut uint32_t integer;float real;};"
      "enum class Event{idle,point(int32_t x,int32_t y),};");
  const bool validFormatting =
      formatted.find("union Bits {") != std::string::npos &&
      formatted.find("point(int32_t x, int32_t y),") != std::string::npos;
  if (!validFormatting) {
    std::cerr << "Formatted sum types:\n" << formatted;
  }
  expect(validFormatting,
         "the formatter should preserve C++-familiar union and payload syntax");
}

} // namespace

int main() {
  testParserSurface();
  testSemanticAndIrModel();
  testUnionSafetyBoundary();
  testPayloadEnumDiagnostics();
  testOrdinaryEnumsAndFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All sum-type tests passed\n";
  return 0;
}
