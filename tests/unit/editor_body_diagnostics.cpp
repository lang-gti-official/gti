// An isolated edited body produces the compiler's diagnostics and can decline a
// query.
#include "gti/frontend.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(editor_body_diagnostics) {
  const std::string prefix =
      "namespace board { struct Cell { int value = 3; int read(int n) { ";
  const std::string suffix =
      "return this.value + n; } }; }\nint main() { return 0; }\n";
  const auto check = [](const std::string &source, bool retain = false) {
    lang::FrontendOptions options;
    options.retainBodyQueryContext = retain;
    return lang::Frontend(options).analyze("editor-body.gti", source);
  };
  const auto original =
      std::make_shared<lang::FrontendResult>(check(prefix + suffix, true));
  const auto query = lang::EditorBodyDiagnostics::takeFrom(original);
  if (!query) {
    std::cerr << "Expected a verified baseline for the class method.\n";
    return 1;
  }
  for (const std::string initializer :
       {"int sample = true; ", "int sample = 42; ", ""}) {
    const auto source = prefix + initializer + suffix;
    const auto answer = query->check(source);
    const auto full = check(source);
    if (!answer || *answer != full.diagnostics ||
        (initializer == "int sample = true; " && answer->empty())) {
      std::cerr << "Body diagnostics differ from the complete frontend for "
                << initializer << '\n';
      return 1;
    }
  }
  std::atomic<bool> cancelled{true};
  if (query->check(prefix + "int sample = 1; " + suffix, &cancelled) ||
      query->check(prefix + "int sample = ; " + suffix) ||
      query->check(prefix + "int sample = 1; " + suffix +
                   "int extra() { return 2; }\n")) {
    std::cerr << "A cancelled, malformed or multi-declaration edit requires "
                 "the full frontend.\n";
    return 1;
  }
  return 0;
}
