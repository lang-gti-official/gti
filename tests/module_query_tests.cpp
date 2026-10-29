#include "gti/language_queries.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
bool contains(const lang::CompletionResult &result, const std::string &name) {
  return std::any_of(
      result.candidates.begin(), result.candidates.end(),
      [&](const auto &candidate) { return candidate.label == name; });
}
} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "module-query-fixture";
  const auto app = root / "app";
  const auto library = root / "library";
  const auto entry = app / "main.gti";
  const auto api = library / "api.gti";
  const auto facade = library / "facade.gti";
  const auto local = app / "geometry.gti";
  const auto hidden = library / "private.gti";
  const std::string apiSource =
      "/* 😀 */ export module net /* name trivia */ . http;\n"
      "export int answer() { return 42; }\n";
  const std::string source =
      "/* 😀 */ import dep.net.http;\nimport dep;\nint main() { return 0; }\n";
  std::unordered_map<std::string, std::string> overlays{
      {api.string(), apiSource},
      {facade.string(), "export module main;\nexport import \"api.gti\";\n"},
      {local.string(), "export module graphics.geometry;\n"},
      {hidden.string(), "export module secret;\n"}};
  std::vector<lang::PackageSourceRoot> owners{
      {.identity = "app",
       .name = "app",
       .packageRoot = app,
       .sourceRoot = app,
       .dependencies = {{"dep", "library"}},
       .modules = {{"graphics.geometry", local}}},
      {.identity = "library",
       .name = "library",
       .packageRoot = library,
       .sourceRoot = library,
       .modules = {{"net.http", api}, {"main", facade}}}};
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Parsing;
  options.cache = std::make_shared<lang::FrontendCache>();
  options.suggestIncludes = false;
  const auto snapshot =
      lang::Frontend(options).analyze(entry, source, {}, overlays, {}, owners);
  expect(snapshot.sourceValid && snapshot.syntaxValid,
         "module query fixture must load");
  const auto unit = snapshot.sourceGraph.entryUnit();
  const auto offset = source.find("dep.net");
  const auto definition =
      lang::LanguageQueries().definition(snapshot, unit, offset);
  expect(definition && definition->target.source == api.string() &&
             definition->origin.start == offset &&
             definition->target.start == apiSource.find("net") &&
             definition->target.end == apiSource.find("http") + 4,
         "import definition uses the full exact module name span, including "
         "trivia");
  const auto hover = lang::LanguageQueries().hover(snapshot, unit, offset);
  expect(hover && hover->signature == "export module net.http;",
         "import hover reports the canonical hierarchical module name");
  const auto references =
      lang::LanguageQueries().references(snapshot, unit, offset, true);
  expect(references && references->sites.size() == 3,
         "module references include the declaration, direct import and exact "
         "re-export");

  lang::CompletionInput input;
  input.entryPath = entry;
  input.sourceOverrides = overlays;
  input.packageSourceRoots = owners;
  input.cache = options.cache;
  input.source = "/* 😀 */ import dep.ne";
  input.byteOffset = input.source.size();
  auto completion = lang::LanguageQueries().complete(input);
  expect(contains(completion, "dep.net.http") &&
             !contains(completion, "dep.secret"),
         "incomplete imports complete only admissible registered dependency "
         "modules");
  input.source = "import graphics.ge";
  input.byteOffset = input.source.size();
  completion = lang::LanguageQueries().complete(input);
  expect(contains(completion, "graphics.geometry"),
         "local dotted modules complete");
  input.source = "import dep /* trivia */ . ne";
  input.byteOffset = input.source.size();
  completion = lang::LanguageQueries().complete(input);
  expect(contains(completion, "dep.net.http"),
         "import trivia does not change completion prefix");
  for (const std::string source :
       {"import ", "import dep.", "export import dep."}) {
    input.source = source;
    input.byteOffset = source.size();
    completion = lang::LanguageQueries().complete(input);
    expect(contains(completion, "dep.net.http"),
           "empty and dotted import prefixes remain completable");
  }

  const auto alternate = library / "alternate.gti";
  overlays[alternate.string()] = "export module net.http;\n";
  owners[1].modules[0].second = alternate;
  const auto remapped = lang::Frontend(options).analyze(
      entry, "import dep.net.http;\n", {}, overlays, {}, owners);
  const auto remappedDefinition = lang::LanguageQueries().definition(
      remapped, remapped.sourceGraph.entryUnit(), 8);
  expect(
      remapped.sourceValid && remappedDefinition &&
          remappedDefinition->target.source == alternate.string(),
      "a module map edit changes navigation despite a shared frontend cache");
  overlays[alternate.string()] = "export module wrong;\n";
  const auto invalid = lang::Frontend(options).analyze(
      entry, "import dep.net.http;\n", {}, overlays, {}, owners);
  expect(
      !invalid.sourceValid && !lang::LanguageQueries().definition(
                                  invalid, invalid.sourceGraph.entryUnit(), 8),
      "an unresolved or mismatched import never invents a definition target");
  std::cout << "Module query checks passed\n";
}
