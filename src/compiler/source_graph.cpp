#include "gti/source_graph.h"
#include <stdexcept>

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>

namespace lang {
void SourceGraph::markCompiledScalarInterface(SourceUnitId id) {
  auto *unit = findUnit(id);
  if (!unit || unit->entry || unit->prelude || isCompilerTrusted(id))
    throw std::invalid_argument("invalid compiled scalar interface unit");
  unit->compiledScalarInterface = true;
}

std::uint64_t sourceTextDigest(std::string_view source) {
  std::uint64_t result = 14695981039346656037ULL;
  for (const unsigned char byte : source) {
    result ^= byte;
    result *= 1099511628211ULL;
  }
  return result;
}

SourceUnitId SourceGraph::entryUnit() const { return entry; }

const std::vector<SourceUnit> &SourceGraph::sourceUnits() const {
  return units;
}

const std::vector<SourceModule> &SourceGraph::sourceModules() const {
  return modules;
}

const std::vector<SourceImport> &SourceGraph::imports() const {
  return sourceImports;
}

const std::shared_ptr<const SourceOwnerRegistry> &
SourceGraph::sourceOwners() const {
  return owners;
}

void SourceGraph::setSourceOwners(
    std::shared_ptr<const SourceOwnerRegistry> value) {
  owners = std::move(value);
}

void SourceGraph::addImport(SourceImport import) {
  sourceImports.push_back(std::move(import));
}

const SourceModule *SourceGraph::findModule(ModuleId id) const {
  return id == 0 || id > modules.size() ? nullptr : &modules[id - 1];
}

ModuleId SourceGraph::moduleForUnit(SourceUnitId id) const {
  const auto *unit = findUnit(id);
  return unit == nullptr ? 0 : unit->moduleId;
}

bool SourceGraph::sameModule(SourceUnitId left, SourceUnitId right) const {
  const ModuleId owner = moduleForUnit(left);
  return owner != 0 && owner == moduleForUnit(right);
}

ModuleId SourceGraph::registerModule(SourceUnitId interfaceUnit,
                                     std::string ownerIdentity,
                                     std::string name) {
  auto *unit = findUnit(interfaceUnit);
  if (unit == nullptr)
    return 0;
  for (const auto &module : modules) {
    if (module.ownerIdentity == ownerIdentity && module.name == name) {
      // A second physical interface is invalid; the loader owns its diagnostic.
      if (module.interfaceUnit != interfaceUnit)
        return 0;
      unit->moduleId = module.id;
      return module.id;
    }
  }
  const ModuleId id = modules.size() + 1;
  modules.push_back(
      {id, std::move(ownerIdentity), std::move(name), interfaceUnit});
  unit->moduleId = id;
  return id;
}

bool SourceGraph::attachModuleImplementation(SourceUnitId interfaceUnit,
                                             SourceUnitId implementationUnit) {
  const ModuleId owner = moduleForUnit(interfaceUnit);
  auto *unit = findUnit(implementationUnit);
  if (owner == 0 || unit == nullptr || unit->moduleDeclaration ||
      (unit->moduleId != 0 && unit->moduleId != owner))
    return false;
  unit->moduleId = owner;
  const auto found = std::find_if(
      implementationRoots.begin(), implementationRoots.end(),
      [&](const SourceImplementation &implementation) {
        return implementation.interfaceUnit == interfaceUnit &&
               implementation.implementationUnit == implementationUnit;
      });
  if (found == implementationRoots.end())
    implementationRoots.push_back({interfaceUnit, implementationUnit});
  return true;
}

const std::vector<SourceDependency> &SourceGraph::dependencyEdges() const {
  return dependencies;
}

const std::vector<SourceImplementation> &
SourceGraph::moduleImplementations() const {
  return implementationRoots;
}

const std::vector<SourceUnitId> &SourceGraph::preludeRoots() const {
  return configuredPreludeRoots;
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
  if (requester == 0 || declaration == 0 || requester == declaration ||
      sameModule(requester, declaration)) {
    return true;
  }
  const SourceUnit *declaringUnit = findUnit(declaration);
  if (declaringUnit != nullptr && declaringUnit->prelude) {
    return true;
  }
  std::unordered_set<SourceUnitId> visited;
  std::vector<SourceUnitId> reexports;
  for (const SourceDependency &dependency : dependencies) {
    if (dependency.source != requester) {
      continue;
    }
    if (dependency.target == declaration) {
      return true;
    }
    if (dependency.kind == SourceDependencyKind::Import ||
        dependency.kind == SourceDependencyKind::ExportImport) {
      reexports.push_back(dependency.target);
    }
  }
  while (!reexports.empty()) {
    const SourceUnitId unit = reexports.back();
    reexports.pop_back();
    if (!visited.insert(unit).second) {
      continue;
    }
    for (const SourceDependency &dependency : dependencies) {
      if (dependency.source != unit ||
          dependency.kind != SourceDependencyKind::ExportImport) {
        continue;
      }
      if (dependency.target == declaration) {
        return true;
      }
      reexports.push_back(dependency.target);
    }
  }
  return false;
}

bool SourceGraph::isExported(SourceUnitId declaration,
                             std::size_t nameOffset) const {
  const SourceUnit *unit = findUnit(declaration);
  if (unit != nullptr && unit->implementationDeclaration)
    return false;
  if (unit == nullptr || !unit->moduleDeclaration) {
    return true;
  }
  const auto contains = [nameOffset](const SourceSpan &span) {
    return span.start <= nameOffset && nameOffset < span.end;
  };
  return std::any_of(unit->exportedDeclarations.begin(),
                     unit->exportedDeclarations.end(), contains) ||
         std::any_of(unit->exportedNamespaces.begin(),
                     unit->exportedNamespaces.end(), contains);
}

bool SourceGraph::isDeclarationVisible(SourceUnitId requester,
                                       SourceUnitId declaration,
                                       std::size_t nameOffset) const {
  return isVisible(requester, declaration) &&
         (requester == declaration || sameModule(requester, declaration) ||
          isExported(declaration, nameOffset));
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
  for (const auto implementation : implementationRoots) {
    // The interface introduces the declarations completed by this unit. Module
    // membership supplies that ordering without manufacturing an import edge.
    visit(implementation.interfaceUnit);
    visit(implementation.implementationUnit);
  }
  visit(entry);
  for (const SourceUnit &unit : units) {
    visit(unit.id);
  }
  return result;
}

void SourceGraph::clear() {
  units.clear();
  modules.clear();
  sourceImports.clear();
  owners.reset();
  dependencies.clear();
  configuredPreludeRoots.clear();
  implementationRoots.clear();
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

void SourceGraph::addPreludeRoot(SourceUnitId id) {
  configuredPreludeRoots.push_back(id);
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
