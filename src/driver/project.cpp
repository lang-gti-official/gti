#include "gti/driver/project.h"

#include "gti/driver/process.h"
#include "gti/format_config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

Diagnostic projectDiagnostic(std::string code, SourceSpan span,
                             std::string message) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        std::move(span), std::move(message));
}

std::size_t editDistance(std::string_view left, std::string_view right) {
  std::vector<std::size_t> previous(right.size() + 1);
  std::vector<std::size_t> current(right.size() + 1);
  for (std::size_t index = 0; index <= right.size(); ++index) {
    previous[index] = index;
  }
  for (std::size_t leftIndex = 0; leftIndex < left.size(); ++leftIndex) {
    current[0] = leftIndex + 1;
    for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
      current[rightIndex + 1] =
          std::min({current[rightIndex] + 1, previous[rightIndex + 1] + 1,
                    previous[rightIndex] +
                        (left[leftIndex] == right[rightIndex] ? 0U : 1U)});
    }
    previous.swap(current);
  }
  return previous.back();
}

template <typename Value, typename Name>
std::optional<std::string_view> nearestName(std::string_view requested,
                                            const std::vector<Value> &values,
                                            Name name) {
  std::optional<std::string_view> nearest;
  std::size_t distance = std::numeric_limits<std::size_t>::max();
  for (const Value &value : values) {
    const std::string_view candidate = name(value);
    const std::size_t candidateDistance = editDistance(requested, candidate);
    if (candidateDistance < distance) {
      nearest = candidate;
      distance = candidateDistance;
    }
  }
  return distance <= 3 ? nearest : std::nullopt;
}

SourceSpan manifestSpan(const ProjectManifest &manifest) {
  if (!manifest.targets().empty()) {
    return manifest.targets().front().declaration;
  }
  return {manifest.path().string(), 0, 1, 1};
}

std::string outputComponent(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    const bool alphabetic = (character >= 'A' && character <= 'Z') ||
                            (character >= 'a' && character <= 'z');
    const bool digit = character >= '0' && character <= '9';
    result.push_back(alphabetic || digit || character == '_' || character == '-'
                         ? character
                         : '_');
  }
  return result.empty() ? "unknown" : result;
}

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end() && candidatePart != candidate.end();
       ++rootPart, ++candidatePart) {
    if (*rootPart != *candidatePart) {
      return false;
    }
  }
  return rootPart == root.end();
}

template <typename Value>
void append(std::vector<Value> &destination, const std::vector<Value> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

bool matches(const ProjectNativePlatform &platform, const TargetInfo &target) {
  return (!platform.os || *platform.os == target.os) &&
         (!platform.vendor || *platform.vendor == target.vendor) &&
         (!platform.arch || *platform.arch == target.arch);
}

template <typename Fragment>
bool validateNativeFragment(const Fragment &fragment, const TargetInfo &target,
                            std::vector<Diagnostic> &diagnostics) {
  bool valid = true;
  const auto spanAt = [&fragment](const std::vector<SourceSpan> &spans,
                                  std::size_t index) {
    return index < spans.size() ? spans[index] : fragment.declaration;
  };
  const auto validatePaths =
      [&](const std::vector<std::filesystem::path> &paths,
          const std::vector<SourceSpan> &spans, bool directory,
          std::string_view kind) {
        for (std::size_t index = 0; index < paths.size(); ++index) {
          std::error_code error;
          const bool exists = std::filesystem::exists(paths[index], error);
          const bool rightKind =
              !error && exists &&
              (directory
                   ? std::filesystem::is_directory(paths[index], error)
                   : std::filesystem::is_regular_file(paths[index], error));
          if (error || !rightKind) {
            diagnostics.push_back(projectDiagnostic(
                "GTI-B1103", spanAt(spans, index),
                "Selected native " + std::string(kind) +
                    " does not name an existing " +
                    (directory ? "directory: " : "regular file: ") +
                    paths[index].string() + "."));
            valid = false;
          }
        }
      };

  validatePaths(fragment.inputs.includeDirectories,
                fragment.includeDirectoryDeclarations, true, "directory");
  validatePaths(fragment.inputs.cSources, fragment.cSourceDeclarations, false,
                "C source");
  validatePaths(fragment.inputs.cppSources, fragment.cppSourceDeclarations,
                false, "C++ source");
  if (target.os != "macos") {
    for (std::size_t index = 0; index < fragment.inputs.cppSources.size();
         ++index) {
      if (fragment.inputs.cppSources[index].extension() == ".mm") {
        diagnostics.push_back(projectDiagnostic(
            "GTI-B1401", spanAt(fragment.cppSourceDeclarations, index),
            "Objective-C++ '.mm' sources require a target whose os is 'macos'; "
            "selected os is '" +
                target.os + "'."));
        valid = false;
      }
    }
  }
  validatePaths(fragment.inputs.libraryDirectories,
                fragment.libraryDirectoryDeclarations, true, "directory");
  validatePaths(fragment.inputs.libraryFiles, fragment.libraryFileDeclarations,
                false, "link file");
  if (target.os != "macos" && !fragment.inputs.frameworks.empty()) {
    diagnostics.push_back(projectDiagnostic(
        "GTI-B1400", spanAt(fragment.frameworkDeclarations, 0),
        "Native frameworks require a target whose os is 'macos'; selected os "
        "is '" +
            target.os + "'."));
    valid = false;
  }
  return valid;
}

bool validateNativeSettings(const ProjectNativeSettings &settings,
                            const TargetInfo &target,
                            std::vector<Diagnostic> &diagnostics) {
  bool valid = validateNativeFragment(settings, target, diagnostics);
  for (const ProjectNativePlatform &platform : settings.platforms) {
    if (matches(platform, target)) {
      valid = validateNativeFragment(platform, target, diagnostics) && valid;
    }
  }
  return valid;
}

void appendOrderedLinkOperands(NativeInputs &destination,
                               const NativeInputs &source) {
  // A manifest fragment has a fixed category order; TOML key order does not
  // interleave categories. More-specific fragments are ordered by the caller.
  for (const std::filesystem::path &file : source.libraryFiles) {
    destination.orderedLinkOperands.push_back(
        {NativeLinkOperandKind::File, file.string()});
  }
  for (const std::string &library : source.libraries) {
    destination.orderedLinkOperands.push_back(
        {NativeLinkOperandKind::Library, library});
  }
  for (const std::string &framework : source.frameworks) {
    destination.orderedLinkOperands.push_back(
        {NativeLinkOperandKind::Framework, framework});
  }
}

void appendSearchPathsAndLinkOperands(NativeInputs &destination,
                                      const ProjectNativeSettings &settings,
                                      const TargetInfo &target) {
  for (const ProjectNativePlatform &platform : settings.platforms) {
    if (!matches(platform, target)) {
      continue;
    }
    append(destination.includeDirectories, platform.inputs.includeDirectories);
    append(destination.cSources, platform.inputs.cSources);
    append(destination.cppSources, platform.inputs.cppSources);
    append(destination.libraryDirectories, platform.inputs.libraryDirectories);
    append(destination.libraryFiles, platform.inputs.libraryFiles);
    append(destination.libraries, platform.inputs.libraries);
    append(destination.frameworks, platform.inputs.frameworks);
    appendOrderedLinkOperands(destination, platform.inputs);
  }
  append(destination.includeDirectories, settings.inputs.includeDirectories);
  append(destination.cSources, settings.inputs.cSources);
  append(destination.cppSources, settings.inputs.cppSources);
  append(destination.libraryDirectories, settings.inputs.libraryDirectories);
  append(destination.libraryFiles, settings.inputs.libraryFiles);
  append(destination.libraries, settings.inputs.libraries);
  append(destination.frameworks, settings.inputs.frameworks);
  appendOrderedLinkOperands(destination, settings.inputs);
}

void appendNativeArguments(NativeInputs &destination,
                           const NativeInputs &source) {
  append(destination.compilerArguments, source.compilerArguments);
  append(destination.cCompilerArguments, source.cCompilerArguments);
  append(destination.linkerArguments, source.linkerArguments);
  append(destination.trailingArguments, source.trailingArguments);
}

void appendArguments(NativeInputs &destination,
                     const ProjectNativeSettings &settings,
                     const TargetInfo &target) {
  appendNativeArguments(destination, settings.inputs);
  for (const ProjectNativePlatform &platform : settings.platforms) {
    if (matches(platform, target)) {
      appendNativeArguments(destination, platform.inputs);
    }
  }
}

std::optional<NativeInputs> resolveNativeInputs(
    const ProjectManifest &manifest, const ProjectTarget &selectedTarget,
    const ProjectProfile &selectedProfile, const TargetInfo &target,
    std::vector<Diagnostic> &diagnostics) {
  const bool validPackage =
      validateNativeSettings(manifest.package().native, target, diagnostics);
  const bool validProfile =
      validateNativeSettings(selectedProfile.native, target, diagnostics);
  const bool validTarget =
      validateNativeSettings(selectedTarget.native, target, diagnostics);
  if (!validPackage || !validProfile || !validTarget) {
    return std::nullopt;
  }

  NativeInputs inputs;
  // Search order is most-specific first so a target-local header or library
  // directory can shadow a broader profile or package directory.
  appendSearchPathsAndLinkOperands(inputs, selectedTarget.native, target);
  appendSearchPathsAndLinkOperands(inputs, selectedProfile.native, target);
  appendSearchPathsAndLinkOperands(inputs, manifest.package().native, target);

  // Compiler/linker arguments are least-specific first.
  // Higher-priority scopes therefore retain conventional last-argument-wins
  // behavior. Link operands use the opposite order above so a more-specific
  // library may depend on symbols supplied by a broader package library.
  appendArguments(inputs, manifest.package().native, target);
  appendArguments(inputs, selectedProfile.native, target);
  appendArguments(inputs, selectedTarget.native, target);
  inputs.cStandard = CStandard::C17;
  if (manifest.package().native.inputs.cStandard) {
    inputs.cStandard = manifest.package().native.inputs.cStandard;
  }
  if (selectedProfile.native.inputs.cStandard) {
    inputs.cStandard = selectedProfile.native.inputs.cStandard;
  }
  if (selectedTarget.native.inputs.cStandard) {
    inputs.cStandard = selectedTarget.native.inputs.cStandard;
  }
  return inputs;
}

// The only opaque-argument form a dependency may contribute to its own
// sources: one exact `-D<name>[=<value>]` or `-U<name>` argv element. A
// macro definition cannot introduce undeclared inputs, change compilation
// modes, or alter driver-owned policy, and values stay one exact argument.
bool composableMacroDefinition(std::string_view argument) {
  if (argument.size() < 3 ||
      (!argument.starts_with("-D") && !argument.starts_with("-U"))) {
    return false;
  }
  const std::string_view name = argument.substr(2);
  const std::size_t nameEnd = argument.starts_with("-U")
                                  ? name.size()
                                  : std::min(name.find('='), name.size());
  if (nameEnd == 0) {
    return false;
  }
  for (std::size_t index = 0; index < nameEnd; ++index) {
    const char character = name[index];
    const bool wordCharacter = (character >= 'A' && character <= 'Z') ||
                               (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') ||
                               character == '_';
    if (!wordCharacter ||
        (index == 0 && character >= '0' && character <= '9')) {
      return false;
    }
  }
  if (argument.starts_with("-U") && name.find('=') != std::string_view::npos) {
    return false;
  }
  return std::none_of(name.begin(), name.end(), [](const char character) {
    return static_cast<unsigned char>(character) < 0x20U;
  });
}

// Composes the selected package's transitive dependency closure into
// per-package native groups, dependents before dependencies. The trust
// boundary the manifest design reserved: structured, package-contained
// inputs and name-resolved libraries compose; opaque argument vectors do
// not, except the exact macro-definition subset above, and linker/raw
// argument vectors never compose.
std::optional<std::vector<NativeDependencyGroup>> composeDependencyNativeGroups(
    const ProjectWorkspace &workspace, const ResolvedProjectPackage &selected,
    const TargetInfo &target, std::vector<Diagnostic> &diagnostics) {
  std::vector<const ResolvedProjectPackage *> postorder;
  std::set<std::string> visited{selected.identity()};
  const std::function<void(const ResolvedProjectPackage &)> visit =
      [&](const ResolvedProjectPackage &package) {
        for (const ResolvedProjectDependency &dependency :
             package.dependencies) {
          if (!visited.insert(dependency.targetIdentity).second) {
            continue;
          }
          const ResolvedProjectPackage *targetPackage =
              workspace.findPackage(dependency.targetIdentity);
          if (targetPackage == nullptr) {
            continue;
          }
          visit(*targetPackage);
          postorder.push_back(targetPackage);
        }
      };
  visit(selected);

  std::vector<NativeDependencyGroup> groups;
  bool valid = true;
  const auto reject = [&](const SourceSpan &span, std::string message,
                          std::string hint) {
    Diagnostic diagnostic =
        projectDiagnostic("GTI-B1606", span, std::move(message));
    diagnostic.hints.push_back(std::move(hint));
    diagnostics.push_back(std::move(diagnostic));
    valid = false;
  };
  // Reverse postorder is a topological dependents-before-dependencies order
  // over dependent -> dependency edges.
  for (auto package = postorder.rbegin(); package != postorder.rend();
       ++package) {
    const ProjectNativeSettings &settings =
        (*package)->manifest.package().native;
    NativeInputs scratch;
    appendSearchPathsAndLinkOperands(scratch, settings, target);
    appendArguments(scratch, settings, target);
    const bool declaresAnything =
        !scratch.includeDirectories.empty() || !scratch.cSources.empty() ||
        !scratch.cppSources.empty() || !scratch.libraryDirectories.empty() ||
        !scratch.orderedLinkOperands.empty() ||
        !scratch.compilerArguments.empty() ||
        !scratch.cCompilerArguments.empty() ||
        !scratch.linkerArguments.empty() || !scratch.trailingArguments.empty();
    if (!declaresAnything && !(*package)->manifest.package().buildScript) {
      continue;
    }
    const std::string identity = (*package)->identity();
    if (!validateNativeSettings(settings, target, diagnostics)) {
      valid = false;
      continue;
    }
    if (!scratch.linkerArguments.empty() ||
        !scratch.trailingArguments.empty()) {
      reject(settings.declaration,
             "Dependency package '" + identity +
                 "' declares native linker or raw argument vectors, which "
                 "cannot be composed across package boundaries.",
             "Linker and raw arguments are global to the final link; declare "
             "them in the selected application package instead.");
      continue;
    }
    bool argumentsComposable = true;
    const auto requireMacros = [&](const std::vector<std::string> &arguments,
                                   std::string_view field) {
      for (const std::string &argument : arguments) {
        if (!composableMacroDefinition(argument)) {
          reject(settings.declaration,
                 "Dependency package '" + identity + "' declares " +
                     std::string(field) + " argument '" + argument +
                     "', which cannot be composed across package boundaries.",
                 "Only exact -D<name>[=<value>] and -U<name> macro "
                 "definitions compose from a dependency; other compiler "
                 "arguments belong to the selected application package.");
          argumentsComposable = false;
        }
      }
    };
    requireMacros(scratch.cCompilerArguments, "a C compiler");
    requireMacros(scratch.compilerArguments, "a compiler");
    if (!argumentsComposable) {
      continue;
    }

    NativeDependencyGroup group{
        .packageIdentity = identity,
        .includeDirectories = std::move(scratch.includeDirectories),
        .cMacroDefinitions = std::move(scratch.cCompilerArguments),
        .cppMacroDefinitions = std::move(scratch.compilerArguments),
        .cSources = std::move(scratch.cSources),
        .cStandard = settings.inputs.cStandard.value_or(CStandard::C17),
        .cppSources = std::move(scratch.cppSources),
        .libraryDirectories = std::move(scratch.libraryDirectories),
        .linkOperands = std::move(scratch.orderedLinkOperands)};
    groups.push_back(std::move(group));
  }
  if (!valid) {
    return std::nullopt;
  }
  return groups;
}

std::optional<std::vector<BuildScriptPlan>>
composeBuildScriptPlans(const ProjectWorkspace &workspace,
                        const ResolvedProjectPackage &selected,
                        const TargetInfo &target, std::string_view profile,
                        std::vector<Diagnostic> &diagnostics) {
  std::vector<const ResolvedProjectPackage *> ordered;
  std::set<std::string> visited;
  const std::function<void(const ResolvedProjectPackage &)> visit =
      [&](const ResolvedProjectPackage &package) {
        if (!visited.insert(package.identity()).second) {
          return;
        }
        for (const ResolvedProjectDependency &dependency :
             package.dependencies) {
          if (const ResolvedProjectPackage *targetPackage =
                  workspace.findPackage(dependency.targetIdentity)) {
            visit(*targetPackage);
          }
        }
        ordered.push_back(&package);
      };
  visit(selected);

  const std::vector<std::string> &allowed =
      selected.manifest.build().allowedBuildScripts;
  std::vector<BuildScriptPlan> plans;
  bool valid = true;
  for (const ResolvedProjectPackage *package : ordered) {
    const std::optional<ProjectBuildScript> &script =
        package->manifest.package().buildScript;
    if (!script) {
      continue;
    }
    const bool isSelected = package->identity() == selected.identity();
    if (!isSelected && std::find(allowed.begin(), allowed.end(),
                                 package->identity()) == allowed.end()) {
      Diagnostic diagnostic = projectDiagnostic(
          "GTI-B2002", script->declaration,
          "Dependency package '" + package->identity() +
              "' declares a host build script that the selected package has "
              "not approved.");
      diagnostic.hints.push_back(
          "Add '" + package->identity() +
          "' to [build].allow-build-scripts after reviewing the pinned "
          "dependency and its build command.");
      diagnostics.push_back(std::move(diagnostic));
      valid = false;
      continue;
    }

    std::filesystem::path output =
        workspace.root() / "build" / "gti" / "build-scripts" /
        portableProjectNameKey(package->manifest.package().name) /
        package->manifest.package().version / targetTriple(target) /
        std::string(profile);
    plans.push_back({.packageIdentity = package->identity(),
                     .packageRoot = package->manifest.packageRoot(),
                     .command = script->command,
                     .source = script->source,
                     .inputs = script->inputs,
                     .declaredInputs = script->declaredInputs,
                     .outputDirectory = std::move(output),
                     .declaration = script->declaration,
                     .selectedPackage = isSelected});
  }
  if (!valid) {
    return std::nullopt;
  }
  return plans;
}

const ProjectProfile *selectProfile(const ProjectManifest &manifest,
                                    std::string_view requested,
                                    std::vector<Diagnostic> &diagnostics) {
  const ProjectProfile *profile = manifest.findProfile(requested);
  if (profile != nullptr) {
    return profile;
  }

  Diagnostic diagnostic = projectDiagnostic("GTI-B1202", manifestSpan(manifest),
                                            "Unknown build profile '" +
                                                std::string(requested) + "'.");
  if (const std::optional<std::string_view> nearest =
          nearestName(requested, manifest.profiles(),
                      [](const ProjectProfile &candidate) -> std::string_view {
                        return candidate.name;
                      })) {
    diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) + "'?");
  }
  diagnostics.push_back(std::move(diagnostic));
  return nullptr;
}

// Finds the `[toolchains.<triple>]` entry configuring `target`. The selected
// package owns the declaration; a workspace member falls back to the
// workspace root package, which is where a workspace-wide cross toolchain
// naturally belongs.
const ProjectToolchain *findDeclaredToolchain(const ProjectWorkspace &workspace,
                                              const ProjectManifest &manifest,
                                              const TargetInfo &target) {
  if (const ProjectToolchain *declared = manifest.findToolchain(target)) {
    return declared;
  }
  for (const ResolvedProjectPackage &package : workspace.packages()) {
    if (package.membership != ProjectPackageMembership::Root) {
      continue;
    }
    return package.manifest.findToolchain(target);
  }
  return nullptr;
}

// Resolves the native toolchain a plan builds with, and fails closed when a
// command that produces a native artifact selected a target that is not the
// build host and no manifest declares how to build for it. Falling back to
// host tools there would silently publish an executable for the wrong
// target, which no later stage can detect.
std::optional<ResolvedProjectToolchain>
resolveBuildToolchain(const ProjectWorkspace &workspace,
                      const ProjectManifest &manifest, const TargetInfo &target,
                      ToolchainSelectionPolicy policy,
                      std::vector<Diagnostic> &diagnostics) {
  ResolvedProjectToolchain resolved;
  const std::string selected = targetTriple(target);
  const std::string host = targetTriple(TargetInfo::host());
  const ProjectToolchain *declared =
      findDeclaredToolchain(workspace, manifest, target);
  if (declared == nullptr) {
    if (selected != host &&
        policy == ToolchainSelectionPolicy::DeclaredForNonHostTarget) {
      Diagnostic diagnostic = projectDiagnostic(
          "GTI-B1904", manifestSpan(manifest),
          "No [toolchains." + selected +
              "] table declares a native toolchain for target '" + selected +
              "'.");
      diagnostic.hints.push_back("Declare [toolchains." + selected +
                                 "] with at least a 'cxx' compiler.");
      diagnostic.hints.push_back(
          "The build host is '" + host +
          "'; host compilers are never reused for another target, because "
          "the published executable would be built for the wrong one.");
      diagnostics.push_back(std::move(diagnostic));
      return std::nullopt;
    }
    return resolved;
  }
  resolved.triple = declared->declaredTriple;
  resolved.cxx = declared->cxx;
  resolved.cc = declared->cc;
  resolved.sysroot = declared->sysroot;
  resolved.declaredSysroot = declared->declaredSysroot;
  resolved.nativeTarget = declared->nativeTarget;
  resolved.declaration = declared->declaration;
  return resolved;
}

// A declared toolchain that names no C compiler cannot build a package with
// C sources: host `cc` discovery would produce host objects for a cross
// link. Only a build that actually compiles C is rejected, so a pure-GTI
// cross build needs no `cc`.
bool checkToolchainCCoverage(const ResolvedProjectToolchain &toolchain,
                             const NativeInputs &inputs,
                             ToolchainSelectionPolicy policy,
                             bool hasCCompilerOverride,
                             std::vector<Diagnostic> &diagnostics) {
  if (policy == ToolchainSelectionPolicy::HostTools || hasCCompilerOverride ||
      !toolchain.declared() || !toolchain.cc.empty()) {
    return true;
  }
  const bool compilesC = !inputs.cSources.empty() ||
                         std::any_of(inputs.dependencyGroups.begin(),
                                     inputs.dependencyGroups.end(),
                                     [](const NativeDependencyGroup &group) {
                                       return !group.cSources.empty();
                                     });
  if (!compilesC) {
    return true;
  }
  Diagnostic diagnostic = projectDiagnostic(
      "GTI-B1905", toolchain.declaration,
      "Toolchain '" + toolchain.triple +
          "' declares no 'cc', but this build compiles native C sources.");
  diagnostic.hints.push_back(
      "Declare cc = \"<c-compiler>\" beside cxx for that target.");
  diagnostics.push_back(std::move(diagnostic));
  return false;
}

ProjectBuildPlan makeBuildPlan(
    const ProjectWorkspace &workspace, const ProjectManifest &manifest,
    const ProjectTarget &selectedTarget, const ProjectProfile &selectedProfile,
    const TargetInfo &target, NativeInputs nativeInputs,
    std::vector<BuildScriptPlan> buildScripts,
    const ProjectBuildOverrides &overrides = {},
    RuntimeProfile runtimeProfile = RuntimeProfile::None,
    ResolvedProjectToolchain toolchain = {}) {
  for (BuildScriptPlan &script : buildScripts) {
    script.outputDirectory /= portableProjectNameKey(selectedTarget.name);
  }
  const OptimizationLevel optimization =
      overrides.optimization.value_or(selectedProfile.optimization);
  const CppStandard cppStandard =
      overrides.cppStandard.value_or(selectedProfile.cppStandard);
  const bool keepCpp = overrides.keepCpp.value_or(selectedProfile.keepCpp);
  const bool debugInfo =
      overrides.debugInfo.value_or(selectedProfile.debugInfo);
  ConfigurationFlags configurationFlags = manifest.build().defines;
  configurationFlags.insert(configurationFlags.end(),
                            overrides.configurationFlags.begin(),
                            overrides.configurationFlags.end());
  std::vector<std::string> disabledWarnings =
      selectedProfile.warningPolicy.disabledGroups();
  disabledWarnings.insert(disabledWarnings.end(),
                          overrides.warningPolicy.disabledGroups().begin(),
                          overrides.warningPolicy.disabledGroups().end());
  WarningPolicy warningPolicy(std::move(disabledWarnings));
  std::vector<std::string> dependencyWarningIdentities;
  const std::string selectedPackageIdentity =
      workspace.selectedPackage().identity();
  for (const ResolvedProjectPackage &package : workspace.packages()) {
    if (package.identity() != selectedPackageIdentity) {
      dependencyWarningIdentities.push_back(package.identity());
    }
  }
  PackageWarningPolicy packageWarningPolicy(
      selectedProfile.dependencyWarnings, selectedPackageIdentity,
      std::move(dependencyWarningIdentities));
  const AnalysisConfiguration configuration(
      target,
      overrides.executionProfile.value_or(selectedProfile.executionProfile),
      runtimeProfile, std::move(configurationFlags));
  const TargetInfo &resolvedTarget = configuration.target();
  std::filesystem::path outputDirectory = workspace.root() / "build" / "gti";
  if (workspace.declared()) {
    outputDirectory /= "packages";
    outputDirectory /= manifest.package().name;
  }
  outputDirectory /= selectedProfile.name;
  outputDirectory /= targetTriple(resolvedTarget);
  std::string executableName = selectedTarget.name;
#if defined(_WIN32)
  executableName += ".exe";
#endif
  const std::filesystem::path output = outputDirectory / executableName;
  const std::filesystem::path generatedSource =
      outputDirectory / ".gti-intermediate" /
      (selectedTarget.name + ".gti.cpp");
  // The selected target and the toolchain realizing it are model facts. A
  // host build and a cross build must never share a cache entry, and
  // changing the declared compiler, sysroot, or native triple must
  // invalidate one.
  nativeInputs.targetPolicy = {.triple = toolchain.nativeTarget,
                               .sysroot = toolchain.sysroot};
  std::string modelIdentity = workspace.modelIdentity();
  modelIdentity += selectedProfile.dependencyWarnings
                       ? "\ndependency-warnings:true"
                       : "\ndependency-warnings:false";
  if (debugInfo)
    modelIdentity += "\ndebug-info:true";
  modelIdentity += "\ntarget:" + targetTriple(resolvedTarget);
  if (toolchain.declared()) {
    modelIdentity +=
        "\ntoolchain:" + toolchain.triple + "|cxx=" + toolchain.cxx;
    if (!toolchain.cc.empty()) {
      modelIdentity += "|cc=" + toolchain.cc;
    }
    if (!toolchain.declaredSysroot.empty()) {
      modelIdentity += "|sysroot=" + toolchain.declaredSysroot;
    }
    if (!toolchain.nativeTarget.empty()) {
      modelIdentity += "|native-target=" + toolchain.nativeTarget;
    }
  }
  for (const std::string &flag : configuration.configurationFlags()) {
    modelIdentity += "\nconfiguration-flag:" + flag;
  }
  for (const BuildScriptPlan &script : buildScripts) {
    modelIdentity += "\nbuild-script:" + script.packageIdentity;
    if (script.source) {
      modelIdentity += "|source=" + std::filesystem::relative(
                                        *script.source, script.packageRoot)
                                        .generic_string();
    }
    for (const std::string &argument : script.command) {
      modelIdentity += "|argv=" + argument;
    }
    for (const std::string &input : script.declaredInputs) {
      modelIdentity += "|input=" + input;
    }
  }

  // Bindgen imports become synthetic package source roots: the generated
  // directory is an ordinary alias edge of the declaring package, so
  // `#include <alias/bindings>` resolves through the existing dependency
  // mechanism and stays invisible to every other package. The declared
  // (relative) header spelling and options join the model identity; the
  // generated unit's content participates in cache identity as ordinary
  // loaded source.
  std::vector<PackageSourceRoot> packageSourceRoots =
      workspace.packageSourceRoots();
  std::vector<BindgenImportPlan> bindgenImports;
  for (const ResolvedProjectPackage &resolved : workspace.packages()) {
    for (const ProjectBindgenImport &import : resolved.manifest.bindgen()) {
      std::filesystem::path importDirectory =
          workspace.root() / "build" / "gti" / "bindgen";
      importDirectory /=
          portableProjectNameKey(resolved.manifest.package().name);
      importDirectory /= import.alias;
      const std::string syntheticIdentity =
          resolved.identity() + "/bindgen/" + import.alias;
      const auto owner =
          std::find_if(packageSourceRoots.begin(), packageSourceRoots.end(),
                       [&resolved](const PackageSourceRoot &root) {
                         return root.identity == resolved.identity();
                       });
      if (owner == packageSourceRoots.end()) {
        continue;
      }
      owner->dependencies.push_back(
          {.alias = import.alias, .targetIdentity = syntheticIdentity});
      packageSourceRoots.push_back({.identity = syntheticIdentity,
                                    .name = import.alias,
                                    .packageRoot = importDirectory,
                                    .sourceRoot = importDirectory,
                                    .dependencies = {}});
      modelIdentity += "\nbindgen:" + resolved.identity() + ":" + import.alias +
                       "=" + import.declaredHeader;
      if (!import.namespaceName.empty()) {
        modelIdentity += "|namespace=" + import.namespaceName;
      }
      for (const std::string &argument : import.macroArguments) {
        modelIdentity += "|macro=" + argument;
      }
      for (const std::string &pattern : import.allowPatterns) {
        modelIdentity += "|allow=" + pattern;
      }
      for (const std::string &pattern : import.denyPatterns) {
        modelIdentity += "|deny=" + pattern;
      }
      for (const auto &[from, to] : import.renames) {
        modelIdentity += "|rename=" + from + ">" + to;
      }
      for (const auto &[function, count] : import.arrayReturns) {
        modelIdentity += "|array=" + function + ">" + count;
      }
      bindgenImports.push_back({.packageName = resolved.manifest.package().name,
                                .alias = import.alias,
                                .header = import.header,
                                .declaredHeader = import.declaredHeader,
                                .namespaceName = import.namespaceName,
                                .includeDirectories = import.includeDirectories,
                                .macroArguments = import.macroArguments,
                                .allowPatterns = import.allowPatterns,
                                .denyPatterns = import.denyPatterns,
                                .renames = import.renames,
                                .arrayReturns = import.arrayReturns,
                                .outputDirectory = std::move(importDirectory),
                                .manifestPath = resolved.manifest.path(),
                                .declaration = import.declaration});
    }
  }

  return ProjectBuildPlan(
      manifest.path(), manifest.packageRoot(), workspace.root(),
      manifest.package().name, manifest.package().version, selectedTarget.name,
      selectedTarget.kind, selectedProfile.name, selectedTarget.root, output,
      generatedSource, resolvedTarget, optimization, cppStandard, keepCpp,
      std::move(nativeInputs), std::move(packageSourceRoots),
      configuration.configurationFlags(), std::move(warningPolicy),
      std::move(modelIdentity), std::move(bindgenImports),
      std::move(buildScripts), std::move(toolchain), debugInfo,
      std::move(packageWarningPolicy));
}

Diagnostic cleanDiagnostic(const std::filesystem::path &manifest,
                           std::string code, std::string message) {
  return projectDiagnostic(std::move(code), {manifest.string(), 0, 1, 1},
                           std::move(message));
}

enum class ScaffoldEntryKind {
  Missing,
  File,
  Directory,
  Other,
  Failure,
};

ScaffoldEntryKind inspectScaffoldEntry(const std::filesystem::path &path,
                                       std::error_code &error) {
  error.clear();
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    return ScaffoldEntryKind::Missing;
  }
  if (error) {
    return ScaffoldEntryKind::Failure;
  }
  if (!std::filesystem::exists(status)) {
    return ScaffoldEntryKind::Missing;
  }
  if (std::filesystem::is_regular_file(status)) {
    return ScaffoldEntryKind::File;
  }
  if (std::filesystem::is_directory(status)) {
    return ScaffoldEntryKind::Directory;
  }
  return ScaffoldEntryKind::Other;
}

bool writeScaffoldFile(const std::filesystem::path &path,
                       std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return static_cast<bool>(output);
}

std::string processFailureDetail(const ProcessResult &process) {
  constexpr std::size_t limit = 400;
  std::string detail = process.driverDiagnostic.value_or("");
  if (!process.output.empty()) {
    if (!detail.empty()) {
      detail += "; ";
    }
    detail += process.output.size() > limit
                  ? "..." + process.output.substr(process.output.size() - limit)
                  : process.output;
  }
  while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
    detail.pop_back();
  }
  return detail.empty() ? "Git returned a nonzero status" : detail;
}

Diagnostic scaffoldDiagnostic(const std::filesystem::path &path,
                              std::string code, std::string message) {
  return projectDiagnostic(std::move(code), {path.string(), 0, 1, 1},
                           std::move(message));
}

std::string scaffoldManifest(std::string_view packageName) {
  return "manifest-version = 1\n\n"
         "[package]\n"
         "name = \"" +
         std::string(packageName) +
         "\"\n"
         "version = \"0.1.0\"\n\n"
         "[targets." +
         std::string(packageName) +
         "]\n"
         "kind = \"executable\"\n"
         "root = \"src/main.gti\"\n";
}

constexpr std::string_view scaffoldMainSource = R"(#include <std/string>
#include <std/vector>

int main(int argc, std::vector<std::string> argv) {
  std::println("Hello, GTI!");
  return 0;
}
)";

constexpr std::string_view scaffoldGitIgnore = R"(# GTI build output
/build/
)";

} // namespace

ProjectBuildRequest::ProjectBuildRequest(
    std::filesystem::path startDirectory, std::optional<std::string> targetName,
    std::string profileName, TargetInfo target, ProjectBuildOverrides overrides,
    std::optional<std::string> packageName,
    WorkspaceDependencyPolicy dependencyPolicy, RuntimeProfile runtimeProfile,
    ToolchainSelectionPolicy toolchainPolicy,
    ProjectToolchainOverrides toolchainOverrides)
    : discoveryStart(std::move(startDirectory)),
      selectedTarget(std::move(targetName)),
      selectedProfile(std::move(profileName)), targetInfo(std::move(target)),
      cliOverrides(std::move(overrides)),
      selectedPackage(std::move(packageName)),
      workspaceDependencyPolicy(dependencyPolicy),
      selectedRuntimeProfile(runtimeProfile),
      selectedToolchainPolicy(toolchainPolicy),
      nativeToolchainOverrides(toolchainOverrides) {}

const WorkspaceDependencyPolicy &ProjectBuildRequest::dependencyPolicy() const {
  return workspaceDependencyPolicy;
}

RuntimeProfile ProjectBuildRequest::runtimeProfile() const {
  return selectedRuntimeProfile;
}

ToolchainSelectionPolicy ProjectBuildRequest::toolchainPolicy() const {
  return selectedToolchainPolicy;
}

const ProjectToolchainOverrides &
ProjectBuildRequest::toolchainOverrides() const {
  return nativeToolchainOverrides;
}

const std::filesystem::path &ProjectBuildRequest::startDirectory() const {
  return discoveryStart;
}

const std::optional<std::string> &ProjectBuildRequest::targetName() const {
  return selectedTarget;
}

const std::string &ProjectBuildRequest::profileName() const {
  return selectedProfile;
}

const TargetInfo &ProjectBuildRequest::target() const { return targetInfo; }

const ProjectBuildOverrides &ProjectBuildRequest::overrides() const {
  return cliOverrides;
}

const std::optional<std::string> &ProjectBuildRequest::packageName() const {
  return selectedPackage;
}

ProjectBuildPlan::ProjectBuildPlan(
    std::filesystem::path manifestPath, std::filesystem::path packageRoot,
    std::filesystem::path workspaceRoot, std::string packageName,
    std::string packageVersion, std::string targetName,
    ProjectTargetKind targetKind, std::string profileName,
    std::filesystem::path entry, std::filesystem::path output,
    std::filesystem::path generatedSource, TargetInfo target,
    OptimizationLevel optimization, CppStandard cppStandard, bool keepCpp,
    NativeInputs nativeInputs,
    std::vector<PackageSourceRoot> packageSourceRoots,
    ConfigurationFlags configurationFlags, WarningPolicy warningPolicy,
    std::string projectModelIdentity,
    std::vector<BindgenImportPlan> bindgenImports,
    std::vector<BuildScriptPlan> buildScripts,
    ResolvedProjectToolchain toolchain, bool debugInfo,
    PackageWarningPolicy packageWarnings)
    : projectManifestPath(std::move(manifestPath)),
      projectRoot(std::move(packageRoot)),
      projectWorkspaceRoot(std::move(workspaceRoot)),
      projectPackageName(std::move(packageName)),
      projectPackageVersion(std::move(packageVersion)),
      projectTargetName(std::move(targetName)), projectTargetKind(targetKind),
      buildProfileName(std::move(profileName)), entryPath(std::move(entry)),
      outputPath(std::move(output)),
      generatedSourcePath(std::move(generatedSource)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard), retainGeneratedSource(keepCpp),
      resolvedNativeInputs(std::move(nativeInputs)),
      resolvedPackageSources(std::move(packageSourceRoots)),
      resolvedConfigurationFlags(std::move(configurationFlags)),
      resolvedWarningPolicy(std::move(warningPolicy)),
      resolvedPackageWarningPolicy(std::move(packageWarnings)),
      resolvedProjectModelIdentity(std::move(projectModelIdentity)),
      resolvedBindgenImports(std::move(bindgenImports)),
      resolvedBuildScripts(std::move(buildScripts)),
      resolvedToolchain(std::move(toolchain)), sourceDebugInfo(debugInfo) {}

const std::filesystem::path &ProjectBuildPlan::manifestPath() const {
  return projectManifestPath;
}

const std::filesystem::path &ProjectBuildPlan::packageRoot() const {
  return projectRoot;
}

const std::filesystem::path &ProjectBuildPlan::workspaceRoot() const {
  return projectWorkspaceRoot;
}

const std::string &ProjectBuildPlan::packageName() const {
  return projectPackageName;
}

const std::string &ProjectBuildPlan::packageVersion() const {
  return projectPackageVersion;
}

const std::string &ProjectBuildPlan::targetName() const {
  return projectTargetName;
}

ProjectTargetKind ProjectBuildPlan::targetKind() const {
  return projectTargetKind;
}

const std::string &ProjectBuildPlan::profileName() const {
  return buildProfileName;
}

const std::filesystem::path &ProjectBuildPlan::entry() const {
  return entryPath;
}

const std::filesystem::path &ProjectBuildPlan::output() const {
  return outputPath;
}

const std::filesystem::path &ProjectBuildPlan::generatedSource() const {
  return generatedSourcePath;
}

const TargetInfo &ProjectBuildPlan::target() const { return targetInfo; }

OptimizationLevel ProjectBuildPlan::optimization() const {
  return optimizationLevel;
}

CppStandard ProjectBuildPlan::cppStandard() const { return backendStandard; }

bool ProjectBuildPlan::keepCpp() const { return retainGeneratedSource; }

bool ProjectBuildPlan::debugInfo() const { return sourceDebugInfo; }

const NativeInputs &ProjectBuildPlan::nativeInputs() const {
  return resolvedNativeInputs;
}

const std::vector<PackageSourceRoot> &ProjectBuildPlan::packageSources() const {
  return resolvedPackageSources;
}

const ConfigurationFlags &ProjectBuildPlan::configurationFlags() const {
  return resolvedConfigurationFlags;
}

const WarningPolicy &ProjectBuildPlan::warnings() const {
  return resolvedWarningPolicy;
}

const PackageWarningPolicy &ProjectBuildPlan::packageWarnings() const {
  return resolvedPackageWarningPolicy;
}

const ResolvedProjectToolchain &ProjectBuildPlan::toolchain() const {
  return resolvedToolchain;
}

const std::string &ProjectBuildPlan::projectModelIdentity() const {
  return resolvedProjectModelIdentity;
}

const std::vector<BindgenImportPlan> &ProjectBuildPlan::bindgenImports() const {
  return resolvedBindgenImports;
}

const std::vector<BuildScriptPlan> &ProjectBuildPlan::buildScripts() const {
  return resolvedBuildScripts;
}

ProjectMetadata::ProjectMetadata(ProjectWorkspace workspace, TargetInfo target,
                                 std::vector<ProjectBuildPlan> plans)
    : projectWorkspace(std::move(workspace)), targetInfo(std::move(target)),
      buildPlans(std::move(plans)) {}

const ProjectManifest &ProjectMetadata::manifest() const {
  return projectWorkspace.selectedPackage().manifest;
}

const ProjectWorkspace &ProjectMetadata::workspace() const {
  return projectWorkspace;
}

const TargetInfo &ProjectMetadata::target() const { return targetInfo; }

const std::vector<ProjectBuildPlan> &ProjectMetadata::plans() const {
  return buildPlans;
}

ProjectScaffoldRequest::ProjectScaffoldRequest(
    ProjectScaffoldMode mode, std::filesystem::path destination,
    std::optional<std::string> packageName,
    std::optional<ProjectScaffoldGit> git)
    : scaffoldMode(mode), destinationPath(std::move(destination)),
      requestedPackageName(std::move(packageName)),
      requestedGit(std::move(git)) {}

ProjectScaffoldMode ProjectScaffoldRequest::mode() const {
  return scaffoldMode;
}

const std::filesystem::path &ProjectScaffoldRequest::destination() const {
  return destinationPath;
}

const std::optional<std::string> &ProjectScaffoldRequest::packageName() const {
  return requestedPackageName;
}

const std::optional<ProjectScaffoldGit> &ProjectScaffoldRequest::git() const {
  return requestedGit;
}

std::string targetTriple(const TargetInfo &target) {
  return outputComponent(target.arch) + "-" + outputComponent(target.vendor) +
         "-" + outputComponent(target.os);
}

ProjectResolutionResult
resolveProjectBuild(const ProjectBuildRequest &request) {
  ProjectResolutionResult result;
  WorkspaceResolutionResult resolvedWorkspace =
      resolveProjectWorkspace(request.startDirectory(), request.packageName(),
                              request.dependencyPolicy());
  result.sources = std::move(resolvedWorkspace.sources);
  if (!resolvedWorkspace.succeeded()) {
    switch (resolvedWorkspace.status) {
    case WorkspaceResolutionStatus::DiscoveryFailure:
      result.status = ProjectResolutionStatus::DiscoveryFailure;
      break;
    case WorkspaceResolutionStatus::ManifestFailure:
      result.status = ProjectResolutionStatus::ManifestFailure;
      break;
    case WorkspaceResolutionStatus::GraphFailure:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    case WorkspaceResolutionStatus::SelectionFailure:
      result.status = ProjectResolutionStatus::SelectionFailure;
      break;
    case WorkspaceResolutionStatus::Success:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    }
    result.diagnostics = std::move(resolvedWorkspace.diagnostics);
    return result;
  }

  const ProjectWorkspace &workspace = *resolvedWorkspace.workspace;
  const ProjectManifest &manifest = workspace.selectedPackage().manifest;
  const ProjectTarget *selectedTarget = nullptr;
  if (request.targetName()) {
    selectedTarget = manifest.findTarget(*request.targetName());
    if (selectedTarget == nullptr) {
      Diagnostic diagnostic =
          projectDiagnostic("GTI-B1200", manifestSpan(manifest),
                            "Unknown target '" + *request.targetName() + "'.");
      if (const std::optional<std::string_view> nearest =
              nearestName(*request.targetName(), manifest.targets(),
                          [](const ProjectTarget &target) -> std::string_view {
                            return target.name;
                          })) {
        diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) +
                                   "'?");
      } else if (manifest.findProfile(*request.targetName()) != nullptr) {
        diagnostic.hints.push_back(
            "'" + *request.targetName() +
            "' is a profile; select it with --profile " +
            *request.targetName() +
            (*request.targetName() == "release" ? " or --release." : "."));
      }
      result.diagnostics.push_back(std::move(diagnostic));
    }
  } else if (manifest.targets().size() == 1) {
    selectedTarget = &manifest.targets().front();
  } else if (manifest.targets().empty()) {
    Diagnostic diagnostic = projectDiagnostic(
        "GTI-B1201", manifestSpan(manifest),
        "Package '" + manifest.package().name +
            "' is source-only and declares no build targets.");
    diagnostic.hints.push_back(
        "Select a workspace package with a target using --package, or declare "
        "a [targets.<name>] table.");
    result.diagnostics.push_back(std::move(diagnostic));
  } else {
    for (const ProjectTarget &target : manifest.targets()) {
      if (target.kind != ProjectTargetKind::Executable) {
        continue;
      }
      if (selectedTarget != nullptr) {
        selectedTarget = nullptr;
        break;
      }
      selectedTarget = &target;
    }
    if (selectedTarget == nullptr) {
      Diagnostic diagnostic = projectDiagnostic(
          "GTI-B1201", manifestSpan(manifest),
          "The manifest declares multiple targets; select one explicitly.");
      std::string names;
      for (const ProjectTarget &target : manifest.targets()) {
        if (!names.empty()) {
          names += ", ";
        }
        names += target.name;
      }
      diagnostic.hints.push_back("Available targets: " + names + ".");
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }

  const ProjectProfile *selectedProfile =
      selectProfile(manifest, request.profileName(), result.diagnostics);

  if (!result.diagnostics.empty() || selectedTarget == nullptr ||
      selectedProfile == nullptr) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  const std::optional<ResolvedProjectToolchain> toolchain =
      resolveBuildToolchain(workspace, manifest, request.target(),
                            request.toolchainPolicy(), result.diagnostics);
  if (!toolchain) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  std::optional<NativeInputs> nativeInputs =
      resolveNativeInputs(manifest, *selectedTarget, *selectedProfile,
                          request.target(), result.diagnostics);
  if (nativeInputs) {
    std::optional<std::vector<NativeDependencyGroup>> composed =
        composeDependencyNativeGroups(workspace, workspace.selectedPackage(),
                                      request.target(), result.diagnostics);
    if (composed) {
      nativeInputs->dependencyGroups = std::move(*composed);
    } else {
      nativeInputs.reset();
    }
  }
  if (!nativeInputs ||
      !checkToolchainCCoverage(
          *toolchain, *nativeInputs, request.toolchainPolicy(),
          request.toolchainOverrides().cCompiler, result.diagnostics)) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }
  std::optional<std::vector<BuildScriptPlan>> buildScripts =
      composeBuildScriptPlans(workspace, workspace.selectedPackage(),
                              request.target(), selectedProfile->name,
                              result.diagnostics);
  if (!buildScripts) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }

  result.status = ProjectResolutionStatus::Success;
  result.plan = makeBuildPlan(
      workspace, manifest, *selectedTarget, *selectedProfile, request.target(),
      std::move(*nativeInputs), std::move(*buildScripts), request.overrides(),
      request.runtimeProfile(), *toolchain);
  return result;
}

ProjectTestResolutionResult
resolveProjectTests(const ProjectBuildRequest &request) {
  ProjectTestResolutionResult result;
  WorkspaceResolutionResult resolvedWorkspace =
      resolveProjectWorkspace(request.startDirectory(), request.packageName(),
                              request.dependencyPolicy());
  result.sources = std::move(resolvedWorkspace.sources);
  if (!resolvedWorkspace.succeeded()) {
    switch (resolvedWorkspace.status) {
    case WorkspaceResolutionStatus::DiscoveryFailure:
      result.status = ProjectResolutionStatus::DiscoveryFailure;
      break;
    case WorkspaceResolutionStatus::ManifestFailure:
      result.status = ProjectResolutionStatus::ManifestFailure;
      break;
    case WorkspaceResolutionStatus::GraphFailure:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    case WorkspaceResolutionStatus::SelectionFailure:
      result.status = ProjectResolutionStatus::SelectionFailure;
      break;
    case WorkspaceResolutionStatus::Success:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    }
    result.diagnostics = std::move(resolvedWorkspace.diagnostics);
    return result;
  }

  const ProjectWorkspace &workspace = *resolvedWorkspace.workspace;
  const ProjectManifest &manifest = workspace.selectedPackage().manifest;
  std::vector<const ProjectTarget *> selectedTargets;
  if (request.targetName()) {
    const ProjectTarget *selected = manifest.findTarget(*request.targetName());
    if (selected == nullptr) {
      Diagnostic diagnostic = projectDiagnostic(
          "GTI-B1200", manifestSpan(manifest),
          "Unknown test target '" + *request.targetName() + "'.");
      std::vector<const ProjectTarget *> testTargets;
      for (const ProjectTarget &target : manifest.targets()) {
        if (target.kind == ProjectTargetKind::Test) {
          testTargets.push_back(&target);
        }
      }
      if (const std::optional<std::string_view> nearest =
              nearestName(*request.targetName(), testTargets,
                          [](const ProjectTarget *target) -> std::string_view {
                            return target->name;
                          })) {
        diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) +
                                   "'?");
      }
      result.diagnostics.push_back(std::move(diagnostic));
    } else if (selected->kind != ProjectTargetKind::Test) {
      Diagnostic diagnostic = projectDiagnostic(
          "GTI-B1204", selected->declaration,
          "Target '" + selected->name + "' is not a test target.");
      diagnostic.hints.push_back(
          "Select a target whose manifest kind is 'test'.");
      result.diagnostics.push_back(std::move(diagnostic));
    } else {
      selectedTargets.push_back(selected);
    }
  } else {
    for (const ProjectTarget &target : manifest.targets()) {
      if (target.kind == ProjectTargetKind::Test) {
        selectedTargets.push_back(&target);
      }
    }
    if (selectedTargets.empty()) {
      Diagnostic diagnostic =
          projectDiagnostic("GTI-B1203", manifestSpan(manifest),
                            "The manifest declares no test targets.");
      diagnostic.hints.push_back(
          "Declare a target with kind = \"test\" before running gti test.");
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }

  const ProjectProfile *selectedProfile =
      selectProfile(manifest, request.profileName(), result.diagnostics);

  if (!result.diagnostics.empty() || selectedProfile == nullptr) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  const std::optional<ResolvedProjectToolchain> toolchain =
      resolveBuildToolchain(workspace, manifest, request.target(),
                            request.toolchainPolicy(), result.diagnostics);
  if (!toolchain) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  std::optional<std::vector<NativeDependencyGroup>> composed =
      composeDependencyNativeGroups(workspace, workspace.selectedPackage(),
                                    request.target(), result.diagnostics);
  if (!composed) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }
  std::optional<std::vector<BuildScriptPlan>> buildScripts =
      composeBuildScriptPlans(workspace, workspace.selectedPackage(),
                              request.target(), selectedProfile->name,
                              result.diagnostics);
  if (!buildScripts) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }

  result.plans.reserve(selectedTargets.size());
  for (const ProjectTarget *selectedTarget : selectedTargets) {
    std::optional<NativeInputs> nativeInputs =
        resolveNativeInputs(manifest, *selectedTarget, *selectedProfile,
                            request.target(), result.diagnostics);
    if (!nativeInputs) {
      result.status = ProjectResolutionStatus::ManifestFailure;
      result.plans.clear();
      return result;
    }
    nativeInputs->dependencyGroups = *composed;
    if (!checkToolchainCCoverage(
            *toolchain, *nativeInputs, request.toolchainPolicy(),
            request.toolchainOverrides().cCompiler, result.diagnostics)) {
      result.status = ProjectResolutionStatus::ManifestFailure;
      result.plans.clear();
      return result;
    }
    result.plans.push_back(makeBuildPlan(
        workspace, manifest, *selectedTarget, *selectedProfile,
        request.target(), std::move(*nativeInputs), *buildScripts,
        request.overrides(), request.runtimeProfile(), *toolchain));
  }

  result.status = ProjectResolutionStatus::Success;
  return result;
}

ProjectTargetSetResolutionResult
resolveAllProjectTargets(const ProjectBuildRequest &request) {
  ProjectTargetSetResolutionResult result;
  if (request.targetName()) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    result.diagnostics.push_back(projectDiagnostic(
        "GTI-B1205", {request.startDirectory().string(), 0, 1, 1},
        "Building every target does not accept a target selection."));
    return result;
  }

  WorkspaceResolutionResult resolvedWorkspace =
      resolveProjectWorkspace(request.startDirectory(), request.packageName());
  result.sources = std::move(resolvedWorkspace.sources);
  if (!resolvedWorkspace.succeeded()) {
    switch (resolvedWorkspace.status) {
    case WorkspaceResolutionStatus::DiscoveryFailure:
      result.status = ProjectResolutionStatus::DiscoveryFailure;
      break;
    case WorkspaceResolutionStatus::ManifestFailure:
      result.status = ProjectResolutionStatus::ManifestFailure;
      break;
    case WorkspaceResolutionStatus::GraphFailure:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    case WorkspaceResolutionStatus::SelectionFailure:
      result.status = ProjectResolutionStatus::SelectionFailure;
      break;
    case WorkspaceResolutionStatus::Success:
      result.status = ProjectResolutionStatus::GraphFailure;
      break;
    }
    result.diagnostics = std::move(resolvedWorkspace.diagnostics);
    return result;
  }

  const ProjectWorkspace &workspace = *resolvedWorkspace.workspace;
  const ProjectManifest &manifest = workspace.selectedPackage().manifest;
  if (manifest.targets().empty()) {
    Diagnostic diagnostic = projectDiagnostic(
        "GTI-B1201", manifestSpan(manifest),
        "Package '" + manifest.package().name +
            "' is source-only and declares no build targets.");
    diagnostic.hints.push_back(
        "Select a workspace package with a target using --package, or declare "
        "a [targets.<name>] table.");
    result.diagnostics.push_back(std::move(diagnostic));
  }

  const ProjectProfile *selectedProfile =
      selectProfile(manifest, request.profileName(), result.diagnostics);
  if (!result.diagnostics.empty() || selectedProfile == nullptr) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  const std::optional<ResolvedProjectToolchain> toolchain =
      resolveBuildToolchain(workspace, manifest, request.target(),
                            request.toolchainPolicy(), result.diagnostics);
  if (!toolchain) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  result.plans.reserve(manifest.targets().size());
  std::optional<std::vector<BuildScriptPlan>> buildScripts =
      composeBuildScriptPlans(workspace, workspace.selectedPackage(),
                              request.target(), selectedProfile->name,
                              result.diagnostics);
  if (!buildScripts) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }
  std::optional<std::vector<NativeDependencyGroup>> composed =
      composeDependencyNativeGroups(workspace, workspace.selectedPackage(),
                                    request.target(), result.diagnostics);
  if (!composed) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    return result;
  }
  for (const ProjectTarget &target : manifest.targets()) {
    std::optional<NativeInputs> nativeInputs =
        resolveNativeInputs(manifest, target, *selectedProfile,
                            request.target(), result.diagnostics);
    if (nativeInputs) {
      nativeInputs->dependencyGroups = *composed;
    }
    if (!nativeInputs ||
        !checkToolchainCCoverage(
            *toolchain, *nativeInputs, request.toolchainPolicy(),
            request.toolchainOverrides().cCompiler, result.diagnostics)) {
      result.status = ProjectResolutionStatus::ManifestFailure;
      result.plans.clear();
      return result;
    }
    result.plans.push_back(makeBuildPlan(
        workspace, manifest, target, *selectedProfile, request.target(),
        std::move(*nativeInputs), *buildScripts, request.overrides(),
        request.runtimeProfile(), *toolchain));
  }

  result.status = ProjectResolutionStatus::Success;
  return result;
}

ProjectMetadataResult resolveProjectMetadata(
    const std::filesystem::path &startDirectory, TargetInfo target,
    std::optional<std::string> packageName, RuntimeProfile runtimeProfile) {
  ProjectMetadataResult result;
  WorkspaceResolutionResult resolvedWorkspace = resolveProjectWorkspace(
      startDirectory, packageName, {.allowAcquisition = false});
  result.sources = std::move(resolvedWorkspace.sources);
  if (!resolvedWorkspace.succeeded()) {
    switch (resolvedWorkspace.status) {
    case WorkspaceResolutionStatus::DiscoveryFailure:
      result.status = ProjectMetadataStatus::DiscoveryFailure;
      break;
    case WorkspaceResolutionStatus::ManifestFailure:
      result.status = ProjectMetadataStatus::ManifestFailure;
      break;
    case WorkspaceResolutionStatus::GraphFailure:
      result.status = ProjectMetadataStatus::GraphFailure;
      break;
    case WorkspaceResolutionStatus::SelectionFailure:
      result.status = ProjectMetadataStatus::SelectionFailure;
      break;
    case WorkspaceResolutionStatus::Success:
      result.status = ProjectMetadataStatus::GraphFailure;
      break;
    }
    result.diagnostics = std::move(resolvedWorkspace.diagnostics);
    return result;
  }

  std::vector<ProjectBuildPlan> plans;
  ProjectWorkspace &workspace = *resolvedWorkspace.workspace;
  const ProjectManifest &manifest = workspace.selectedPackage().manifest;
  // Metadata is read-only and never compiles: it reports whatever toolchain
  // the manifest declares for the requested target and never requires one.
  const ResolvedProjectToolchain metadataToolchain =
      resolveBuildToolchain(workspace, manifest, target,
                            ToolchainSelectionPolicy::HostTools,
                            result.diagnostics)
          .value_or(ResolvedProjectToolchain{});
  std::optional<std::vector<NativeDependencyGroup>> composed =
      composeDependencyNativeGroups(workspace, workspace.selectedPackage(),
                                    target, result.diagnostics);
  if (!composed) {
    result.status = ProjectMetadataStatus::ManifestFailure;
    return result;
  }
  plans.reserve(manifest.targets().size() * manifest.profiles().size());
  for (const ProjectTarget &projectTarget : manifest.targets()) {
    for (const ProjectProfile &profile : manifest.profiles()) {
      std::optional<std::vector<BuildScriptPlan>> buildScripts =
          composeBuildScriptPlans(workspace, workspace.selectedPackage(),
                                  target, profile.name, result.diagnostics);
      if (!buildScripts) {
        result.status = ProjectMetadataStatus::ManifestFailure;
        return result;
      }
      std::optional<NativeInputs> nativeInputs = resolveNativeInputs(
          manifest, projectTarget, profile, target, result.diagnostics);
      if (!nativeInputs) {
        result.status = ProjectMetadataStatus::ManifestFailure;
        return result;
      }
      nativeInputs->dependencyGroups = *composed;
      plans.push_back(makeBuildPlan(workspace, manifest, projectTarget, profile,
                                    target, std::move(*nativeInputs),
                                    std::move(*buildScripts), {},
                                    runtimeProfile, metadataToolchain));
    }
  }

  const AnalysisConfiguration metadataConfiguration(
      std::move(target), ExecutionProfile::SingleThreaded, runtimeProfile);
  result.status = ProjectMetadataStatus::Success;
  result.metadata.emplace(std::move(workspace), metadataConfiguration.target(),
                          std::move(plans));
  return result;
}

ProjectCleanResult cleanProject(const std::filesystem::path &startDirectory) {
  ProjectCleanResult result;
  ManifestDiscoveryResult discovery = discoverProjectManifest(startDirectory);
  if (!discovery.succeeded()) {
    result.status = ProjectCleanStatus::DiscoveryFailure;
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }

  std::filesystem::path manifest = *discovery.path;
  std::filesystem::path packageRoot = manifest.parent_path();
  WorkspaceResolutionResult resolvedWorkspace = resolveProjectWorkspace(
      startDirectory, std::nullopt, {.allowAcquisition = false});
  if (resolvedWorkspace.succeeded()) {
    packageRoot = resolvedWorkspace.workspace->root();
    manifest = packageRoot / "gti.toml";
  }
  const std::filesystem::path buildParent = packageRoot / "build";
  result.buildRoot = buildParent / "gti";

  if (packageRoot.empty() || packageRoot == packageRoot.root_path() ||
      result.buildRoot.filename() != "gti" ||
      result.buildRoot.parent_path().filename() != "build" ||
      !pathIsWithin(packageRoot, result.buildRoot) ||
      result.buildRoot == packageRoot) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(
        cleanDiagnostic(manifest, "GTI-B1300",
                        "Refusing to clean an unsafe project build path."));
    return result;
  }

  std::error_code error;
  const std::filesystem::file_status buildStatus =
      std::filesystem::symlink_status(buildParent, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the project build directory: " + error.message() +
            "."));
    return result;
  }
  error.clear();
  const std::filesystem::file_status rootStatus =
      std::filesystem::symlink_status(result.buildRoot, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the GTI build directory: " + error.message() + "."));
    return result;
  }
  if (std::filesystem::is_symlink(buildStatus) ||
      std::filesystem::is_symlink(rootStatus)) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1300",
        "Refusing to clean through a symbolic-link build path."));
    return result;
  }

  error.clear();
  const bool exists = std::filesystem::exists(result.buildRoot, error);
  if (error) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the GTI build directory: " + error.message() + "."));
    return result;
  }
  if (!exists) {
    result.status = ProjectCleanStatus::Success;
    return result;
  }
  if (!std::filesystem::is_directory(result.buildRoot, error) || error) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1300",
        "Refusing to clean because the GTI build path is not a directory."));
    return result;
  }

  result.removedEntries = std::filesystem::remove_all(result.buildRoot, error);
  if (error) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to clean the GTI build directory: " + error.message() + "."));
    return result;
  }
  result.status = ProjectCleanStatus::Success;
  return result;
}

ProjectScaffoldResult scaffoldProject(const ProjectScaffoldRequest &request) {
  ProjectScaffoldResult result;
  if (request.destination().empty()) {
    result.status = ProjectScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        {}, "GTI-B1500", "A project destination path is required."));
    return result;
  }
  if (request.git() &&
      request.mode() != ProjectScaffoldMode::ExistingDirectory) {
    result.status = ProjectScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        request.destination(), "GTI-B1500",
        "Git initialization is supported only by existing-directory project "
        "scaffolding."));
    return result;
  }
  if (request.git() && request.git()->executable.empty()) {
    result.status = ProjectScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        request.destination(), "GTI-B1500",
        "Git initialization requires a non-empty Git executable."));
    return result;
  }
  if (request.git()) {
    // Git repository-local environment overrides take precedence over -C.
    // Refuse them before creating any scaffold output: otherwise `git init`
    // could mutate external metadata which local rollback cannot restore.
    for (const char *variable :
         {"GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
          "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
          "GIT_IMPLICIT_WORK_TREE"}) {
      if (std::getenv(variable) != nullptr) {
        result.status = ProjectScaffoldStatus::InvalidRequest;
        result.diagnostics.push_back(scaffoldDiagnostic(
            request.destination(), "GTI-B1500",
            "Cannot initialize a standalone Git repository while '" +
                std::string(variable) +
                "' is set; unset that environment variable and retry."));
        return result;
      }
    }
  }

  std::error_code error;
  result.packageRoot = std::filesystem::absolute(request.destination(), error)
                           .lexically_normal();
  if (error) {
    result.status = ProjectScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        request.destination(), "GTI-B1504",
        "Failed to resolve the project destination: " + error.message() + "."));
    return result;
  }
  if (result.packageRoot.filename().empty() &&
      result.packageRoot != result.packageRoot.root_path()) {
    result.packageRoot = result.packageRoot.parent_path();
  }
  if (result.packageRoot.empty() ||
      result.packageRoot == result.packageRoot.root_path()) {
    result.status = ProjectScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.packageRoot, "GTI-B1500",
        "Refusing to scaffold a package at a filesystem root."));
    return result;
  }

  result.packageName =
      request.packageName().value_or(result.packageRoot.filename().string());
  if (!isPortableProjectName(result.packageName)) {
    result.status = ProjectScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.packageRoot, "GTI-B1500",
        "Package names must match [A-Za-z][A-Za-z0-9_-]* and cannot use a "
        "reserved portable device name; use --name to select another "
        "portable package name."));
    return result;
  }

  const ScaffoldEntryKind rootKind =
      inspectScaffoldEntry(result.packageRoot, error);
  if (rootKind == ScaffoldEntryKind::Failure) {
    result.status = ProjectScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.packageRoot, "GTI-B1504",
        "Failed to inspect the project destination: " + error.message() + "."));
    return result;
  }
  if (request.mode() == ProjectScaffoldMode::NewPackage &&
      rootKind != ScaffoldEntryKind::Missing) {
    result.status = ProjectScaffoldStatus::Conflict;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.packageRoot, "GTI-B1501",
        "Cannot create a new package because the destination already "
        "exists."));
    return result;
  }
  if (request.mode() == ProjectScaffoldMode::ExistingDirectory &&
      rootKind != ScaffoldEntryKind::Directory) {
    result.status = ProjectScaffoldStatus::Conflict;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.packageRoot, "GTI-B1502",
        rootKind == ScaffoldEntryKind::Missing
            ? "Cannot initialize a package because the destination does not "
              "exist."
            : "Cannot initialize a package because the destination is not a "
              "directory."));
    return result;
  }

  const std::filesystem::path manifest = result.packageRoot / "gti.toml";
  const std::filesystem::path sourceDirectory = result.packageRoot / "src";
  const std::filesystem::path source = sourceDirectory / "main.gti";
  const std::filesystem::path gitMetadata = result.packageRoot / ".git";
  const std::filesystem::path gitIgnore = result.packageRoot / ".gitignore";
  const ScaffoldEntryKind manifestKind = inspectScaffoldEntry(manifest, error);
  if (manifestKind == ScaffoldEntryKind::Failure) {
    result.status = ProjectScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        manifest, "GTI-B1504",
        "Failed to inspect the project manifest path: " + error.message() +
            "."));
    return result;
  }
  if (manifestKind != ScaffoldEntryKind::Missing) {
    result.status = ProjectScaffoldStatus::Conflict;
    result.diagnostics.push_back(scaffoldDiagnostic(
        manifest, "GTI-B1503", "A project manifest already exists."));
    return result;
  }

  if (request.git()) {
    const ScaffoldEntryKind gitMetadataKind =
        inspectScaffoldEntry(gitMetadata, error);
    if (gitMetadataKind == ScaffoldEntryKind::Failure) {
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitMetadata, "GTI-B1504",
          "Failed to inspect the Git metadata path: " + error.message() + "."));
      return result;
    }
    if (gitMetadataKind != ScaffoldEntryKind::Missing) {
      result.status = ProjectScaffoldStatus::Conflict;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitMetadata, "GTI-B1503",
          "Cannot initialize Git because a '.git' entry already exists."));
      return result;
    }

    const ScaffoldEntryKind gitIgnoreKind =
        inspectScaffoldEntry(gitIgnore, error);
    if (gitIgnoreKind == ScaffoldEntryKind::Failure) {
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitIgnore, "GTI-B1504",
          "Failed to inspect the Git ignore path: " + error.message() + "."));
      return result;
    }
    if (gitIgnoreKind != ScaffoldEntryKind::Missing) {
      result.status = ProjectScaffoldStatus::Conflict;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitIgnore, "GTI-B1503",
          "Cannot create the canonical GTI '.gitignore' because that entry "
          "already exists."));
      return result;
    }
  }

  const ScaffoldEntryKind sourceDirectoryKind =
      inspectScaffoldEntry(sourceDirectory, error);
  if (sourceDirectoryKind == ScaffoldEntryKind::Failure) {
    result.status = ProjectScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        sourceDirectory, "GTI-B1504",
        "Failed to inspect the source directory: " + error.message() + "."));
    return result;
  }
  if (sourceDirectoryKind != ScaffoldEntryKind::Missing &&
      sourceDirectoryKind != ScaffoldEntryKind::Directory) {
    result.status = ProjectScaffoldStatus::Conflict;
    result.diagnostics.push_back(scaffoldDiagnostic(
        sourceDirectory, "GTI-B1503",
        "Cannot scaffold the package because 'src' is not a directory."));
    return result;
  }

  ScaffoldEntryKind sourceKind = ScaffoldEntryKind::Missing;
  if (sourceDirectoryKind == ScaffoldEntryKind::Directory) {
    sourceKind = inspectScaffoldEntry(source, error);
    if (sourceKind == ScaffoldEntryKind::Failure) {
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          source, "GTI-B1504",
          "Failed to inspect the entry source: " + error.message() + "."));
      return result;
    }
    if (sourceKind != ScaffoldEntryKind::Missing &&
        sourceKind != ScaffoldEntryKind::File) {
      result.status = ProjectScaffoldStatus::Conflict;
      result.diagnostics.push_back(scaffoldDiagnostic(
          source, "GTI-B1503",
          "Cannot scaffold the package because 'src/main.gti' is not a "
          "regular file."));
      return result;
    }
  }

  const bool createdRoot = request.mode() == ProjectScaffoldMode::NewPackage;
  const bool createSourceDirectory =
      sourceDirectoryKind == ScaffoldEntryKind::Missing;
  if (createdRoot) {
    std::filesystem::create_directories(result.packageRoot, error);
    if (error) {
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          result.packageRoot, "GTI-B1504",
          "Failed to create the package directory: " + error.message() + "."));
      return result;
    }
  }
  if (createSourceDirectory) {
    std::filesystem::create_directory(sourceDirectory, error);
    if (error) {
      if (createdRoot) {
        std::error_code rollbackError;
        std::filesystem::remove_all(result.packageRoot, rollbackError);
      }
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          sourceDirectory, "GTI-B1504",
          "Failed to create the source directory: " + error.message() + "."));
      return result;
    }
  }

  bool manifestWriteAttempted = false;
  bool gitIgnoreWriteAttempted = false;
  bool createdGitMetadata = false;
  const auto rollback = [&] {
    std::error_code rollbackError;
    if (createdGitMetadata) {
      std::filesystem::remove_all(gitMetadata, rollbackError);
    }
    if (gitIgnoreWriteAttempted) {
      rollbackError.clear();
      std::filesystem::remove(gitIgnore, rollbackError);
    }
    if (manifestWriteAttempted) {
      rollbackError.clear();
      std::filesystem::remove(manifest, rollbackError);
    }
    if (createdRoot) {
      rollbackError.clear();
      std::filesystem::remove_all(result.packageRoot, rollbackError);
      return;
    }
    if (result.createdSource) {
      rollbackError.clear();
      std::filesystem::remove(source, rollbackError);
    }
    if (createSourceDirectory) {
      rollbackError.clear();
      std::filesystem::remove(sourceDirectory, rollbackError);
    }
  };

  if (sourceKind == ScaffoldEntryKind::Missing) {
    result.createdSource = true;
    if (!writeScaffoldFile(source, scaffoldMainSource)) {
      rollback();
      result.createdSource = false;
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          source, "GTI-B1504", "Failed to write the package entry source."));
      return result;
    }
  }

  manifestWriteAttempted = true;
  if (!writeScaffoldFile(manifest, scaffoldManifest(result.packageName))) {
    rollback();
    result.createdSource = false;
    result.status = ProjectScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        manifest, "GTI-B1504", "Failed to write the project manifest."));
    return result;
  }

  if (request.git()) {
    gitIgnoreWriteAttempted = true;
    if (!writeScaffoldFile(gitIgnore, scaffoldGitIgnore)) {
      rollback();
      result.createdSource = false;
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitIgnore, "GTI-B1504",
          "Failed to write the canonical GTI '.gitignore'."));
      return result;
    }
    result.createdGitIgnore = true;

    // Reserve the exact metadata directory before starting Git. A missing
    // entry observed during preflight is not ownership: another process may
    // have created it since, and rollback must never delete that entry.
    createdGitMetadata = std::filesystem::create_directory(gitMetadata, error);
    if (!createdGitMetadata) {
      const std::error_code reservationError = error;
      rollback();
      result.createdSource = false;
      result.createdGitIgnore = false;
      result.status = reservationError
                          ? ProjectScaffoldStatus::FilesystemFailure
                          : ProjectScaffoldStatus::Conflict;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitMetadata, reservationError ? "GTI-B1504" : "GTI-B1503",
          reservationError ? "Failed to reserve the Git metadata directory: " +
                                 reservationError.message() + "."
                           : "Cannot initialize Git because a '.git' entry "
                             "already exists."));
      return result;
    }
    const ProcessResult git =
        invokeProcess({request.git()->executable, "-C",
                       result.packageRoot.string(), "init", "--quiet"},
                      {.outputMode = ProcessOutputMode::Capture,
                       .captureSuccessfulOutput = false,
                       .description = "Git initialization"});
    if (!git.succeeded()) {
      rollback();
      result.createdSource = false;
      result.createdGitIgnore = false;
      result.status = ProjectScaffoldStatus::ToolFailure;
      result.diagnostics.push_back(
          scaffoldDiagnostic(result.packageRoot, "GTI-B1504",
                             "Failed to initialize the Git repository: " +
                                 processFailureDetail(git) + "."));
      return result;
    }

    const ScaffoldEntryKind initializedGitKind =
        inspectScaffoldEntry(gitMetadata, error);
    if (initializedGitKind == ScaffoldEntryKind::Failure) {
      rollback();
      result.createdSource = false;
      result.createdGitIgnore = false;
      result.status = ProjectScaffoldStatus::FilesystemFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitMetadata, "GTI-B1504",
          "Failed to inspect the initialized Git repository: " +
              error.message() + "."));
      return result;
    }
    if (initializedGitKind != ScaffoldEntryKind::Directory) {
      rollback();
      result.createdSource = false;
      result.createdGitIgnore = false;
      result.status = ProjectScaffoldStatus::ToolFailure;
      result.diagnostics.push_back(scaffoldDiagnostic(
          gitMetadata, "GTI-B1504",
          "Git reported success without creating repository metadata."));
      return result;
    }
    result.initializedGit = true;
  }

  result.status = ProjectScaffoldStatus::Success;
  return result;
}

FormatConfigScaffoldResult
scaffoldFormatConfig(const std::filesystem::path &destinationDirectory) {
  FormatConfigScaffoldResult result;
  if (destinationDirectory.empty()) {
    result.diagnostics.push_back(scaffoldDiagnostic(
        {}, "GTI-B1500", "A format configuration destination is required."));
    return result;
  }

  std::error_code error;
  std::filesystem::path destination =
      std::filesystem::absolute(destinationDirectory, error).lexically_normal();
  if (error) {
    result.status = FormatConfigScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        destinationDirectory, "GTI-B1504",
        "Failed to resolve the format configuration destination: " +
            error.message() + "."));
    return result;
  }
  if (destination.filename().empty() &&
      destination != destination.root_path()) {
    destination = destination.parent_path();
  }
  if (destination.empty() || destination == destination.root_path()) {
    result.status = FormatConfigScaffoldStatus::InvalidRequest;
    result.diagnostics.push_back(scaffoldDiagnostic(
        destination, "GTI-B1500",
        "Refusing to initialize format configuration at a filesystem root."));
    return result;
  }

  const ScaffoldEntryKind destinationKind =
      inspectScaffoldEntry(destination, error);
  if (destinationKind == ScaffoldEntryKind::Failure) {
    result.status = FormatConfigScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        destination, "GTI-B1504",
        "Failed to inspect the format configuration destination: " +
            error.message() + "."));
    return result;
  }
  if (destinationKind != ScaffoldEntryKind::Directory) {
    result.status = FormatConfigScaffoldStatus::Conflict;
    result.diagnostics.push_back(scaffoldDiagnostic(
        destination, "GTI-B1502",
        destinationKind == ScaffoldEntryKind::Missing
            ? "Cannot initialize format configuration because the destination "
              "does not exist."
            : "Cannot initialize format configuration because the destination "
              "is not a directory."));
    return result;
  }

  result.configPath = destination / ".gti-format";
  const ScaffoldEntryKind configKind =
      inspectScaffoldEntry(result.configPath, error);
  if (configKind == ScaffoldEntryKind::Failure) {
    result.status = FormatConfigScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(scaffoldDiagnostic(
        result.configPath, "GTI-B1504",
        "Failed to inspect the format configuration path: " + error.message() +
            "."));
    return result;
  }
  if (configKind != ScaffoldEntryKind::Missing) {
    result.status = FormatConfigScaffoldStatus::Conflict;
    result.diagnostics.push_back(
        scaffoldDiagnostic(result.configPath, "GTI-B1503",
                           "A .gti-format configuration already exists."));
    return result;
  }

  if (!writeScaffoldFile(result.configPath, defaultFormatConfig())) {
    std::error_code rollbackError;
    std::filesystem::remove(result.configPath, rollbackError);
    result.status = FormatConfigScaffoldStatus::FilesystemFailure;
    result.diagnostics.push_back(
        scaffoldDiagnostic(result.configPath, "GTI-B1504",
                           "Failed to write the format configuration."));
    return result;
  }

  result.status = FormatConfigScaffoldStatus::Success;
  return result;
}

} // namespace lang::driver
