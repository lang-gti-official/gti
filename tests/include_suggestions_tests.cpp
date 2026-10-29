#include "gti/driver/project_facts.h"
#include "gti/frontend.h"

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
}
} // namespace

int main() {
  internalTrialsDoNotReportProgress();
  projectDeclarations();
  missingOverloadDeclarations();
  snapshotsAndAlternatives();
  conceptsAndGenericDeclarations();
  requestingModuleHeader();
  declaredDependencies();
  registeredPublicModules();
  return failures ? 1 : 0;
}
