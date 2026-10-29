#include "gti/driver/project.h"
#include "gti/driver/project_facts.h"
#include "gti/source_loader.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("gti-module-facts-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "src");
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  std::ofstream(root / "src/main.gti")
      << "import native.bindings; int main() { return 0; }\n";
  std::ofstream(root / "api.h") << "int native_value(void);\n";
  std::ofstream(root / "gti.toml") << R"(manifest-version = 1
[package]
name = "module_facts"
version = "0.1.0"
[targets.app]
kind = "executable"
root = "src/main.gti"
[bindgen.native]
header = "api.h"
namespace = "native_api"
)";
  const auto facts = lang::driver::resolveProjectFacts(root);
  expect(facts.succeeded() && facts.facts,
         "bindgen project facts resolve before generation");
  const auto &owners = facts.facts->packageSourceRoots();
  expect(owners.size() == 2,
         "read-only facts retain the generated source owner");
  const auto generated =
      std::find_if(owners.begin(), owners.end(), [](const auto &owner) {
        return owner.kind == lang::SourceOwnerKind::Generated;
      });
  expect(generated != owners.end() && generated->modules.size() == 1 &&
             generated->modules.front().first == "bindings",
         "generated owner pins its public module name");
  expect(!std::filesystem::exists(root / "build"),
         "read-only facts never generate bindings");
  const lang::driver::ProjectBuildRequest request(root, std::nullopt, "dev",
                                                  lang::TargetInfo::host());
  const auto plan = lang::driver::resolveProjectBuild(request);
  expect(plan.succeeded() && plan.plan,
         "the same package produces a build plan");
  expect(lang::SourceOwnerRegistry(owners).resolutionIdentity() ==
             lang::SourceOwnerRegistry(plan.plan->packageSources())
                 .resolutionIdentity(),
         "editor and builder share identical generated owner registration");
  lang::SourceLoader absent;
  absent.load(root / "src/main.gti", "import native.bindings;", {}, {}, {},
              std::nullopt, owners);
  expect(absent.hadError() &&
             std::any_of(absent.errors().begin(), absent.errors().end(),
                         [](const auto &error) {
                           return error.message.find("gti check") !=
                                  std::string::npos;
                         }),
         "missing generated bindings have an actionable source-loading "
         "diagnostic");
  lang::SourceLoader present;
  present.load(
      root / "src/main.gti", "import native.bindings;", {},
      {{generated->modules.front().second.string(), "export module bindings;"}},
      {}, std::nullopt, owners);
  expect(!present.hadError(),
         "existing generated bindings resolve without driver generation");
  std::ofstream(root / "gti.toml", std::ios::app)
      << "\n[bindgen.class]\nheader = \"api.h\"\n";
  const auto reserved = lang::driver::loadProjectManifest(root / "gti.toml");
  expect(!reserved.succeeded() && !reserved.diagnostics.empty(),
         "keyword aliases are rejected by manifest validation before source "
         "resolution");
  std::cout << "driver module facts checks passed\n";
}
