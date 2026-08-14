#include "gti/source_graph.h"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>

namespace lang {

SourceUnitId SourceGraph::entryUnit() const { return entry; }

const std::vector<SourceUnit> &SourceGraph::sourceUnits() const {
  return units;
}

const std::vector<SourceDependency> &SourceGraph::dependencyEdges() const {
  return dependencies;
}

const SourceUnit *SourceGraph::findUnit(SourceUnitId id) const {
  return id == 0 || id > units.size() ? nullptr : &units[id - 1];
}

SourceUnit *SourceGraph::findUnit(SourceUnitId id) {
  return id == 0 || id > units.size() ? nullptr : &units[id - 1];
}

SourceUnitId SourceGraph::sourceUnitForPath(std::string_view path) const {
  const auto found = unitsByPath.find(std::string(path));
  return found == unitsByPath.end() ? 0 : found->second;
}

bool SourceGraph::isCompilerTrusted(SourceUnitId id) const {
  const SourceUnit *unit = findUnit(id);
  return unit != nullptr && unit->role != SourceUnitRole::Application;
}

bool SourceGraph::isVisible(SourceUnitId requester,
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

std::vector<SourceUnitId> SourceGraph::compilationOrder() const {
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

void SourceGraph::clear() {
  units.clear();
  dependencies.clear();
  unitsByPath.clear();
  entry = 0;
}

SourceUnitId
SourceGraph::addUnit(std::filesystem::path path, bool isEntry, bool isPrelude,
                     std::optional<std::string> standardLibraryName,
                     SourceUnitRole role,
                     std::optional<std::string> packageIdentity,
                     std::optional<std::string> packageRelativePath) {
  const SourceUnitId id = units.size() + 1;
  const std::string key = path.string();
  units.push_back(
      SourceUnit{.id = id,
                 .path = std::move(path),
                 .standardLibraryName = std::move(standardLibraryName),
                 .packageIdentity = std::move(packageIdentity),
                 .packageRelativePath = std::move(packageRelativePath),
                 .role = role,
                 .entry = isEntry,
                 .prelude = isPrelude});
  unitsByPath.emplace(key, id);
  if (isEntry) {
    entry = id;
  }
  return id;
}

void SourceGraph::addDependency(SourceDependency dependency) {
  dependencies.push_back(std::move(dependency));
}

bool SourceGraph::hasDirectDependency(SourceUnitId source,
                                      SourceUnitId target) const {
  return std::any_of(dependencies.begin(), dependencies.end(),
                     [source, target](const SourceDependency &dependency) {
                       return dependency.source == source &&
                              dependency.target == target;
                     });
}

bool SourceGraph::hasDependencyPath(SourceUnitId source,
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

} // namespace lang
