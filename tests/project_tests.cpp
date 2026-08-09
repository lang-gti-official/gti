#include "gti/driver/manifest.h"
#include "gti/driver/project.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gti-project-test-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  [[nodiscard]] const std::filesystem::path &root() const { return path; }

private:
  std::filesystem::path path;
};

bool writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
  return static_cast<bool>(output);
}

const lang::Diagnostic *
findDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
               std::string_view code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

std::string validManifest(std::string_view targets = R"(
[targets.game]
kind = "executable"
root = "src/main.gti"
)") {
  return "manifest-version = 1\n\n"
         "[package]\n"
         "name = \"engine_tools\"\n"
         "version = \"0.1.0\"\n" +
         std::string(targets) +
         "\n[profiles.release]\n"
         "optimization = 2\n"
         "cpp-standard = \"c++20\"\n"
         "keep-cpp = true\n";
}

void testDiscoveryParsingAndResolution() {
  TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / "src/main.gti";
  const std::filesystem::path manifest = temporary.root() / "gti.toml";
  const std::filesystem::path nested = temporary.root() / "src/nested/deeper";
  expect(writeFile(source, "int main() { return 0; }\n"),
         "the project source fixture should be writable");
  expect(writeFile(manifest, validManifest()),
         "the project manifest fixture should be writable");
  std::filesystem::create_directories(nested);

  const lang::driver::ManifestDiscoveryResult discovery =
      lang::driver::discoverProjectManifest(nested);
  expect(discovery.succeeded() &&
             *discovery.path == std::filesystem::canonical(manifest),
         "manifest discovery should walk upward from nested directories");

  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifest);
  expect(loaded.succeeded(), "a schema version 1 manifest should parse");
  if (loaded.manifest) {
    const lang::driver::ProjectManifest &project = *loaded.manifest;
    expect(project.package().name == "engine_tools" &&
               project.package().version == "0.1.0",
           "the package identity should be retained");
    const lang::driver::ProjectTarget *target = project.findTarget("game");
    expect(target != nullptr &&
               target->root == std::filesystem::canonical(source),
           "target roots should resolve relative to the manifest");
    const lang::driver::ProjectProfile *development =
        project.findProfile("dev");
    const lang::driver::ProjectProfile *release =
        project.findProfile("release");
    expect(development != nullptr &&
               development->optimization == lang::OptimizationLevel::O0 &&
               development->cppStandard == lang::CppStandard::Cpp23 &&
               !development->keepCpp,
           "the built-in dev profile should have stable defaults");
    expect(release != nullptr &&
               release->optimization == lang::OptimizationLevel::O2 &&
               release->cppStandard == lang::CppStandard::Cpp20 &&
               release->keepCpp,
           "manifest profile fields should refine built-in defaults");
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = lang::OptimizationLevel::O3;
  overrides.cppStandard = lang::CppStandard::Cpp23;
  overrides.keepCpp = false;
  const lang::TargetInfo targetInfo{
      .os = "testos", .vendor = "testvendor", .arch = "testarch"};
  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          nested, std::nullopt, "release", targetInfo, overrides));
  expect(resolution.succeeded(),
         "one target should be inferred from a nested project directory");
  if (resolution.plan) {
    const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
    const std::filesystem::path expectedDirectory =
        std::filesystem::canonical(temporary.root()) /
        "build/gti/release/testarch-testvendor-testos";
    expect(plan.entry() == std::filesystem::canonical(source) &&
               plan.output().parent_path() == expectedDirectory &&
               plan.generatedSource() ==
                   expectedDirectory / "intermediate/game.gti.cpp",
           "project artifacts should use the deterministic project layout");
    expect(plan.optimization() == lang::OptimizationLevel::O3 &&
               plan.cppStandard() == lang::CppStandard::Cpp23 &&
               !plan.keepCpp(),
           "explicit build overrides should win over profile settings");
  }

  const lang::driver::ProjectMetadataResult metadata =
      lang::driver::resolveProjectMetadata(nested, targetInfo);
  expect(metadata.succeeded() && metadata.metadata &&
             metadata.metadata->manifest().targets().size() == 1 &&
             metadata.metadata->manifest().profiles().size() == 2 &&
             metadata.metadata->plans().size() == 2,
         "metadata should enumerate every target/profile plan without "
         "requiring target selection");
  expect(!std::filesystem::exists(temporary.root() / "build"),
         "metadata resolution should not create project output directories");
}

void testManifestDiagnostics() {
  TemporaryDirectory temporary;
  const std::filesystem::path package = temporary.root() / "package";
  const std::filesystem::path manifest = package / "gti.toml";
  expect(writeFile(package / "src/main.gti", "int main() { return 0; }\n"),
         "the diagnostic project source should be writable");

  expect(writeFile(manifest, "manifest-version = [\n"),
         "the malformed manifest should be writable");
  lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *parseError =
      findDiagnostic(loaded.diagnostics, "GTI-B1000");
  expect(loaded.status == lang::driver::ManifestLoadStatus::ParseFailure &&
             parseError != nullptr &&
             loaded.sources.locate(parseError->primary).line == 1,
         "invalid TOML should retain an exact parser source span");

  const std::string unknownRoot =
      "manifest-version = 1\n"
      "[package]\nname = \"sample\"\nversion = \"1.0.0\"\n"
      "[targets.sample]\nkind = \"executable\"\n"
      "rot = \"src/main.gti\"\n";
  expect(writeFile(manifest, unknownRoot),
         "the unknown-field manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *unknown =
      findDiagnostic(loaded.diagnostics, "GTI-B1001");
  expect(unknown != nullptr && !unknown->hints.empty() &&
             unknown->hints.front().find("root") != std::string::npos &&
             loaded.sources.locate(unknown->primary).line == 7,
         "unknown fields should point at the key and suggest the nearest name");

  std::string unsupported = validManifest();
  unsupported.replace(unsupported.find("1"), 1, "2");
  expect(writeFile(manifest, unsupported),
         "the unsupported-version manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1003") != nullptr,
         "unsupported schema versions should have a focused diagnostic");

  expect(
      writeFile(temporary.root() / "outside.gti", "int main() { return 0; }\n"),
      "the escaping target fixture should be writable");
  const std::string escaping =
      "manifest-version = 1\n"
      "[package]\nname = \"sample\"\nversion = \"1.0.0\"\n"
      "[targets.sample]\nkind = \"executable\"\n"
      "root = \"../outside.gti\"\n";
  expect(writeFile(manifest, escaping),
         "the escaping-root manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1104") != nullptr,
         "canonical target roots outside the package should be rejected");
}

void testTargetSelectionDiagnostics() {
  TemporaryDirectory temporary;
  expect(writeFile(temporary.root() / "src/alpha.gti",
                   "int main() { return 0; }\n") &&
             writeFile(temporary.root() / "src/beta.gti",
                       "int main() { return 0; }\n"),
         "multi-target sources should be writable");
  const std::string targets = "\n[targets.alpha]\nkind = \"executable\"\n"
                              "root = \"src/alpha.gti\"\n"
                              "\n[targets.beta]\nkind = \"executable\"\n"
                              "root = \"src/beta.gti\"\n";
  expect(writeFile(temporary.root() / "gti.toml", validManifest(targets)),
         "the multi-target manifest should be writable");

  const lang::TargetInfo host = lang::TargetInfo::host();
  lang::driver::ProjectResolutionResult result =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          temporary.root(), std::nullopt, "dev", host));
  expect(findDiagnostic(result.diagnostics, "GTI-B1201") != nullptr,
         "an omitted target should be rejected when selection is ambiguous");

  result = lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("alpah"), "dev", host));
  const lang::Diagnostic *unknownTarget =
      findDiagnostic(result.diagnostics, "GTI-B1200");
  expect(unknownTarget != nullptr && !unknownTarget->hints.empty() &&
             unknownTarget->hints.front().find("alpha") != std::string::npos,
         "unknown targets should suggest the nearest declared target");

  result = lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("release"), "dev", host));
  const lang::Diagnostic *profileAsTarget =
      findDiagnostic(result.diagnostics, "GTI-B1200");
  expect(profileAsTarget != nullptr && !profileAsTarget->hints.empty() &&
             profileAsTarget->hints.front().find("--profile release") !=
                 std::string::npos,
         "a profile entered as a target should explain how to select it");

  result = lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("alpha"), "relase", host));
  const lang::Diagnostic *unknownProfile =
      findDiagnostic(result.diagnostics, "GTI-B1202");
  expect(unknownProfile != nullptr && !unknownProfile->hints.empty() &&
             unknownProfile->hints.front().find("release") != std::string::npos,
         "unknown profiles should suggest the nearest available profile");

  const lang::driver::ManifestDiscoveryResult missing =
      lang::driver::discoverProjectManifest(temporary.root().parent_path());
  expect(!missing.succeeded() &&
             findDiagnostic(missing.diagnostics, "GTI-B1100") != nullptr,
         "discovery should report a focused error when no manifest exists");
}

void testCleanSafety() {
  TemporaryDirectory temporary;
  const std::filesystem::path package = temporary.root() / "package";
  const std::filesystem::path external = temporary.root() / "external";
  expect(writeFile(package / "gti.toml", "not valid TOML\n") &&
             writeFile(package / "build/gti/dev/artifact", "artifact") &&
             writeFile(package / "build/keep.txt", "keep") &&
             writeFile(external / "sentinel.txt", "sentinel"),
         "clean safety fixtures should be writable");

  lang::driver::ProjectCleanResult clean = lang::driver::cleanProject(package);
  expect(clean.succeeded() && clean.removedEntries != 0 &&
             !std::filesystem::exists(package / "build/gti") &&
             std::filesystem::exists(package / "build/keep.txt") &&
             std::filesystem::exists(external / "sentinel.txt"),
         "clean should remove only the package's literal build/gti subtree, "
         "even when the manifest cannot be parsed");

  clean = lang::driver::cleanProject(package);
  expect(clean.succeeded() && clean.removedEntries == 0,
         "clean should be idempotent when no GTI build subtree exists");

  std::error_code error;
  std::filesystem::remove_all(package / "build", error);
  error.clear();
  std::filesystem::create_directory_symlink(external, package / "build", error);
  if (!error) {
    clean = lang::driver::cleanProject(package);
    expect(clean.status == lang::driver::ProjectCleanStatus::UnsafePath &&
               findDiagnostic(clean.diagnostics, "GTI-B1300") != nullptr &&
               std::filesystem::exists(external / "sentinel.txt"),
           "clean should refuse a symbolic-link build boundary and preserve "
           "its target");
  }
}

} // namespace

int main() {
  testDiscoveryParsingAndResolution();
  testManifestDiagnostics();
  testTargetSelectionDiagnostics();
  testCleanSafety();

  if (failures != 0) {
    std::cerr << failures << " project test(s) failed\n";
    return 1;
  }
  std::cout << "All project tests passed\n";
  return 0;
}
