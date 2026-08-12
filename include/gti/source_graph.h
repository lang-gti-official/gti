#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

using SourceUnitId = std::size_t;

enum class SourceDependencyKind {
  Include,
  Prelude,
  StandardLibrary,
};

enum class SourceUnitRole {
  Application,
  Prelude,
  StandardLibrary,
};

struct SourceDependency {
  SourceUnitId source = 0;
  SourceUnitId target = 0;
  SourceDependencyKind kind = SourceDependencyKind::Include;
  std::optional<SourceSpan> directive;
};

struct SourceUnit {
  SourceUnitId id = 0;
  std::filesystem::path path;
  std::vector<Token> tokens;
  std::size_t declarationStart = 0;
  std::size_t declarationCount = 0;
  std::optional<std::string> standardLibraryName;
  SourceUnitRole role = SourceUnitRole::Application;
  bool entry = false;
  bool prelude = false;
};

class SourceGraph {
public:
  [[nodiscard]] SourceUnitId entryUnit() const { return entry; }

  [[nodiscard]] const std::vector<SourceUnit> &sourceUnits() const {
    return units;
  }

  [[nodiscard]] const std::vector<SourceDependency> &dependencyEdges() const {
    return dependencies;
  }

  [[nodiscard]] const SourceUnit *findUnit(SourceUnitId id) const {
    return id == 0 || id > units.size() ? nullptr : &units[id - 1];
  }

  [[nodiscard]] SourceUnitId sourceUnitForPath(std::string_view path) const {
    const auto found = unitsByPath.find(std::string(path));
    return found == unitsByPath.end() ? 0 : found->second;
  }

  [[nodiscard]] bool isCompilerTrusted(SourceUnitId id) const {
    const SourceUnit *unit = findUnit(id);
    return unit != nullptr && unit->role != SourceUnitRole::Application;
  }

  [[nodiscard]] bool isVisible(SourceUnitId requester,
                               SourceUnitId declaration) const {
    if (requester == 0 || declaration == 0 || requester == declaration) {
      return true;
    }
    const SourceUnit *declaringUnit = findUnit(declaration);
    if (declaringUnit != nullptr && declaringUnit->prelude) {
      return true;
    }
    return std::any_of(
        dependencies.begin(), dependencies.end(),
        [requester, declaration](const SourceDependency &dependency) {
          return dependency.source == requester &&
                 dependency.target == declaration;
        });
  }

  [[nodiscard]] std::vector<SourceUnitId> compilationOrder() const {
    std::vector<SourceUnitId> result;
    std::unordered_set<SourceUnitId> visiting;
    std::unordered_set<SourceUnitId> visited;
    const std::function<void(SourceUnitId)> visit = [&](SourceUnitId id) {
      if (id == 0 || visited.contains(id) || visiting.contains(id)) {
        return;
      }
      visiting.insert(id);
      for (const SourceDependency &dependency : dependencies) {
        if (dependency.source == id) {
          visit(dependency.target);
        }
      }
      visiting.erase(id);
      visited.insert(id);
      result.push_back(id);
    };

    for (const SourceUnit &unit : units) {
      if (unit.prelude) {
        visit(unit.id);
      }
    }
    visit(entry);
    for (const SourceUnit &unit : units) {
      visit(unit.id);
    }
    return result;
  }

private:
  friend class Frontend;
  friend class SourceLoader;

  [[nodiscard]] SourceUnit *findUnit(SourceUnitId id) {
    return id == 0 || id > units.size() ? nullptr : &units[id - 1];
  }

  void clear() {
    units.clear();
    dependencies.clear();
    unitsByPath.clear();
    entry = 0;
  }

  SourceUnitId
  addUnit(std::filesystem::path path, bool isEntry, bool isPrelude,
          std::optional<std::string> standardLibraryName = std::nullopt,
          SourceUnitRole role = SourceUnitRole::Application) {
    const SourceUnitId id = units.size() + 1;
    const std::string key = path.string();
    units.push_back(
        SourceUnit{.id = id,
                   .path = std::move(path),
                   .standardLibraryName = std::move(standardLibraryName),
                   .role = role,
                   .entry = isEntry,
                   .prelude = isPrelude});
    unitsByPath.emplace(key, id);
    if (isEntry) {
      entry = id;
    }
    return id;
  }

  void addDependency(SourceDependency dependency) {
    dependencies.push_back(std::move(dependency));
  }

  [[nodiscard]] bool hasDirectDependency(SourceUnitId source,
                                         SourceUnitId target) const {
    return std::any_of(dependencies.begin(), dependencies.end(),
                       [source, target](const SourceDependency &dependency) {
                         return dependency.source == source &&
                                dependency.target == target;
                       });
  }

  [[nodiscard]] bool hasDependencyPath(SourceUnitId source,
                                       SourceUnitId target) const {
    std::unordered_set<SourceUnitId> visited;
    const std::function<bool(SourceUnitId)> reaches = [&](SourceUnitId unit) {
      if (unit == target) {
        return true;
      }
      if (!visited.insert(unit).second) {
        return false;
      }
      for (const SourceDependency &dependency : dependencies) {
        if (dependency.source == unit && reaches(dependency.target)) {
          return true;
        }
      }
      return false;
    };
    return source != 0 && target != 0 && reaches(source);
  }

  std::vector<SourceUnit> units;
  std::vector<SourceDependency> dependencies;
  std::unordered_map<std::string, SourceUnitId> unitsByPath;
  SourceUnitId entry = 0;
};

} // namespace lang
