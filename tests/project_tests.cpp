#include "gti/driver/build_script.h"
#include "gti/driver/dependencies.h"
#include "gti/driver/manifest.h"
#include "gti/driver/project.h"
#include "gti/driver/project_facts.h"
#include "gti/format_config.h"
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
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(
    !std::is_constructible_v<
        lang::driver::ProjectWorkspace, std::filesystem::path,
        std::vector<lang::driver::ResolvedProjectPackage>, std::string, bool>);

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
         "disabled-warnings = [\"unused-mut\"]\n"
         "keep-cpp = true\n"
         "debug-info = true\n"
         "dependency-warnings = false\n"
         "raw-pointer-audit = \"warn\"\n";
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

  const std::filesystem::path redirected = temporary.root() / "redirected";
  expect(writeFile(redirected / "gti.toml", validManifest()),
         "the redirected manifest fixture should be writable");
  const std::filesystem::path redirectedMember =
      temporary.root() / "redirected-member";
  std::filesystem::create_directory(redirectedMember);
  symlinkError.clear();
  std::filesystem::create_symlink(redirected / "gti.toml",
                                  redirectedMember / "gti.toml", symlinkError);
  if (!symlinkError) {
    const lang::driver::ManifestDiscoveryResult redirectedLocal =
        lang::driver::discoverProjectManifest(redirectedMember);
    expect(redirectedLocal.status ==
                   lang::driver::ManifestDiscoveryStatus::FilesystemFailure &&
               !redirectedLocal.path &&
               findDiagnostic(redirectedLocal.diagnostics, "GTI-B1101") !=
                   nullptr,
           "manifest discovery should reject a valid local gti.toml symlink "
           "before canonicalization can redirect package ownership");
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
               !development->keepCpp && !development->debugInfo &&
               development->defines.empty() && release &&
               release->defines == lang::ConfigurationFlags({"NDEBUG"}) &&
               development->dependencyWarnings,
           "the built-in dev profile should have stable defaults");
    expect(
        development &&
            development->rawPointerAudit == lang::RawPointerAuditMode::Off &&
            release &&
            release->rawPointerAudit == lang::RawPointerAuditMode::Warn,
        "raw pointer auditing defaults off and retains exact profile policy");
    expect(release != nullptr &&
               release->optimization == lang::OptimizationLevel::O2 &&
               release->cppStandard == lang::CppStandard::Cpp20 &&
               release->executionProfile ==
                   lang::ExecutionProfile::Concurrent &&
               release->warningPolicy.disabledGroups() ==
                   std::vector<std::string>{"unused-mut"} &&
               release->keepCpp && release->debugInfo &&
               !release->dependencyWarnings,
           "manifest profile fields should refine built-in defaults");
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = lang::OptimizationLevel::O3;
  overrides.cppStandard = lang::CppStandard::Cpp23;
  overrides.executionProfile = lang::ExecutionProfile::SingleThreaded;
  overrides.rawPointerAudit = lang::RawPointerAuditMode::Strict;
  overrides.keepCpp = false;
  overrides.debugInfo = false;
  overrides.warningPolicy = lang::WarningPolicy({"unreachable-code"});
  const lang::TargetInfo targetInfo{
      .os = "testos", .vendor = "testvendor", .arch = "testarch"};
  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          nested, std::nullopt, "release", targetInfo, overrides));
  expect(resolution.succeeded(),
         "one target should be inferred from a nested project directory");
  if (resolution.plan) {
    const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
    expect(plan.rawPointerAudit() == lang::RawPointerAuditMode::Strict,
           "explicit audit override wins over the profile");
    const std::filesystem::path expectedDirectory =
        std::filesystem::canonical(temporary.root()) /
        "build/gti/release/testarch-testvendor-testos";
    expect(plan.entry() == std::filesystem::canonical(source) &&
               plan.output().parent_path() == expectedDirectory &&
               plan.generatedSource() ==
                   expectedDirectory / ".gti-intermediate/game.gti.cpp",
           "project artifacts should use the deterministic project layout");
    expect(
        plan.optimization() == lang::OptimizationLevel::O3 &&
            plan.cppStandard() == lang::CppStandard::Cpp23 &&
            plan.target().executionProfile ==
                lang::ExecutionProfile::SingleThreaded &&
            plan.warnings().disabledGroups() ==
                (std::vector<std::string>{"unreachable-code", "unused-mut"}) &&
            !plan.keepCpp() && !plan.debugInfo() &&
            !plan.packageWarnings().dependencyWarnings() &&
            plan.packageWarnings().selectedPackage() == "engine_tools@0.1.0",
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

  const lang::driver::ProjectResolutionResult hostedRelease =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          nested, std::nullopt, "release", lang::TargetInfo::host(), {},
          std::nullopt, {}, lang::driver::RuntimeProfile::Hosted));
  expect(hostedRelease.succeeded() && hostedRelease.plan &&
             hostedRelease.plan->target().executionProfile ==
                 lang::ExecutionProfile::Concurrent &&
             hostedRelease.plan->target().runtimeCapabilities.threads,
         "a concurrent manifest profile should retain hosted thread support "
         "through the shared project analysis configuration");
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

  std::string invalidWarning = validManifest();
  for (const std::string spelling : {"true", "3", "[]", "\"unknown\""}) {
    std::string invalidAudit = validManifest();
    const auto valueOffset = invalidAudit.find("\"warn\"");
    invalidAudit.replace(valueOffset, 6, spelling);
    expect(writeFile(manifest, invalidAudit),
           "invalid audit fixture should be writable");
    const auto badAudit = lang::driver::loadProjectManifest(manifest);
    const auto *diagnostic =
        findDiagnostic(badAudit.diagnostics,
                       spelling == "\"unknown\"" ? "GTI-B1005" : "GTI-B1004");
    expect(!badAudit.succeeded() && diagnostic &&
               diagnostic->primary.start == valueOffset &&
               diagnostic->primary.end == valueOffset + spelling.size(),
           "invalid audit policy reports the exact manifest value");
  }
  invalidWarning.replace(invalidWarning.find("unused-mut"),
                         std::string_view("unused-mut").size(),
                         "unused-mutable-binding");
  expect(writeFile(manifest, invalidWarning),
         "the invalid warning-group fixture should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *unknownWarning =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(unknownWarning != nullptr &&
             unknownWarning->message.find("unknown warning group") !=
                 std::string::npos &&
             loaded.sources.locate(unknownWarning->primary).line == 15,
         "manifest warning suppression should reject unknown groups at the "
         "array element");

  for (const std::string spelling : {"1", "0", "\"false\"", "[]"}) {
    std::string invalidDependencyWarnings = validManifest();
    const std::size_t valueOffset = invalidDependencyWarnings.find("false");
    invalidDependencyWarnings.replace(valueOffset, 5, spelling);
    expect(writeFile(manifest, invalidDependencyWarnings),
           "the invalid dependency-warning fixture should be writable");
    loaded = lang::driver::loadProjectManifest(manifest);
    const lang::Diagnostic *invalidDependencyWarning =
        findDiagnostic(loaded.diagnostics, "GTI-B1004");
    expect(!loaded.succeeded() && invalidDependencyWarning != nullptr &&
               invalidDependencyWarning->message.find(
                   "'dependency-warnings' must be a Boolean") !=
                   std::string::npos &&
               invalidDependencyWarning->primary.start == valueOffset &&
               invalidDependencyWarning->primary.end ==
                   valueOffset + spelling.size(),
           "dependency-warnings should reject non-Booleans at the exact "
           "manifest value: " +
               spelling);
  }

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

  const std::string collidingTargets =
      "manifest-version = 1\n"
      "[package]\nname = \"sample\"\nversion = \"1.0.0\"\n"
      "[targets.Tool]\nkind = \"executable\"\nroot = \"src/main.gti\"\n"
      "[targets.tool]\nkind = \"executable\"\nroot = \"src/main.gti\"\n";
  expect(writeFile(manifest, collidingTargets),
         "the portable target-collision manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *targetCollision =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(targetCollision != nullptr &&
             targetCollision->message.find(
                 "case-insensitive artifact identity") != std::string::npos &&
             !targetCollision->related.empty(),
         "case-only target names should be rejected before they can publish "
         "the same artifact on a supported filesystem");

  const std::string collidingProfiles =
      validManifest("\n[profiles.Fast]\noptimization = 1\n"
                    "[profiles.fast]\noptimization = 2\n");
  expect(writeFile(manifest, collidingProfiles),
         "the portable profile-collision manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *profileCollision =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(profileCollision != nullptr &&
             profileCollision->message.find(
                 "case-insensitive artifact identity") != std::string::npos,
         "case-only profile names should be rejected before sharing a build "
         "directory on a supported filesystem");

  const std::string reservedPackage =
      "manifest-version = 1\n"
      "[package]\nname = \"CON\"\nversion = \"1.0.0\"\n";
  expect(writeFile(manifest, reservedPackage),
         "the reserved portable-name manifest should be writable");
  loaded = lang::driver::loadProjectManifest(manifest);
  const lang::Diagnostic *reservedName =
      findDiagnostic(loaded.diagnostics, "GTI-B1005");
  expect(reservedName != nullptr &&
             reservedName->message.find("reserved portable device name") !=
                 std::string::npos,
         "project artifact names should reject portable device names before "
         "filesystem publication");

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

void testConfigurationFlagManifest() {
  TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / "src/main.gti";
  const std::filesystem::path manifest = temporary.root() / "gti.toml";
  expect(writeFile(source, "#ifdef DEBUG\nint main() { return 0; }\n"
                           "#else\nint main() { return 1; }\n#endif\n"),
         "the configuration project source should be writable");
  expect(
      writeFile(manifest, validManifest() +
                              "\n[build]\ndefines = [\"FEATURE_X\", \"DEBUG\", "
                              "\"DEBUG\"]\n"),
      "the configuration manifest should be writable");

  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifest);
  expect(loaded.succeeded() && loaded.manifest &&
             loaded.manifest->build().defines ==
                 lang::ConfigurationFlags({"DEBUG", "FEATURE_X"}) &&
             loaded.manifest->build().defineDeclarations.size() == 2 &&
             loaded.manifest->build().defineDeclarations[0].start >
                 loaded.manifest->build().defineDeclarations[1].start,
         "[build].defines should validate, deduplicate, and normalize "
         "valueless flags without losing their declaration spans");

  lang::driver::ProjectBuildOverrides overrides;
  overrides.configurationFlags = {"CLI_FLAG", "DEBUG"};
  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          temporary.root(), std::nullopt, "dev", lang::TargetInfo::host(),
          std::move(overrides)));
  expect(resolution.succeeded() && resolution.plan &&
             resolution.plan->configurationFlags() ==
                 lang::ConfigurationFlags({"CLI_FLAG", "DEBUG", "FEATURE_X"}) &&
             resolution.plan->projectModelIdentity().find(
                 "configuration-flag:CLI_FLAG") != std::string::npos,
         "project plans should merge CLI and manifest defines into their "
         "compilation and model identity");

  expect(
      writeFile(manifest, validManifest() +
                              "\n[build]\ndefines = [\"GOOD\", \"not-valid!\", "
                              "\"class\"]\n"),
      "the invalid configuration manifest should be writable");
  const lang::driver::ManifestLoadResult invalid =
      lang::driver::loadProjectManifest(manifest);
  expect(!invalid.succeeded() &&
             findDiagnostic(invalid.diagnostics, "GTI-B1005") != nullptr,
         "manifest defines should reject names that are not ordinary GTI "
         "identifiers");
}

void testScopedConfigurationFlags() {
  TemporaryDirectory temporary;
  const auto root = temporary.root();
  const auto manifest = root / "gti.toml";
  expect(writeFile(root / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(root / "src/test.gti", "int main() { return 0; }\n"),
         "scoped define roots should exist");
  const std::string text = validManifest(R"(
[targets.game]
kind = "executable"
root = "src/main.gti"
defines = ["GAME", "COMMON", "GAME"]
[targets.unit]
kind = "test"
root = "src/test.gti"
defines = ["UNIT"]
)") + R"(
[build]
defines = ["BASE", "COMMON"]
[profiles.dev]
defines = ["DEBUG", "COMMON", "DEBUG"]
)";
  expect(writeFile(manifest, text), "scoped defines manifest should exist");
  const auto loaded = lang::driver::loadProjectManifest(manifest);
  expect(loaded.succeeded(), "target and profile defines should be accepted");
  if (!loaded.manifest) {
    return;
  }
  const auto &game = loaded.manifest->targets().front();
  const auto &dev = loaded.manifest->profiles().front();
  expect(game.defines == lang::ConfigurationFlags({"COMMON", "GAME"}) &&
             game.defineDeclarations.size() == 2 &&
             text.substr(game.defineDeclarations[1].start,
                         game.defineDeclarations[1].end -
                             game.defineDeclarations[1].start) == "\"GAME\"" &&
             dev.defines == lang::ConfigurationFlags({"COMMON", "DEBUG"}) &&
             dev.defineDeclarations.size() == 2,
         "scoped defines should normalize names and preserve exact spans");

  const auto facts = lang::driver::resolveProjectFacts(root);
  expect(facts.succeeded() && facts.facts,
         "scoped project facts should resolve");
  if (!facts.facts) {
    return;
  }
  expect(facts.facts->configurationFlags() ==
                 lang::ConfigurationFlags({"BASE", "COMMON"}) &&
             facts.facts->findTarget("game") &&
             facts.facts->findTarget("game")->defines == game.defines &&
             facts.facts->findProfile("dev")->defines == dev.defines,
         "read-only facts should retain scoped defines separately");
  const auto &resolved = *facts.facts;
  const auto *gameTarget = resolved.targetForSource(root / "src/main.gti");
  const auto *unitTarget = resolved.targetForSource(root / "src/test.gti");
  expect(gameTarget && gameTarget->name == "game" && unitTarget &&
             unitTarget->name == "unit" &&
             resolved.targetForSource(root / "src/shared.gti") == nullptr,
         "editor target inference must not mix unrelated targets");
  for (const std::string profile : {"dev", "release"}) {
    for (const std::string target : {"game", "unit"}) {
      const auto plan =
          lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
              root, target, profile, lang::TargetInfo::host()));
      const auto configuration = resolved.analysisConfiguration(
          profile, lang::TargetInfo::host(), lang::driver::RuntimeProfile::None,
          target);
      expect(plan.succeeded() && plan.plan && configuration &&
                 plan.plan->configurationFlags() ==
                     configuration->configurationFlags() &&
                 plan.plan->declaredConfigurationFlags() ==
                     configuration->declaredConfigurationFlags() &&
                 configuration->declaredConfigurationFlags() ==
                     lang::ConfigurationFlags(
                         {"BASE", "COMMON", "DEBUG", "GAME", "UNIT"}),
             "build and editor configurations must agree for each selection");
      if (configuration) {
        const auto &flags = configuration->configurationFlags();
        expect(std::binary_search(flags.begin(), flags.end(), "DEBUG") ==
                       (profile == "dev") &&
                   std::binary_search(flags.begin(), flags.end(), "GAME") ==
                       (target == "game") &&
                   std::binary_search(flags.begin(), flags.end(), "UNIT") ==
                       (target == "unit"),
               "only selected target/profile defines may enter configuration");
      }
    }
  }
  const auto profileOnly = resolved.analysisConfiguration(
      "dev", lang::TargetInfo::host(), lang::driver::RuntimeProfile::None);
  expect(profileOnly &&
             profileOnly->configurationFlags() ==
                 lang::ConfigurationFlags({"BASE", "COMMON", "DEBUG"}) &&
             !resolved.analysisConfiguration("dev", lang::TargetInfo::host(),
                                             lang::driver::RuntimeProfile::None,
                                             "missing"),
         "unselected and unknown targets must not leak target configuration");

  lang::driver::ProjectBuildOverrides overrides;
  overrides.configurationFlags = {"CLI", "DEBUG", "COMMON"};
  const auto cli =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, "game", "dev", lang::TargetInfo::host(), overrides));
  expect(cli.succeeded() && cli.plan &&
             cli.plan->configurationFlags() ==
                 lang::ConfigurationFlags(
                     {"BASE", "CLI", "COMMON", "DEBUG", "GAME"}),
         "CLI defines should form a normalized union with all selected scopes");

  std::string ambiguous = text;
  ambiguous.replace(ambiguous.find("src/test.gti"), 12, "src/main.gti");
  expect(writeFile(manifest, ambiguous),
         "ambiguous entry fixture should exist");
  const auto ambiguousFacts = lang::driver::resolveProjectFacts(root);
  expect(ambiguousFacts.facts && ambiguousFacts.facts->targetForSource(
                                     root / "src/main.gti") == nullptr,
         "shared entry roots must not silently select either target's flags");
  expect(writeFile(manifest, validManifest()),
         "single-target fixture should exist");
  const auto single = lang::driver::resolveProjectFacts(root);
  expect(
      single.facts && single.facts->targetForSource(root / "src/shared.gti") &&
          single.facts->targetForSource(root / "src/shared.gti")->name ==
              "game",
      "a single-target package can supply its configuration to shared files");

  for (const std::string scope : {"target", "profile"}) {
    for (const std::string value : {"\"DEBUG\"", "[3]", "[\"not-valid!\"]",
                                    "[\"DEBUG=1\"]", "[\"class\"]", "[\"\"]"}) {
      const auto prefix = scope == "target"
                              ? "[targets.game]\nkind = \"executable\"\nroot = "
                                "\"src/main.gti\"\n"
                              : "[profiles.dev]\n";
      const auto invalidText = validManifest(scope == "target" ? "" : R"(
[targets.game]
kind = "executable"
root = "src/main.gti"
)") + "\n" + prefix +
                               "defines = " + value + "\n";
      expect(writeFile(manifest, invalidText),
             "invalid scoped fixture should exist");
      const auto invalid = lang::driver::loadProjectManifest(manifest);
      const auto code =
          (value == "\"DEBUG\"" || value == "[3]") ? "GTI-B1004" : "GTI-B1005";
      const auto *diagnostic = findDiagnostic(invalid.diagnostics, code);
      expect(!invalid.succeeded() && diagnostic &&
                 diagnostic->phase == lang::DiagnosticPhase::Driver &&
                 diagnostic->primary.source ==
                     std::filesystem::canonical(manifest).string() &&
                 diagnostic->primary.start ==
                     invalidText.find("defines = ") +
                         std::string_view("defines = ").size() +
                         (value.front() == '[' ? 1 : 0) &&
                 invalidText.substr(diagnostic->primary.start,
                                    diagnostic->primary.end -
                                        diagnostic->primary.start) ==
                     (value.front() == '[' ? value.substr(1, value.size() - 2)
                                           : value),
             "malformed " + scope +
                 " defines should have located driver errors");
    }
  }
}

void testDependencyConfigurationFlags() {
  TemporaryDirectory temporary;
  const auto root = temporary.root();
  const auto app = root / "app";
  const std::string prefix =
      "manifest-version = 1\n[package]\nversion = \"1.0.0\"\nname = ";
  const auto package = [&](std::string_view name, std::string_view tail) {
    return prefix + "\"" + std::string(name) + "\"\n" + std::string(tail);
  };
  expect(writeFile(root / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(app / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(root / "other/src/main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(root / "left/src/api.gti",
                       "namespace left { int api() { return 0; } }\n"
                       "int main() { return left::api(); }\n") &&
             writeFile(root / "right/src/api.gti", "namespace right {}\n") &&
             writeFile(root / "leaf/src/api.gti", "namespace leaf {}\n"),
         "dependency configuration sources should be writable");
  expect(
      writeFile(
          root / "gti.toml",
          package(
              "workspace_root",
              "[build]\ndefines = [\"ROOT_ONLY\"]\n"
              "[targets.root]\nkind = \"executable\"\nroot = \"src/main.gti\"\n"
              "[workspace]\nmembers = [\"app\", \"other\"]\n")) &&
          writeFile(app / "gti.toml",
                    package("app",
                            "[build]\ndefines = [\"APP\", \"COMMON\"]\n"
                            "[dependencies]\nleft = { path = \"../left\" }\n"
                            "right = { path = \"../right\" }\n"
                            "[targets.app]\nkind = \"executable\"\nroot = "
                            "\"src/main.gti\"\n")) &&
          writeFile(root / "other/gti.toml",
                    package("other",
                            "[build]\ndefines = [\"UNRELATED\"]\n"
                            "[targets.other]\nkind = \"executable\"\nroot = "
                            "\"src/main.gti\"\n")) &&
          writeFile(
              root / "left/gti.toml",
              package(
                  "left",
                  "[build]\ndefines = [\"LEFT\", \"COMMON\"]\n"
                  "[dependencies]\nleaf = { path = \"../leaf\" }\n"
                  "[profiles.dev]\ndefines = [\"DEPENDENCY_PROFILE\"]\n"
                  "[targets.left]\nkind = \"test\"\nroot = \"src/api.gti\"\n"
                  "defines = [\"DEPENDENCY_TARGET\"]\n")) &&
          writeFile(
              root / "right/gti.toml",
              package("right",
                      "[build]\ndefines = [\"RIGHT\"]\n"
                      "[dependencies]\nleaf = { path = \"../leaf\" }\n")) &&
          writeFile(
              root / "leaf/gti.toml",
              package("leaf", "[build]\ndefines = [\"LEAF\", \"COMMON\"]\n")),
      "dependency configuration manifests should be writable");
  const auto dependencyManifest =
      lang::driver::loadProjectManifest(root / "left/gti.toml");
  expect(
      dependencyManifest.succeeded(),
      "the dependency test-target fixture must satisfy the manifest contract");
  const auto facts = lang::driver::resolveProjectFacts(app);
  const lang::ConfigurationFlags expected{"APP", "COMMON", "LEAF", "LEFT",
                                          "RIGHT"};
  expect(facts.succeeded() && facts.facts &&
             facts.facts->configurationFlags() == expected,
         "configuration flags must include transitive/diamond dependencies "
         "once, without workspace-root or unrelated-member flags");
  lang::driver::ProjectBuildOverrides overrides;
  overrides.configurationFlags = {"CLI", "COMMON"};
  const auto build =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          app, std::nullopt, "dev", lang::TargetInfo::host(), overrides));
  expect(build.succeeded() && build.plan &&
             build.plan->configurationFlags() ==
                 lang::ConfigurationFlags(
                     {"APP", "CLI", "COMMON", "LEAF", "LEFT", "RIGHT"}),
         "build configuration must share dependency facts and merge CLI flags");
  expect(build.plan && build.plan->declaredConfigurationFlags() == expected,
         "dependency target/profile flags must not suppress unknown-flag "
         "diagnostics in the consumer");
  const auto rootFacts =
      lang::driver::resolveProjectFacts(root, "workspace_root");
  expect(rootFacts.succeeded() && rootFacts.facts &&
             rootFacts.facts->configurationFlags() ==
                 lang::ConfigurationFlags({"ROOT_ONLY"}),
         "selecting another workspace package must recompute its own closure");
  expect(writeFile(
             root / "leaf/gti.toml",
             package("leaf", "[build]\ndefines = [\"UPDATED\", \"COMMON\"]\n")),
         "dependency configuration mutation should be writable");
  const auto changed =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          app, std::nullopt, "dev", lang::TargetInfo::host(), overrides));
  expect(changed.succeeded() && changed.plan && build.plan &&
             changed.plan->projectModelIdentity() !=
                 build.plan->projectModelIdentity(),
         "changing a dependency's flag set must invalidate build identity");
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

  const std::string intermediateTarget =
      "\n[targets.intermediate]\nkind = \"executable\"\n"
      "root = \"src/alpha.gti\"\n";
  expect(writeFile(temporary.root() / "gti.toml",
                   validManifest(intermediateTarget)),
         "the intermediate-named target manifest should be writable");
  result = lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
      temporary.root(), std::nullopt, "dev", host));
  expect(result.succeeded() && result.plan &&
             result.plan->output().filename() == "intermediate" &&
             result.plan->generatedSource().parent_path().filename() ==
                 ".gti-intermediate",
         "a user target named 'intermediate' should not collide with the "
         "driver-owned generated-source directory");

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
                                  "\"-oelsewhere\", \"-c\", \"-x\", "
                                  "\"--target=other-unknown-os\", "
                                  "\"-Xclang\", \"--driver-mode=cl\"]\n"
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
  bool exactTargetSpan = false;
  bool exactDriverEscapeSpan = false;
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
    if (selected == "\"--target=other-unknown-os\"") {
      exactTargetSpan = true;
    }
    if (selected == "\"-Xclang\"") {
      exactDriverEscapeSpan = true;
    }
  }
  expect(exactArgumentSpan && exactForwardedSpan && exactJoinedOutputSpan &&
             exactTargetSpan && exactDriverEscapeSpan,
         "native argument diagnostics should retain the exact offending "
         "element span after an earlier invalid array element, including "
         "target selection, raw native-driver escapes, and a non-first or "
         "joined forwarded linker output mode");

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
name = "App"
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
         "workspace package names should be unique under portable "
         "case-insensitive identity across canonical roots");

  const std::string nestedWorkspaceManifest = mathManifest + "\n[workspace]\n"
                                                             "members = []\n";
  expect(writeFile(math / "gti.toml", nestedWorkspaceManifest),
         "nested workspace fixture should be writable");
  workspace = lang::driver::resolveProjectWorkspace(math / "lib");
  expect(workspace.status ==
                 lang::driver::WorkspaceResolutionStatus::GraphFailure &&
             findDiagnostic(workspace.diagnostics, "GTI-B1600") != nullptr,
         "workspace members should not introduce nested workspace roots");

  const std::filesystem::path redirected = root / "redirected-package";
  const std::string redirectedManifest = R"(manifest-version = 1

[package]
name = "redirected"
version = "1.0.0"
)";
  expect(writeFile(redirected / "gti.toml", redirectedManifest),
         "the redirected package manifest fixture should be writable");
  std::error_code memberSymlinkError;
  std::filesystem::remove(math / "gti.toml", memberSymlinkError);
  memberSymlinkError.clear();
  std::filesystem::create_symlink(redirected / "gti.toml", math / "gti.toml",
                                  memberSymlinkError);
  if (!memberSymlinkError) {
    workspace = lang::driver::resolveProjectWorkspace(root);
    expect(workspace.status ==
                   lang::driver::WorkspaceResolutionStatus::ManifestFailure &&
               findDiagnostic(workspace.diagnostics, "GTI-B1608") != nullptr,
           "a workspace member manifest symlink must not redirect package "
           "ownership outside the declared canonical member directory");
  }

  const std::filesystem::path dependencyApp = root / "dependency-app";
  const std::filesystem::path declaredDependency = root / "declared-dependency";
  const std::filesystem::path externalDependency = root / "external-dependency";
  const std::string dependencyAppManifest = R"(manifest-version = 1

[package]
name = "dependency_app"
version = "1.0.0"

[dependencies]
redirected = { path = "../declared-dependency" }

[targets.app]
kind = "executable"
root = "src/main.gti"
)";
  const std::string externalDependencyManifest = R"(manifest-version = 1

[package]
name = "external_dependency"
version = "1.0.0"
)";
  expect(
      writeFile(dependencyApp / "src/main.gti", "int main() { return 0; }\n") &&
          writeFile(dependencyApp / "gti.toml", dependencyAppManifest) &&
          writeFile(externalDependency / "gti.toml",
                    externalDependencyManifest),
      "path-dependency redirection fixtures should be writable");
  std::filesystem::create_directories(declaredDependency);
  std::error_code dependencySymlinkError;
  std::filesystem::create_symlink(externalDependency / "gti.toml",
                                  declaredDependency / "gti.toml",
                                  dependencySymlinkError);
  if (!dependencySymlinkError) {
    workspace = lang::driver::resolveProjectWorkspace(dependencyApp);
    expect(workspace.status ==
                   lang::driver::WorkspaceResolutionStatus::GraphFailure &&
               findDiagnostic(workspace.diagnostics, "GTI-B1608") != nullptr,
           "a path-dependency manifest symlink must not redirect package "
           "ownership outside the declared canonical dependency directory");
  }
}

void testProjectFacts() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  const std::filesystem::path app = root / "packages/app";
  const std::filesystem::path math = root / "packages/math";
  expect(writeFile(root / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(app / "src/main.gti",
                       "#include <math/add>\nint main() { return 0; }\n") &&
             writeFile(math / "lib/add.gti",
                       "namespace math { int add(int a, int b) { return a + b; "
                       "} }\n"),
         "project-facts source fixtures should be writable");

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

[build]
defines = ["FEATURE_FAST"]

[dependencies]
math = { path = "../math" }

[targets.app]
kind = "executable"
root = "src/main.gti"

[targets.app_tests]
kind = "test"
root = "src/main.gti"

[profiles.release]
execution-profile = "concurrent"
raw-pointer-audit = "strict"
dependency-warnings = false
disabled-warnings = ["unused-mut"]
)";
  const std::string mathManifest = R"(manifest-version = 1

[package]
name = "math"
version = "2.0.0"
source-root = "lib"
[profiles.dev]
dependency-warnings = false
)";
  expect(writeFile(root / "gti.toml", rootManifest) &&
             writeFile(app / "gti.toml", appManifest) &&
             writeFile(math / "gti.toml", mathManifest),
         "project-facts manifests should be writable");

  const lang::driver::ProjectFactsResult resolved =
      lang::driver::resolveProjectFacts(app / "src");
  expect(resolved.succeeded() && resolved.facts,
         "project facts should resolve from a directory inside a member "
         "package");
  if (!resolved.facts) {
    return;
  }
  const lang::driver::ProjectFacts &facts = *resolved.facts;

  expect(facts.workspaceRoot() == std::filesystem::canonical(root) &&
             facts.declaredWorkspace() && facts.packages().size() == 3,
         "project facts should publish the declared workspace root and its "
         "full package closure");
  expect(facts.selectedPackage().identity == "app@1.0.0" &&
             facts.selectedPackage().membership ==
                 lang::driver::ProjectPackageMembership::Member &&
             facts.selectedPackage().selectable,
         "project facts should select the package containing the start "
         "directory");

  // The fact a consumer cannot reconstruct from one manifest, and the one an
  // editor that reads gti.toml itself silently loses.
  const std::vector<lang::PackageSourceRoot> &roots =
      facts.packageSourceRoots();
  const auto appRoot = std::find_if(roots.begin(), roots.end(),
                                    [](const lang::PackageSourceRoot &source) {
                                      return source.name == "app";
                                    });
  const auto mathRoot = std::find_if(roots.begin(), roots.end(),
                                     [](const lang::PackageSourceRoot &source) {
                                       return source.name == "math";
                                     });
  expect(appRoot != roots.end() && mathRoot != roots.end() &&
             appRoot->dependencies.size() == 1 &&
             appRoot->dependencies.front().alias == "math" &&
             appRoot->dependencies.front().targetIdentity == "math@2.0.0" &&
             mathRoot->sourceRoot == std::filesystem::canonical(math / "lib"),
         "project facts should publish the package source-root graph package "
         "includes resolve through");

  const lang::driver::ProjectFactsPackage *describedMath =
      facts.findPackage("math@2.0.0");
  expect(describedMath != nullptr && describedMath->name == "math" &&
             describedMath->version == "2.0.0" &&
             describedMath->membership ==
                 lang::driver::ProjectPackageMembership::Member &&
             describedMath->selectable &&
             describedMath->sourceRoot ==
                 std::filesystem::canonical(math / "lib") &&
             !describedMath->sourceIdentity.has_value(),
         "a declared workspace member should be described as selectable with "
         "its declared source root and no git provenance");

  const lang::driver::ProjectFactsPackage *describedApp =
      facts.findPackage("app@1.0.0");
  expect(describedApp != nullptr && describedApp->dependencies.size() == 1 &&
             describedApp->dependencies.front().alias == "math" &&
             describedApp->dependencies.front().targetIdentity == "math@2.0.0",
         "project facts should publish each package's direct alias edges");

  expect(facts.defaultProfileName() == "dev" &&
             facts.findProfile("dev") != nullptr &&
             facts.findProfile("release") != nullptr &&
             facts.findProfile("missing") == nullptr,
         "project facts should publish declared profiles and the profile a "
         "command selects when none is named");
  const lang::driver::ProjectFactsProfile *releaseProfile =
      facts.findProfile("release");
  expect(releaseProfile != nullptr &&
             releaseProfile->rawPointerAudit ==
                 lang::RawPointerAuditMode::Strict &&
             releaseProfile->executionProfile ==
                 lang::ExecutionProfile::Concurrent &&
             !releaseProfile->dependencyWarnings &&
             releaseProfile->warningPolicy.disabledGroups() ==
                 std::vector<std::string>{"unused-mut"},
         "a declared profile should carry its resolved execution and warning "
         "policies");
  const lang::driver::PackageWarningPolicy packageWarnings =
      facts.packageWarnings("release");
  expect(!packageWarnings.dependencyWarnings() &&
             packageWarnings.selectedPackage() == "app@1.0.0" &&
             packageWarnings.dependencyPackages() ==
                 (std::vector<std::string>{"math@2.0.0",
                                           "workspace_root@0.1.0"}) &&
             facts.packageWarnings("dev").dependencyWarnings() &&
             facts.packageWarnings("missing").dependencyWarnings(),
         "read-only facts should resolve selected-package warning ownership "
         "and preserve visible warnings for default or unknown profiles");

  expect(facts.configurationFlags().size() == 1 &&
             facts.configurationFlags().front() == "FEATURE_FAST",
         "project facts should publish package-level configuration flags");

  const std::optional<lang::driver::AnalysisConfiguration> configuration =
      facts.analysisConfiguration("release", lang::TargetInfo::host(),
                                  lang::driver::RuntimeProfile::Hosted);
  expect(configuration.has_value() &&
             configuration->configurationFlags().size() == 1 &&
             configuration->runtimeProfile() ==
                 lang::driver::RuntimeProfile::Hosted,
         "project facts should assemble an analysis configuration for a "
         "declared profile");
  expect(!facts
              .analysisConfiguration("missing", lang::TargetInfo::host(),
                                     lang::driver::RuntimeProfile::Hosted)
              .has_value(),
         "an undeclared profile should not produce an analysis configuration");

  const auto selectedMath =
      lang::driver::resolveProjectFacts(root, std::string("math"));
  expect(
      selectedMath.succeeded() && selectedMath.facts &&
          !selectedMath.facts->packageWarnings("dev").dependencyWarnings() &&
          selectedMath.facts->packageWarnings("dev").selectedPackage() ==
              "math@2.0.0" &&
          selectedMath.facts->packageWarnings("dev").dependencyPackages() ==
              (std::vector<std::string>{"app@1.0.0", "workspace_root@0.1.0"}),
      "explicit workspace-member selection should change warning ownership "
      "instead of treating every member as a dependency of the root");

  expect(facts.targets().size() == 2 && facts.targets().front().name == "app" &&
             facts.targets().front().kind ==
                 lang::driver::ProjectTargetKind::Executable &&
             facts.targets().back().kind ==
                 lang::driver::ProjectTargetKind::Test,
         "project facts should publish the selected package's declared targets "
         "and kinds");

  // Resolution is read-only: it plans nothing and creates no managed output.
  expect(!std::filesystem::exists(root / "build") &&
             !std::filesystem::exists(app / "build"),
         "resolving project facts should not create build directories");

  // The facts must be the build path's facts, not a second interpretation.
  const lang::driver::ProjectBuildRequest buildRequest(
      app / "src", std::nullopt, "release", lang::TargetInfo::host());
  const lang::driver::ProjectResolutionResult built =
      lang::driver::resolveProjectBuild(buildRequest);
  expect(built.succeeded() && built.plan, "the same project should build-plan");
  if (built.plan) {
    expect(built.plan->packageSources().size() == roots.size() &&
               built.plan->configurationFlags() == facts.configurationFlags() &&
               built.plan->packageWarnings().dependencyWarnings() ==
                   packageWarnings.dependencyWarnings() &&
               built.plan->packageWarnings().selectedPackage() ==
                   packageWarnings.selectedPackage() &&
               built.plan->packageWarnings().dependencyPackages() ==
                   packageWarnings.dependencyPackages(),
           "project facts should agree with the build plan about source roots "
           "and configuration flags");
    // A build plan extends the workspace-graph identity with configuration
    // and bindgen facts, so the facts' identity is its base rather than its
    // equal.
    expect(built.plan->projectModelIdentity().rfind(facts.modelIdentity(), 0) ==
               0,
           "a build plan's model identity should extend the workspace-graph "
           "identity project facts publish");
  }

  // An already-resolved workspace describes identically without resolving
  // twice.
  const lang::driver::WorkspaceResolutionResult workspace =
      lang::driver::resolveProjectWorkspace(app / "src", std::nullopt,
                                            {.allowAcquisition = false});
  expect(workspace.succeeded() && workspace.workspace,
         "the workspace should resolve for direct description");
  if (workspace.workspace) {
    const lang::driver::ProjectFacts reused =
        lang::driver::projectFactsFromWorkspace(*workspace.workspace);
    expect(reused.modelIdentity() == facts.modelIdentity() &&
               reused.selectedPackage().identity ==
                   facts.selectedPackage().identity &&
               reused.packageSourceRoots().size() == roots.size(),
           "describing an already-resolved workspace should match resolving "
           "facts directly");
  }
}

void testProjectFactsDiagnostics() {
  TemporaryDirectory temporary;
  const lang::driver::ProjectFactsResult missing =
      lang::driver::resolveProjectFacts(temporary.root());
  expect(!missing.succeeded() && !missing.facts &&
             missing.status ==
                 lang::driver::ProjectFactsStatus::DiscoveryFailure,
         "project facts should report discovery failure without a manifest");

  const std::filesystem::path broken = temporary.root() / "broken";
  expect(writeFile(broken / "gti.toml", "manifest-version = 1\n\n[package]\n"),
         "a malformed manifest should be writable");
  const lang::driver::ProjectFactsResult invalid =
      lang::driver::resolveProjectFacts(broken);
  expect(!invalid.succeeded() && !invalid.facts &&
             invalid.status ==
                 lang::driver::ProjectFactsStatus::ManifestFailure &&
             !invalid.diagnostics.empty(),
         "project facts should report manifest failure with diagnostics rather "
         "than partial facts");

  const std::filesystem::path package = temporary.root() / "package";
  expect(writeFile(package / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(package / "gti.toml",
                       "manifest-version = 1\n\n[package]\nname = \"solo\"\n"
                       "version = \"0.1.0\"\n\n[targets.solo]\n"
                       "kind = \"executable\"\nroot = \"src/main.gti\"\n"),
         "a standalone package should be writable");
  const lang::driver::ProjectFactsResult standalone =
      lang::driver::resolveProjectFacts(package);
  expect(standalone.succeeded() && standalone.facts &&
             !standalone.facts->declaredWorkspace() &&
             standalone.facts->packages().size() == 1 &&
             standalone.facts->configurationFlags().empty(),
         "a standalone package should resolve as its own undeclared workspace");
  const lang::driver::ProjectFactsResult unknownPackage =
      lang::driver::resolveProjectFacts(package, std::string("absent"));
  expect(!unknownPackage.succeeded() &&
             unknownPackage.status ==
                 lang::driver::ProjectFactsStatus::SelectionFailure,
         "selecting an undeclared package should report selection failure");

  // A path dependency outside any declared workspace is a Dependency, not a
  // member, and cannot be selected as a command target.
  const std::filesystem::path consumer = temporary.root() / "consumer";
  const std::filesystem::path library = temporary.root() / "library";
  expect(writeFile(consumer / "src/main.gti",
                   "#include <lib/unit>\nint main() { return 0; }\n") &&
             writeFile(library / "src/unit.gti",
                       "namespace lib { int value() { return 1; } }\n") &&
             writeFile(consumer / "gti.toml",
                       "manifest-version = 1\n\n[package]\nname = "
                       "\"consumer\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                       "lib = { path = \"../library\" }\n\n"
                       "[targets.consumer]\nkind = \"executable\"\n"
                       "root = \"src/main.gti\"\n") &&
             writeFile(library / "gti.toml",
                       "manifest-version = 1\n\n[package]\nname = "
                       "\"library\"\nversion = \"3.0.0\"\n"),
         "a standalone package with a path dependency should be writable");
  const lang::driver::ProjectFactsResult dependent =
      lang::driver::resolveProjectFacts(consumer);
  expect(dependent.succeeded() && dependent.facts &&
             !dependent.facts->declaredWorkspace() &&
             dependent.facts->packages().size() == 2,
         "a path dependency should resolve into an undeclared workspace's "
         "package closure");
  if (dependent.facts) {
    const lang::driver::ProjectFactsPackage *describedLibrary =
        dependent.facts->findPackage("library@3.0.0");
    expect(describedLibrary != nullptr &&
               describedLibrary->membership ==
                   lang::driver::ProjectPackageMembership::Dependency &&
               !describedLibrary->selectable,
           "a path dependency package should be described as an unselectable "
           "dependency");
    expect(dependent.facts->packageSourceRoots().size() == 2,
           "a path dependency should contribute a package source root");
  }
}

void testProjectScaffolding(const std::filesystem::path &testExecutable) {
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

  const std::filesystem::path gitRoot =
      temporary.root() / "git package;literal";
  std::filesystem::create_directory(gitRoot);
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, gitRoot,
      std::string("git_package"),
      lang::driver::ProjectScaffoldGit{.executable = testExecutable.string()}));
  expect(scaffold.succeeded() && scaffold.initializedGit &&
             scaffold.createdGitIgnore &&
             std::filesystem::is_directory(gitRoot / ".git") &&
             readFile(gitRoot / ".gitignore") ==
                 "# GTI build output\n/build/\n" &&
             std::filesystem::is_regular_file(gitRoot / "gti.toml") &&
             std::filesystem::is_regular_file(gitRoot / "src/main.gti"),
         "git init scaffolding should create repository metadata and the "
         "canonical GTI ignore file");

  const std::filesystem::path existingIgnoreRoot =
      temporary.root() / "existing-ignore";
  std::filesystem::create_directory(existingIgnoreRoot);
  expect(writeFile(existingIgnoreRoot / ".gitignore", "user-owned\n"),
         "the existing ignore fixture should be writable");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, existingIgnoreRoot,
      std::string("existing_ignore"),
      lang::driver::ProjectScaffoldGit{.executable = testExecutable.string()}));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1503") != nullptr &&
             readFile(existingIgnoreRoot / ".gitignore") == "user-owned\n" &&
             !std::filesystem::exists(existingIgnoreRoot / "gti.toml"),
         "git init scaffolding should preserve an existing .gitignore and "
         "fail before project mutation");

  const std::filesystem::path existingGitRoot =
      temporary.root() / "existing-git";
  std::filesystem::create_directories(existingGitRoot / ".git");
  expect(writeFile(existingGitRoot / ".git/sentinel", "repository-owned\n"),
         "the existing Git metadata fixture should be writable");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, existingGitRoot,
      std::string("existing_git"),
      lang::driver::ProjectScaffoldGit{.executable = testExecutable.string()}));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1503") != nullptr &&
             readFile(existingGitRoot / ".git/sentinel") ==
                 "repository-owned\n" &&
             !std::filesystem::exists(existingGitRoot / "gti.toml"),
         "git init scaffolding should refuse existing repository metadata "
         "before project mutation");

  for (const std::string entry : {".git", ".gitignore"}) {
    const auto redirectedRoot =
        temporary.root() / ("redirected-" + entry.substr(1));
    std::filesystem::create_directory(redirectedRoot);
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        existingGitRoot / ".git", redirectedRoot / entry, linkError);
    if (!linkError) {
      scaffold =
          lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
              lang::driver::ProjectScaffoldMode::ExistingDirectory,
              redirectedRoot, std::string("redirected_git"),
              lang::driver::ProjectScaffoldGit{.executable =
                                                   testExecutable.string()}));
      expect(scaffold.status == lang::driver::ProjectScaffoldStatus::Conflict &&
                 std::filesystem::is_symlink(redirectedRoot / entry) &&
                 !std::filesystem::exists(redirectedRoot / "gti.toml") &&
                 readFile(existingGitRoot / ".git/sentinel") ==
                     "repository-owned\n",
             "Git scaffold must reject existing metadata and ignore symlinks "
             "without following or removing them");
    }
  }

  const std::filesystem::path failedGitRoot = temporary.root() / "failed-git";
  std::filesystem::create_directory(failedGitRoot);
  const std::filesystem::path failedGitSource = failedGitRoot / "src/main.gti";
  const std::string failedGitSourceContents = "int main() { return 17; }\n";
  expect(writeFile(failedGitSource, failedGitSourceContents),
         "the failed Git init source fixture should be writable");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, failedGitRoot,
      std::string("failed_git"),
      lang::driver::ProjectScaffoldGit{
          .executable = (temporary.root() / "missing-git").string()}));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::ToolFailure &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1504") != nullptr &&
             !std::filesystem::exists(failedGitRoot / "gti.toml") &&
             readFile(failedGitSource) == failedGitSourceContents &&
             !std::filesystem::exists(failedGitRoot / ".gitignore") &&
             !std::filesystem::exists(failedGitRoot / ".git"),
         "a Git process failure should roll back scaffold-owned output while "
         "preserving an existing entry source");

  const std::filesystem::path partialGitRoot =
      temporary.root() / "partial-git-failure";
  std::filesystem::create_directory(partialGitRoot);
  expect(writeFile(partialGitRoot / "sentinel", "user-owned\n"),
         "the partial Git initialization fixture should be writable");
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::ExistingDirectory, partialGitRoot,
      std::string("partial_git_failure"),
      lang::driver::ProjectScaffoldGit{.executable = testExecutable.string()}));
  expect(scaffold.status == lang::driver::ProjectScaffoldStatus::ToolFailure &&
             readFile(partialGitRoot / "sentinel") == "user-owned\n" &&
             !std::filesystem::exists(partialGitRoot / ".git") &&
             !std::filesystem::exists(partialGitRoot / ".gitignore") &&
             !std::filesystem::exists(partialGitRoot / "gti.toml") &&
             !std::filesystem::exists(partialGitRoot / "src"),
         "a failed Git child must remove its reserved partial metadata but "
         "preserve unrelated files");

  const std::filesystem::path unsupportedNewRoot =
      temporary.root() / "new-with-git";
  scaffold = lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
      lang::driver::ProjectScaffoldMode::NewPackage, unsupportedNewRoot,
      std::string("new_with_git"),
      lang::driver::ProjectScaffoldGit{.executable = testExecutable.string()}));
  expect(scaffold.status ==
                 lang::driver::ProjectScaffoldStatus::InvalidRequest &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1500") != nullptr &&
             !std::filesystem::exists(unsupportedNewRoot),
         "Git initialization should remain scoped to gti init");

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

void testFormatConfigScaffolding() {
  TemporaryDirectory temporary;
  const std::filesystem::path destination = temporary.root() / "configured";
  std::filesystem::create_directory(destination);

  lang::driver::FormatConfigScaffoldResult scaffold =
      lang::driver::scaffoldFormatConfig(destination);
  const lang::FormatConfigResult parsed =
      lang::parseFormatConfig(readFile(destination / ".gti-format"));
  expect(scaffold.succeeded() &&
             scaffold.configPath == destination / ".gti-format" &&
             readFile(scaffold.configPath) == lang::defaultFormatConfig() &&
             parsed.issues.empty() &&
             parsed.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "format configuration scaffolding should write the parseable GTI "
         "defaults into an existing destination");

  const std::string original = readFile(scaffold.configPath);
  scaffold = lang::driver::scaffoldFormatConfig(destination);
  expect(scaffold.status ==
                 lang::driver::FormatConfigScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1503") != nullptr &&
             readFile(destination / ".gti-format") == original,
         "format configuration scaffolding should refuse to replace an "
         "existing configuration");

  scaffold = lang::driver::scaffoldFormatConfig(temporary.root() / "missing");
  expect(scaffold.status ==
                 lang::driver::FormatConfigScaffoldStatus::Conflict &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1502") != nullptr,
         "format configuration scaffolding should require an existing "
         "directory");

  scaffold = lang::driver::scaffoldFormatConfig(temporary.root().root_path());
  expect(scaffold.status ==
                 lang::driver::FormatConfigScaffoldStatus::InvalidRequest &&
             findDiagnostic(scaffold.diagnostics, "GTI-B1500") != nullptr,
         "format configuration scaffolding should refuse a filesystem root");

  const std::filesystem::path symlinkTarget = temporary.root() / "target";
  const std::filesystem::path symlinkDestination = temporary.root() / "link";
  std::filesystem::create_directory(symlinkTarget);
  std::error_code symlinkError;
  std::filesystem::create_directory_symlink(symlinkTarget, symlinkDestination,
                                            symlinkError);
  if (!symlinkError) {
    scaffold = lang::driver::scaffoldFormatConfig(symlinkDestination);
    expect(scaffold.status ==
                   lang::driver::FormatConfigScaffoldStatus::Conflict &&
               findDiagnostic(scaffold.diagnostics, "GTI-B1502") != nullptr &&
               !std::filesystem::exists(symlinkTarget / ".gti-format"),
           "format configuration scaffolding should not follow a destination "
           "symlink");
  }
}

} // namespace

void testDependencyLockModel() {
  using lang::driver::DependencyLock;
  using lang::driver::LockedGitPackage;

  expect(lang::driver::isFullGitRevision(
             "0123456789abcdef0123456789abcdef01234567") &&
             !lang::driver::isFullGitRevision("0123456789abcdef") &&
             !lang::driver::isFullGitRevision(
                 "0123456789ABCDEF0123456789abcdef01234567") &&
             !lang::driver::isFullGitRevision(
                 "0123456789abcdef0123456789abcdef0123456z"),
         "full git revisions are exactly 40 lowercase hex characters");

  const std::string checksum = "sha256:" + std::string(64, 'a');
  DependencyLock lock;
  lock.packages.push_back(
      {.name = "zeta",
       .version = "0.2.0",
       .url = "https://example.invalid/zeta.git",
       .revision = "0123456789abcdef0123456789abcdef01234567",
       .checksum = checksum,
       .dependencies = {"beta", "alpha"}});
  lock.packages.push_back(
      {.name = "alpha",
       .version = "1.0.0",
       .url = "https://example.invalid/alpha.git",
       .revision = "fedcba9876543210fedcba9876543210fedcba98",
       .checksum = checksum,
       .dependencies = {}});
  const std::string rendered = lang::driver::renderDependencyLock(lock);
  expect(rendered.find("name = \"alpha\"") < rendered.find("name = \"zeta\"") &&
             rendered.find("[\"alpha\", \"beta\"]") != std::string::npos,
         "the lock emitter sorts packages and dependency names "
         "deterministically");
  expect(rendered == lang::driver::renderDependencyLock(lock),
         "lock rendering is deterministic");

  TemporaryDirectory temporary;
  std::string writeError;
  expect(lang::driver::writeDependencyLock(temporary.root(), lock, writeError),
         "the lock should write atomically: " + writeError);
  const lang::driver::DependencyLockLoadResult loaded =
      lang::driver::loadDependencyLock(temporary.root());
  expect(loaded.succeeded() && loaded.lock.packages.size() == 2 &&
             loaded.lock.packages[0].name == "alpha" &&
             loaded.lock.packages[1].name == "zeta" &&
             loaded.lock.packages[1].dependencies ==
                 std::vector<std::string>({"alpha", "beta"}) &&
             lang::driver::renderDependencyLock(loaded.lock) == rendered,
         "a written lock should load back to an identical closure");
  const lang::driver::LockedGitPackage *found = loaded.lock.find(
      {.url = "https://example.invalid/zeta.git",
       .revision = "0123456789abcdef0123456789abcdef01234567"});
  expect(found != nullptr && found->name == "zeta",
         "lock lookup resolves by exact URL and revision");

  expect(
      lang::driver::loadDependencyLock(temporary.root() / "missing").status ==
          lang::driver::DependencyLockLoadStatus::Missing,
      "a missing lock reports Missing rather than an error");

  const auto rejects = [&](std::string_view text, std::string_view why) {
    writeFile(temporary.root() / "gti.lock", text);
    const lang::driver::DependencyLockLoadResult result =
        lang::driver::loadDependencyLock(temporary.root());
    expect(result.status ==
                   lang::driver::DependencyLockLoadStatus::ParseFailure &&
               !result.diagnostics.empty() &&
               result.diagnostics.front().code == "GTI-B1702",
           std::string(why));
  };
  rejects("lock-version = 3\n", "an unsupported lock version is rejected");
  rejects("lock-version = 1\nmystery = true\n",
          "unknown lock fields are rejected");
  rejects("lock-version = 1\n[[package]]\nname = \"a\"\nversion = \"1\"\n"
          "source = \"git+u\"\nrev = \"abc\"\nchecksum = \"sha256:" +
              std::string(64, 'a') + "\"\n",
          "a short revision is rejected");
  rejects("lock-version = 1\n[[package]]\nname = \"a\"\nversion = \"1\"\n"
          "source = \"https://u\"\nrev = \"" +
              std::string(40, 'a') +
              "\"\nchecksum = \"sha256:" + std::string(64, 'a') + "\"\n",
          "a non-git source is rejected");
  rejects("lock-version = 1\n[[package]]\nname = \"a\"\nversion = \"1\"\n"
          "source = \"git+u\"\nrev = \"" +
              std::string(40, 'a') + "\"\nchecksum = \"bad\"\n",
          "a malformed checksum is rejected");
  const std::string entry =
      "[[package]]\nname = \"a\"\nversion = \"1\"\nsource = \"git+u\"\n"
      "rev = \"" +
      std::string(40, 'a') + "\"\nchecksum = \"sha256:" + std::string(64, 'a') +
      "\"\n";
  rejects("lock-version = 1\n" + entry + entry,
          "duplicate package names are rejected");
  rejects("not toml at all [", "invalid TOML is rejected with GTI-B1702");
}

void testGitDependencyManifestValidation() {
  TemporaryDirectory temporary;
  const std::filesystem::path manifest = temporary.root() / "gti.toml";
  const auto load = [&](std::string_view dependency) {
    writeFile(manifest, "manifest-version = 1\n\n[package]\nname = \"app\"\n"
                        "version = \"0.1.0\"\n\n[dependencies]\n" +
                            std::string(dependency) +
                            "\n\n[targets.app]\nkind = \"executable\"\n"
                            "root = \"src/main.gti\"\n");
    writeFile(temporary.root() / "src/main.gti", "int main() { return 0; }\n");
    return lang::driver::loadProjectManifest(manifest);
  };
  const auto failsWith = [&](std::string_view dependency, std::string_view code,
                             std::string_view why) {
    const lang::driver::ManifestLoadResult result = load(dependency);
    const bool matched =
        !result.succeeded() &&
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    });
    expect(matched, std::string(why));
  };

  const std::string revision(40, 'a');
  failsWith("dep = { git = \"https://example.invalid/dep.git\" }", "GTI-B1006",
            "a git dependency without rev is rejected");
  failsWith("dep = { git = \"https://example.invalid/dep.git\", rev = "
            "\"abc\" }",
            "GTI-B1006", "a short rev is rejected");
  failsWith("dep = { path = \"../dep\", git = \"https://example.invalid\", "
            "rev = \"" +
                revision + "\" }",
            "GTI-B1006", "path and git together are rejected");
  failsWith("dep = { rev = \"" + revision + "\" }", "GTI-B1006",
            "rev without git is rejected");
  failsWith("dep = { git = \"https://example.invalid/with space\", rev = \"" +
                revision + "\" }",
            "GTI-B1006", "whitespace in a git URL is rejected");
  failsWith("dep = { git = \"https://example.invalid/dep.git\", rev = \"" +
                revision + "\", branch = \"main\" }",
            "GTI-B1001", "branch selectors are rejected as unknown fields");

  const lang::driver::ManifestLoadResult valid =
      load("dep = { git = \"https://example.invalid/dep.git\", rev = \"" +
           std::string(40, 'A') + "\" }");
  expect(valid.succeeded() && valid.manifest->dependencies().size() == 1 &&
             valid.manifest->dependencies().front().git.has_value() &&
             valid.manifest->dependencies().front().git->revision ==
                 std::string(40, 'a') &&
             valid.manifest->dependencies().front().packageRoot.empty(),
         "a valid git dependency parses with a lowercase-normalized revision "
         "and no resolved package root");
}

void testNativeDependencyComposition() {
  TemporaryDirectory temporary;
  const std::filesystem::path appRoot = temporary.root() / "app";
  const std::filesystem::path wrapperRoot = temporary.root() / "wrapper";
  const std::filesystem::path baseRoot = temporary.root() / "base";

  writeFile(appRoot / "src/main.gti", "int main() { return 0; }\n");
  writeFile(wrapperRoot / "src/wrap.gti", "\n");
  writeFile(wrapperRoot / "native/include/wrap.h", "\n");
  writeFile(wrapperRoot / "native/impl.c", "int impl(void) { return 0; }\n");
  writeFile(baseRoot / "src/decl.gti", "\n");
  writeFile(baseRoot / "native/base.c", "int base(void) { return 0; }\n");
  writeFile(baseRoot / "gti.toml", "manifest-version = 1\n\n[package]\n"
                                   "name = \"base\"\nversion = \"0.1.0\"\n\n"
                                   "[package.native]\n"
                                   "c-sources = [\"native/base.c\"]\n");

  const auto writeWrapper = [&](std::string_view nativeLines) {
    writeFile(wrapperRoot / "gti.toml",
              "manifest-version = 1\n\n[package]\nname = \"wrapper\"\n"
              "version = \"0.1.0\"\n\n[package.native]\n" +
                  std::string(nativeLines) +
                  "\n[dependencies]\nbase = { path = \"../base\" }\n");
  };
  const auto writeApp = [&] {
    writeFile(appRoot / "gti.toml",
              "manifest-version = 1\n\n[package]\nname = \"app\"\n"
              "version = \"0.1.0\"\n\n[dependencies]\n"
              "wrap = { path = \"../wrapper\" }\n\n[targets.app]\n"
              "kind = \"executable\"\nroot = \"src/main.gti\"\n");
  };
  writeApp();
  const auto resolve = [&] {
    return lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
        appRoot, std::nullopt, "dev",
        lang::TargetInfo{.os = "macos", .vendor = "apple", .arch = "arm64"}));
  };

  writeWrapper("include-dirs = [\"native/include\"]\n"
               "c-sources = [\"native/impl.c\"]\n"
               "c-standard = \"c23\"\n"
               "c-compile-args = [\"-DWRAP_BIAS=7\", \"-UNDEBUG\"]\n"
               "libraries = [\"m\"]\n\n"
               "[[package.native.platforms]]\n"
               "os = \"linux\"\n"
               "raw-args = [\"--unselected-platform\"]\n");
  const lang::driver::ProjectResolutionResult composed = resolve();
  expect(composed.succeeded(), "structured dependency native inputs compose");
  if (composed.succeeded()) {
    const lang::driver::NativeInputs &inputs = composed.plan->nativeInputs();
    expect(inputs.dependencyGroups.size() == 2 &&
               inputs.dependencyGroups[0].packageIdentity == "wrapper@0.1.0" &&
               inputs.dependencyGroups[1].packageIdentity == "base@0.1.0",
           "composition orders dependents before their dependencies");
    const lang::driver::NativeDependencyGroup &wrapper =
        inputs.dependencyGroups[0];
    expect(wrapper.includeDirectories.size() == 1 &&
               wrapper.cSources.size() == 1 &&
               wrapper.cStandard == lang::driver::CStandard::C23 &&
               wrapper.cMacroDefinitions ==
                   std::vector<std::string>({"-DWRAP_BIAS=7", "-UNDEBUG"}) &&
               wrapper.linkOperands.size() == 1 &&
               wrapper.linkOperands[0].kind ==
                   lang::driver::NativeLinkOperandKind::Library &&
               wrapper.linkOperands[0].value == "m",
           "a composed group carries the dependency's selected structured "
           "inputs and validated macros");
    expect(inputs.includeDirectories.empty() && inputs.cSources.empty() &&
               inputs.orderedLinkOperands.empty(),
           "dependency native inputs stay scoped to their group and never "
           "merge into the application's own inputs");
    const lang::driver::NativeDependencyGroup &base =
        inputs.dependencyGroups[1];
    expect(base.cSources.size() == 1 &&
               base.cStandard == lang::driver::CStandard::C17,
           "a transitive dependency composes its own group with the default "
           "C standard");
  }

  const auto rejects = [&](std::string_view nativeLines,
                           std::string_view needle, std::string_view why) {
    writeWrapper(nativeLines);
    const lang::driver::ProjectResolutionResult result = resolve();
    const bool matched =
        !result.succeeded() &&
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == "GTI-B1606" &&
                             diagnostic.message.find(needle) !=
                                 std::string::npos;
                    });
    expect(matched, std::string(why));
  };
  rejects("c-sources = [\"native/impl.c\"]\nlink-args = [\"-Wl,-S\"]\n",
          "linker or raw argument", "dependency link arguments never compose");
  rejects("c-sources = [\"native/impl.c\"]\nraw-args = [\"-fanything\"]\n",
          "linker or raw argument", "dependency raw arguments never compose");
  rejects("c-sources = [\"native/impl.c\"]\n"
          "c-compile-args = [\"-include\", \"evil.h\"]\n",
          "'-include'", "a non-macro dependency compiler argument is refused");
  rejects("cpp-sources = []\ncompile-args = [\"-fno-builtin\"]\n",
          "'-fno-builtin'",
          "non-macro compiler arguments from a dependency are refused");
  rejects("c-sources = [\"native/impl.c\"]\n"
          "c-compile-args = [\"-D1BAD=2\"]\n",
          "'-D1BAD=2'", "an invalid macro name is refused");

  // A dependency's target-scoped native table is not part of package-level
  // composition; only [package.native] contributes.
  writeFile(wrapperRoot / "gti.toml",
            "manifest-version = 1\n\n[package]\nname = \"wrapper\"\n"
            "version = \"0.1.0\"\n\n[targets.tool]\n"
            "kind = \"executable\"\nroot = \"src/wrap.gti\"\n\n"
            "[targets.tool.native]\nc-sources = [\"native/impl.c\"]\n\n"
            "[dependencies]\nbase = { path = \"../base\" }\n");
  const lang::driver::ProjectResolutionResult targetScoped = resolve();
  expect(targetScoped.succeeded() &&
             targetScoped.plan->nativeInputs().dependencyGroups.size() == 1 &&
             targetScoped.plan->nativeInputs()
                     .dependencyGroups[0]
                     .packageIdentity == "base@0.1.0",
         "a dependency's target-scoped native table does not compose");
}

// `[bindgen.<alias>]` imports: parsing and validation, the resolved plan's
// generation requests, the synthetic package source root and its alias edge
// on the declaring package, and model-identity participation.
void testBindgenImportManifest() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  writeFile(root / "src" / "main.gti", "int main() { return 0; }\n");
  writeFile(root / "native" / "engine.h", "int engine_start(int mode);\n");
  writeFile(root / "gti.toml", validManifest() +
                                   "\n[bindgen.engine]\n"
                                   "header = \"native/engine.h\"\n"
                                   "namespace = \"eng\"\n"
                                   "include-dirs = [\"native\"]\n"
                                   "defines = [\"ENGINE_STEP=2\"]\n"
                                   "undefines = [\"ENGINE_TRACE\"]\n");

  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", lang::TargetInfo::host()));
  expect(resolution.succeeded(), "the bindgen manifest should resolve");
  if (!resolution.succeeded()) {
    return;
  }
  const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
  expect(plan.bindgenImports().size() == 1,
         "the plan should carry one bindgen import");
  if (plan.bindgenImports().size() == 1) {
    const lang::driver::BindgenImportPlan &import = plan.bindgenImports()[0];
    expect(import.alias == "engine" && import.namespaceName == "eng",
           "the import should carry its alias and namespace");
    expect(import.header ==
               std::filesystem::weakly_canonical(root / "native" / "engine.h"),
           "the header should resolve inside the package");
    expect(import.macroArguments ==
               std::vector<std::string>{"-DENGINE_STEP=2", "-UENGINE_TRACE"},
           "defines and undefines should become ordered macro arguments");
    expect(import.outputDirectory.string().find("build") != std::string::npos &&
               import.outputDirectory.filename() == "engine",
           "the output directory should live under the managed build tree");
  }
  const lang::PackageSourceRoot *owner = nullptr;
  const lang::PackageSourceRoot *synthetic = nullptr;
  for (const lang::PackageSourceRoot &sourceRoot : plan.packageSources()) {
    if (sourceRoot.name == "engine_tools") {
      owner = &sourceRoot;
    }
    if (sourceRoot.name == "engine") {
      synthetic = &sourceRoot;
    }
  }
  expect(owner != nullptr && synthetic != nullptr,
         "the plan should carry the package and the synthetic bindgen root");
  if (owner != nullptr && synthetic != nullptr) {
    const bool edge = std::any_of(
        owner->dependencies.begin(), owner->dependencies.end(),
        [&synthetic](const lang::PackageSourceDependency &dependency) {
          return dependency.alias == "engine" &&
                 dependency.targetIdentity == synthetic->identity;
        });
    expect(edge, "the declaring package should gain the alias edge");
    expect(synthetic->sourceRoot == synthetic->packageRoot &&
               synthetic->sourceRoot ==
                   plan.bindgenImports()[0].outputDirectory,
           "the synthetic root should be the generated directory");
  }
  expect(plan.projectModelIdentity().find(
             "bindgen:engine_tools@0.1.0:engine=native/engine.h") !=
             std::string::npos,
         "the bindgen import should participate in the model identity");
}

void testBindgenImportManifestDiagnostics() {
  const auto loadWith = [](std::string_view bindgen,
                           const std::filesystem::path &root) {
    writeFile(root / "src" / "main.gti", "int main() { return 0; }\n");
    writeFile(root / "native" / "engine.h", "int engine_start(int mode);\n");
    writeFile(root / "gti.toml", validManifest() + std::string(bindgen));
    return lang::driver::loadProjectManifest(root / "gti.toml");
  };

  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nnamespace = \"eng\"\n", temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1002") != nullptr,
           "a bindgen import without 'header' should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded = loadWith(
        "\n[bindgen.engine]\nheader = \"../outside.h\"\n", temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1104") != nullptr,
           "a header escaping the package should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/missing.h\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1802") != nullptr,
           "a missing header should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded = loadWith(
        "\n[bindgen.std]\nheader = \"native/engine.h\"\n", temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1800") != nullptr,
           "the reserved 'std' alias should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const std::filesystem::path root = temporary.root();
    writeFile(root / "dep" / "gti.toml",
              "manifest-version = 1\n[package]\nname = \"dep\"\n"
              "version = \"0.1.0\"\n");
    writeFile(root / "dep" / "src" / "lib.gti", "\n");
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[dependencies]\nengine = { path = \"dep\" }\n"
                 "\n[bindgen.engine]\nheader = \"native/engine.h\"\n",
                 root);
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1801") != nullptr,
           "a bindgen alias colliding with a dependency should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/engine.h\"\n"
                 "defines = [\"2BAD\"]\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1804") != nullptr,
           "an invalid macro name should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/engine.h\"\n"
                 "output = \"somewhere\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1001") != nullptr,
           "an unknown bindgen import field should be rejected");
  }
}

// C-07 opt-in policy in the manifest: selectors, renames, and declared
// array returns reach the build plan sorted, and join its model identity so
// changing the policy cannot reuse a stale generated import.
void testBindgenImportPolicyManifest() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  writeFile(root / "src" / "main.gti", "int main() { return 0; }\n");
  writeFile(root / "native" / "engine.h", "int engine_start(int mode);\n");
  writeFile(root / "gti.toml",
            validManifest() + "\n[bindgen.engine]\n"
                              "header = \"native/engine.h\"\n"
                              "deny = [\"engine_internal_*\"]\n"
                              "allow = [\"engine_start\", \"Engine\"]\n"
                              "rename = { engine__state = \"EngineState\" }\n"
                              "array-returns = ["
                              "{ function = \"engine_items\", "
                              "count = \"count\" }]\n");

  const lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", lang::TargetInfo::host()));
  expect(resolution.succeeded(), "the policy manifest should resolve");
  if (!resolution.succeeded() || resolution.plan->bindgenImports().empty()) {
    return;
  }
  const lang::driver::BindgenImportPlan &import =
      resolution.plan->bindgenImports()[0];
  expect(import.denyPatterns == std::vector<std::string>{"engine_internal_*"},
         "the deny list should reach the plan");
  expect(import.allowPatterns ==
             std::vector<std::string>{"Engine", "engine_start"},
         "allow patterns should reach the plan sorted");
  expect(import.renames.size() == 1 &&
             import.renames[0].first == "engine__state" &&
             import.renames[0].second == "EngineState",
         "the rename should reach the plan");
  expect(import.arrayReturns.size() == 1 &&
             import.arrayReturns[0].first == "engine_items" &&
             import.arrayReturns[0].second == "count",
         "the declared array return should reach the plan");
  const std::string identity = resolution.plan->projectModelIdentity();
  expect(identity.find("|deny=engine_internal_*") != std::string::npos &&
             identity.find("|rename=engine__state>EngineState") !=
                 std::string::npos &&
             identity.find("|array=engine_items>count") != std::string::npos,
         "the policy should participate in the model identity");
}

void testBindgenImportPolicyDiagnostics() {
  const auto loadWith = [](std::string_view bindgen,
                           const std::filesystem::path &root) {
    writeFile(root / "src" / "main.gti", "int main() { return 0; }\n");
    writeFile(root / "native" / "engine.h", "int engine_start(int mode);\n");
    writeFile(root / "gti.toml", validManifest() + std::string(bindgen));
    return lang::driver::loadProjectManifest(root / "gti.toml");
  };
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/engine.h\"\n"
                 "deny = [\"engine_*_bad\"]\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1808") != nullptr,
           "a wildcard outside the trailing position should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/engine.h\"\n"
                 "rename = { engine = \"not an identifier\" }\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1808") != nullptr,
           "a rename target that is not an identifier should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[bindgen.engine]\nheader = \"native/engine.h\"\n"
                 "array-returns = [{ function = \"engine_items\" }]\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1808") != nullptr,
           "an array return without a count should be rejected");
  }
}

// A target that is not this build host, used to prove cross-target
// resolution without depending on which machine runs the tests.
lang::TargetInfo nonHostTarget() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  const lang::TargetTripleParseResult linux =
      lang::parseTargetTripleResult("x86_64-unknown-linux-gnu");
  const lang::TargetTripleParseResult macos =
      lang::parseTargetTripleResult("arm64-apple-macos");
  const lang::TargetInfo &candidate =
      lang::driver::targetTriple(*linux.target) ==
              lang::driver::targetTriple(host)
          ? *macos.target
          : *linux.target;
  return candidate;
}

void testCrossToolchainManifest() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  const lang::TargetInfo target = nonHostTarget();
  const std::string triple = lang::driver::targetTriple(target);
  expect(writeFile(root / "src/main.gti", "int main() { return 0; }\n"),
         "the cross-toolchain project source should be writable");
  std::filesystem::create_directories(root / "sysroots/target");
  expect(writeFile(root / "gti.toml",
                   validManifest() + "\n[toolchains." + triple +
                       "]\ncxx = \"target-c++\"\ncc = \"target-cc\"\n"
                       "sysroot = \"sysroots/target\"\n"
                       "native-target = \"vendor-native-triple\"\n"),
         "the cross-toolchain manifest should be writable");

  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(root / "gti.toml");
  expect(loaded.succeeded() && loaded.manifest &&
             loaded.manifest->toolchains().size() == 1,
         "a [toolchains.<triple>] table should parse into the manifest model");
  if (loaded.manifest && loaded.manifest->toolchains().size() == 1) {
    const lang::driver::ProjectToolchain &toolchain =
        loaded.manifest->toolchains().front();
    expect(toolchain.declaredTriple == triple &&
               toolchain.cxx == "target-c++" && toolchain.cc == "target-cc" &&
               toolchain.nativeTarget == "vendor-native-triple" &&
               toolchain.declaredSysroot == "sysroots/target" &&
               toolchain.sysroot ==
                   std::filesystem::canonical(root / "sysroots/target"),
           "declared toolchain fields should resolve into the manifest model");
    expect(loaded.manifest->findToolchain(target) == &toolchain &&
               loaded.manifest->findToolchain(
                   lang::TargetInfo{.os = "nosuchos",
                                    .vendor = "nosuchvendor",
                                    .arch = "nosucharch"}) == nullptr,
           "toolchain lookup should select by normalized OS, vendor, and "
           "architecture");
  }

  // A cross build resolves the declared toolchain into the plan, the native
  // target policy, and the model identity.
  const lang::driver::ProjectResolutionResult cross =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", target, {}, std::nullopt, {},
          lang::driver::RuntimeProfile::Hosted,
          lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget));
  expect(cross.succeeded(),
         "a non-host build with a declared toolchain should resolve");
  if (cross.plan) {
    const lang::driver::ProjectBuildPlan &plan = *cross.plan;
    expect(plan.toolchain().declared() &&
               plan.toolchain().cxx == "target-c++" &&
               plan.toolchain().cc == "target-cc",
           "the declared compilers should reach the build plan");
    expect(plan.nativeInputs().targetPolicy.triple == "vendor-native-triple" &&
               plan.nativeInputs().targetPolicy.sysroot ==
                   std::filesystem::canonical(root / "sysroots/target"),
           "the declared native target and sysroot should reach the native "
           "target policy");
    expect(plan.output().parent_path().filename() == triple,
           "cross artifacts should publish under the selected triple");
    const std::string identity = plan.projectModelIdentity();
    expect(identity.find("target:" + triple) != std::string::npos &&
               identity.find("toolchain:" + triple + "|cxx=target-c++") !=
                   std::string::npos &&
               identity.find("|sysroot=sysroots/target") != std::string::npos &&
               identity.find("|native-target=vendor-native-triple") !=
                   std::string::npos,
           "the selected target and its toolchain should participate in the "
           "project model identity");
  }

  // The host build of the same package keeps working unchanged, and cannot
  // share a model identity with the cross build.
  const lang::driver::ProjectResolutionResult host =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", lang::TargetInfo::host(), {}, std::nullopt,
          {}, lang::driver::RuntimeProfile::Hosted,
          lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget));
  expect(host.succeeded() && host.plan && !host.plan->toolchain().declared() &&
             host.plan->nativeInputs().targetPolicy.empty(),
         "a host build needs no declared toolchain and emits no cross policy");
  if (host.plan && cross.plan) {
    expect(host.plan->projectModelIdentity() !=
               cross.plan->projectModelIdentity(),
           "a host build and a cross build must not share a cache identity");
  }

  // A manifest key spelled differently but normalizing to the same target
  // still selects the entry, so a second spelling table cannot appear.
  {
    TemporaryDirectory alternate;
    expect(writeFile(alternate.root() / "src/main.gti",
                     "int main() { return 0; }\n"),
           "the alternate-spelling source should be writable");
    const lang::TargetTripleParseResult spelled =
        lang::parseTargetTripleResult("aarch64-apple-darwin");
    expect(writeFile(alternate.root() / "gti.toml",
                     validManifest() + "\n[toolchains.aarch64-apple-darwin]\n"
                                       "cxx = \"aarch64-apple-c++\"\n"),
           "the alternate-spelling manifest should be writable");
    const lang::driver::ManifestLoadResult alternateLoad =
        lang::driver::loadProjectManifest(alternate.root() / "gti.toml");
    expect(alternateLoad.succeeded() && alternateLoad.manifest &&
               spelled.succeeded() &&
               alternateLoad.manifest->findToolchain(*spelled.target) !=
                   nullptr,
           "a toolchain key should be matched through the normalized target "
           "vocabulary, not by literal spelling");
  }
}

void testCrossToolchainFailClosed() {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root();
  const lang::TargetInfo target = nonHostTarget();
  const std::string triple = lang::driver::targetTriple(target);
  expect(writeFile(root / "src/main.gti", "int main() { return 0; }\n"),
         "the fail-closed project source should be writable");
  expect(writeFile(root / "gti.toml", validManifest()),
         "the fail-closed manifest should be writable");

  const lang::driver::ProjectResolutionResult missing =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", target, {}, std::nullopt, {},
          lang::driver::RuntimeProfile::Hosted,
          lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget));
  expect(!missing.succeeded() &&
             findDiagnostic(missing.diagnostics, "GTI-B1904") != nullptr,
         "a non-host build with no declared toolchain must fail closed rather "
         "than silently reuse host tools");
  if (const lang::Diagnostic *diagnostic =
          findDiagnostic(missing.diagnostics, "GTI-B1904")) {
    expect(diagnostic->message.find("[toolchains." + triple + "]") !=
               std::string::npos,
           "the fail-closed diagnostic should name the missing table");
  }

  // The library contract is unchanged: a caller that supplies its own
  // toolchain, and `gti check`, analyze any supported target.
  const lang::driver::ProjectResolutionResult analysis =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", target, {}, std::nullopt, {},
          lang::driver::RuntimeProfile::Hosted,
          lang::driver::ToolchainSelectionPolicy::HostTools));
  expect(analysis.succeeded() && analysis.plan &&
             !analysis.plan->toolchain().declared(),
         "host-tools resolution should analyze a non-host target without a "
         "declared toolchain");

  // Tests and whole-package builds fail closed on the same rule.
  const lang::driver::ProjectTargetSetResolutionResult all =
      lang::driver::resolveAllProjectTargets(lang::driver::ProjectBuildRequest(
          root, std::nullopt, "dev", target, {}, std::nullopt, {},
          lang::driver::RuntimeProfile::Hosted,
          lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget));
  expect(!all.succeeded() &&
             findDiagnostic(all.diagnostics, "GTI-B1904") != nullptr,
         "building every target for a non-host triple must fail closed too");

  // A declared toolchain without `cc` cannot build declared C sources.
  {
    TemporaryDirectory cPackage;
    expect(
        writeFile(cPackage.root() / "src/main.gti",
                  "int main() { return 0; }\n") &&
            writeFile(cPackage.root() / "native/support.c", "int gti(void);\n"),
        "the C-source fixture should be writable");
    expect(
        writeFile(cPackage.root() / "gti.toml",
                  validManifest() +
                      "\n[package.native]\nc-sources = [\"native/support.c\"]\n"
                      "\n[toolchains." +
                      triple + "]\ncxx = \"target-c++\"\n"),
        "the cc-less cross manifest should be writable");
    const lang::driver::ProjectResolutionResult withoutCc =
        lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
            cPackage.root(), std::nullopt, "dev", target, {}, std::nullopt, {},
            lang::driver::RuntimeProfile::Hosted,
            lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget));
    expect(!withoutCc.succeeded() &&
               findDiagnostic(withoutCc.diagnostics, "GTI-B1905") != nullptr,
           "a declared toolchain with no 'cc' must reject a build that "
           "compiles native C sources");

    const lang::driver::ProjectResolutionResult checkedWithoutCc =
        lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
            cPackage.root(), std::nullopt, "dev", target, {}, std::nullopt, {},
            lang::driver::RuntimeProfile::Hosted,
            lang::driver::ToolchainSelectionPolicy::HostTools));
    expect(checkedWithoutCc.succeeded(),
           "analysis must not require a native C compiler it never invokes");

    const lang::driver::ProjectResolutionResult overriddenCc =
        lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
            cPackage.root(), std::nullopt, "dev", target, {}, std::nullopt, {},
            lang::driver::RuntimeProfile::Hosted,
            lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget,
            {.cCompiler = true}));
    expect(overriddenCc.succeeded(),
           "an explicit C compiler override should satisfy a cc-less cross "
           "toolchain");
  }
}

void testCrossToolchainManifestDiagnostics() {
  const auto loadWith = [](std::string_view toolchains,
                           const std::filesystem::path &root) {
    writeFile(root / "src" / "main.gti", "int main() { return 0; }\n");
    writeFile(root / "gti.toml", validManifest() + std::string(toolchains));
    return lang::driver::loadProjectManifest(root / "gti.toml");
  };
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.definitely-not-a-triple]\ncxx = \"c++\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1900") != nullptr,
           "a toolchain key that is not a supported triple should be "
           "rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.aarch64-apple-darwin]\ncxx = \"a\"\n"
                 "\n[toolchains.arm64-apple-macos]\ncxx = \"b\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1901") != nullptr,
           "two keys normalizing to one target should be rejected rather "
           "than silently ordered");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.x86_64-unknown-linux-gnu]\ncc = \"cc\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1902") != nullptr,
           "a toolchain table without 'cxx' should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.x86_64-unknown-linux-gnu]\ncxx = \"-flto\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1902") != nullptr,
           "a toolchain compiler spelled as an option should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.x86_64-unknown-linux-gnu]\ncxx = \"c++\"\n"
                 "sysroot = \"../outside\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1903") != nullptr,
           "a relative sysroot escaping the package should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.x86_64-unknown-linux-gnu]\ncxx = \"c++\"\n"
                 "sysroot = \"sysroots/missing\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1903") != nullptr,
           "a sysroot that names no existing directory should be rejected");
  }
  {
    TemporaryDirectory temporary;
    const lang::driver::ManifestLoadResult loaded =
        loadWith("\n[toolchains.x86_64-unknown-linux-gnu]\ncxx = \"c++\"\n"
                 "linker = \"ld\"\n",
                 temporary.root());
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1001") != nullptr,
           "an unknown toolchain field should be rejected by the shared "
           "field validator");
  }
  {
    // Written without the profile suffix so the scalar stays top-level.
    TemporaryDirectory temporary;
    writeFile(temporary.root() / "src" / "main.gti",
              "int main() { return 0; }\n");
    writeFile(temporary.root() / "gti.toml",
              "manifest-version = 1\ntoolchains = \"host\"\n\n[package]\n"
              "name = \"engine_tools\"\nversion = \"0.1.0\"\n");
    const lang::driver::ManifestLoadResult loaded =
        lang::driver::loadProjectManifest(temporary.root() / "gti.toml");
    expect(!loaded.succeeded() &&
               findDiagnostic(loaded.diagnostics, "GTI-B1004") != nullptr,
           "a non-table 'toolchains' value should be rejected");
  }
}

void testPackageBuildScripts() {
  {
    TemporaryDirectory sourcePackage;
    expect(writeFile(sourcePackage.root() / "src/main.gti",
                     "int main() { return 0; }\n") &&
               writeFile(sourcePackage.root() / "build.gti",
                         "int main() { return 0; }\n") &&
               writeFile(sourcePackage.root() / "gti.toml",
                         validManifest("\n[package.build-script]\n"
                                       "source = \"build.gti\"\n"
                                       "inputs = []\n")),
           "GTI build-script manifest fixture should be writable");
    const lang::driver::ManifestLoadResult loaded =
        lang::driver::loadProjectManifest(sourcePackage.root() / "gti.toml");
    expect(loaded.succeeded() && loaded.manifest &&
               loaded.manifest->package().buildScript &&
               loaded.manifest->package().buildScript->source ==
                   std::filesystem::canonical(sourcePackage.root() /
                                              "build.gti") &&
               loaded.manifest->package().buildScript->command.empty() &&
               loaded.manifest->package().buildScript->inputs.size() == 1,
           "a GTI source build program should parse and automatically enter "
           "its own content into the cache inputs");

    expect(writeFile(sourcePackage.root() / "gti.toml",
                     validManifest("\n[package.build-script]\n"
                                   "source = \"build.gti\"\n"
                                   "command = [\"true\"]\n"
                                   "inputs = []\n")),
           "ambiguous build-script fixture should be writable");
    const lang::driver::ManifestLoadResult ambiguous =
        lang::driver::loadProjectManifest(sourcePackage.root() / "gti.toml");
    expect(!ambiguous.succeeded() &&
               findDiagnostic(ambiguous.diagnostics, "GTI-B1002") != nullptr,
           "a package must choose either a GTI source program or an external "
           "command, never both");

    expect(writeFile(sourcePackage.root() / "build.txt", "not GTI\n") &&
               writeFile(sourcePackage.root() / "gti.toml",
                         validManifest("\n[package.build-script]\n"
                                       "source = \"build.txt\"\n"
                                       "inputs = []\n")),
           "wrong-extension build-script fixture should be writable");
    const lang::driver::ManifestLoadResult wrongExtension =
        lang::driver::loadProjectManifest(sourcePackage.root() / "gti.toml");
    expect(!wrongExtension.succeeded() &&
               findDiagnostic(wrongExtension.diagnostics, "GTI-B2001") !=
                   nullptr,
           "a GTI build-program source must use the .gti extension");
  }

  TemporaryDirectory temporary;
  const std::filesystem::path app = temporary.root() / "app";
  const std::filesystem::path native = temporary.root() / "native";
  expect(writeFile(app / "src/main.gti", "int main() { return 0; }\n") &&
             writeFile(native / "src/.gitkeep", "") &&
             writeFile(native / "build.cmake",
                       R"(file(MAKE_DIRECTORY "$ENV{GTI_BUILD_OUT_DIR}/include")
file(WRITE "$ENV{GTI_BUILD_OUT_DIR}/libnative.a" "")
file(WRITE "$ENV{GTI_BUILD_OUTPUT}" "version = 1\ninclude-dirs = [\"include\"]\nlink-files = [\"libnative.a\"]\n")
)") &&
             writeFile(native / "gti.toml",
                       "manifest-version = 1\n\n[package]\n"
                       "name = \"native_pkg\"\nversion = \"0.1.0\"\n\n"
                       "[package.build-script]\n"
                       "command = [\"cmake\", \"-P\", \"build.cmake\"]\n"
                       "inputs = [\"build.cmake\"]\n") &&
             writeFile(app / "gti.toml",
                       "manifest-version = 1\n\n[package]\n"
                       "name = \"build_script_app\"\nversion = \"0.1.0\"\n\n"
                       "[dependencies]\nnative = { path = \"../native\" }\n\n"
                       "[build]\nallow-build-scripts = "
                       "[\"native_pkg@0.1.0\"]\n\n"
                       "[targets.app]\nkind = \"executable\"\n"
                       "root = \"src/main.gti\"\n"),
         "build-script fixtures should be writable");

  lang::driver::ProjectResolutionResult resolved =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          app, std::nullopt, "dev", lang::TargetInfo::host(), {}, std::nullopt,
          {}, lang::driver::RuntimeProfile::Hosted));
  expect(resolved.succeeded() && resolved.plan &&
             resolved.plan->buildScripts().size() == 1 &&
             resolved.plan->nativeInputs().dependencyGroups.size() == 1,
         "an explicitly approved dependency build script should enter the "
         "resolved plan without executing during resolution");
  if (resolved.plan) {
    expect(!std::filesystem::exists(
               resolved.plan->buildScripts().front().outputDirectory /
               "gti-build-output.toml"),
           "project resolution must not execute a build script");
    const lang::driver::BuildScriptExecutionResult execution =
        lang::driver::executeBuildScripts(
            *resolved.plan,
            {.cCompiler = "cc", .cppCompiler = "c++", .jobs = 1});
    expect(execution.succeeded() && execution.nativeInputs &&
               execution.nativeInputs->dependencyGroups.size() == 1 &&
               execution.nativeInputs->dependencyGroups.front()
                       .includeDirectories.size() == 1 &&
               execution.nativeInputs->dependencyGroups.front()
                       .linkOperands.size() == 1,
           "a successful build script should contribute validated native "
           "outputs to its dependency group");
    expect(execution.outcomes.size() == 1 && !execution.outcomes.front().reused,
           "the first build-script execution should report a native build");
    const lang::driver::BuildScriptExecutionResult reused =
        lang::driver::executeBuildScripts(
            *resolved.plan,
            {.cCompiler = "cc", .cppCompiler = "c++", .jobs = 1});
    expect(reused.succeeded() && reused.outcomes.size() == 1 &&
               reused.outcomes.front().reused,
           "unchanged declared inputs should reuse validated build-script "
           "outputs");
  }

  expect(writeFile(app / "gti.toml",
                   "manifest-version = 1\n\n[package]\n"
                   "name = \"build_script_app\"\nversion = \"0.1.0\"\n\n"
                   "[dependencies]\nnative = { path = \"../native\" }\n\n"
                   "[targets.app]\nkind = \"executable\"\n"
                   "root = \"src/main.gti\"\n"),
         "unapproved build-script fixture should be writable");
  resolved =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          app, std::nullopt, "dev", lang::TargetInfo::host(), {}, std::nullopt,
          {}, lang::driver::RuntimeProfile::Hosted));
  expect(!resolved.succeeded() &&
             findDiagnostic(resolved.diagnostics, "GTI-B2002") != nullptr,
         "a dependency build script must be rejected until the selected "
         "package explicitly approves it");
}

void testObjectiveCppSourceResolution() {
  TemporaryDirectory temporary;
  const auto root = temporary.root();
  const std::string manifest =
      "manifest-version = 1\n[package]\nname = \"bridge\"\nversion = "
      "\"1.0.0\"\n"
      "[package.native]\ncpp-sources = [\"native/bridge.mm\"]\n"
      "[targets.bridge]\nkind = \"executable\"\nroot = \"src/main.gti\"\n";
  expect(writeFile(root / "gti.toml", manifest) &&
             writeFile(root / "native/bridge.mm", "// Objective-C++\n") &&
             writeFile(root / "src/main.gti", "int main() { return 0; }\n"),
         "Objective-C++ fixtures should be writable");
  const auto resolve = [&](std::string os) {
    return lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
        root, std::nullopt, "dev",
        {.os = std::move(os), .vendor = "apple", .arch = "arm64"}));
  };
  const auto mac = resolve("macos");
  expect(mac.succeeded() && mac.plan &&
             mac.plan->nativeInputs().cppSources ==
                 std::vector<std::filesystem::path>{
                     std::filesystem::canonical(root / "native/bridge.mm")},
         "Objective-C++ should use the existing immutable C++ source plan");
  for (const std::string os : {"linux", "windows"}) {
    const auto other = resolve(os);
    const auto *diagnostic = findDiagnostic(other.diagnostics, "GTI-B1401");
    expect(!other.succeeded() && diagnostic &&
               diagnostic->message.find("Objective-C++") != std::string::npos &&
               diagnostic->message.find(os) != std::string::npos &&
               diagnostic->primary.source ==
                   std::filesystem::canonical(root / "gti.toml").string() &&
               manifest.substr(diagnostic->primary.start,
                               diagnostic->primary.end -
                                   diagnostic->primary.start) ==
                   "\"native/bridge.mm\"" &&
               diagnostic->fixes.empty(),
           "unsupported Objective-C++ targets should point at the exact source "
           "without guessing a fix");
  }
  expect(
      writeFile(
          root / "gti.toml",
          "manifest-version = 1\n[package]\nname = \"bridge\"\nversion = "
          "\"1.0.0\"\n"
          "[[package.native.platforms]]\nos = \"macos\"\n"
          "cpp-sources = [\"missing.mm\"]\n"
          "[targets.bridge]\nkind = \"executable\"\nroot = \"src/main.gti\"\n"),
      "unselected Objective-C++ fixture should be writable");
  expect(resolve("linux").succeeded(),
         "unselected macOS bridge paths must not be checked on other targets");
  expect(findDiagnostic(resolve("macos").diagnostics, "GTI-B1103") != nullptr,
         "selected Objective-C++ paths must still exist");

  // Approved build programs use the same language boundary for generated
  // sources, both on a fresh result and when considering cached output.
  expect(
      writeFile(root / "emit.cmake",
                R"(file(WRITE "$ENV{GTI_BUILD_OUT_DIR}/bridge.mm" "// bridge\n")
file(WRITE "$ENV{GTI_BUILD_OUTPUT}" "version = 1\ncpp-sources = [\"bridge.mm\"]\n")
)") && writeFile(root / "gti.toml",
                 "manifest-version = 1\n[package]\nname = \"bridge\"\nversion "
                 "= \"1.0.0\"\n"
                 "[package.build-script]\ncommand = [\"cmake\", \"-P\", "
                 "\"emit.cmake\"]\ninputs = [\"emit.cmake\"]\n"
                 "[targets.bridge]\nkind = \"executable\"\nroot = "
                 "\"src/main.gti\"\n"),
      "generated Objective-C++ fixtures should be writable");
  for (const std::string os : {"macos", "linux"}) {
    const auto plan = resolve(os);
    expect(plan.succeeded() && plan.plan,
           "bridge build-script plan should resolve");
    if (!plan.plan) {
      continue;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
      const auto output = lang::driver::executeBuildScripts(
          *plan.plan, {.cCompiler = "cc", .cppCompiler = "c++", .jobs = 1});
      if (os == "macos") {
        expect(
            output.succeeded() && output.nativeInputs &&
                output.nativeInputs->cppSources.size() == 1 &&
                output.nativeInputs->cppSources.front().extension() == ".mm",
            "approved build outputs should admit macOS Objective-C++ sources");
      } else {
        const auto *diagnostic =
            findDiagnostic(output.diagnostics, "GTI-B2005");
        expect(
            !output.succeeded() && diagnostic &&
                diagnostic->message.find("macOS") != std::string::npos,
            "generated Objective-C++ sources must reject a non-macOS target");
      }
    }
  }
}

void testModuleManifest() {
  TemporaryDirectory temporary;
  const auto root = temporary.root();
  const auto manifest = root / "gti.toml";
  const auto first = root / "src/api.gti";
  const auto second = root / "src/other.gti";
  expect(
      writeFile(first,
                "export module main; export int answer() { return 42; }\n") &&
          writeFile(
              second,
              "export module main; export int answer() { return 43; }\n") &&
          writeFile(root / "src/main.gti",
                    "import main; int main() { return answer(); }\n"),
      "module fixtures should be writable");
  const std::string base = validManifest();
  expect(writeFile(manifest, base + "\n[modules]\nmain = \"src/api.gti\"\n"),
         "module manifest should be writable");
  const auto loaded = lang::driver::loadProjectManifest(manifest);
  expect(loaded.succeeded() && loaded.manifest->package().modules.size() == 1 &&
             loaded.manifest->package().modules.front().second ==
                 std::filesystem::canonical(first),
         "module paths resolve once into immutable package inputs");
  const auto workspace = lang::driver::resolveProjectWorkspace(root);
  expect(workspace.succeeded() &&
             !workspace.workspace->packageSourceRoots().front().modules.empty(),
         "the resolved workspace forwards module maps to the compiler");
  const std::string identity =
      workspace.succeeded() ? workspace.workspace->modelIdentity() : "";
  expect(writeFile(manifest, base + "\n[modules]\nmain = \"src/other.gti\"\n"),
         "module registration should be editable");
  const auto moved = lang::driver::resolveProjectWorkspace(root);
  expect(moved.succeeded() && moved.workspace->modelIdentity() != identity,
         "moving a public module changes project/cache identity");
  for (const std::string registration :
       {"main = 42", "main = \"missing.gti\"", "main = \"\"",
        "main = \"/outside.gti\"", "main = \"C:/outside.gti\"",
        "main = \"src/api.cpp\"", "main = \"src\"", "import = \"src/api.gti\"",
        "\"bad.name\" = \"src/api.gti\"", "__gti_hidden = \"src/api.gti\"",
        "main = \"src/api.gti\"\nother = \"src/api.gti\"",
        "main = \"src/api.gti\"\nMAIN = \"src/other.gti\"",
        "std = \"src/api.gti\""}) {
    expect(writeFile(manifest, base + "\n[modules]\n" + registration + "\n"),
           "invalid module fixture should be writable");
    const auto invalid = lang::driver::loadProjectManifest(manifest);
    expect(!invalid.succeeded() && !invalid.diagnostics.empty(),
           "invalid module map must fail before frontend entry: " +
               registration);
  }
  expect(writeFile(manifest, base + "\n[modules]\nstd = \"src/api.gti\"\n"),
         "reserved module fixture should be writable");
  const auto reserved = lang::driver::loadProjectManifest(manifest);
  const auto *reservedDiagnostic =
      findDiagnostic(reserved.diagnostics, "GTI-B1005");
  expect(reservedDiagnostic != nullptr &&
             reservedDiagnostic->message.find("reserved for the standard "
                                              "library") != std::string::npos,
         "the standard library owns the module name 'std'");
  TemporaryDirectory outside;
  expect(writeFile(outside.root() / "api.gti", "export module main;"),
         "external fixture exists");
  std::error_code error;
  std::filesystem::create_symlink(outside.root() / "api.gti",
                                  root / "escape.gti", error);
  if (!error) {
    expect(writeFile(manifest, base + "\n[modules]\nmain = \"escape.gti\"\n"),
           "escape fixture exists");
    const auto escaped = lang::driver::loadProjectManifest(manifest);
    expect(!escaped.succeeded() &&
               findDiagnostic(escaped.diagnostics, "GTI-B1104") != nullptr,
           "module registrations cannot escape their package through symlinks");
  }
}

int main(int argc, char *argv[]) {
  if (argc == 5 && std::string_view(argv[1]) == "-C" &&
      std::string_view(argv[3]) == "init" &&
      std::string_view(argv[4]) == "--quiet") {
    const std::filesystem::path root(argv[2]);
    std::error_code error;
    if (!std::filesystem::is_directory(root / ".git", error) || error) {
      return 2;
    }
    // A child may fail after publishing partial metadata. Only the directory
    // reserved by this scaffold operation is eligible for rollback.
    if (root.filename() == "partial-git-failure") {
      if (!writeFile(root / ".git/partial", "partial repository state\n")) {
        return 3;
      }
      return 1;
    }
    return 0;
  }
  lang::installCrashHandlers("gti_project_tests");
  testDiscoveryParsingAndResolution();
  testManifestDiagnostics();
  testConfigurationFlagManifest();
  testScopedConfigurationFlags();
  testDependencyConfigurationFlags();
  testTargetSelectionDiagnostics();
  testTestTargetResolution();
  testNativeManifestResolution();
  testNativeManifestDiagnostics();
  testObjectiveCppSourceResolution();
  testCleanSafety();
  testWorkspaceAndPathDependencies();
  testDependencyLockModel();
  testGitDependencyManifestValidation();
  testNativeDependencyComposition();
  testProjectFacts();
  testProjectFactsDiagnostics();
  testProjectScaffolding(std::filesystem::absolute(argv[0]));
  testFormatConfigScaffolding();
  testBindgenImportManifest();
  testBindgenImportManifestDiagnostics();
  testBindgenImportPolicyManifest();
  testBindgenImportPolicyDiagnostics();
  testCrossToolchainManifest();
  testCrossToolchainFailClosed();
  testCrossToolchainManifestDiagnostics();
  testPackageBuildScripts();
  testModuleManifest();

  if (failures != 0) {
    std::cerr << failures << " project test(s) failed\n";
    return 1;
  }
  std::cout << "All project tests passed\n";
  return 0;
}
