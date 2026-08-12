#include "gti/lexer.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/rewrite.h"
#include "gti/support.h"

#include <string>
#include <string_view>

int main() {
  lang::installCrashHandlers("gti_compiler_library_smoke");
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan("int main() { return 0; }", "library-smoke.gti");
  if (lexer.hadError() || tokens.empty() ||
      tokens.front().kind != lang::TokenKind::INT ||
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
  if (mir.find("mir-body-v2") == std::string::npos || !division.mayTrap ||
      moduleAddress.kind != lang::MirBodyKind::Module ||
      lang::name(lang::IntrinsicKind::StorageDestroy) != "storage-destroy") {
    return 4;
  }

  return 0;
}
