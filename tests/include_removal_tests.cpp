#include "gti/frontend.h"
#include "gti/source_loader.h"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace
int main() {
  const auto root = std::filesystem::current_path() / "module-only-fixture";
  const auto entry = root / "main.gti";
  const auto dependency = root / "api.gti";
  const std::unordered_map<std::string, std::string> overlays{
      {dependency.string(), "export module api;\nexport int answer() { return "
                            "42; }\nint hidden() { return 1; }\n"}};
  for (const std::string &directive :
       {std::string("#include \"api.gti\""),
        std::string("#include <std/vector>"),
        std::string("export #include \"api.gti\"")}) {
    const std::string source = "// #include is only text here\n/* 😀 */ " +
                               directive + "\nint main() {}\n";
    lang::SourceLoader loader;
    const auto graph = loader.load(entry, source, {}, overlays);
    expect(loader.hadError() && graph.sourceUnits().size() == 1,
           "rejected includes must not load legacy dependencies");
    expect(loader.errors().size() == 1, "one removal error per directive");
    const auto &error = loader.errors().front();
    const auto offset = source.find("#include", source.find("/*"));
    expect(error.code == "GTI-I0014" &&
               error.phase == lang::DiagnosticPhase::SourceLoading &&
               error.severity == lang::DiagnosticSeverity::Error &&
               error.primary.source == entry.string() &&
               error.primary.start == offset &&
               error.primary.end == offset + 8 &&
               error.message.find("removed") != std::string::npos &&
               !error.hints.empty() && error.fixes.empty(),
           "removal has an exact keyword span, stable code, advice and no "
           "speculative rewrite");
    expect(!lang::Frontend()
                .analyze(entry, source, {}, overlays)
                .canGenerateCode(),
           "removed syntax never reaches code generation");
  }
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  options.cache = std::make_shared<lang::FrontendCache>();
  options.suggestIncludes = false;
  const std::string source =
      "import \"api.gti\";\nint main() { return answer(); }\n";
  for (int pass = 0; pass < 2; ++pass) {
    const auto result =
        lang::Frontend(options).analyze(entry, source, {}, overlays);
    expect(result.semanticValid && result.diagnostics.empty(),
           "explicit relative module interfaces work in fresh and cached "
           "analysis");
  }
  const auto privateUse = lang::Frontend(options).analyze(
      entry, "import \"api.gti\";\nint main() { return hidden(); }\n", {},
      overlays);
  expect(!privateUse.semanticValid,
         "mechanical migration must not bypass private exports");
  const auto legacy = lang::Frontend(options).analyze(
      entry, source, {},
      {{dependency.string(), "int answer() { return 42; }\n"}});
  expect(!legacy.sourceValid,
         "import must not implicitly export a legacy source file");
  std::cout << "Module-only source loading checks passed\n";
}
