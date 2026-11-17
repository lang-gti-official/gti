#include "gti/driver/project_facts.h"
#include "gti/frontend.h"
#include "gti/semantic_preamble.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Fixture {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("gti-include-suggestions-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  lang::FrontendOptions options;
  std::unordered_map<std::string, std::string> overlays;
  Fixture() {
    std::filesystem::create_directories(root);
    root = std::filesystem::canonical(root);
    options.stopAfter = lang::FrontendPhase::Hir;
    options.includeDiscoveryLimits.timeBudget =
        std::chrono::milliseconds::max();
    options.includeSearchRoots.push_back({root, std::nullopt, {}});
    options.cache = std::make_shared<lang::FrontendCache>();
  }
  ~Fixture() { std::filesystem::remove_all(root); }
  void write(const std::string &name, const std::string &text) {
    std::filesystem::create_directories((root / name).parent_path());
    std::ofstream(root / name) << text;
  }
  auto analyze(const std::string &text,
               const std::vector<lang::PackageSourceRoot> &packages = {}) {
    return lang::Frontend(options).analyze(root / "main.gti", text, {},
                                           overlays, {}, packages);
  }
};

std::vector<lang::FixIt> fixes(const lang::FrontendResult &result) {
  std::vector<lang::FixIt> out;
  for (const auto &diagnostic : result.diagnostics) {
    out.insert(out.end(), diagnostic.fixes.begin(), diagnostic.fixes.end());
  }
  return out;
}

void internalTrialsDoNotReportProgress() {
  struct Counter final : lang::ProgressObserver {
    int loads = 0;
    int parses = 0;
    int checks = 0;
    void onProgress(const lang::ProgressEvent &event) noexcept override {
      if (event.kind != lang::ProgressKind::Begin)
        return;
      if (event.phase == lang::ProgressPhase::Loading)
        ++loads;
      if (event.phase == lang::ProgressPhase::Parsing)
        ++parses;
      if (event.phase == lang::ProgressPhase::Checking)
        ++checks;
    }
  } counter;
  Fixture f;
  f.options.progress = &counter;
  f.write("api.gti", "export module api;\nexport struct ProgressValue { int "
                     "value = 7;  public: "
                     "ProgressValue() = default; };\n");
  const auto result =
      f.analyze("int main() { ProgressValue item{}; return item.value; }\n");
  const auto suggestions = fixes(result);
  expect(result.includeSuggestionsRequested &&
             std::any_of(suggestions.begin(), suggestions.end(),
                         [](const auto &fix) {
                           return fix.replacement.find("import \"api.gti\";") !=
                                  std::string::npos;
                         }),
         "silent internal trials must still validate a usable import fix");
  expect(counter.loads == 1 && counter.parses == 1 && counter.checks == 1,
         "only the original analysis reports loading, parsing and checking");
}

void immutableDiscoveryAndCancellation() {
  Fixture f;
  f.write("api.gti",
          "export module api;\nexport int discovered() { return 7; }\n");
  f.options.suggestIncludes = false;
  const auto base = f.analyze("int main() { return discovered(); }\n");
  expect(lang::canSuggestIncludes(base) && fixes(base).empty(),
         "ordinary diagnostics are complete without speculative discovery");
  auto advice = lang::discoverIncludeSuggestions(
      base, f.options, f.root / "main.gti", {}, f.overlays, {}, {});
  expect(!advice.cancelled &&
             std::any_of(advice.diagnostics.begin(), advice.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return !diagnostic.fixes.empty();
                         }),
         "immutable discovery retains compiler-validated fixes");
  expect(fixes(base).empty() && !base.includeSuggestionsRequested &&
             base.includeSuggestionInputs.empty(),
         "discovery never mutates a snapshot already published to queries");
  std::atomic<bool> cancellation{true};
  f.options.cancellation = &cancellation;
  advice = lang::discoverIncludeSuggestions(
      base, f.options, f.root / "main.gti", {}, f.overlays, {}, {});
  expect(advice.cancelled && advice.inputs.empty(),
         "cancelled discovery stops before scanning any candidate");
  expect(fixes(base).empty() && !base.cancelled,
         "cancelled advice cannot invalidate the original semantic snapshot");
}

void boundedDiscovery() {
  Fixture f;
  f.write("a.gti", "export module a;\nexport int first() { return 1; }\n");
  f.write("b.gti", "export module b;\nexport int second() { return 2; }\n");
  f.options.suggestIncludes = false;
  f.options.includeDiscoveryCache =
      std::make_shared<lang::IncludeDiscoveryCache>();
  const auto base = f.analyze("int main() { return first() + second(); }\n");
  const auto discover = [&] {
    return lang::discoverIncludeSuggestions(
        base, f.options, f.root / "main.gti", {}, f.overlays, {}, {});
  };
  f.options.includeDiscoveryLimits.maximumTrials = 0;
  auto advice = discover();
  expect(advice.budgetExhausted && !advice.cancelled && advice.inputs.empty(),
         "zero trials preserves diagnostics without scanning");
  expect(advice.diagnostics.size() == base.diagnostics.size(),
         "exhaustion does not discard original diagnostics");
  f.options.includeDiscoveryLimits.maximumTrials = 1;
  advice = discover();
  expect(advice.budgetExhausted && !advice.cancelled &&
             f.options.includeDiscoveryCache->stats().trials == 1,
         "a discovery performs no more than its trial allowance");
  expect(std::any_of(advice.diagnostics.begin(), advice.diagnostics.end(),
                     [](const auto &d) {
                       return d.code == "GTI-S2089" && !d.fixes.empty() &&
                              !d.hints.empty();
                     }),
         "a proved fix is finalized even when later work exceeds the budget");
  f.options.includeDiscoveryLimits.maximumTrials = 8;
  f.options.includeDiscoveryLimits.timeBudget = std::chrono::milliseconds{0};
  advice = discover();
  expect(advice.budgetExhausted && !advice.cancelled && advice.inputs.empty() &&
             f.options.includeDiscoveryCache->stats().trials == 1,
         "zero time budget starts no speculative work");
}

void trialsDoNotBuildPreambles() {
  Fixture f;
  f.write("stdlib/prelude.gti", "namespace std {}\n");
  f.write("stdlib/module-map.json", R"({"modules":{"cmath":"std/cmath.gti"}})");
  f.write("stdlib/std/cmath.gti", "export module cmath; namespace std { export "
                                  "int discovered() { return 7; } }\n");
  f.options.suggestIncludes = false;
  f.options.includeSearchRoots.clear();
  f.options.includeDiscoveryCache =
      std::make_shared<lang::IncludeDiscoveryCache>();
  const auto base = lang::Frontend(f.options).analyze(
      f.root / "main.gti", "int main() { return std::discovered(); }\n",
      {f.root / "stdlib/prelude.gti"}, {}, {f.root / "stdlib"});
  f.options.preambleCache = std::make_shared<lang::SemanticPreambleCache>();
  const auto control = lang::Frontend(f.options).analyze(
      f.root / "main.gti",
      "import std.cmath; int main() { return std::discovered(); }\n",
      {f.root / "stdlib/prelude.gti"}, {}, {f.root / "stdlib"});
  expect(control.semanticValid && f.options.preambleCache->stats().entries != 0,
         "the registered fixture builds a preamble in an ordinary analysis");
  f.options.preambleCache->clear();
  const auto advice = lang::discoverIncludeSuggestions(
      base, f.options, f.root / "main.gti", {f.root / "stdlib/prelude.gti"}, {},
      {f.root / "stdlib"}, {});
  expect(std::any_of(advice.diagnostics.begin(), advice.diagnostics.end(),
                     [](const auto &d) { return !d.fixes.empty(); }),
         "a cold preamble is not needed to prove an import");
  expect(f.options.includeDiscoveryCache->stats().trials == 1 &&
             f.options.preambleCache->stats().entries == 0,
         "speculation never populates the caller's preamble cache on a miss");
}

void boundedCandidateScans() {
  Fixture f;
  f.write("stdlib/prelude.gti", "namespace std {}\n");
  f.write("stdlib/module-map.json",
          R"({"modules":{"a":"std/a.gti","b":"std/b.gti"}})");
  f.write("stdlib/std/a.gti", "export module a; namespace std { int noise() { "
                              "Missing value{}; return 0; } }");
  f.write("stdlib/std/b.gti",
          "export module b; export import \"detail/types.gti\";");
  f.write("stdlib/std/detail/types.gti",
          "export module detail.types; namespace std { export struct Missing "
          "{}; }");
  f.options.suggestIncludes = false;
  f.options.includeSearchRoots.clear();
  f.options.includeDiscoveryCache =
      std::make_shared<lang::IncludeDiscoveryCache>();
  const auto base = lang::Frontend(f.options).analyze(
      f.root / "main.gti",
      "using Selected = std::Missing; int main() { return 0; }",
      {f.root / "stdlib/prelude.gti"}, {}, {f.root / "stdlib"});
  const auto discover = [&] {
    return lang::discoverIncludeSuggestions(
        base, f.options, f.root / "main.gti", {f.root / "stdlib/prelude.gti"},
        {}, {f.root / "stdlib"}, {});
  };
  f.options.includeDiscoveryLimits.maximumCandidateScans = 0;
  expect(discover().budgetExhausted &&
             f.options.includeDiscoveryCache->stats().scanMisses == 0,
         "zero scan allowance performs no candidate analysis");
  f.options.includeDiscoveryLimits.maximumCandidateScans = 1;
  const auto limited = discover();
  expect(limited.budgetExhausted &&
             f.options.includeDiscoveryCache->stats().scanMisses == 1 &&
             f.options.includeDiscoveryCache->stats().trials == 0,
         "candidate preparation is capped even before any useful trial");
  f.options.includeDiscoveryLimits.maximumCandidateScans = 2;
  const auto advice = discover();
  expect(
      std::any_of(advice.diagnostics.begin(), advice.diagnostics.end(),
                  [](const auto &d) { return !d.fixes.empty(); }),
      "a narrow re-export remains discoverable after an unrelated reference");
  expect(f.options.includeDiscoveryCache->stats().trials == 1,
         "the re-exported declaration receives semantic confirmation");
}

void lexicalDeclarationPriority() {
  Fixture f;
  f.options.includeDiscoveryLimits.maximumCandidateScans = 1;
  f.write("a_use.gti",
          "export module a_use; int noise() { Named value{}; return 0; }");
  f.write("z_api.gti", "export module z_api; export struct Named {};");
  const auto result =
      f.analyze("using Selected = Named; int main() { return 0; }");
  expect(!fixes(result).empty(),
         "a type declaration ranks before a mere use under a single-scan cap");
}

void namedModulePriority() {
  Fixture f;
  f.options.includeDiscoveryLimits.maximumTrials = 1;
  f.options.includeDiscoveryCache =
      std::make_shared<lang::IncludeDiscoveryCache>();
  f.write("a_unrelated.gti", "export module a_unrelated; namespace other { "
                             "export int chosen() { return 1; } }\n");
  f.write("chosen.gti",
          "export module chosen; export int chosen() { return 2; }\n");
  const auto result = f.analyze("int main() { return chosen(); }\n");
  const auto suggestions = fixes(result);
  expect(std::any_of(suggestions.begin(), suggestions.end(),
                     [](const auto &fix) {
                       return fix.replacement.find("import \"chosen.gti\";") !=
                              std::string::npos;
                     }),
         "a matching module receives the proof budget before an unrelated "
         "namespace");
  expect(f.options.includeDiscoveryCache->stats().trials == 1,
         "prioritizing a module does not bypass semantic confirmation");
}

void projectDeclarations() {
  Fixture f;
  f.write("api.gti", R"(export module api;
export namespace example {
  int test() { return 7; }
  struct Value { int n = 0;  public: Value() = default; };
  using Number = int;
  enum class Mode { ready };
  constexpr int count = 2;
  class Factory { public: static int make() { return 3; }  public: Factory() = default; };
}
)");
  for (const std::string source :
       {"int main() { return example::test(); }",
        "int main() { example::Value value{}; return value.n; }",
        "using Selected = example::Number;",
        "int main() { auto state = example::Mode::ready; return 0; }",
        "int main() { return example::count; }",
        "int main() { return example::Factory::make(); }",
        "namespace ex = example; int main() { return ex::test(); }",
        "namespace example { int use() { return test(); } }"}) {
    auto result = f.analyze(source);
    const auto edits = fixes(result);
    if (edits.empty()) {
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    }
    expect(!edits.empty(), "discover declaration for: " + source);
    if (!edits.empty()) {
      expect(std::all_of(edits.begin(), edits.end(),
                         [](const auto &edit) {
                           return edit.replacement == "import \"api.gti\";\n" &&
                                  edit.preferred;
                         }),
             "each unresolved use offers the same exact defining interface");
      expect(f.analyze(edits[0].replacement + source).hirValid,
             "applying import restores actual frontend validity");
    }
  }
  expect(fixes(f.analyze("int main() { return wrong::test(); }")).empty(),
         "equal leaf names in the wrong namespace must not guess an import");
  f.write("private.gti", "export module private_api;\nexport class Hidden { "
                         "static int inaccessible() { return 1; "
                         "}  public: Hidden() = default; };");
  expect(
      fixes(f.analyze("int main() { return Hidden::inaccessible(); }")).empty(),
      "no import fix bypasses private member access");
  f.write("broken.gti", "export module broken;\nexport namespace example { int "
                        "broken() { return no_such_value; } }");
  expect(fixes(f.analyze("int main() { return example::broken(); }")).empty(),
         "a candidate that introduces semantic errors is not a safe edit");
  f.write("inactive.gti", "export module inactive_api;\n#if target.os == "
                          "\"impossible\"\nexport int inactive() { "
                          "return 1; }\n#endif\n");
  expect(fixes(f.analyze("int main() { return inactive(); }")).empty(),
         "inactive declarations are not candidates");
  f.write("module.gti",
          "export module secret;\nint private_value() { return 1; }\n");
  expect(
      fixes(f.analyze("int main() { return private_value(); }")).empty(),
      "private module declarations must not be exposed through import edits");
  f.write("integer.gti", "export module integer;\nexport namespace overload { "
                         "int choose(int n) { return n; } }");
  f.write("boolean.gti", "export module boolean;\nexport namespace overload { "
                         "int choose(bool n) { if (n) { "
                         "return 1; } return 0; } }");
  const auto selected =
      fixes(f.analyze("int main() { return overload::choose(3); }"));
  expect(selected.size() == 1 &&
             selected.front().replacement == "import \"integer.gti\";\n",
         "candidate checking uses exact call overload resolution, not just a "
         "matching name");
}

void missingOverloadDeclarations() {
  Fixture f;
  f.write("extra.gti", "export module extra;\nexport namespace example { int "
                       "choose(int a, int b) { return a + b; } }");
  f.write("wrong.gti", "export module wrong;\nexport namespace other { int "
                       "choose(int a, int b) { return a + b; } }");
  const std::string source =
      "namespace example { int choose(int a) { return a; } "
      "int choose(bool a) { return 0; } }\n"
      "int main() { return example::choose(1, 2); }\n";
  const auto result = f.analyze(source);
  const auto edits = fixes(result);
  expect(edits.size() == 1 &&
             edits.front().replacement == "import \"extra.gti\";\n" &&
             edits.front().preferred,
         "arity failures discover exact missing user-defined overloads, not "
         "same-spelled unrelated declarations");
  if (!edits.empty())
    expect(f.analyze(edits.front().replacement + source).hirValid,
           "the proposed overload import must restore frontend validity");
  f.overlays[(f.root / "extra.gti").string()] =
      "export module extra;\nexport namespace example { int choose(int a, int "
      "b) { return missing; } }";
  expect(fixes(f.analyze(source)).empty(),
         "a missing overload with a broken body cannot receive a safe fix");
}

void snapshotsAndAlternatives() {
  Fixture f;
  f.write("first.gti",
          "export module first;\nexport int answer() { return 1; }");
  f.write("second.gti",
          "export module second;\nexport int answer() { return 2; }");
  const std::string source = "int main() { return answer(); }\n";
  auto edits = fixes(f.analyze(source));
  expect(edits.size() == 2 && !edits[0].preferred && !edits[1].preferred,
         "ambiguous defining files are separate non-preferred alternatives");
  f.overlays[(f.root / "first.gti").string()] =
      "export module first;\nexport int changed() { return 3; }";
  edits = fixes(f.analyze(source));
  expect(edits.size() == 1 &&
             edits[0].replacement.find("second.gti") != std::string::npos,
         "dirty overlays replace disk declaration content");
  std::filesystem::remove(f.root / "second.gti");
  expect(fixes(f.analyze(source)).empty(),
         "deleted candidate is not cached as a fix");
  f.overlays[(f.root / "unsaved.gti").string()] =
      "export module unsaved;\nexport int answer() { return 4; }";
  edits = fixes(f.analyze("\xef\xbb\xbf// 😀\r\n" + source));
  expect(edits.size() == 1 && edits[0].span.start == 3 &&
             edits[0].replacement == "import \"unsaved.gti\";\r\n",
         "new unsaved files participate; insertion preserves BOM and CRLF");
  f.overlays.clear();
  std::filesystem::remove(f.root / "first.gti");
  f.write("build/generated.gti",
          "export module generated;\nexport int answer() { return 5; }");
  f.write(".hidden/private.gti",
          "export module hidden_api;\nexport int answer() { return 6; }");
  expect(fixes(f.analyze(source)).empty(),
         "generated and hidden directories are excluded");
  Fixture external;
  external.write("outside.gti",
                 "export module outside;\nexport int answer() { return 7; }");
  std::error_code ec;
  std::filesystem::create_symlink(external.root / "outside.gti",
                                  f.root / "escape.gti", ec);
  if (!ec)
    expect(fixes(f.analyze(source)).empty(),
           "symlinks cannot escape search authority");
}

void conceptsAndGenericDeclarations() {
  Fixture f;
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  f.write("generic.gti",
          "export module generic;\nexport namespace example { concept "
          "Number<T> = std::numeric<T>; T "
          "identity<T>(T value) requires std::copyable<T> { return value; } "
          "struct Box<T> { T value; }; }");
  for (const std::string source :
       {"T identity<T>(T value) requires example::Number<T> { return value; } "
        "int main() { return identity(2); }",
        "int main() { return example::identity(4); }",
        "using Boxed = example::Box<int>;"}) {
    auto result = lang::Frontend(f.options).analyze(
        f.root / "main.gti", source, {library / "prelude.gti"}, {}, {library});
    auto edits = fixes(result);
    expect(!edits.empty() &&
               edits.front().replacement == "import \"generic.gti\";\n",
           "generic/concept declaration: " + source);
    if (!edits.empty()) {
      auto corrected = lang::Frontend(f.options).analyze(
          f.root / "main.gti", edits.front().replacement + source,
          {library / "prelude.gti"}, {}, {library});
      expect(corrected.hirValid,
             "generic/concept import edit is accepted by HIR");
    }
  }
}

void requestingModuleHeader() {
  Fixture f;
  f.write("helper.gti",
          "export module helper;\nexport int helper() { return 8; }");
  lang::PackageSourceRoot package;
  package.identity = "library@0.1.0";
  package.name = "library";
  package.packageRoot = f.root;
  package.sourceRoot = f.root;
  package.modules.emplace_back("library", f.root / "main.gti");
  const std::string source =
      "export module library;\r\nexport int use() { return helper(); }\r\n";
  const auto result = f.analyze(source, {package});
  const auto edits = fixes(result);
  if (edits.empty()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(edits.size() == 1 &&
             edits[0].span.start == source.find("export int") &&
             edits[0].replacement == "import \"helper.gti\";\r\n",
         "an import must follow the requesting interface's required module "
         "header");
  if (edits.size() == 1) {
    auto corrected = source;
    corrected.insert(edits[0].span.start, edits[0].replacement);
    expect(f.analyze(corrected, {package}).hirValid,
           "module insertion preserves valid source order");
  }
}

void declaredDependencies() {
  Fixture f;
  f.write("gti.toml",
          "manifest-version = 1\n[package]\nname = \"app\"\nversion = "
          "\"0.1.0\"\n[dependencies]\napi = { path = \"dep\" "
          "}\n[targets.app]\nkind = \"executable\"\nroot = \"src/main.gti\"\n");
  f.write("src/main.gti", "int main() { return 0; }");
  f.write("dep/gti.toml",
          "manifest-version = 1\n[package]\nname = \"library\"\nversion = "
          "\"0.1.0\"\nsource-root = \"src\"\n"
          "[modules]\napi = \"src/api.gti\"\n");
  f.write(
      "dep/src/api.gti",
      "export module api;\nexport namespace dep { int test() { return 9; } }");
  const auto project = lang::driver::resolveProjectFacts(f.root);
  expect(project.succeeded(),
         "fixture resolves through canonical project facts");
  if (!project.succeeded())
    for (const auto &d : project.diagnostics)
      std::cerr << d.message << '\n';
  if (!project.facts)
    return;
  f.options.includeSearchRoots =
      lang::driver::projectIncludeSearchRoots(*project.facts);
  auto result = f.analyze("int main() { return dep::test(); }",
                          project.facts->packageSourceRoots());
  auto edits = fixes(result);
  expect(edits.size() == 1 && edits[0].replacement == "import api.api;\n",
         "direct dependencies use their declared dependency alias and module "
         "registration");
  f.write("unlisted/gti.toml", "manifest-version = 1\n[package]\nname = "
                               "\"unlisted\"\nversion = \"0.1.0\"\n");
  f.write("unlisted/api.gti",
          "export module api;\nexport int secret() { return 1; }");
  expect(
      fixes(f.analyze("int main() { return secret(); }",
                      project.facts->packageSourceRoots()))
          .empty(),
      "nested undeclared packages are not part of the selected package search");
  f.overlays[(f.root / "unlisted/unsaved.gti").string()] =
      "export module unsaved;\nexport int secret() { return 2; }";
  expect(fixes(f.analyze("int main() { return secret(); }",
                         project.facts->packageSourceRoots()))
             .empty(),
         "an unsaved file must not bypass a nested package boundary");
}
void registeredPublicModules() {
  Fixture f;
  f.write("dep/gti.toml", "manifest-version = 1\n");
  f.write("dep/api.gti",
          "export module net.http;\nexport int remote() { return 7; }");
  f.write("dep/private.gti",
          "export module private_api;\nexport int secret() { return 3; }");
  std::vector<lang::PackageSourceRoot> owners{
      {.identity = "app",
       .name = "app",
       .packageRoot = f.root,
       .sourceRoot = f.root,
       .dependencies = {{"dep", "library"}}},
      {.identity = "library",
       .name = "library",
       .packageRoot = f.root / "dep",
       .sourceRoot = f.root / "dep",
       .modules = {{"net.http", f.root / "dep/api.gti"}}}};
  const auto result = f.analyze("int main() { return remote(); }", owners);
  const auto edits = fixes(result);
  expect(edits.size() == 1 &&
             edits.front().replacement == "import dep.net.http;\n",
         "import fixes use hierarchical public map registrations");
  const auto hidden = f.analyze("int main() { return secret(); }", owners);
  expect(fixes(hidden).empty() &&
             std::find(hidden.includeSuggestionInputs.begin(),
                       hidden.includeSuggestionInputs.end(),
                       f.root / "dep/private.gti") ==
                 hidden.includeSuggestionInputs.end(),
         "dependency-private files are not probed as public fix candidates");
  f.write("dep/api.gti",
          "export module net.http;\nexport import \"implementation.gti\";\n");
  f.write("dep/implementation.gti",
          "export module internals;\n"
          "export int remote() { return 7; }\nint secret() { return 3; }\n");
  const auto facadeEdits =
      fixes(f.analyze("int main() { return remote(); }", owners));
  expect(facadeEdits.size() == 1 &&
             facadeEdits.front().replacement == "import dep.net.http;\n",
         "a public facade suggests its registered import for a private-file "
         "reexport");
  owners[1].modules.emplace_back("internals",
                                 f.root / "dep/implementation.gti");
  const auto precise =
      fixes(f.analyze("int main() { return remote(); }", owners));
  expect(precise.size() == 1 &&
             precise.front().replacement == "import dep.internals;\n",
         "a direct public import avoids redundant broader facade trials");
  owners[1].modules.pop_back();
  expect(fixes(f.analyze("int main() { return secret(); }", owners)).empty(),
         "facade traversal does not publish private declarations");
  f.write("dep/api.gti",
          "export module net.http;\nimport \"implementation.gti\";\n");
  expect(fixes(f.analyze("int main() { return remote(); }", owners)).empty(),
         "ordinary private imports are not a facade public surface");
  f.write("dep/api.gti", "export module net.http;\nexport int remote();\n");
  f.write("dep/body.gti", "module net.http;\nint remote() { return 7; }\n");
  owners[1].moduleImplementations.emplace_back(
      "net.http", std::vector<std::filesystem::path>{f.root / "dep/body.gti"});
  const auto bodyEdits =
      fixes(f.analyze("int main() { return remote(); }", owners));
  expect(bodyEdits.size() == 1 &&
             bodyEdits.front().replacement == "import dep.net.http;\n",
         "split function definitions are suggested through their exported "
         "declaration");
}
// A shared discovery cache reuses candidate scans only while their source
// text and resolution context are unchanged, so every suggestion matches an
// uncached discovery. The standard library's public modules are candidates
// here, as in an editor.
void discoveryReusesCandidateScans() {
  Fixture f;
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto cache = std::make_shared<lang::IncludeDiscoveryCache>();
  const auto analyze = [&](const std::string &source, bool cached = true) {
    auto options = f.options;
    if (cached)
      options.includeDiscoveryCache = cache;
    return lang::Frontend(options).analyze(f.root / "main.gti", source,
                                           {library / "prelude.gti"},
                                           f.overlays, {library});
  };
  const auto replacements = [&](const std::string &source) {
    std::vector<std::string> cachedEdits;
    for (const auto &fix : fixes(analyze(source)))
      cachedEdits.push_back(fix.replacement);
    std::vector<std::string> uncachedEdits;
    for (const auto &fix : fixes(analyze(source, false)))
      uncachedEdits.push_back(fix.replacement);
    expect(cachedEdits == uncachedEdits,
           "cached discovery matches an uncached discovery for: " + source);
    return cachedEdits;
  };
  const std::vector<std::string> api{"import \"api.gti\";\n"};
  f.write("api.gti", "export module api;\n"
                     "namespace api { export int value() { return 7; } }\n");
  const std::string source = "int main() { return api::value(); }\n";

  auto before = cache->stats();
  expect(replacements(source) == api, "the defining interface is offered");
  auto after = cache->stats();
  expect(after.scanMisses > before.scanMisses &&
             after.trials == before.trials + 1 &&
             after.qualifierSkips > before.qualifierSkips,
         "library interfaces that declare `value` but never spell `api` are "
         "not speculatively analysed for `api::value`");

  before = after;
  expect(replacements("// edited\n" + source) == api,
         "an edited requester keeps its fix");
  after = cache->stats();
  expect(after.scanMisses == before.scanMisses &&
             after.lexiconMisses == before.lexiconMisses &&
             after.scanHits > before.scanHits,
         "unchanged candidates are neither lexed nor loaded again");

  // A candidate interface edited on disk is loaded again; its new export is
  // offered and the next discovery reuses the new scan.
  f.write("api.gti", "export module api;\n"
                     "namespace api { export int value() { return 7; }\n"
                     "  export int wobble() { return 8; } }\n");
  const std::string added = "int main() { return api::wobble(); }\n";
  before = cache->stats();
  expect(replacements(added) == api, "an edited candidate's new export");
  after = cache->stats();
  expect(after.scanMisses > before.scanMisses,
         "an edited candidate is not reused");
  before = after;
  expect(replacements(added) == api, "the edited candidate's scan is reused");
  expect(cache->stats().scanMisses == before.scanMisses,
         "the edited candidate's new scan is remembered");

  // An unsaved buffer replaces the disk text in both directions.
  f.overlays[(f.root / "api.gti").string()] =
      "export module api;\nnamespace api { export int renamed() { return 7; } "
      "}\n";
  expect(replacements(source).empty(),
         "an unsaved edit removing the export removes the fix");
  f.overlays.clear();
  expect(replacements(source) == api, "closing the buffer restores the fix");

  // New and deleted candidate files.
  f.write("other.gti", "export module other;\n"
                       "export namespace api { int value() { return 9; } }\n");
  expect(replacements(source) ==
             std::vector<std::string>{"import \"api.gti\";\n",
                                      "import \"other.gti\";\n"},
         "a new candidate file is an alternative");
  std::filesystem::remove(f.root / "other.gti");
  expect(replacements(source) == api, "a deleted candidate is not remembered");

  // A candidate's re-exported surface is part of its scan. The re-exported
  // unit sits in an excluded directory, so only the facade is a candidate.
  f.write("facade.gti",
          "export module facade;\nexport import \"build/inner.gti\";\n");
  f.write("build/inner.gti", "export module inner;\n"
                             "export namespace inner_api { int depth() { "
                             "return 1; } }\n");
  const std::string nested = "int main() { return inner_api::depth(); }\n";
  expect(replacements(nested) ==
             std::vector<std::string>{"import \"facade.gti\";\n"},
         "a facade offers its re-exported declaration");
  f.write("build/inner.gti", "export module inner;\n"
                             "export namespace inner_api { int width() { "
                             "return 1; } }\n");
  before = cache->stats();
  expect(replacements(nested).empty(),
         "editing a re-exported unit invalidates the facade's scan");
  expect(cache->stats().scanMisses > before.scanMisses,
         "the facade is loaded again after its re-exported unit changed");

  // Qualifiers the program already spells do not restrict candidates: an
  // alias or a local declaration may give the written name its meaning.
  f.write("real.gti", "export module real_api;\n"
                      "export namespace real { int reached() { return 6; } "
                      "}\n");
  expect(replacements("namespace real { int other() { return 0; } }\n"
                      "namespace alias = real;\n"
                      "int main() { return alias::reached(); }\n") ==
             std::vector<std::string>{"import \"real.gti\";\n"},
         "an alias qualifier reaches a reopened namespace's import");
  expect(replacements("int main() { return wrong::value(); }\n").empty(),
         "an unrelated qualifier still offers nothing");
}

// Source-owner registrations are part of every scan's identity: changing a
// module map changes the suggested import.
void moduleMapChangesInvalidateScans() {
  Fixture f;
  const auto cache = std::make_shared<lang::IncludeDiscoveryCache>();
  f.options.includeDiscoveryCache = cache;
  f.write("dep/gti.toml", "manifest-version = 1\n");
  f.write("dep/api.gti",
          "export module net.http;\nexport int remote() { return 7; }");
  std::vector<lang::PackageSourceRoot> owners{
      {.identity = "app",
       .name = "app",
       .packageRoot = f.root,
       .sourceRoot = f.root,
       .dependencies = {{"dep", "library"}}},
      {.identity = "library",
       .name = "library",
       .packageRoot = f.root / "dep",
       .sourceRoot = f.root / "dep",
       .modules = {{"net.http", f.root / "dep/api.gti"}}}};
  const std::string source = "int main() { return remote(); }";
  const auto suggested = [&] {
    std::vector<std::string> result;
    for (const auto &fix : fixes(f.analyze(source, owners)))
      result.push_back(fix.replacement);
    return result;
  };
  expect(suggested() == std::vector<std::string>{"import dep.net.http;\n"},
         "the registered module name is suggested");
  owners[0].dependencies = {{"remote_dep", "library"}};
  expect(suggested() ==
             std::vector<std::string>{"import remote_dep.net.http;\n"},
         "a renamed dependency alias replaces the remembered suggestion");
  expect(cache->stats().scanHits == 0,
         "no scan is reused across different source-owner registrations");
  f.write("dep/v2.gti",
          "export module net.http;\nexport int other() { return 8; }");
  owners[1].modules = {{"net.http", f.root / "dep/v2.gti"}};
  expect(suggested().empty(),
         "a module mapped to another interface no longer offers the old one");
  owners[1].modules = {{"net.http", f.root / "dep/api.gti"}};
  const auto hits = cache->stats().scanHits;
  expect(suggested() ==
             std::vector<std::string>{"import remote_dep.net.http;\n"},
         "restoring the registration restores the suggestion");
  expect(cache->stats().scanHits > hits,
         "an identical registration reuses the remembered scan");
}
// Opt-in performance probe; no wall-clock threshold makes ordinary tests flaky.
// Supply the repository root and optionally "preamble" to compare reuse costs.
int profile(const std::filesystem::path &root, bool preamble, bool bounded,
            std::string_view only) {
  for (const auto &name : {"set", "queue", "unordered_set", "value"}) {
    if (!only.empty() && only != name)
      continue;
    Fixture f;
    f.options.includeSearchRoots.clear();
    f.options.includeDiscoveryLimits.maximumTrials = 1000;
    f.options.includeDiscoveryLimits.maximumCandidateScans = 1000;
    if (bounded)
      f.options.includeDiscoveryLimits = {};
    f.options.includeDiscoveryLimits.collectTimings = true;
    f.options.includeDiscoveryCache =
        std::make_shared<lang::IncludeDiscoveryCache>();
    if (preamble)
      f.options.preambleCache = std::make_shared<lang::SemanticPreambleCache>();
    const std::string body =
        std::string(name) == "value"
            ? "return value;"
            : "std::" + std::string(name) + "<int> values{}; return 0;";
    for (int pass = 0; pass != 2; ++pass) {
      const auto begin = std::chrono::steady_clock::now();
      const auto result = lang::Frontend(f.options).analyze(
          f.root / "main.gti", "import std.string; int main() { " + body + " }",
          {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
      const auto stats = f.options.includeDiscoveryCache->stats();
      std::cout << name << " preamble=" << preamble << " pass=" << pass
                << " bounded=" << bounded << " total_us="
                << std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - begin)
                       .count()
                << " cumulative_trials=" << stats.trials
                << " trial_us=" << stats.trialMicroseconds
                << " semantic_us=" << stats.trialSemanticMicroseconds
                << " hir_us=" << stats.trialHirMicroseconds
                << " preamble_build_us=" << stats.trialPreambleBuildMicroseconds
                << " preamble_adopt_us="
                << stats.trialPreambleAdoptMicroseconds;
      for (const auto &fix : fixes(result))
        std::cout << " fix=" << fix.message;
      std::cout << std::endl;
    }
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "--profile")
    return profile(std::filesystem::canonical(argv[2]),
                   argc > 3 && std::string(argv[3]) == "preamble",
                   argc > 4 && std::string(argv[4]) == "bounded",
                   argc > 5 ? argv[5] : "");
  boundedDiscovery();
  boundedCandidateScans();
  lexicalDeclarationPriority();
  namedModulePriority();
  trialsDoNotBuildPreambles();
  discoveryReusesCandidateScans();
  moduleMapChangesInvalidateScans();
  internalTrialsDoNotReportProgress();
  immutableDiscoveryAndCancellation();
  projectDeclarations();
  missingOverloadDeclarations();
  snapshotsAndAlternatives();
  conceptsAndGenericDeclarations();
  requestingModuleHeader();
  declaredDependencies();
  registeredPublicModules();
  return failures ? 1 : 0;
}
