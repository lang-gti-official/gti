#include "gti/ast_printer.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/rewrite.h"
#include "gti/support.h"
#include "gti/target.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

static_assert(std::is_base_of_v<lang::ExprVisitor, lang::AstPrinter>);

int main() {
  lang::installCrashHandlers("gti_compiler_library_smoke");
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(
      "double precise = 0.1d; int main() { return 0; }", "library-smoke.gti");
  const lang::BinaryFloat *precise = nullptr;
  for (const lang::Token &token : tokens) {
    if (token.kind == lang::TokenKind::FLOAT_LITERAL) {
      precise = std::get_if<lang::BinaryFloat>(&token.literal);
      break;
    }
  }
  if (lexer.hadError() || tokens.empty() ||
      tokens.front().kind != lang::TokenKind::DOUBLE || precise == nullptr ||
      precise->format != lang::BinaryFloatFormat::Binary64 ||
      precise->bits != 0x3fb999999999999aULL ||
      tokens.back().kind != lang::TokenKind::END_OF_FILE) {
    return 1;
  }

  (void)lexer.scan("@runtime(\"stdout\")", "library-diagnostic.gti");
  if (lexer.hadError()) {
    return 2;
  }

  (void)lexer.scan("`", "library-diagnostic.gti");
  if (!lexer.hadError() || lexer.errors().size() != 1 ||
      lexer.errors().front().code != std::string_view("GTI-L0002")) {
    return 3;
  }

  const std::string mir = lang::MirPrinter().print(lang::MirBody{});
  const lang::MirBodyAddress moduleAddress{};
  const lang::MirEffectTraits division =
      lang::effects(lang::MirOperation::Divide);
  if (mir.find("mir-body-v28") == std::string::npos || !division.mayTrap ||
      moduleAddress.kind != lang::MirBodyKind::Module ||
      lang::name(lang::IntrinsicKind::StorageDestroy) != "storage-destroy") {
    return 4;
  }

  const lang::TargetInfo host = lang::TargetInfo::host();
  const auto matchesNative = [&host](lang::TargetScalarKind kind,
                                     std::size_t size, std::size_t alignment) {
    const std::optional<lang::TargetTypeLayout> layout =
        host.dataLayout.scalarLayout(kind);
    return layout && layout->sizeBytes == size &&
           layout->abiAlignmentBytes == alignment &&
           layout->preferredAlignmentBytes == alignment;
  };
  if (!host.dataLayout.supported() ||
      host.dataLayout.pointerWidthBits() != sizeof(void *) * 8 ||
      !matchesNative(lang::TargetScalarKind::Bool, sizeof(bool),
                     alignof(bool)) ||
      !matchesNative(lang::TargetScalarKind::Char, sizeof(std::uint8_t),
                     alignof(std::uint8_t)) ||
      !matchesNative(lang::TargetScalarKind::Int8, sizeof(std::int8_t),
                     alignof(std::int8_t)) ||
      !matchesNative(lang::TargetScalarKind::Int16, sizeof(std::int16_t),
                     alignof(std::int16_t)) ||
      !matchesNative(lang::TargetScalarKind::Int32, sizeof(std::int32_t),
                     alignof(std::int32_t)) ||
      !matchesNative(lang::TargetScalarKind::Int64, sizeof(std::int64_t),
                     alignof(std::int64_t)) ||
      !matchesNative(lang::TargetScalarKind::UInt8, sizeof(std::uint8_t),
                     alignof(std::uint8_t)) ||
      !matchesNative(lang::TargetScalarKind::UInt16, sizeof(std::uint16_t),
                     alignof(std::uint16_t)) ||
      !matchesNative(lang::TargetScalarKind::UInt32, sizeof(std::uint32_t),
                     alignof(std::uint32_t)) ||
      !matchesNative(lang::TargetScalarKind::UInt64, sizeof(std::uint64_t),
                     alignof(std::uint64_t)) ||
      !matchesNative(lang::TargetScalarKind::Float32, sizeof(float),
                     alignof(float)) ||
      !matchesNative(lang::TargetScalarKind::Float64, sizeof(double),
                     alignof(double)) ||
      !matchesNative(lang::TargetScalarKind::Pointer, sizeof(void *),
                     alignof(void *))) {
    return 5;
  }

  lang::LiteralExpr literal(lang::Literal{std::uint64_t{1}});
  lang::SemanticModel semantics;
  const lang::SemanticOccurrence occurrence{.name = "value"};
  if (lang::AstPrinter().print(literal) != "1" ||
      lang::SignaturePrinter(semantics).binding(occurrence) !=
          "unknown value") {
    return 6;
  }

  return 0;
}
