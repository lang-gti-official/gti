#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_printer.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/consuming_range.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  expect(frontend.canGenerateCode(), "consuming ranges have verified MIR");
  return failures ? 1 : 0;
}
