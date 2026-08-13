#include "gti/driver/manifest.h"
#include "gti/driver/project.h"
#include "gti/support.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
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
         "execution-profile = \"concurrent\"\n"
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

  const std::filesystem::path shadowed = temporary.root() / "shadowed";
  std::filesystem::create_directory(shadowed);
  std::error_code symlinkError;
  std::filesystem::create_symlink(shadowed / "missing-manifest.toml",
                                  shadowed / "gti.toml", symlinkError);
  if (!symlinkError) {
    const lang::driver::ManifestDiscoveryResult brokenLocal =
        lang::driver::discoverProjectManifest(shadowed);
    expect(brokenLocal.status ==
                   lang::driver::ManifestDiscoveryStatus::FilesystemFailure &&
               findDiagnostic(brokenLocal.diagnostics, "GTI-B1101") != nullptr,
           "a broken local gti.toml symlink should block discovery instead of "
           "silently selecting a parent project");
  }

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
               target->kind == lang::driver::ProjectTargetKind::Executable &&
               target->root == std::filesystem::canonical(source),
           "executable target roots and kinds should resolve from the "
           "manifest");
    const lang::driver::ProjectProfile *development =
        project.findProfile("dev");
    const lang::driver::ProjectProfile *release =
        project.findProfile("release");
    expect(development != nullptr &&
               development->optimization == lang::OptimizationLevel::O0 &&
               development->cppStandard == lang::CppStandard::Cpp23 &&
               development->executionProfile ==
                   lang::ExecutionProfile::SingleThreaded &&
               !development->keepCpp,
           "the built-in dev profile should have stable defaults");
    expect(release != nullptr &&
               release->optimization == lang::OptimizationLevel::O2 &&
               release->cppStandard == lang::CppStandard::Cpp20 &&
               release->executionProfile ==
                   lang::ExecutionProfile::Concurrent &&
               release->keepCpp,
           "manifest profile fields should refine built-in defaults");
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = lang::OptimizationLevel::O3;
  overrides.cppStandard = lang::CppStandard::Cpp23;
  overrides.executionProfile = lang::ExecutionProfile::SingleThreaded;
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
               plan.target().executionProfile ==
                   lang::ExecutionProfile::SingleThreaded &&
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
  if (metadata.metadata) {
    const auto releasePlan = std::find_if(
        metadata.metadata->plans().begin(), metadata.metadata->plans().end(),
        [](const lang::driver::ProjectBuildPlan &plan) {
          return plan.profileName() == "release";
        });
    expect(releasePlan != metadata.metadata->plans().end() &&
               releasePlan->target().executionProfile ==
                   lang::ExecutionProfile::Concurrent,
           "metadata plans should retain each manifest profile's execution "
           "semantics");
  }
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

  std::string invalidExecutionProfile = validManifest();
  invalidExecutionProfile.replace(invalidExecutionProfile.find("concurrent"),
                                  std::string_view("concurrent").size(),
                                  "parallel");
  expect(writeFile(manifest, invalidExecutionProfile),
         "the invalid execution-profile fixture should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *invalidProfile =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(invalidProfile != nullptr &&
             invalidProfile->message.find("single-threaded") !=
                 std::string::npos &&
             invalidProfile->message.find("concurrent") != std::string::npos,
         "manifest execution profiles should use the exact compiler-owned "
         "vocabulary");

  std::string invalidKind = validManifest();
  invalidKind.replace(invalidKind.find("executable"),
                      std::string_view("executable").size(), "benchmark");
  expect(writeFile(manifest, invalidKind),
         "the invalid target-kind fixture should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *targetKind =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(targetKind != nullptr &&
             targetKind->message.find("'executable' or 'test'") !=
                 std::string::npos &&
             targetKind->primary.source ==
                 std::filesystem::canonical(manifest).string() &&
             targetKind->primary.start < targetKind->primary.end,
         "manifest target kinds should accept only executable and test");

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

#if !defined(_WIN32)
  const std::filesystem::path foreignAbsolute = package / "C:\\outside.gti";
  expect(writeFile(foreignAbsolute, "int main() { return 0; }\n"),
         "the foreign absolute-path fixture should be writable");
  const std::string foreignAbsoluteManifest =
      "manifest-version = 1\n"
      "[package]\nname = \"sample\"\nversion = \"1.0.0\"\n"
      "[targets.sample]\nkind = \"executable\"\n"
      "root = 'C:\\outside.gti'\n";
  expect(writeFile(manifest, foreignAbsoluteManifest),
         "the foreign absolute-root manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *foreignRoot =
      findDiagnostic(loaded.diagnostics, "GTI-B1103");
  expect(foreignRoot != nullptr &&
             foreignRoot->message.find("relative to gti.toml") !=
                 std::string::npos,
         "target roots should reject Windows-absolute syntax consistently on "
         "non-Windows hosts");
#endif
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

void testTestTargetResolution() {
  TemporaryDirectory temporary;
  const std::filesystem::path manifest = temporary.root() / "gti.toml";
  expect(writeFile(temporary.root() / "src/app.gti",
                   "int main() { return 0; }\n") &&
             writeFile(temporary.root() / "tests/integration.gti",
                       "int main() { return 0; }\n") &&
             writeFile(temporary.root() / "tests/unit.gti",
                       "int main() { return 0; }\n"),
         "project test target sources should be writable");
  const std::string targets =
      "\n[targets.unit]\nkind = \"test\"\nroot = \"tests/unit.gti\"\n"
      "\n[targets.app]\nkind = \"executable\"\nroot = \"src/app.gti\"\n"
      "\n[targets.integration]\nkind = \"test\"\n"
      "root = \"tests/integration.gti\"\n";
  expect(writeFile(manifest, validManifest(targets)),
         "the mixed executable/test manifest should be writable");

  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifest);
  const lang::driver::ProjectTarget *unit =
      loaded.manifest ? loaded.manifest->findTarget("unit") : nullptr;
  expect(loaded.succeeded() && unit != nullptr &&
             unit->kind == lang::driver::ProjectTargetKind::Test &&
             lang::driver::projectTargetKindName(unit->kind) == "test",
         "manifest version 1 should retain test target kinds");

  const lang::driver::ProjectResolutionResult defaultBuild =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          temporary.root(), std::nullopt, "dev", lang::TargetInfo::host()));
  expect(defaultBuild.succeeded() && defaultBuild.plan &&
             defaultBuild.plan->targetName() == "app",
         "a sole executable should remain the default build target when test "
         "targets are present");

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = lang::OptimizationLevel::O3;
  const lang::TargetInfo targetInfo{
      .os = "testos", .vendor = "testvendor", .arch = "testarch"};
  lang::driver::ProjectTestResolutionResult tests =
      lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
          temporary.root(), std::nullopt, "dev", targetInfo, overrides));
  expect(tests.succeeded() && tests.plans.size() == 2 &&
             tests.plans[0].targetName() == "integration" &&
             tests.plans[1].targetName() == "unit" &&
             tests.plans[0].targetKind() ==
                 lang::driver::ProjectTargetKind::Test &&
             tests.plans[1].targetKind() ==
                 lang::driver::ProjectTargetKind::Test,
         "gti test planning should select every test target in deterministic "
         "name order");
  if (tests.plans.size() == 2) {
    expect(tests.plans[0].optimization() == lang::OptimizationLevel::O3 &&
               tests.plans[0].output() != tests.plans[1].output(),
           "test plans should retain CLI overrides and independent outputs");
  }

  tests = lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("unit"), "release", targetInfo));
  expect(tests.succeeded() && tests.plans.size() == 1 &&
             tests.plans.front().targetName() == "unit" &&
             tests.plans.front().profileName() == "release",
         "an explicitly named test should produce one selected test plan");

  tests = lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("unt"), "dev", targetInfo));
  const lang::Diagnostic *unknown =
      findDiagnostic(tests.diagnostics, "GTI-B1200");
  expect(unknown != nullptr && !unknown->hints.empty() &&
             unknown->message.find("Unknown test target") !=
                 std::string::npos &&
             unknown->hints.front().find("unit") != std::string::npos,
         "unknown test targets should suggest the nearest test target");

  tests = lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
      temporary.root(), std::string("app"), "dev", targetInfo));
  const lang::Diagnostic *wrongKind =
      findDiagnostic(tests.diagnostics, "GTI-B1204");
  expect(wrongKind != nullptr && !wrongKind->hints.empty() &&
             wrongKind->primary.source ==
                 std::filesystem::canonical(manifest).string(),
         "selecting an executable as a test should retain the target "
         "declaration span and an actionable hint");

  const std::string noTests =
      "\n[targets.app]\nkind = \"executable\"\nroot = \"src/app.gti\"\n";
  expect(writeFile(manifest, validManifest(noTests)),
         "the no-test manifest should be writable");
  tests = lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
      temporary.root(), std::nullopt, "dev", targetInfo));
  const lang::Diagnostic *missing =
      findDiagnostic(tests.diagnostics, "GTI-B1203");
  expect(missing != nullptr && !missing->hints.empty() &&
             missing->message.find("no test targets") != std::string::npos &&
             missing->hints.front().find("kind = \"test\"") !=
                 std::string::npos,
         "gti test should diagnose a manifest with no test targets");
}

void testNativeManifestResolution() {
  TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / "src/main.gti";
  expect(writeFile(source, "int main() { return 0; }\n"),
         "the native project source should be writable");

  const std::vector<std::string> directories{
      "package/base/include", "package/os/include", "package/arch/include",
      "profile/base/include", "profile/os/include", "target/base/include",
      "target/os/include",    "package/base/lib",   "package/os/lib",
      "package/arch/lib",     "profile/base/lib",   "profile/os/lib",
      "target/base/lib",      "target/os/lib",
  };
  for (const std::string &directory : directories) {
    std::filesystem::create_directories(temporary.root() / directory);
  }
  const std::vector<std::string> files{
      "package/base/lib/package.a",      "package/os/lib/package-os.a",
      "package/arch/lib/package-arch.a", "profile/base/lib/profile.a",
      "profile/os/lib/profile-os.a",     "target/base/lib/target.a",
      "target/os/lib/target-os.a",
  };
  for (const std::string &file : files) {
    expect(writeFile(temporary.root() / file, "archive"),
           "native link-file fixtures should be writable");
  }
  const std::vector<std::string> cSources{
      "package/base/native.c", "package/os/native.c", "package/arch/native.c",
      "profile/base/native.c", "profile/os/native.c", "target/base/native.c",
      "target/os/native.c",
  };
  for (const std::string &file : cSources) {
    expect(writeFile(temporary.root() / file, "int native_value(void);\n"),
           "native C source fixtures should be writable");
  }
  const std::vector<std::string> cppSources{
      "package/base/native.cpp", "package/os/native.cc",
      "package/arch/native.cxx", "profile/base/native.cpp",
      "profile/os/native.cxx",   "target/base/native.cpp",
      "target/os/native.cc",
  };
  for (const std::string &file : cppSources) {
    expect(writeFile(temporary.root() / file, "int native_cpp_value();\n"),
           "native C++ source fixtures should be writable");
  }

  const std::string manifest = R"(manifest-version = 1

[package]
name = "native_sample"
version = "1.0.0"

[package.native]
include-dirs = ["package/base/include"]
c-sources = ["package/base/native.c"]
cpp-sources = ["package/base/native.cpp"]
c-standard = "c11"
c-compile-args = ["-DC_PACKAGE=1", "-DC_ORDER=package"]
library-dirs = ["package/base/lib"]
link-files = ["package/base/lib/package.a"]
libraries = ["package", "duplicate"]
compile-args = ["-DPACKAGE=1", "-DORDER=package"]
link-args = ["-Wl,package"]
raw-args = ["-Wl,--package-raw"]

[[package.native.platforms]]
os = "testos"
include-dirs = ["package/os/include"]
c-sources = ["package/os/native.c"]
cpp-sources = ["package/os/native.cc"]
c-compile-args = ["-DC_OS=1"]
library-dirs = ["package/os/lib"]
link-files = ["package/os/lib/package-os.a"]
libraries = ["package-os", "duplicate"]
compile-args = ["-DOS=1"]

[[package.native.platforms]]
arch = "testarch"
include-dirs = ["package/arch/include"]
c-sources = ["package/arch/native.c"]
cpp-sources = ["package/arch/native.cxx"]
c-compile-args = ["-DC_ARCH=1"]
library-dirs = ["package/arch/lib"]
link-files = ["package/arch/lib/package-arch.a"]
libraries = ["package-arch"]

[[package.native.platforms]]
os = "other-os"
include-dirs = ["not-present-on-this-target/include"]
link-files = ["not-present-on-this-target/library.a"]

[targets.game]
kind = "executable"
root = "src/main.gti"

[targets.game.native]
include-dirs = ["target/base/include"]
c-sources = ["target/base/native.c"]
cpp-sources = ["target/base/native.cpp"]
c-standard = "c17"
c-compile-args = ["-DC_ORDER=target"]
library-dirs = ["target/base/lib"]
link-files = ["target/base/lib/target.a"]
libraries = ["target"]
compile-args = ["-DORDER=target"]
link-args = ["-Wl,target"]
raw-args = ["-Wl,--target-raw"]

[[targets.game.native.platforms]]
os = "testos"
include-dirs = ["target/os/include"]
c-sources = ["target/os/native.c"]
cpp-sources = ["target/os/native.cc"]
c-compile-args = ["-DC_TARGET_OS=1"]
library-dirs = ["target/os/lib"]
link-files = ["target/os/lib/target-os.a"]
libraries = ["target-os"]
compile-args = ["-DTARGET_OS=1"]

[profiles.release]
optimization = 3

[profiles.release.native]
include-dirs = ["profile/base/include"]
c-sources = ["profile/base/native.c"]
cpp-sources = ["profile/base/native.cpp"]
c-standard = "c23"
c-compile-args = ["-DC_ORDER=profile"]
library-dirs = ["profile/base/lib"]
link-files = ["profile/base/lib/profile.a"]
libraries = ["profile"]
compile-args = ["-DORDER=profile"]
link-args = ["-Wl,profile"]

[[profiles.release.native.platforms]]
os = "testos"
include-dirs = ["profile/os/include"]
c-sources = ["profile/os/native.c"]
cpp-sources = ["profile/os/native.cxx"]
c-compile-args = ["-DC_PROFILE_OS=1"]
library-dirs = ["profile/os/lib"]
link-files = ["profile/os/lib/profile-os.a"]
libraries = ["profile-os"]
compile-args = ["-DPROFILE_OS=1"]
)";
  const std::filesystem::path manifestPath = temporary.root() / "gti.toml";
  expect(writeFile(manifestPath, manifest),
         "the structured native manifest should be writable");

  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifestPath);
  expect(loaded.succeeded(),
         "package, profile, target, and platform native tables should parse");

  const lang::TargetInfo selected{
      .os = "testos", .vendor = "testvendor", .arch = "testarch"};
  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          temporary.root(), std::nullopt, "release", selected));
  expect(resolution.succeeded(),
         "native settings should resolve from the explicit request target");
  if (!resolution.plan) {
    return;
  }

  const lang::driver::NativeInputs &inputs = resolution.plan->nativeInputs();
  const auto paths =
      [&temporary](std::initializer_list<std::string_view> names) {
        std::vector<std::filesystem::path> result;
        for (const std::string_view name : names) {
          result.push_back(std::filesystem::canonical(temporary.root() / name));
        }
        return result;
      };
  expect(inputs.includeDirectories ==
             paths({"target/os/include", "target/base/include",
                    "profile/os/include", "profile/base/include",
                    "package/os/include", "package/arch/include",
                    "package/base/include"}),
         "native include search paths should be most-specific first while "
         "retaining matching-platform declaration order");
  expect(inputs.cSources ==
             paths({"target/os/native.c", "target/base/native.c",
                    "profile/os/native.c", "profile/base/native.c",
                    "package/os/native.c", "package/arch/native.c",
                    "package/base/native.c"}),
         "native C sources should resolve additively in deterministic "
         "target-to-package order");
  expect(inputs.cppSources ==
             paths({"target/os/native.cc", "target/base/native.cpp",
                    "profile/os/native.cxx", "profile/base/native.cpp",
                    "package/os/native.cc", "package/arch/native.cxx",
                    "package/base/native.cpp"}),
         "native C++ sources should resolve additively in deterministic "
         "target-to-package order");
  expect(inputs.libraryDirectories ==
             paths({"target/os/lib", "target/base/lib", "profile/os/lib",
                    "profile/base/lib", "package/os/lib", "package/arch/lib",
                    "package/base/lib"}),
         "native library search paths should be most-specific first");
  expect(inputs.libraryFiles ==
             paths({"target/os/lib/target-os.a", "target/base/lib/target.a",
                    "profile/os/lib/profile-os.a", "profile/base/lib/profile.a",
                    "package/os/lib/package-os.a",
                    "package/arch/lib/package-arch.a",
                    "package/base/lib/package.a"}),
         "exact native link files should put more-specific dependents before "
         "broader package operands");
  expect(inputs.libraries ==
             std::vector<std::string>({"target-os", "target", "profile-os",
                                       "profile", "package-os", "duplicate",
                                       "package-arch", "package", "duplicate"}),
         "native libraries should use dependent-first scope ordering while "
         "preserving declaration order and intentional duplicates");
  const auto fileOperand = [&temporary](std::string_view name) {
    return lang::driver::NativeLinkOperand{
        lang::driver::NativeLinkOperandKind::File,
        std::filesystem::canonical(temporary.root() / name).string()};
  };
  const auto libraryOperand = [](std::string value) {
    return lang::driver::NativeLinkOperand{
        lang::driver::NativeLinkOperandKind::Library, std::move(value)};
  };
  expect(inputs.orderedLinkOperands ==
             std::vector<lang::driver::NativeLinkOperand>({
                 fileOperand("target/os/lib/target-os.a"),
                 libraryOperand("target-os"),
                 fileOperand("target/base/lib/target.a"),
                 libraryOperand("target"),
                 fileOperand("profile/os/lib/profile-os.a"),
                 libraryOperand("profile-os"),
                 fileOperand("profile/base/lib/profile.a"),
                 libraryOperand("profile"),
                 fileOperand("package/os/lib/package-os.a"),
                 libraryOperand("package-os"),
                 libraryOperand("duplicate"),
                 fileOperand("package/arch/lib/package-arch.a"),
                 libraryOperand("package-arch"),
                 fileOperand("package/base/lib/package.a"),
                 libraryOperand("package"),
                 libraryOperand("duplicate"),
             }),
         "resolved native plans should retain the exact heterogeneous "
         "dependent-first operand order consumed by the native command");
  expect(inputs.compilerArguments ==
             std::vector<std::string>(
                 {"-DPACKAGE=1", "-DORDER=package", "-DOS=1", "-DORDER=profile",
                  "-DPROFILE_OS=1", "-DORDER=target", "-DTARGET_OS=1"}),
         "native compiler arguments should compose package-to-target so later "
         "scopes have conventional flag precedence");
  expect(inputs.cCompilerArguments ==
                 std::vector<std::string>(
                     {"-DC_PACKAGE=1", "-DC_ORDER=package", "-DC_OS=1",
                      "-DC_ARCH=1", "-DC_ORDER=profile", "-DC_PROFILE_OS=1",
                      "-DC_ORDER=target", "-DC_TARGET_OS=1"}) &&
             inputs.cStandard == lang::driver::CStandard::C17,
         "native C arguments should compose package-to-target while the most "
         "specific declared C standard wins");
  expect(inputs.linkerArguments ==
                 std::vector<std::string>(
                     {"-Wl,package", "-Wl,profile", "-Wl,target"}) &&
             inputs.trailingArguments ==
                 std::vector<std::string>(
                     {"-Wl,--package-raw", "-Wl,--target-raw"}),
         "link and trusted raw arguments should preserve deterministic scope "
         "and declaration order");

  const lang::driver::ProjectMetadataResult metadata =
      lang::driver::resolveProjectMetadata(temporary.root(), selected);
  expect(metadata.succeeded() && metadata.metadata &&
             metadata.metadata->plans().size() == 2,
         "metadata planning should resolve effective native inputs for every "
         "target/profile pair without building");
}

void testNativeManifestDiagnostics() {
  TemporaryDirectory temporary;
  const std::filesystem::path package = temporary.root() / "package";
  const std::filesystem::path manifestPath = package / "gti.toml";
  expect(
      writeFile(package / "src/main.gti", "int main() { return 0; }\n") &&
          writeFile(temporary.root() / "outside.a", "archive") &&
          writeFile(temporary.root() / "outside.c", "int outside(void);\n") &&
          writeFile(temporary.root() / "outside.cpp", "int outside();\n"),
      "native diagnostic fixtures should be writable");

  const auto nativeManifest = [](std::string_view native) {
    return "manifest-version = 1\n"
           "[package]\nname = \"sample\"\nversion = \"1.0.0\"\n" +
           std::string(native) +
           "\n[targets.sample]\nkind = \"executable\"\n"
           "root = \"src/main.gti\"\n";
  };

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "include-dirs = [\"..\"]\n"
                                  "c-sources = [\"../../outside.c\"]\n"
                                  "cpp-sources = [\"../../outside.cpp\"]\n"
                                  "link-files = [\"../../outside.a\"]\n")),
         "the escaping native-path manifest should be writable");
  lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifestPath);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1104") != nullptr,
         "structured native paths must remain inside the package");

  std::error_code symlinkError;
  std::filesystem::create_directories(temporary.root() / "external/include");
  std::filesystem::create_directory_symlink(temporary.root() / "external",
                                            package / "vendor", symlinkError);
  if (!symlinkError) {
    expect(writeFile(manifestPath,
                     nativeManifest("[package.native]\n"
                                    "include-dirs = [\"vendor/include\"]\n")),
           "the symlink-escaping native manifest should be writable");
    loaded = lang::driver::loadProjectManifest(manifestPath);
    expect(findDiagnostic(loaded.diagnostics, "GTI-B1104") != nullptr,
           "native path containment should resolve symbolic-link escapes");
  }

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "compile-args = [\"-isystem\", "
                                  "\"../trusted-external/include\"]\n"
                                  "link-args = [\"-Wl,-rpath,../trusted\"]\n"
                                  "raw-args = [\"../opaque-operand\"]\n")),
         "the trusted path-bearing argument manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  expect(
      loaded.succeeded(),
      "trusted exact argument fields should not pretend to containment-check "
      "embedded or positional paths");

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "raw-args = [\"@flags.rsp\", \"-o\", "
                                  "\"-oelsewhere\", \"-c\", \"-x\"]\n"
                                  "compile-args = [7, \"-std=c++20\", \"-O3\", "
                                  "\"-Oexperimental\", \"-ansi\", "
                                  "\"--config=flags.cfg\"]\n"
                                  "c-standard = \"c99\"\n"
                                  "c-compile-args = [\"-std=c11\", \"-O2\"]\n"
                                  "link-args = [\"--output=elsewhere\", "
                                  "\"-Wl,@link.rsp\", \"-Wl,-ojoined\", "
                                  "\"-Wl,-shared\", \"-emit-llvm\", "
                                  "\"-Wl,-z,defs,-o,elsewhere\", "
                                  "\"-Xlinker=--output=elsewhere\"]\n")),
         "the policy-override native manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1005") != nullptr,
         "native argument escape hatches must not override resolved build "
         "policy, outputs, modes, or response-file inputs");
  bool exactArgumentSpan = false;
  bool exactForwardedSpan = false;
  bool exactJoinedOutputSpan = false;
  const std::string *argumentSource =
      loaded.sources.find(std::filesystem::canonical(manifestPath).string());
  for (const lang::Diagnostic &diagnostic : loaded.diagnostics) {
    if (diagnostic.code != "GTI-B1005" || argumentSource == nullptr ||
        diagnostic.primary.end > argumentSource->size()) {
      continue;
    }
    const std::string_view spelling(*argumentSource);
    const std::string_view selected =
        spelling.substr(diagnostic.primary.start,
                        diagnostic.primary.end - diagnostic.primary.start);
    if (selected == "\"-std=c++20\"") {
      exactArgumentSpan = true;
    }
    if (selected == "\"-Wl,-z,defs,-o,elsewhere\"") {
      exactForwardedSpan = true;
    }
    if (selected == "\"-Wl,-ojoined\"") {
      exactJoinedOutputSpan = true;
    }
  }
  expect(exactArgumentSpan && exactForwardedSpan && exactJoinedOutputSpan,
         "native argument diagnostics should retain the exact offending "
         "element span after an earlier invalid array element, including a "
         "non-first or joined forwarded linker output mode");

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "c-sources = [\"src/main.gti\"]\n")),
         "the invalid C-source-extension manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  const lang::Diagnostic *invalidCSource =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(invalidCSource != nullptr &&
             invalidCSource->message.find("'.c' extension") !=
                 std::string::npos,
         "native C sources should reject files without the exact .c extension");

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "cpp-sources = [\"src/main.gti\"]\n")),
         "the invalid C++-source-extension manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  const lang::Diagnostic *invalidCppSource =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(invalidCppSource != nullptr &&
             invalidCppSource->message.find("'.cpp'") != std::string::npos,
         "native C++ sources should reject files without a supported C++ "
         "extension");

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "libraries = [\"bad\\u0000name\"]\n"
                                  "frameworks = [\"bad\\u0001name\", "
                                  "\"bad/name\"]\n"
                                  "raw-args = [\"bad\\u0000arg\", "
                                  "\"line\\nbreak\", \"escape\\u001b\"]\n")),
         "the native control-character manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1005") != nullptr,
         "native names and exact argv elements must reject controls that "
         "cannot be preserved through process invocation");
  bool exactFrameworkSpan = false;
  const std::string *controlSource =
      loaded.sources.find(std::filesystem::canonical(manifestPath).string());
  for (const lang::Diagnostic &diagnostic : loaded.diagnostics) {
    if (diagnostic.code != "GTI-B1005" || controlSource == nullptr ||
        diagnostic.primary.end > controlSource->size()) {
      continue;
    }
    const std::string_view selected(*controlSource);
    if (selected.substr(diagnostic.primary.start,
                        diagnostic.primary.end - diagnostic.primary.start) ==
        "\"bad/name\"") {
      exactFrameworkSpan = true;
      break;
    }
  }
  expect(exactFrameworkSpan,
         "structured native-name diagnostics should select the exact invalid "
         "array element");

  expect(writeFile(manifestPath, nativeManifest("[package.native]\n"
                                                "libaries = [\"typo\"]\n"
                                                "[[package.native.platforms]]\n"
                                                "os = \"linux\"\n"
                                                "libraries = [\"one\"]\n"
                                                "[[package.native.platforms]]\n"
                                                "os = \"linux\"\n"
                                                "libraries = [\"two\"]\n")),
         "the duplicate-selector native manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  const lang::Diagnostic *unknown =
      findDiagnostic(loaded.diagnostics, "GTI-B1001");
  expect(unknown != nullptr && !unknown->hints.empty() &&
             unknown->hints.front().find("libraries") != std::string::npos &&
             findDiagnostic(loaded.diagnostics, "GTI-B1005") != nullptr,
         "unknown native keys should suggest known fields and duplicate exact "
         "platform selectors should be rejected");

  expect(writeFile(manifestPath, nativeManifest("[package.native]\n"
                                                "[[package.native.platforms]]\n"
                                                "libraries = [\"wildcard\"]\n"
                                                "[[package.native.platforms]]\n"
                                                "os = 42\n")),
         "the malformed platform-selector manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  expect(findDiagnostic(loaded.diagnostics, "GTI-B1004") != nullptr &&
             findDiagnostic(loaded.diagnostics, "GTI-B1005") != nullptr,
         "platform entries must have at least one well-typed non-empty exact "
         "TargetInfo selector and can never degrade into a wildcard");

  std::filesystem::create_directories(package / "linux-only/include");
  const std::string conditional =
      nativeManifest("[package.native]\n"
                     "[[package.native.platforms]]\n"
                     "os = \"linux\"\n"
                     "include-dirs = [\"linux-only/include\"]\n"
                     "link-files = [\"src\"]\n"
                     "[[package.native.platforms]]\n"
                     "os = \"macos\"\n"
                     "frameworks = [\"CoreFoundation\"]\n");
  expect(writeFile(manifestPath, conditional),
         "the target-selected native manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifestPath);
  expect(loaded.succeeded(),
         "unselected platform path kinds should not be validated against the "
         "process host");

  lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "windows", .vendor = "pc", .arch = "x86_64"}));
  expect(resolution.succeeded(),
         "platform fragments must select from the explicit TargetInfo rather "
         "than the process host");
  resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "linux", .vendor = "unknown", .arch = "x86_64"}));
  const lang::Diagnostic *invalidLinkFile =
      findDiagnostic(resolution.diagnostics, "GTI-B1103");
  expect(invalidLinkFile != nullptr &&
             invalidLinkFile->message.find("link file") != std::string::npos,
         "a selected exact link file must exist and be a regular file");
  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "c-sources = [\"native/missing.c\"]\n")),
         "the missing selected C-source manifest should be writable");
  resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "linux", .vendor = "unknown", .arch = "x86_64"}));
  const lang::Diagnostic *missingCSource =
      findDiagnostic(resolution.diagnostics, "GTI-B1103");
  expect(missingCSource != nullptr &&
             missingCSource->message.find("C source") != std::string::npos,
         "a selected native C source must exist and be a regular file");
  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "cpp-sources = [\"native/missing.cpp\"]\n")),
         "the missing selected C++-source manifest should be writable");
  resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "linux", .vendor = "unknown", .arch = "x86_64"}));
  const lang::Diagnostic *missingCppSource =
      findDiagnostic(resolution.diagnostics, "GTI-B1103");
  expect(missingCppSource != nullptr &&
             missingCppSource->message.find("C++ source") != std::string::npos,
         "a selected native C++ source must exist and be a regular file");
  expect(writeFile(manifestPath, conditional),
         "the target-selected native manifest should be restorable");
  resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "macos", .vendor = "apple", .arch = "arm64"}));
  expect(resolution.succeeded() && resolution.plan &&
             resolution.plan->nativeInputs().frameworks ==
                 std::vector<std::string>({"CoreFoundation"}),
         "frameworks should resolve only for an explicitly selected macOS "
         "target");

  expect(writeFile(manifestPath,
                   nativeManifest("[package.native]\n"
                                  "frameworks = [\"CoreFoundation\"]\n")),
         "the unconditional framework manifest should be writable");
  resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          package, std::nullopt, "dev",
          {.os = "linux", .vendor = "unknown", .arch = "x86_64"}));
  expect(findDiagnostic(resolution.diagnostics, "GTI-B1400") != nullptr,
         "effective frameworks should be rejected for non-macOS targets");
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

void testWorkspaceAndPathDependencies() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  const std::filesystem::path app = root / "packages/app";
  const std::filesystem::path math = root / "packages/math";
  expect(
      writeFile(root / "src/main.gti", "int main() { return 0; }\n") &&
          writeFile(app / "src/main.gti",
                    "#include <math/add>\nint main() { return 0; }\n") &&
          writeFile(math / "lib/add.gti",
                    "namespace math { int add(int a, int b) { return a + b; } "
                    "}\n"),
      "workspace source fixtures should be writable");

  const std::string rootManifest = R"(manifest-version = 1

[package]
name = "workspace_root"
version = "0.1.0"

[targets.root]
kind = "executable"
root = "src/main.gti"

[workspace]
members = ["packages/math", "packages/app"]
)";
  const std::string appManifest = R"(manifest-version = 1

[package]
name = "app"
version = "1.0.0"

[dependencies]
math = { path = "../math" }

[targets.app]
kind = "executable"
root = "src/main.gti"
)";
  const std::string mathManifest = R"(manifest-version = 1

[package]
name = "math"
version = "2.0.0"
source-root = "lib"
)";
  expect(writeFile(root / "gti.toml", rootManifest) &&
             writeFile(app / "gti.toml", appManifest) &&
             writeFile(math / "gti.toml", mathManifest),
         "workspace manifests should be writable");

  lang::driver::WorkspaceResolutionResult workspace =
      lang::driver::resolveProjectWorkspace(app / "src");
  expect(workspace.succeeded() && workspace.workspace &&
             workspace.workspace->declared() &&
             workspace.workspace->root() == std::filesystem::canonical(root) &&
             workspace.workspace->selectedPackage().manifest.package().name ==
                 "app" &&
             workspace.workspace->packages().size() == 3,
         "workspace discovery should select the containing member and load a "
         "deterministic package graph");
  if (workspace.workspace) {
    const std::vector<lang::PackageSourceRoot> roots =
        workspace.workspace->packageSourceRoots();
    const auto appRoot = std::find_if(
        roots.begin(), roots.end(), [](const lang::PackageSourceRoot &source) {
          return source.name == "app";
        });
    expect(appRoot != roots.end() && appRoot->dependencies.size() == 1 &&
               appRoot->dependencies.front().alias == "math" &&
               appRoot->dependencies.front().targetIdentity == "math@2.0.0",
           "workspace source roots should retain direct aliases and stable "
           "package identities");
    const auto mathRoot = std::find_if(
        roots.begin(), roots.end(), [](const lang::PackageSourceRoot &source) {
          return source.name == "math";
        });
    expect(mathRoot != roots.end() &&
               mathRoot->sourceRoot == std::filesystem::canonical(math / "lib"),
           "an explicit package source-root should control angle-unit "
           "resolution without requiring a build target");
  }

  lang::driver::ProjectResolutionResult build =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", lang::TargetInfo::host(), {},
          std::string("app")));
  expect(build.succeeded() && build.plan &&
             build.plan->packageName() == "app" &&
             build.plan->workspaceRoot() == std::filesystem::canonical(root) &&
             build.plan->output().string().find("build/gti/packages/app/dev") !=
                 std::string::npos &&
             build.plan->packageSources().size() == 3,
         "explicit package selection should create an immutable plan under "
         "the shared workspace output root");

  workspace = lang::driver::resolveProjectWorkspace(
      root, std::optional<std::string>("missing"));
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::SelectionFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1607") != nullptr,
         "unknown workspace package selection should be diagnosed");

  const std::string cyclicMathManifest = R"(manifest-version = 1

[package]
name = "math"
version = "2.0.0"
source-root = "lib"

[dependencies]
app = { path = "../app" }
)";
  expect(writeFile(math / "gti.toml", cyclicMathManifest),
         "cyclic dependency fixture should be writable");
  workspace = lang::driver::resolveProjectWorkspace(math / "lib");
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::GraphFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1603") != nullptr,
         "package dependency cycles should fail before target selection");

  const std::string duplicateAliasManifest = R"(manifest-version = 1

[package]
name = "app"
version = "1.0.0"

[dependencies]
math = { path = "../math" }
same_math = { path = "../math" }

[targets.app]
kind = "executable"
root = "src/main.gti"
)";
  expect(writeFile(math / "gti.toml", mathManifest) &&
             writeFile(app / "gti.toml", duplicateAliasManifest),
         "duplicate dependency root fixture should be writable");
  workspace = lang::driver::resolveProjectWorkspace(root);
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::GraphFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1602") != nullptr,
         "one package should not bind two aliases to the same canonical "
         "dependency root");

  const std::string duplicateNameManifest = R"(manifest-version = 1

[package]
name = "app"
version = "2.0.0"
source-root = "lib"
)";
  expect(writeFile(app / "gti.toml", appManifest) &&
             writeFile(math / "gti.toml", duplicateNameManifest),
         "duplicate package name fixture should be writable");
  workspace = lang::driver::resolveProjectWorkspace(root);
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::GraphFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1604") != nullptr,
         "workspace package names should be unique across canonical roots");

  const std::string nestedWorkspaceManifest = mathManifest + "\n[workspace]\n"
                                                             "members = []\n";
  expect(writeFile(math / "gti.toml", nestedWorkspaceManifest),
         "nested workspace fixture should be writable");
  workspace = lang::driver::resolveProjectWorkspace(math / "lib");
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::GraphFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1600") != nullptr,
         "workspace members should not introduce nested workspace roots");
}

void testProjectScaffolding() {
  TemporaryDirectory temporary;
  const std::string expectedSource = R"(#include <std/string>
#include <std/vector>

int main(int argc, std::vector<std::string> argv) {
  std::println("Hello, GTI!");
  return 0;
}
)";
  const std::filesystem::path createdRoot =
      temporary.root() / "created-package";
  lang::driver::ProjectScaffoldResult scaffold =
      lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
          lang::driver::ProjectScaffoldMode::NewPackage, createdRoot));
  expect(scaffold.succeeded() && scaffold.packageName == "created-package" &&
             scaffold.createdSource &&
             std::filesystem::is_regular_file(createdRoot / "gti.toml") &&
             std::filesystem::is_regular_file(createdRoot / "src/main.gti") &&
             readFile(createdRoot / "src/main.gti") == expectedSource,
         "new-package scaffolding should create a manifest and entry source");
  const lang::driver::ManifestLoadResult createdManifest =
      lang::driver::loadProjectManifest(createdRoot / "gti.toml");
  expect(createdManifest.succeeded() && createdManifest.manifest &&
             createdManifest.manifest->package().name == "created-package" &&
             createdManifest.manifest->findTarget("created-package") != nullptr,
         "new-package scaffolding should produce a valid schema version 1 "
         "manifest");
  const std::string originalManifest = readFile(createdRoot / "gti.toml");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::NewPackage, createdRoot));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1501") != nullptr &&
             readFile(createdRoot / "gti.toml") == originalManifest,
         "new-package scaffolding should refuse an existing destination "
         "without changing it");

  const std::filesystem::path invalidRoot = temporary.root() / "123-invalid";
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::NewPackage, invalidRoot));
  expect(scaffold.status ==
                 lang::driver::ProjectScaffoldStatus::InvalidRequest &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1500") != nullptr &&
             !std::filesystem::exists(invalidRoot),
         "an invalid derived package name should fail before creating files");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::NewPackage, invalidRoot,
      std::string("valid_name")));
  expect(scaffold.succeeded() && scaffold.packageName == "valid_name",
         "an explicit portable name should permit a differently named "
         "destination directory");

  const std::filesystem::path existingRoot = temporary.root() / "existing";
  const std::filesystem::path existingSource = existingRoot / "src/main.gti";
  const std::string existingContents = "int main() { return 7; }\n";
  expect(writeFile(existingSource, existingContents),
         "the existing init source fixture should be writable");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, existingRoot,
      std::string("initialized")));
  expect(scaffold.succeeded() && !scaffold.createdSource &&
             readFile(existingSource) == existingContents &&
             lang::driver::loadProjectManifest(existingRoot / "gti.toml")
                 .succeeded(),
         "init scaffolding should preserve an existing entry source and add "
         "only the manifest");
  const std::string initializedManifest = readFile(existingRoot / "gti.toml");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, existingRoot,
      std::string("initialized")));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1503") != nullptr &&
             readFile(existingRoot / "gti.toml") == initializedManifest &&
             readFile(existingSource) == existingContents,
         "init scaffolding should refuse to replace an existing manifest");

  const std::filesystem::path emptyRoot = temporary.root() / "empty";
  std::filesystem::create_directory(emptyRoot);
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, emptyRoot));
  expect(scaffold.succeeded() && scaffold.createdSource &&
             std::filesystem::is_regular_file(emptyRoot / "src/main.gti") &&
             readFile(emptyRoot / "src/main.gti") == expectedSource,
         "init scaffolding should create an entry source when one is absent");

  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory,
      temporary.root() / "missing"));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1502") != nullptr,
         "init scaffolding should require an existing directory");

  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory,
      temporary.root().root_path(), std::string("root_package")));
  expect(scaffold.status ==
                 lang::driver::ProjectScaffoldStatus::InvalidRequest &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1500") != nullptr,
         "project scaffolding should always refuse a filesystem root");

  const std::filesystem::path symlinkTarget =
      temporary.root() / "symlink-target";
  const std::filesystem::path symlinkRoot = temporary.root() / "symlink-root";
  std::filesystem::create_directory(symlinkTarget);
  std::error_code symlinkError;
  std::filesystem::create_directory_symlink(symlinkTarget, symlinkRoot,
                                            symlinkError);
  if (!symlinkError) {
    scaffold =
        lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
            lang::driver::ProjectScaffoldMode::ExistingDirectory, symlinkRoot,
            std::string("symlink_package")));
    expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
               findDiagnostic(scaffold.diagnostics, "GTI-B1502") != nullptr &&
               !std::filesystem::exists(symlinkTarget / "gti.toml"),
           "init scaffolding should refuse a symlink destination without "
           "following or modifying it");
  }
}

} // namespace

int main() {
  lang::installCrashHandlers("gti_project_tests");
  testDiscoveryParsingAndResolution();
  testManifestDiagnostics();
  testTargetSelectionDiagnostics();
  testTestTargetResolution();
  testNativeManifestResolution();
  testNativeManifestDiagnostics();
  testCleanSafety();
  testWorkspaceAndPathDependencies();
  testProjectScaffolding();

  if (failures != 0) {
    std::cerr << failures << " project test(s) failed\n";
    return 1;
  }
  std::cout << "All project tests passed\n";
  return 0;
}
