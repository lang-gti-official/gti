#include "gti/driver/artifact.h"
#include "gti/driver/manifest.h"
#include "gti/driver/process.h"
#include "gti/driver/registry.h"
#include "gti/driver/workspace.h"
#include "manifest_source.h"
#include "registry_cache.h"
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace lang::driver {
namespace {
std::string read(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(
          std::filesystem::symlink_status(path, error)) ||
      error || std::filesystem::file_size(path, error) > 64U * 1024U * 1024U ||
      error) {
    throw RegistryError("GTI-B1722",
                        "Cannot read a regular package source file safely.");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw RegistryError("GTI-B1722", "Cannot open package source file.");
  }
  std::string text{std::istreambuf_iterator<char>(input), {}};
  if (input.bad()) {
    throw RegistryError("GTI-B1722",
                        "Cannot finish reading package source file.");
  }
  return text;
}
void write(const std::filesystem::path &path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  if (!output) {
    throw RegistryError("GTI-B1722",
                        "Cannot stage a package manifest or lockfile.");
  }
}
void fail(RegistryCommandResult &result, const std::filesystem::path &path,
          const RegistryError &error) {
  auto diagnostic = makeDiagnostic(error.code, DiagnosticPhase::Driver,
                                   {path.string(), 0, 1, 1}, error.what());
  if (error.code == "GTI-B1713") {
    diagnostic.hints.push_back(
        "Set the configured token-env variable to a token with the required "
        "package permissions (GTI_REGISTRY_TOKEN for the default official "
        "registry); anonymous reads depend on server policy.");
  }
  result.diagnostics.push_back(std::move(diagnostic));
}
std::size_t offset(std::string_view text, toml::source_position position) {
  return detail::manifestSourceOffset(text, position.line, position.column);
}
std::string addText(std::string text, std::string_view alias,
                    std::string_view package, std::string_view requirement,
                    std::string_view registry) {
  auto parsed = toml::parse(text);
  if (!parsed) {
    throw RegistryError("GTI-B1000", "Cannot edit an invalid manifest.");
  }
  std::string value;
  if (registry == "official" && alias == package) {
    value = "\"" + std::string(requirement) + "\"";
  } else {
    toml::table node{{"version", std::string(requirement)},
                     {"registry", std::string(registry)},
                     {"package", std::string(package)}};
    node.is_inline(true);
    std::ostringstream output;
    output << node;
    value = output.str();
  }
  const auto entry = std::string(alias) + " = " + value;
  const auto *table = parsed.table()["dependencies"].as_table();
  if (table && table->is_inline()) {
    const auto end = offset(text, table->source().end);
    if (end == 0 || text[end - 1] != '}') {
      throw RegistryError("GTI-B1723",
                          "Cannot safely edit the inline dependencies table; "
                          "add the dependency explicitly.");
    }
    text.insert(end - 1, (table->empty() ? " " : ", ") + entry + " ");
    return text;
  }
  for (std::size_t start = 0; start < text.size();) {
    const auto newline = text.find('\n', start);
    const auto finish = newline == text.npos ? text.size() : newline;
    const auto line = std::string_view(text).substr(start, finish - start);
    auto header = toml::parse(line);
    if (header && header.table().size() == 1) {
      if (const auto *deps = header.table()["dependencies"].as_table();
          deps && deps->empty() && !deps->is_inline()) {
        text.insert(finish, "\n" + entry);
        return text;
      }
    }
    if (newline == text.npos) {
      break;
    }
    start = newline + 1;
  }
  // Dotted declarations keep implicit parent tables valid without redefining
  // an existing table and leave every unrelated byte/comment untouched.
  return "dependencies." + entry + "\n" + text;
}
std::vector<PackageArchiveEntry>
trackedFiles(const std::filesystem::path &root) {
  const auto process = invokeProcess(
      {discoverGitExecutable(), "-c", "core.fsmonitor=false", "-C",
       root.string(), "ls-files", "--stage", "-z", "--", "."},
      {.captureSuccessfulOutput = true,
       .description = "package file inventory"});
  if (!process.succeeded()) {
    throw RegistryError("GTI-B1718",
                        "Publishing requires a Git-tracked package inventory; "
                        "add intended package files to Git before publishing.");
  }
  std::vector<PackageArchiveEntry> entries;
  std::size_t start = 0;
  std::size_t total = 0;
  while (start < process.output.size()) {
    const auto end = process.output.find('\0', start);
    if (end == process.output.npos) {
      throw RegistryError("GTI-B1718", "Malformed package file inventory.");
    }
    const auto record = process.output.substr(start, end - start);
    start = end + 1;
    const auto tab = record.find('\t');
    const bool executable = record.starts_with("100755 ");
    if (tab == record.npos || (!executable && !record.starts_with("100644 ")) ||
        tab < 2 || record.substr(tab - 2, 2) != " 0") {
      throw RegistryError("GTI-B1718",
                          "Package inventory contains a submodule, merge "
                          "conflict or unsupported file mode.");
    }
    const auto name = record.substr(tab + 1);
    const auto path = root / name;
    // Reject symlink parents as well as symlink leaf files.
    auto parent = path;
    while (parent != root && parent != parent.parent_path()) {
      std::error_code error;
      if (std::filesystem::is_symlink(
              std::filesystem::symlink_status(parent, error)) ||
          error) {
        throw RegistryError("GTI-B1718",
                            "Package inventory contains a symbolic link or "
                            "inaccessible entry.");
      }
      parent = parent.parent_path();
    }
    auto contents = read(path);
    total += contents.size() + name.size() + 13;
    if (entries.size() >= 10000 || total > 256U * 1024U * 1024U) {
      throw RegistryError(
          "GTI-B1718",
          "Package inventory exceeds the bootstrap archive limits.");
    }
    entries.push_back({name, std::move(contents), executable});
  }
  return entries;
}
} // namespace

RegistryCommandResult addRegistryDependency(const std::filesystem::path &start,
                                            std::string_view package,
                                            std::string_view requirement,
                                            std::string_view registry) {
  RegistryCommandResult result;
  auto discovery = discoverProjectManifest(start);
  if (!discovery.succeeded()) {
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }
  const auto path = *discovery.path;
  try {
    if (!validRegistryName(package) || !validRegistryName(registry) ||
        (!requirement.empty() &&
         !PackageVersionRequirement::parse(requirement))) {
      throw RegistryError("GTI-B1710",
                          "Use a portable lowercase registry package name and "
                          "a valid SemVer requirement.");
    }
    auto loaded = loadProjectManifest(path);
    result.sources = loaded.sources;
    if (!loaded.succeeded()) {
      result.diagnostics = std::move(loaded.diagnostics);
      return result;
    }
    std::string alias(package);
    std::replace(alias.begin(), alias.end(), '-', '_');
    if (alias == "std") {
      throw RegistryError("GTI-B1710",
                          "The dependency alias 'std' is reserved.");
    }
    for (const auto &existing : loaded.manifest->dependencies()) {
      if (existing.alias != alias) {
        continue;
      }
      if (existing.registry && existing.registry->package == package &&
          existing.registry->registry == registry &&
          (requirement.empty() ||
           existing.registry->requirement == requirement)) {
        result.summary =
            "Dependency '" + alias +
            "' is already declared; use `gti fetch` to materialize it.";
        return result;
      }
      throw RegistryError(
          "GTI-B1723",
          "Dependency alias already exists with another source or requirement; "
          "edit it explicitly rather than silently broadening it.");
    }
    const auto original = read(path);
    const auto proposed =
        addText(original, alias, package,
                requirement.empty() ? "*" : requirement, registry);
    auto workspace = resolveProjectWorkspace(
        start, std::nullopt,
        {.requireLock = false,
         .acquireRegistry = true,
         .materializeRegistry = true,
         .preferLatestRegistryPackage =
             RegistryPackageKey{std::string(registry), std::string(package)}},
        WorkspaceManifestOverlay{path, proposed});
    if (!workspace.succeeded()) {
      result.sources = std::move(workspace.sources);
      result.diagnostics = std::move(workspace.diagnostics);
      return result;
    }
    const auto lock = lockFromWorkspace(*workspace.workspace);
    const auto *chosen = lock.findRegistry(registry, package);
    if (!chosen) {
      throw RegistryError("GTI-B1716",
                          "No coherent package selection was produced.");
    }
    const auto finalRange =
        requirement.empty() ? "^" + chosen->version : std::string(requirement);
    const auto final = addText(original, alias, package, finalRange, registry);
    const auto validation = loadProjectManifest(path, final);
    if (!validation.succeeded()) {
      result.sources = validation.sources;
      result.diagnostics = validation.diagnostics;
      return result;
    }
    if (read(path) != original) {
      throw RegistryError("GTI-B1723",
                          "gti.toml changed during resolution; no manifest "
                          "edit was applied. Retry after other edits finish.");
    }
    const auto destination = dependencyLockPath(workspace.workspace->root());
    const auto manifestStage = stagedArtifactPath(path),
               lockStage = stagedArtifactPath(destination);
    TemporaryArtifact manifestCleanup(manifestStage, true),
        lockCleanup(lockStage, true);
    write(manifestStage, final);
    write(lockStage, renderDependencyLock(lock));
    // Stage both before publication. A crash between these two atomic renames
    // leaves a detectable stale lock, never an unchecked or half-written file.
    if (!publishArtifact(manifestStage, path).succeeded()) {
      throw RegistryError(
          "GTI-B1722",
          "Cannot publish gti.toml; the manifest was left unchanged.");
    }
    if (!publishArtifact(lockStage, destination).succeeded()) {
      const auto rollback = stagedArtifactPath(path);
      TemporaryArtifact cleanup(rollback, true);
      write(rollback, original);
      if (!publishArtifact(rollback, path).succeeded()) {
        throw RegistryError(
            "GTI-B1722", "Lock publication and manifest rollback failed; run "
                         "`gti fetch` to reconcile the declared dependency.");
      }
      throw RegistryError(
          "GTI-B1722",
          "Cannot publish gti.lock; the original manifest was restored.");
    }
    result.summary = "Added " + alias + " " + finalRange + " from " +
                     std::string(registry) + "; updated gti.toml and gti.lock.";
  } catch (const RegistryError &error) {
    fail(result, path, error);
  } catch (const std::filesystem::filesystem_error &) {
    fail(result, path,
         RegistryError("GTI-B1722",
                       "Cannot safely access project package files."));
  }
  return result;
}

RegistryCommandResult publishRegistryPackage(const std::filesystem::path &start,
                                             std::string_view registry,
                                             std::string_view publicName,
                                             bool dryRun) {
  RegistryCommandResult result;
  auto discovery = discoverProjectManifest(start);
  if (!discovery.succeeded()) {
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }
  const auto path = *discovery.path;
  try {
    auto loaded = loadProjectManifest(path);
    result.sources = loaded.sources;
    if (!loaded.succeeded()) {
      result.diagnostics = std::move(loaded.diagnostics);
      return result;
    }
    const auto &manifest = *loaded.manifest;
    const auto name =
        publicName.empty() ? manifest.package().name : std::string(publicName);
    if (!validRegistryName(name) || !validRegistryName(registry) ||
        !parsePackageVersion(manifest.package().version) ||
        manifest.workspace()) {
      throw RegistryError(
          "GTI-B1710",
          "Publish one package with a portable lowercase registry name and a "
          "complete SemVer version (not a workspace root).");
    }
    RegistryPackageMetadata metadata{.key = {std::string(registry), name},
                                     .name = manifest.package().name,
                                     .version = manifest.package().version};
    for (const auto &dep : manifest.dependencies()) {
      if (!dep.registry) {
        throw RegistryError(
            "GTI-B1710",
            "Published packages require registry-only dependencies; replace "
            "path/Git sources with registry requirements first.");
      }
      metadata.dependencies.push_back(
          {dep.alias,
           {dep.registry->registry, dep.registry->package},
           dep.registry->requirement});
    }
    const auto entries = trackedFiles(manifest.packageRoot());
    const auto archive = encodePackageArchive(entries);
    detail::validateRegistryArchive(archive, metadata);
    metadata.checksum = packageChecksum(archive);
    const auto text = renderRegistryMetadata(metadata);
    (void)parseRegistryMetadata(text, metadata.key, metadata.version);
    if (dryRun) {
      result.summary = "Validated " + name + "@" + metadata.version + " (" +
                       std::to_string(entries.size()) + " tracked files, " +
                       std::to_string(archive.size()) + " archive bytes, " +
                       metadata.checksum + "); nothing uploaded.";
    } else {
      auto registries = configuredRegistries();
      registries->get(registry).publish(name, metadata.version, archive, text);
      result.summary = "Published " + name + "@" + metadata.version + " to " +
                       std::string(registry) + " (" + metadata.checksum + ").";
    }
  } catch (const RegistryError &error) {
    fail(result, path, error);
  } catch (const std::filesystem::filesystem_error &) {
    fail(result, path,
         RegistryError("GTI-B1722",
                       "Cannot safely read the package inventory."));
  }
  return result;
}
} // namespace lang::driver
