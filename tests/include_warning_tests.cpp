#include "gti/frontend.h"
#include "gti/source_loader.h"

#include <cstdlib>
#include <iostream>
#include <map>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "include-warning-fixture";
  const auto entry = root / "main.gti";
  const std::string source = "// #include in a comment\n/* 😀 */ #include "
                             "\"a.gti\"\n#include \"b.gti\"\nint main() {}\n";
  std::unordered_map<std::string, std::string> overlays{
      {(root / "a.gti").string(), "#include \"b.gti\"\n#include \"b.gti\"\n"},
      {(root / "b.gti").string(), ""}};
  auto cache = std::make_shared<lang::FrontendCache>();
  lang::FrontendOptions options;
  options.cache = cache;
  options.warnLegacyIncludes = true;
  options.stopAfter = lang::FrontendPhase::Semantics;
  for (int pass = 0; pass < 2; ++pass) {
    const auto result =
        lang::Frontend(options).analyze(entry, source, {}, overlays);
    expect(result.semanticValid,
           "warning must not invalidate source or semantics");
    std::map<std::string, int> counts;
    for (const auto &diagnostic : result.diagnostics) {
      if (diagnostic.code != "GTI-I0013")
        continue;
      ++counts[diagnostic.primary.source];
      expect(diagnostic.severity == lang::DiagnosticSeverity::Warning &&
                 diagnostic.phase == lang::DiagnosticPhase::SourceLoading &&
                 diagnostic.group == "legacy-include" &&
                 diagnostic.fixes.empty() && diagnostic.hints.size() == 1 &&
                 diagnostic.message.find("Prefer module imports") !=
                     std::string::npos,
             "warning retains its shared identity, advice and no speculative "
             "edit");
      const auto *text = result.sources.find(diagnostic.primary.source);
      const auto start = diagnostic.primary.source == entry.string()
                             ? source.find("#include \"a.gti\"")
                             : 0;
      expect(text && diagnostic.primary.start == start &&
                 diagnostic.primary.end == start + 8,
             "warning highlights the first real include keyword in each file");
      expect(lang::WarningPolicy({"legacy-include"}).suppresses(diagnostic),
             "warning group can suppress the migration advice");
    }
    expect(
        counts.size() == 2 && counts[entry.string()] == 1 &&
            counts[(root / "a.gti").string()] == 1,
        "fresh and cached source loads warn once per file, not once per graph");
  }
  options.warnLegacyIncludes = false;
  expect(lang::Frontend(options)
             .analyze(entry, source, {}, overlays)
             .diagnostics.empty(),
         "ordinary compilation does not opt into editor migration advice");
  options.warnLegacyIncludes = true;
  expect(lang::Frontend(options)
             .analyze(entry, "int main() {}\n", {}, overlays)
             .diagnostics.empty(),
         "removing includes clears cached warnings");
  lang::SourceLoader loader(nullptr, cache, nullptr, nullptr, {}, true);
  const std::string modules = "import \"one.gti\";\nimport \"two.gti\";\n";
  overlays[(root / "one.gti").string()] =
      "export module one;\nexport #include \"a.gti\"\n";
  overlays[(root / "two.gti").string()] =
      "export module two;\nexport #include \"a.gti\"\n";
  for (int pass = 0; pass < 2; ++pass) {
    (void)loader.load(entry, modules, {}, overlays);
    expect(!loader.hadError(), "module configuration replays remain valid");
    int count = 0;
    for (const auto &diagnostic : loader.errors())
      count += diagnostic.code == "GTI-I0013";
    expect(count == 3,
           "shared include replay emits one warning per canonical file");
  }
  std::cout << "Include migration warning checks passed\n";
}
