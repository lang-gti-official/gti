#include "gti/driver/manifest.h"
#include "gti/driver/registry.h"
#include "registry_cache.h"

#include <algorithm>
#include <functional>
#include <set>

namespace lang::driver {
std::map<RegistryPackageKey, RegistrySelection> resolveRegistryPackages(
    const std::vector<RegistryDependency> &roots, const DependencyLock &lock,
    RegistryProvider &registries, bool lockedOnly, bool offline,
    const std::optional<RegistryPackageKey> &preferLatest,
    const std::vector<std::string> &reservedCanonicalNames) {
  using Constraints =
      std::map<RegistryPackageKey, std::vector<PackageVersionRequirement>>;
  using Selected = std::map<RegistryPackageKey, RegistrySelection>;
  Constraints rootConstraints;
  for (const auto &root : roots) {
    auto range = PackageVersionRequirement::parse(root.requirement);
    if (!range || !validRegistryName(root.key.registry) ||
        !validRegistryName(root.key.package)) {
      throw RegistryError(
          "GTI-B1710",
          "Invalid registry package identity or version requirement.");
    }
    rootConstraints[root.key].push_back(*range);
  }
  std::map<std::pair<RegistryPackageKey, std::string>,
           std::optional<RegistrySelection>>
      metadata;
  std::map<RegistryPackageKey, std::vector<std::string>> available;
  std::optional<RegistryPackageKey> missing;
  std::size_t metadataBytes = 0;
  const auto getMetadata =
      [&](const RegistryPackageKey &key,
          const std::string &version) -> std::optional<RegistrySelection> {
    const auto cacheKey = std::make_pair(key, version);
    if (auto it = metadata.find(cacheKey); it != metadata.end()) {
      return it->second;
    }
    if (metadata.size() >= 1024) {
      throw RegistryError(
          "GTI-B1721", "Package resolution exceeds its metadata-query budget.");
    }
    const auto *locked = lock.findRegistry(key.registry, key.package);
    auto text = detail::readCachedRegistryMetadata(key, version);
    const bool cached = text.has_value();
    if (!text) {
      if (offline) {
        throw RegistryError("GTI-B1703",
                            "Registry metadata is not cached; run `gti fetch` "
                            "before offline builds or editor analysis.");
      }
      text = registries.get(key.registry).metadata(key.package, version);
    }
    if (!text) {
      if (locked && locked->version == version) {
        throw RegistryError("GTI-B1714", "Locked package metadata is missing "
                                         "or its publication is incomplete.");
      }
      metadata.emplace(cacheKey, std::nullopt);
      return std::nullopt;
    }
    if (locked && locked->version == version &&
        packageChecksum(*text) != locked->metadataChecksum) {
      throw RegistryError("GTI-B1717",
                          "Locked package metadata changed under the same "
                          "version; refusing registry mutation.");
    }
    RegistrySelection selection{parseRegistryMetadata(*text, key, version),
                                *text};
    metadataBytes += text->size();
    if (metadataBytes > 32U * 1024U * 1024U) {
      throw RegistryError(
          "GTI-B1721",
          "Package resolution exceeds its metadata memory budget.");
    }
    if (locked && locked->version == version &&
        selection.metadata.checksum != locked->checksum) {
      throw RegistryError(
          "GTI-B1717",
          "Locked package archive checksum changed under the same version.");
    }
    if (!cached) {
      detail::storeCachedRegistryMetadata(key, version, *text);
    }
    metadata.emplace(cacheKey, selection);
    return selection;
  };
  const auto getVersions =
      [&](const RegistryPackageKey &key) -> const std::vector<std::string> & {
    if (auto it = available.find(key); it != available.end()) {
      return it->second;
    }
    auto versions = registries.get(key.registry).listVersions(key.package);
    // A missing dependency on one candidate is a backtracking failure, not
    // permission to abort before trying an older compatible parent version.
    if (versions.empty()) {
      missing = key;
    }
    if (versions.size() > 10000) {
      throw RegistryError(
          "GTI-B1721", "Registry version count exceeds the bootstrap limit.");
    }
    std::erase_if(versions, [](const auto &version) {
      return !parsePackageVersion(version);
    });
    std::sort(versions.begin(), versions.end(),
              [](const auto &a, const auto &b) {
                const auto comparison =
                    *parsePackageVersion(a) <=> *parsePackageVersion(b);
                return comparison == 0 ? a > b : comparison > 0;
              });
    versions.erase(std::unique(versions.begin(), versions.end()),
                   versions.end());
    return available.emplace(key, std::move(versions)).first->second;
  };
  std::size_t attempts = 0;
  std::function<std::optional<Selected>(Constraints, Selected, bool)> solve;
  solve = [&](Constraints constraints, Selected selected,
              bool onlyLock) -> std::optional<Selected> {
    if (++attempts > 8192 || constraints.size() > 256) {
      throw RegistryError(
          "GTI-B1721",
          "Package resolution exceeds the bounded bootstrap resource limit.");
    }
    for (const auto &[key, selection] : selected) {
      const auto version = *parsePackageVersion(selection.metadata.version);
      for (const auto &range : constraints[key]) {
        if (!range.accepts(version)) {
          return std::nullopt;
        }
      }
    }
    auto next = std::find_if(
        constraints.begin(), constraints.end(),
        [&](const auto &item) { return !selected.contains(item.first); });
    if (preferLatest && !onlyLock && !selected.contains(*preferLatest)) {
      if (auto target = constraints.find(*preferLatest);
          target != constraints.end()) {
        next = target;
      }
    }
    if (next == constraints.end()) {
      std::set<std::string> names;
      for (const auto &name : reservedCanonicalNames) {
        names.insert(portableProjectNameKey(name));
      }
      for (const auto &[key, package] : selected) {
        if (!names.insert(portableProjectNameKey(package.metadata.name))
                 .second) {
          return std::nullopt;
        }
      }
      // Package cycles are an ordinary graph incompatibility. Reject them at
      // the metadata boundary so older acyclic candidates can be considered
      // before any source archive is acquired. The workspace still verifies
      // the final materialized graph independently.
      std::map<RegistryPackageKey, int> colors;
      std::function<bool(const RegistryPackageKey &)> visit =
          [&](const auto &key) {
            if (colors[key] == 1) {
              return false;
            }
            if (colors[key] == 2) {
              return true;
            }
            colors[key] = 1;
            for (const auto &dependency :
                 selected.at(key).metadata.dependencies) {
              if (!visit(dependency.key)) {
                return false;
              }
            }
            colors[key] = 2;
            return true;
          };
      for (const auto &[key, package] : selected) {
        if (!visit(key)) {
          return std::nullopt;
        }
      }
      return selected;
    }
    const auto key = next->first;
    const auto tryVersion =
        [&](const std::string &text) -> std::optional<Selected> {
      auto version = parsePackageVersion(text);
      if (!version) {
        return std::nullopt;
      }
      for (const auto &range : next->second) {
        if (!range.accepts(*version)) {
          return std::nullopt;
        }
      }
      auto candidate = getMetadata(key, text);
      if (!candidate) {
        return std::nullopt;
      }
      auto expanded = constraints;
      for (const auto &dep : candidate->metadata.dependencies) {
        expanded[dep.key].push_back(
            *PackageVersionRequirement::parse(dep.requirement));
      }
      auto assigned = selected;
      assigned.emplace(key, *candidate);
      return solve(std::move(expanded), std::move(assigned), onlyLock);
    };
    const auto *locked = lock.findRegistry(key.registry, key.package);
    const bool latest = preferLatest && key == *preferLatest && !onlyLock;
    if (locked && !latest) {
      if (auto solution = tryVersion(locked->version)) {
        return solution;
      }
    }
    if (onlyLock) {
      return std::nullopt;
    }
    for (const auto &version : getVersions(key)) {
      if (locked && !latest && locked->version == version) {
        continue;
      }
      if (auto solution = tryVersion(version)) {
        return solution;
      }
    }
    return std::nullopt;
  };
  // Fetch prefers a complete existing solution without version discovery.
  // Add instead chooses its requested direct package first, newest-first.
  const bool latestResolution = preferLatest && !lockedOnly && !offline;
  auto solution = solve(rootConstraints, {}, !latestResolution);
  if (!solution && (lockedOnly || offline)) {
    throw RegistryError(
        "GTI-B1701", "gti.lock has no compatible registry dependency closure; "
                     "run `gti fetch` online to resolve the current manifest.");
  }
  if (!solution && !latestResolution) {
    solution = solve(rootConstraints, {}, false);
  }
  if (!solution) {
    if (missing && rootConstraints.contains(*missing)) {
      throw RegistryError("GTI-B1711", "Package '" + missing->identity() +
                                           "' was not found in the registry.");
    }
    throw RegistryError(
        "GTI-B1716",
        "No compatible package versions satisfy the complete dependency graph; "
        "conflicting requirements cannot share one package version.");
  }
  // Even a forged/incomplete edge list cannot describe another closure while
  // retaining valid per-package metadata and archives.
  if (lockedOnly) {
    for (const auto &[key, selection] : *solution) {
      const auto *entry = lock.findRegistry(key.registry, key.package);
      std::vector<std::string> edges;
      for (const auto &dep : selection.metadata.dependencies) {
        edges.push_back(dep.key.identity() + "@" +
                        solution->at(dep.key).metadata.version);
      }
      std::sort(edges.begin(), edges.end());
      if (!entry || edges != entry->dependencies) {
        throw RegistryError(
            "GTI-B1701", "gti.lock registry dependency edges do not describe "
                         "the selected exact graph; run `gti fetch`.");
      }
    }
  }
  return *solution;
}
} // namespace lang::driver
