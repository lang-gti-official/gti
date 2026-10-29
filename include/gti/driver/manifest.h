#pragma once

#include "gti/backend.h"
#include "gti/configuration_flags.h"
#include "gti/cpp_standard.h"
#include "gti/diagnostic.h"
#include "gti/driver/native_toolchain.h"
#include "gti/optimizer.h"
#include "gti/target.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang::driver {

inline constexpr int currentManifestVersion = 1;

// Manifest admission policy. Production enables library requests; callers may
// still explicitly disable them. Backend closure admission governs execution.
struct ManifestFeaturePolicy {
  bool dormantLibraryTargets = true;
};

[[nodiscard]] bool isPortableProjectName(std::string_view name);
[[nodiscard]] std::string portableProjectNameKey(std::string_view name);

struct ProjectNativePlatform {
  std::optional<std::string> os;
  std::optional<std::string> vendor;
  std::optional<std::string> arch;
  NativeInputs inputs;
  std::vector<SourceSpan> includeDirectoryDeclarations;
  std::vector<SourceSpan> cSourceDeclarations;
  std::vector<SourceSpan> cppSourceDeclarations;
  std::vector<SourceSpan> libraryDirectoryDeclarations;
  std::vector<SourceSpan> libraryFileDeclarations;
  std::vector<SourceSpan> frameworkDeclarations;
  SourceSpan declaration;
};

struct ProjectNativeSettings {
  NativeInputs inputs;
  std::vector<SourceSpan> includeDirectoryDeclarations;
  std::vector<SourceSpan> cSourceDeclarations;
  std::vector<SourceSpan> cppSourceDeclarations;
  std::vector<SourceSpan> libraryDirectoryDeclarations;
  std::vector<SourceSpan> libraryFileDeclarations;
  std::vector<SourceSpan> frameworkDeclarations;
  SourceSpan declaration;
  std::vector<ProjectNativePlatform> platforms;
};

// A host-side package build program. The command is an exact argv vector run
// with the package directory as its working directory. Inputs are declared as
// package-contained paths so the driver can form a deterministic identity;
// outputs are reported by the program through GTI_BUILD_OUTPUT.
struct ProjectBuildScript {
  std::vector<std::string> command;
  std::optional<std::filesystem::path> source;
  std::optional<std::string> declaredSource;
  std::vector<std::filesystem::path> inputs;
  std::vector<std::string> declaredInputs;
  SourceSpan declaration;
  SourceSpan sourceDeclaration;
  std::vector<SourceSpan> commandDeclarations;
  std::vector<SourceSpan> inputDeclarations;
};

struct ProjectPackage {
  std::string name;
  std::string version;
  std::filesystem::path sourceRoot;
  SourceSpan sourceRootDeclaration;
  ProjectNativeSettings native;
  std::optional<ProjectBuildScript> buildScript;
  std::vector<std::pair<std::string, std::filesystem::path>> modules;
  std::vector<std::pair<std::string, std::vector<std::filesystem::path>>>
      moduleImplementations;
  std::vector<std::pair<std::string, std::filesystem::path>> moduleLookupPaths;
};

// A pinned git dependency source as spelled in the manifest: an exact URL
// plus a mandatory full 40-hex commit. Branch, tag, and range selectors are
// deliberately rejected so a build plan never depends on mutable remote
// state.
struct ProjectGitDependencySource {
  std::string url;
  std::string revision;
  SourceSpan urlDeclaration;
  SourceSpan revisionDeclaration;
};

struct ProjectRegistryDependencySource {
  std::string registry = "official";
  std::string package;
  std::string requirement;
};

struct ProjectDependency {
  std::string alias;
  // For a path dependency, the resolved package directory. A Git dependency
  // leaves this empty until workspace resolution materializes its verified
  // checkout.
  std::filesystem::path packageRoot;
  std::optional<ProjectGitDependencySource> git;
  std::optional<ProjectRegistryDependencySource> registry;
  SourceSpan declaration;
  SourceSpan pathDeclaration;
};

struct ProjectWorkspaceManifest {
  std::vector<std::filesystem::path> members;
  std::vector<SourceSpan> memberDeclarations;
  SourceSpan declaration;
};

// One `[bindgen.<alias>]` import: the driver generates GTI declarations from
// the named C header during project builds and exposes them to this
// package's sources as `#include <alias/bindings>`. The alias occupies the
// same per-package namespace as dependency aliases.
struct ProjectBindgenImport {
  std::string alias;
  // Resolved absolute header path, contained in the package directory.
  std::filesystem::path header;
  // The package-relative spelling as declared, for relocation-stable model
  // identity.
  std::string declaredHeader;
  // Optional GTI namespace wrapped around the generated source names.
  std::string namespaceName;
  std::vector<std::filesystem::path> includeDirectories;
  // Validated `-D<name>[=<value>]` / `-U<name>` preprocessor arguments in
  // declaration order.
  std::vector<std::string> macroArguments;
  // Opt-in import policy (C-07): name selectors, GTI source renames, and
  // declared pointer-plus-count results. Function renames retain their C
  // symbol through [[link_name]]. Sorted for deterministic identity.
  std::vector<std::string> allowPatterns;
  std::vector<std::string> denyPatterns;
  std::vector<std::pair<std::string, std::string>> renames;
  std::vector<std::pair<std::string, std::string>> arrayReturns;
  SourceSpan declaration;
  SourceSpan headerDeclaration;
};

// One declared cross-compilation toolchain: `[toolchains.<triple>]`. The
// table key names the target the entry configures and is normalized through
// the compiler's own triple vocabulary, so a manifest never introduces a
// second spelling table. Selection compares only the OS, vendor, and
// architecture; the data layout always comes from the resolved build target.
struct ProjectToolchain {
  // The triple exactly as spelled in the manifest. Retained for diagnostics
  // and for a relocation-stable project model identity.
  std::string declaredTriple;
  // The normalized target the key selects.
  TargetInfo target;
  // Native C++ compiler and linker for this target. Required, because a
  // declared toolchain that named no compiler would silently fall back to
  // host discovery and link host objects into a cross build.
  std::string cxx;
  // Native C compiler. Optional here; a build that compiles C requires this
  // value or an explicit caller override.
  std::string cc;
  // Resolved platform sysroot, or empty when the entry declares none. A
  // package-contained relative spelling resolves against the package root;
  // an absolute spelling is taken as declared.
  std::filesystem::path sysroot;
  // The sysroot exactly as spelled, for model identity.
  std::string declaredSysroot;
  // Optional native triple spelling passed to the compiler as
  // `--target=<value>`. Omitted for a target-prefixed cross compiler that
  // already knows its target; required for a multi-target Clang. GTI cannot
  // verify that this spelling agrees with the selected target, so it is an
  // explicit declaration in the same sense as a native argument vector.
  std::string nativeTarget;
  SourceSpan declaration;
  SourceSpan cxxDeclaration;
  SourceSpan ccDeclaration;
  SourceSpan sysrootDeclaration;
  SourceSpan nativeTargetDeclaration;
};

enum class ProjectTargetKind {
  Executable,
  Test,
  Library,
};

[[nodiscard]] std::string_view projectTargetKindName(ProjectTargetKind kind);

enum class ProjectTargetLinkage {
  None,
  Shared,
};

[[nodiscard]] std::string_view
projectTargetLinkageName(ProjectTargetLinkage linkage);

struct ProjectExport {
  std::string function;
  std::string symbol;
  SourceSpan declaration;
  SourceSpan functionDeclaration;
  SourceSpan symbolDeclaration;
};

struct ProjectTarget {
  std::string name;
  ProjectTargetKind kind = ProjectTargetKind::Executable;
  ProjectTargetLinkage linkage = ProjectTargetLinkage::None;
  std::filesystem::path root;
  SourceSpan declaration;
  std::vector<ProjectExport> exports;
  ConfigurationFlags defines;
  std::vector<SourceSpan> defineDeclarations;
  ProjectNativeSettings native;
};

struct ProjectProfile {
  std::string name;
  OptimizationLevel optimization = OptimizationLevel::O0;
  CppStandard cppStandard = CppStandard::Cpp23;
  ExecutionProfile executionProfile = ExecutionProfile::SingleThreaded;
  RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off;
  // Report audit findings located inside the standard library; see
  // FrontendOptions::reportLibraryAuditFindings.
  bool rawPointerAuditLibraryFindings = false;
  WarningPolicy warningPolicy;
  bool dependencyWarnings = true;
  bool keepCpp = false;
  bool debugInfo = false;
  SourceSpan declaration;
  ConfigurationFlags defines;
  std::vector<SourceSpan> defineDeclarations;
  ProjectNativeSettings native;
  // Unset selects the target's default backend unless the profile retains
  // C++ output, which configures the C++ backend.
  std::optional<BackendKind> backend;
};

struct ProjectBuildSettings {
  ConfigurationFlags defines;
  std::vector<SourceSpan> defineDeclarations;
  // Consumer-owned permission list. Dependency packages cannot authorize
  // their own build programs: only the selected root/member's [build] table
  // is consulted when plans are assembled.
  std::vector<std::string> allowedBuildScripts;
  std::vector<SourceSpan> allowedBuildScriptDeclarations;
  SourceSpan declaration;
};

class ProjectManifest final {
public:
  ProjectManifest(
      std::filesystem::path path, ProjectPackage package,
      std::vector<ProjectTarget> targets, std::vector<ProjectProfile> profiles,
      std::vector<ProjectDependency> dependencies = {},
      std::optional<ProjectWorkspaceManifest> workspace = std::nullopt,
      ProjectBuildSettings build = {},
      std::vector<ProjectBindgenImport> bindgenImports = {},
      std::vector<ProjectToolchain> toolchains = {});

  [[nodiscard]] const std::filesystem::path &path() const;
  [[nodiscard]] const std::filesystem::path &packageRoot() const;
  [[nodiscard]] const ProjectPackage &package() const;
  [[nodiscard]] const std::vector<ProjectTarget> &targets() const;
  [[nodiscard]] const std::vector<ProjectProfile> &profiles() const;
  [[nodiscard]] const std::vector<ProjectDependency> &dependencies() const;
  [[nodiscard]] const std::optional<ProjectWorkspaceManifest> &
  workspace() const;
  [[nodiscard]] const ProjectBuildSettings &build() const;
  [[nodiscard]] const std::vector<ProjectBindgenImport> &bindgen() const;
  [[nodiscard]] const std::vector<ProjectToolchain> &toolchains() const;
  // Finds the declared toolchain whose normalized key selects `target`.
  // Only the OS, vendor, and architecture participate.
  [[nodiscard]] const ProjectToolchain *
  findToolchain(const TargetInfo &target) const;
  [[nodiscard]] const ProjectTarget *findTarget(std::string_view name) const;
  [[nodiscard]] const ProjectProfile *findProfile(std::string_view name) const;

private:
  std::filesystem::path manifestPath;
  std::filesystem::path rootPath;
  ProjectPackage packageIdentity;
  std::vector<ProjectTarget> projectTargets;
  std::vector<ProjectProfile> buildProfiles;
  std::vector<ProjectDependency> packageDependencies;
  std::optional<ProjectWorkspaceManifest> workspaceManifest;
  ProjectBuildSettings projectBuildSettings;
  std::vector<ProjectBindgenImport> projectBindgenImports;
  std::vector<ProjectToolchain> projectToolchains;
};

enum class ManifestDiscoveryStatus {
  Found,
  NotFound,
  FilesystemFailure,
};

struct ManifestDiscoveryResult {
  ManifestDiscoveryStatus status = ManifestDiscoveryStatus::NotFound;
  std::optional<std::filesystem::path> path;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ManifestDiscoveryStatus::Found && path.has_value();
  }
};

enum class ManifestLoadStatus {
  Success,
  IoFailure,
  ParseFailure,
  ValidationFailure,
};

struct ManifestLoadResult {
  ManifestLoadStatus status = ManifestLoadStatus::ValidationFailure;
  std::optional<ProjectManifest> manifest;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ManifestLoadStatus::Success && manifest.has_value();
  }
};

[[nodiscard]] ManifestDiscoveryResult
discoverProjectManifest(const std::filesystem::path &startDirectory);

[[nodiscard]] ManifestLoadResult loadProjectManifest(
    const std::filesystem::path &manifestPath,
    const std::optional<std::string> &textOverride = std::nullopt,
    ManifestFeaturePolicy features = {});

} // namespace lang::driver
