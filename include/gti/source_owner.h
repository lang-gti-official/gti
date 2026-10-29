#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

// Ownership controls source containment and public module resolution. It does
// not confer compiler trust; trusted declaration provenance is a separate fact.
enum class SourceOwnerKind { Package, Toolchain, Generated, Direct };

struct PackageSourceDependency {
  std::string alias;
  std::string targetIdentity;
};

struct SourceOwnerDescriptor {
  std::string identity;
  std::string name;
  std::filesystem::path packageRoot;
  std::filesystem::path sourceRoot;
  std::vector<PackageSourceDependency> dependencies;
  // Public owner-local module name -> canonical interface path. Names may
  // contain dots; path layout does not establish public visibility.
  std::vector<std::pair<std::string, std::filesystem::path>> modules;
  std::vector<std::pair<std::string, std::vector<std::filesystem::path>>>
      moduleImplementations;
  SourceOwnerKind kind = SourceOwnerKind::Package;
  // A configured toolchain may expose an owner to every source compilation.
  // This is resolution metadata, never authority to access private
  // capabilities.
  std::optional<std::string> implicitAlias;
  std::vector<std::filesystem::path> additionalRoots;
  std::string defaultModule = "main";
  // Authored absolute lookup paths are notification metadata, never authority.
  // Canonical modules above remain the validated resolution identities.
  std::vector<std::pair<std::string, std::filesystem::path>> moduleLookupPaths;
};

// Preserve the public frontend embedding API while all callers use the same
// descriptor and registry, including toolchain and generated source owners.
using PackageSourceRoot = SourceOwnerDescriptor;

class SourceOwnerRegistry final {
public:
  explicit SourceOwnerRegistry(std::vector<SourceOwnerDescriptor> owners = {},
                               bool directMode = true)
      : descriptors(std::move(owners)), directMode(directMode) {}

  [[nodiscard]] const std::vector<SourceOwnerDescriptor> &owners() const {
    return descriptors;
  }
  [[nodiscard]] bool isDirectMode() const { return directMode; }
  // Logical resolution metadata joins whole-program cache keys without
  // embedding installation or checkout roots in relocatable source identity.
  [[nodiscard]] std::string resolutionIdentity() const {
    std::string result;
    const auto append = [&](std::string_view value) {
      result += std::to_string(value.size()) + ":";
      result += value;
    };
    std::vector<const SourceOwnerDescriptor *> ordered;
    for (const auto &owner : descriptors)
      ordered.push_back(&owner);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto *left, const auto *right) {
                return left->identity < right->identity;
              });
    for (const auto *owner : ordered) {
      append(owner->identity);
      append(std::to_string(static_cast<int>(owner->kind)));
      append(owner->implicitAlias.value_or(""));
      append(owner->defaultModule);
      const auto logicalPath = [&](const std::filesystem::path &path) {
        auto relative = path.lexically_relative(owner->packageRoot);
        if (!relative.empty() && *relative.begin() != "..")
          return relative.generic_string();
        for (std::size_t index = 0; index < owner->additionalRoots.size();
             ++index) {
          relative = path.lexically_relative(owner->additionalRoots[index]);
          if (!relative.empty() && *relative.begin() != "..")
            return "root" + std::to_string(index + 1) + ":" +
                   relative.generic_string();
        }
        return path.generic_string();
      };
      auto modules = owner->modules;
      std::sort(modules.begin(), modules.end());
      append(std::to_string(modules.size()));
      for (const auto &[name, path] : modules) {
        append(name);
        append(logicalPath(path));
      }
      append(std::to_string(owner->dependencies.size()));
      for (const auto &edge : owner->dependencies) {
        append(edge.alias);
        append(edge.targetIdentity);
      }
      append(std::to_string(owner->moduleImplementations.size()));
      for (const auto &[name, paths] : owner->moduleImplementations) {
        append(name);
        append(std::to_string(paths.size()));
        for (const auto &path : paths)
          append(logicalPath(path));
      }
    }
    return result;
  }

  [[nodiscard]] const SourceOwnerDescriptor *
  find(std::string_view identity) const {
    for (const auto &owner : descriptors)
      if (owner.identity == identity)
        return &owner;
    return nullptr;
  }
  [[nodiscard]] const SourceOwnerDescriptor *
  findImplicitAlias(std::string_view alias) const {
    for (const auto &owner : descriptors)
      if (owner.implicitAlias && *owner.implicitAlias == alias)
        return &owner;
    return nullptr;
  }
  [[nodiscard]] const SourceOwnerDescriptor *
  ownerForPath(const std::filesystem::path &path) const {
    const SourceOwnerDescriptor *selected = nullptr;
    std::size_t selectedDepth = 0;
    const auto consider = [&](const auto &owner, const auto &root) {
      if (root.empty())
        return;
      auto part = path.begin();
      std::size_t depth = 0;
      for (auto rootPart = root.begin(); rootPart != root.end();
           ++rootPart, ++part, ++depth)
        if (part == path.end() || *part != *rootPart)
          return;
      if (selected == nullptr || depth > selectedDepth) {
        selected = &owner;
        selectedDepth = depth;
      }
    };
    for (const auto &owner : descriptors) {
      consider(owner, owner.packageRoot);
      for (const auto &root : owner.additionalRoots)
        consider(owner, root);
    }
    if (selected == nullptr && directMode)
      return find("direct");
    return selected;
  }

private:
  std::vector<SourceOwnerDescriptor> descriptors;
  bool directMode;
};

} // namespace lang
