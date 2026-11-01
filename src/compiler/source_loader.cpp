#include "gti/source_loader.h"

#include "diagnostic_registry.h"

#include "configuration_directive_processor.h"
#include "gti/module_name.h"
#include "loaded_lexical_unit.h"

#include <fstream>
#include <llvm/Support/JSON.h>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lang {
namespace {

std::filesystem::path authoredPath(const std::filesystem::path &path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

void digestInteger(std::uint64_t &digest, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    digest ^= static_cast<unsigned char>(value >> shift);
    digest *= 1099511628211ULL;
  }
}

void digestText(std::uint64_t &digest, std::string_view value) {
  digestInteger(digest, value.size());
  for (const unsigned char byte : value) {
    digest ^= byte;
    digest *= 1099511628211ULL;
  }
}

void digestLiteral(std::uint64_t &digest, const Literal &literal) {
  digestInteger(digest, literal.index());
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::uint64_t>) {
          digestInteger(digest, value);
        } else if constexpr (std::is_same_v<Value, BinaryFloat>) {
          digestInteger(digest, value.bits);
          digestInteger(digest, static_cast<std::uint64_t>(value.format));
        } else if constexpr (std::is_same_v<Value, CharacterLiteral>) {
          digestInteger(digest, value.value);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          digestText(digest, value);
        } else if constexpr (std::is_same_v<Value, bool>) {
          digestInteger(digest, value ? 1 : 0);
        }
      },
      literal);
}

std::uint64_t syntaxDigest(const std::vector<Token> &tokens) {
  std::uint64_t result = 14695981039346656037ULL;
  for (const Token &token : tokens) {
    digestInteger(result, static_cast<std::uint64_t>(token.kind));
    digestInteger(result, token.position);
    digestInteger(result, static_cast<std::uint64_t>(token.line));
    digestText(result, token.lexeme);
    digestLiteral(result, token.literal);
    digestInteger(result, token.completion ? 1 : 0);
    digestInteger(result, token.generated ? 1 : 0);
    digestInteger(result, token.configurationFlagReference ? 1 : 0);
    digestInteger(result, token.configurationFlagDefined.has_value() ? 1 : 0);
    if (token.configurationFlagDefined) {
      digestInteger(result, *token.configurationFlagDefined ? 1 : 0);
    }
    if (token.leadingDocumentationMarkdown) {
      digestInteger(result, 1);
      digestText(result, *token.leadingDocumentationMarkdown);
    } else {
      digestInteger(result, 0);
    }
  }
  return result;
}

bool hasErrors(const std::vector<Diagnostic> &diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic &diagnostic) {
                       return diagnostic.severity == DiagnosticSeverity::Error;
                     });
}

} // namespace

SourceGraph SourceLoader::load(
    const std::filesystem::path &entryPath,
    std::optional<std::string> entrySource,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    std::optional<std::size_t> completionOffset,
    const std::vector<PackageSourceRoot> &packageSourceRoots, TargetInfo target,
    ConfigurationFlags configurationFlags,
    ConfigurationFlags declaredConfigurationFlags) {
  diagnostics.clear();
  states.clear();
  flagIndependentUnits.clear();
  graph.clear();
  sourceManager.clear();
  this->entrySource = std::move(entrySource);
  this->completionOffset = completionOffset;
  targetInfo = std::move(target);
  normalizeConfigurationFlags(configurationFlags);
  activeFlags.clear();
  declaredFlags.clear();
  activeFlags.insert(configurationFlags.begin(), configurationFlags.end());
  declaredFlags.insert(configurationFlags.begin(), configurationFlags.end());
  declaredFlags.insert(declaredConfigurationFlags.begin(),
                       declaredConfigurationFlags.end());
  initialFlags = activeFlags;
  this->sourceOverrides = &sourceOverrides;
  this->standardLibraryRoots.clear();
  this->standardLibraryRoots.reserve(standardLibraryRoots.size());
  for (const std::filesystem::path &root : standardLibraryRoots) {
    this->standardLibraryRoots.emplace_back(canonicalPath(root));
  }
  std::vector<SourceOwnerDescriptor> owners = packageSourceRoots;
  for (auto &owner : owners) {
    owner.packageRoot = canonicalPath(owner.packageRoot);
    owner.sourceRoot = canonicalPath(owner.sourceRoot);
    for (auto &[name, path] : owner.modules) {
      if (std::none_of(owner.moduleLookupPaths.begin(),
                       owner.moduleLookupPaths.end(),
                       [&](const auto &entry) { return entry.first == name; }))
        owner.moduleLookupPaths.emplace_back(name, authoredPath(path));
      path = canonicalPath(path);
    }
    for (auto &[name, path] : owner.moduleLookupPaths)
      path = authoredPath(path);
    for (auto &root : owner.additionalRoots)
      root = canonicalPath(root);
    std::sort(owner.dependencies.begin(), owner.dependencies.end(),
              [](const auto &left, const auto &right) {
                return left.alias < right.alias;
              });
  }
  const bool directMode =
      std::none_of(owners.begin(), owners.end(), [](const auto &owner) {
        return owner.kind == SourceOwnerKind::Package ||
               owner.kind == SourceOwnerKind::Generated;
      });
  if (directMode)
    owners.push_back({.identity = "direct",
                      .name = "direct",
                      .kind = SourceOwnerKind::Direct});
  // Toolchain roots supply resolution metadata only. Canonical trusted-source
  // provenance continues to be established separately when a unit is loaded.
  if (!this->standardLibraryRoots.empty() &&
      std::none_of(owners.begin(), owners.end(), [](const auto &owner) {
        return owner.implicitAlias && *owner.implicitAlias == "std";
      })) {
    SourceOwnerDescriptor standard{
        .identity = "toolchain:std",
        .name = "std",
        .packageRoot = this->standardLibraryRoots.front(),
        .sourceRoot = this->standardLibraryRoots.front(),
        .kind = SourceOwnerKind::Toolchain,
        .implicitAlias = "std"};
    for (const auto &root : this->standardLibraryRoots) {
      if (root != standard.packageRoot)
        standard.additionalRoots.push_back(root);
      const auto mapPath = root / "module-map.json";
      std::ifstream input(mapPath, std::ios::binary);
      if (!input)
        continue; // Missing maps never authorize a directory-based public API.
      std::ostringstream buffer;
      buffer << input.rdbuf();
      const std::string text = buffer.str();
      sourceManager.set(mapPath.string(), text);
      const auto failMap = [&](std::string message) {
        diagnostics.push_back(
            makeDiagnostic("GTI-I0012", DiagnosticPhase::SourceLoading,
                           {mapPath.string(), 0, text.empty() ? 0U : 1U, 1},
                           std::move(message)));
      };
      auto parsed = llvm::json::parse(text);
      if (!parsed) {
        failMap("Invalid source module map: " +
                llvm::toString(parsed.takeError()));
        continue;
      }
      const auto *object = parsed->getAsObject();
      const auto *modules = object ? object->getObject("modules") : nullptr;
      if (modules == nullptr) {
        failMap("A source module map requires a 'modules' object.");
        continue;
      }
      if (object->get("default") && !object->getString("default"))
        failMap("The default module must be a valid module name.");
      if (const auto defaultName = object->getString("default")) {
        if (!isModuleName(defaultName->str()))
          failMap("The default module must be a valid module name.");
        else
          standard.defaultModule = defaultName->str();
      }
      for (const auto &[key, value] : *modules) {
        const std::string name = key.str();
        const auto spelling = value.getAsString();
        if (!isModuleName(name) || !spelling || spelling->empty() ||
            spelling->find('\0') != llvm::StringRef::npos) {
          failMap("A public module map entry requires a module name and "
                  "relative .gti path.");
          continue;
        }
        const std::filesystem::path relative(spelling->str());
        const auto path = canonicalPath(root / relative);
        if (relative.is_absolute() || relative.extension() != ".gti" ||
            !pathIsWithin(root, path)) {
          failMap(
              "A public module interface must remain inside its source owner.");
          continue;
        }
        if (std::none_of(
                standard.modules.begin(), standard.modules.end(),
                [&](const auto &module) { return module.first == name; })) {
          standard.modules.emplace_back(name, path);
          standard.moduleLookupPaths.emplace_back(
              name, authoredPath(root / relative));
        }
      }
    }
    std::sort(standard.modules.begin(), standard.modules.end());
    owners.push_back(std::move(standard));
  }
  sourceOwners = std::make_shared<const SourceOwnerRegistry>(std::move(owners),
                                                             directMode);
  const auto invalidOwner = [&](std::string message) {
    diagnostics.push_back(makeDiagnostic(
        "GTI-I0012", DiagnosticPhase::SourceLoading,
        {canonicalPath(entryPath).string(), 0, 0, 1}, std::move(message)));
  };
  std::unordered_set<std::string> identities;
  std::unordered_set<std::string> aliases;
  std::unordered_set<std::string> roots;
  for (const auto &owner : sourceOwners->owners()) {
    if (owner.identity.empty() || !identities.insert(owner.identity).second)
      invalidOwner("Source owner identities must be nonempty and unique.");
    if (owner.implicitAlias && (!isModuleNameComponent(*owner.implicitAlias) ||
                                !aliases.insert(*owner.implicitAlias).second))
      invalidOwner("Configured source owner aliases must be valid and unique.");
    if (owner.kind == SourceOwnerKind::Direct)
      continue;
    if (!roots.insert(owner.packageRoot.string()).second)
      invalidOwner(
          "Distinct source owners cannot claim the same canonical root.");
    for (const auto &root : owner.additionalRoots)
      if (!roots.insert(root.string()).second)
        invalidOwner(
            "Distinct source owners cannot claim the same canonical root.");
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> interfaces;
    for (const auto &[name, path] : owner.modules) {
      const auto *actual = sourceOwners->ownerForPath(path);
      if (!isModuleName(name) || !names.insert(name).second ||
          !interfaces.insert(path.string()).second ||
          path.extension() != ".gti" || actual == nullptr ||
          actual->identity != owner.identity)
        invalidOwner("Public module registrations require unique valid names "
                     "and contained .gti interfaces.");
    }
    std::unordered_set<std::string> lookupNames;
    for (const auto &[name, path] : owner.moduleLookupPaths) {
      const auto registered =
          std::find_if(owner.modules.begin(), owner.modules.end(),
                       [&](const auto &entry) { return entry.first == name; });
      if (!lookupNames.insert(name).second ||
          registered == owner.modules.end() ||
          canonicalPath(path) != registered->second)
        invalidOwner("Authored module lookup paths must identify their "
                     "canonical registration.");
    }
    std::unordered_set<std::string> dependencyAliases;
    for (const auto &dependency : owner.dependencies)
      if (!isModuleNameComponent(dependency.alias) ||
          !dependencyAliases.insert(dependency.alias).second ||
          sourceOwners->find(dependency.targetIdentity) == nullptr ||
          sourceOwners->findImplicitAlias(dependency.alias) != nullptr)
        invalidOwner("Direct dependency aliases must be valid, unique, and "
                     "refer to a registered source owner.");
  }
  graph.setSourceOwners(sourceOwners);
  entrySourceConsumed = false;
  std::vector<SourceUnitId> preludes;
  const std::filesystem::path canonicalEntry = canonicalPath(entryPath);
  for (const std::filesystem::path &preludePath : preludePaths) {
    const SourceUnitId prelude =
        loadFile(canonicalPath(preludePath), false, true, nullptr, std::nullopt,
                 SourceUnitRole::Prelude);
    preludes.push_back(prelude);
    graph.addPreludeRoot(prelude);
  }
  const std::optional<std::string> entryStandardLibraryName =
      standardLibraryNameForExistingPath(canonicalEntry);
  const SourceUnitId entry =
      loadFile(canonicalEntry, true, false, nullptr, entryStandardLibraryName,
               entryStandardLibraryName ? SourceUnitRole::StandardLibrary
                                        : SourceUnitRole::Application);

  if (const auto *entryUnit = graph.findUnit(entry);
      entryUnit && entryUnit->implementationDeclaration) {
    const auto *owner = packageForPath(canonicalEntry);
    const std::string name = entryUnit->implementationDeclaration->lexeme;
    if (owner) {
      const auto implementation = std::find_if(
          owner->moduleImplementations.begin(),
          owner->moduleImplementations.end(), [&](const auto &item) {
            return item.first == name &&
                   std::any_of(item.second.begin(), item.second.end(),
                               [&](const auto &path) {
                                 return canonicalPath(path) == canonicalEntry;
                               });
          });
      const auto interface =
          std::find_if(owner->modules.begin(), owner->modules.end(),
                       [&](const auto &item) { return item.first == name; });
      if (implementation != owner->moduleImplementations.end() &&
          interface != owner->modules.end())
        loadFile(interface->second, false, false, nullptr);
    }
  }

  // Complete registered implementations after ordinary imports. Membership
  // makes the interface available without an artificial dependency cycle.
  std::unordered_set<std::string> loadedImplementations;
  for (std::size_t index = 0; index < graph.sourceUnits().size(); ++index) {
    const auto moduleDeclaration = graph.sourceUnits()[index].moduleDeclaration;
    const auto unitPath = graph.sourceUnits()[index].path;
    if (!moduleDeclaration)
      continue;
    const auto *package = packageForPath(unitPath);
    if (package == nullptr)
      continue;
    const auto registered =
        std::find_if(package->modules.begin(), package->modules.end(),
                     [&](const auto &entry) {
                       return entry.first == moduleDeclaration->lexeme &&
                              canonicalPath(entry.second) == unitPath;
                     });
    if (registered == package->modules.end())
      continue;
    for (const auto &[name, paths] : package->moduleImplementations) {
      if (name != registered->first)
        continue;
      for (const auto &path : paths) {
        const auto canonical = canonicalPath(path);
        if (packageForPath(canonical) != package ||
            canonical.extension() != ".gti") {
          report(
              *moduleDeclaration,
              "A module implementation must remain inside its owning package.",
              "GTI-I0012");
          continue;
        }
        if (!loadedImplementations.insert(canonical.string()).second) {
          if (!graph.attachModuleImplementation(
                  graph.sourceUnits()[index].id,
                  graph.sourceUnitForPath(canonical.string())))
            report(*moduleDeclaration,
                   "An implementation file cannot belong to two modules.",
                   "GTI-I0012");
          continue;
        }
        auto savedFlags = activeFlags;
        activeFlags = initialFlags;
        const auto implementation =
            loadFile(canonical, false, false, &*moduleDeclaration);
        activeFlags = std::move(savedFlags);
        const auto *implementationUnit = graph.findUnit(implementation);
        if (!implementationUnit ||
            !implementationUnit->implementationDeclaration ||
            implementationUnit->implementationDeclaration->lexeme != name) {
          report(implementationUnit &&
                         implementationUnit->implementationDeclaration
                     ? *implementationUnit->implementationDeclaration
                     : *moduleDeclaration,
                 "A registered implementation must begin with 'module " + name +
                     ";'.",
                 "GTI-I0012");
          continue;
        }
        if (!graph.attachModuleImplementation(graph.sourceUnits()[index].id,
                                              implementation))
          report(*moduleDeclaration,
                 "An implementation file cannot belong to two modules.",
                 "GTI-I0012");
      }
    }
  }

  for (const auto &unit : graph.sourceUnits()) {
    if (unit.implementationDeclaration && unit.moduleId == 0)
      report(*unit.implementationDeclaration,
             "A module implementation must be registered with its matching "
             "interface by its source owner.",
             "GTI-I0012");
  }

  // File loading alone cannot detect cycles whose edges originate in different
  // registered bodies: their interfaces may already be completely loaded.
  // Validate imports after membership is complete, treating each interface and
  // all its bodies as one module. Prelude layering is not an explicit import.
  if (!hadError()) {
    const auto group = [&](SourceUnitId unit) {
      const auto *module = graph.findModule(graph.moduleForUnit(unit));
      return module ? module->interfaceUnit : unit;
    };
    std::unordered_map<SourceUnitId, std::vector<const SourceImport *>>
        outgoing;
    for (const auto &import : graph.imports()) {
      if (import.target && group(import.requester) != group(*import.target))
        outgoing[group(import.requester)].push_back(&import);
    }
    std::unordered_map<SourceUnitId, LoadState> visited;
    std::function<bool(SourceUnitId)> visit = [&](SourceUnitId unit) {
      visited.emplace(unit, LoadState::Visiting);
      if (const auto edges = outgoing.find(unit); edges != outgoing.end()) {
        for (const auto *import : edges->second) {
          const auto target = group(*import->target);
          const auto prior = visited.find(target);
          if (prior != visited.end() && prior->second == LoadState::Visiting) {
            diagnostics.push_back(makeDiagnostic(
                "GTI-I0001", DiagnosticPhase::SourceLoading, import->name,
                "Module import cycle detected for '" + import->spelling +
                    "'."));
            return false;
          }
          if (prior == visited.end() && !visit(target))
            return false;
        }
      }
      visited[unit] = LoadState::Loaded;
      return true;
    };
    for (const auto &unit : graph.sourceUnits()) {
      const auto identity = group(unit.id);
      if (!visited.contains(identity) && !visit(identity))
        break;
    }
  }

  // Preludes are layered in order. A unit that an earlier prelude already
  // reaches, or that prelude itself, lies below every later prelude and never
  // depends on one; a later prelude root sees the earlier ones. Otherwise an
  // extra prelude, such as a test suite's, becomes reachable from the core
  // prelude, and the cycle check below then hides the core prelude from the
  // units that extra prelude includes.
  std::unordered_map<SourceUnitId, std::size_t> preludeLayer;
  for (const SourceUnit &unit : graph.sourceUnits()) {
    for (std::size_t index = 0; index < preludes.size(); ++index) {
      if (unit.id == preludes[index] ||
          graph.hasDependencyPath(preludes[index], unit.id)) {
        preludeLayer.emplace(unit.id, index);
        break;
      }
    }
  }
  for (const SourceUnit &unit : graph.sourceUnits()) {
    const auto layer = preludeLayer.find(unit.id);
    const std::size_t visible =
        layer == preludeLayer.end() ? preludes.size() : layer->second;
    for (std::size_t index = 0; index < visible; ++index) {
      const SourceUnitId prelude = preludes[index];
      if (unit.id != prelude && !graph.hasDirectDependency(unit.id, prelude) &&
          !graph.hasDependencyPath(prelude, unit.id)) {
        graph.addDependency({.source = unit.id,
                             .target = prelude,
                             .kind = SourceDependencyKind::Prelude});
      }
    }
  }
  if (entry != 0) {
    graph.entry = entry;
    if (SourceUnit *entryUnit = graph.findUnit(entry)) {
      entryUnit->entry = true;
    }
  }
  for (const SourceUnit &unit : graph.units) {
    if (!unit.tokens || flagIndependentUnits.contains(unit.id)) {
      continue;
    }
    for (const Token &token : *unit.tokens) {
      if (!token.configurationFlagReference) {
        continue;
      }
      if (declaredFlags.contains(token.lexeme)) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          DiagnosticId::UnknownConfigurationFlag, token, {token.lexeme});
      diagnostic.hints.emplace_back(
          "Check the spelling or define the flag in source, with '-D " +
          token.lexeme + "', or in manifest build/profile/target 'defines'.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  return std::move(graph);
}

bool SourceLoader::hadError() const { return hasErrors(diagnostics); }

const std::vector<SourceDiagnostic> &SourceLoader::errors() const {
  return diagnostics;
}

const SourceManager &SourceLoader::sources() const { return sourceManager; }

std::filesystem::path
SourceLoader::canonicalPath(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
    error.clear();
  }

  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.lexically_normal() : canonical;
}

bool SourceLoader::isImportPathSegment(std::string_view segment) {
  if (segment.empty()) {
    return false;
  }
  const auto validStart = [](char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalpha(character) != 0 || value == '_';
  };
  const auto validPart = [](char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_';
  };
  return validStart(segment.front()) &&
         std::all_of(segment.begin() + 1, segment.end(), validPart);
}

bool SourceLoader::pathIsWithin(const std::filesystem::path &root,
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

const PackageSourceRoot *
SourceLoader::packageForPath(const std::filesystem::path &path) const {
  return sourceOwners ? sourceOwners->ownerForPath(path) : nullptr;
}

const PackageSourceRoot *
SourceLoader::packageByIdentity(std::string_view identity) const {
  return sourceOwners ? sourceOwners->find(identity) : nullptr;
}

std::optional<std::string> SourceLoader::standardLibraryNameForExistingPath(
    const std::filesystem::path &path) const {
  for (const std::filesystem::path &root : standardLibraryRoots) {
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
      continue;
    }

    std::vector<std::string> segments;
    bool contained = true;
    for (const std::filesystem::path &component : relative) {
      const std::string segment = component.string();
      if (segment.empty() || segment == ".") {
        continue;
      }
      if (segment == "..") {
        contained = false;
        break;
      }
      segments.push_back(segment);
    }
    if (!contained || segments.size() < 2 || segments.front() != "std") {
      continue;
    }

    std::filesystem::path leaf(segments.back());
    if (leaf.extension() != ".gti") {
      continue;
    }
    segments.back() = leaf.stem().string();
    if (std::any_of(segments.begin(), segments.end(),
                    [](const std::string &segment) {
                      return !isImportPathSegment(segment);
                    })) {
      continue;
    }

    // Most application units are outside every configured standard-library
    // root. Establish lexical containment first so those common lookups do
    // not issue filesystem status calls; retain the same existence checks for
    // the only paths that could be trusted library units.
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
      continue;
    }
    error.clear();
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }

    std::string importName;
    for (const std::string &segment : segments) {
      if (!importName.empty()) {
        importName += '/';
      }
      importName += segment;
    }
    return importName;
  }
  return std::nullopt;
}

std::optional<std::string> SourceLoader::standardModuleNameForExistingPath(
    const std::filesystem::path &path) const {
  for (const std::filesystem::path &root : standardLibraryRoots) {
    if (path == canonicalPath(root / "std.gti"))
      return "std";
  }
  // Interfaces retain their established standard-library source identities;
  // trust still requires a path under a configured standard-library root.
  return standardLibraryNameForExistingPath(path);
}

SourceUnitId
SourceLoader::loadFile(const std::filesystem::path &path, bool isEntry,
                       bool isPrelude, const Token *includeToken,
                       std::optional<std::string> standardLibraryName,
                       SourceUnitRole role) {
  const std::string key = path.string();
  SourceUnitId unitId = 0;
  if (const auto state = states.find(key); state != states.end()) {
    unitId = state->second.unit;
    if (state->second.state == LoadState::Visiting && includeToken != nullptr) {
      report(*includeToken, "Module import cycle detected for '" + key + "'.",
             "GTI-I0001");
    }
    if (SourceUnit *unit = graph.findUnit(unitId)) {
      unit->entry = unit->entry || isEntry;
      unit->prelude = unit->prelude || isPrelude;
      if (role == SourceUnitRole::Prelude ||
          (role == SourceUnitRole::StandardLibrary &&
           unit->role == SourceUnitRole::Application)) {
        unit->role = role;
      }
      if (standardLibraryName) {
        unit->standardLibraryName = std::move(standardLibraryName);
      }
    }
    if (isEntry) {
      graph.entry = unitId;
    }
    return unitId;
  } else {
    std::optional<std::string> packageIdentity;
    std::optional<std::string> packageRelativePath;
    if (role == SourceUnitRole::Application) {
      if (const PackageSourceRoot *package = packageForPath(path);
          package && package->kind != SourceOwnerKind::Direct &&
          package->kind != SourceOwnerKind::Toolchain) {
        packageIdentity = package->identity;
        packageRelativePath =
            path.lexically_relative(package->packageRoot).generic_string();
      }
    }
    unitId = graph.addUnit(
        path, isEntry, isPrelude, std::move(standardLibraryName), role,
        std::move(packageIdentity), std::move(packageRelativePath));
    states.emplace(key,
                   FileState{.state = LoadState::Visiting, .unit = unitId});
  }
  std::shared_ptr<const ProcessedSourceUnit> processed;
  std::shared_ptr<const std::vector<Token>> lexedTokens;
  std::optional<LoadedLexicalUnit> lexical;
  if (!processed) {
    std::optional<std::string_view> exactSource;
    std::optional<std::size_t> unitCompletionOffset;
    if (isEntry && entrySource && !entrySourceConsumed) {
      entrySourceConsumed = true;
      exactSource = *entrySource;
      unitCompletionOffset = completionOffset;
    } else if (const auto override = sourceOverrides->find(key);
               override != sourceOverrides->end()) {
      exactSource = override->second;
    }
    std::uint64_t generation = 0;
    if (sourceGenerations != nullptr) {
      if (const auto found = sourceGenerations->find(key);
          found != sourceGenerations->end()) {
        generation = found->second;
      }
    }
    std::uint64_t knownSourceDigest = 0;
    if (!exactSource && sourceDigests != nullptr) {
      if (const auto found = sourceDigests->find(key);
          found != sourceDigests->end()) {
        knownSourceDigest = found->second;
      }
    }
    lexical = LoadedLexicalUnitLoader(workMetrics, cache)
                  .load({.path = path,
                         .canonicalPath = key,
                         .exactSource = exactSource,
                         .completionOffset = unitCompletionOffset,
                         .sourceGeneration = generation,
                         .knownSourceDigest = knownSourceDigest,
                         .cacheContext = cacheContext});
    sourceManager.set(key, lexical->source);
    if (SourceUnit *unit = graph.findUnit(unitId)) {
      unit->sourceDigest = lexical->sourceDigest;
    }

    for (const Diagnostic &diagnostic : lexical->diagnostics) {
      Diagnostic forwarded = diagnostic;
      if (includeToken != nullptr) {
        forwarded.related.push_back(
            {tokenSpan(*includeToken), "Imported from here."});
      }
      diagnostics.emplace_back(std::move(forwarded));
    }
    if (lexical->hadError) {
      states[key].state = LoadState::Loaded;
      return unitId;
    }
    processed = lexical->processed;
    lexedTokens = lexical->tokens;
  }
  static const std::vector<Token> noTokens;
  const std::vector<Token> &lexed = lexedTokens ? *lexedTokens : noTokens;

  std::optional<Token> moduleDeclaration;
  std::optional<SourceSpan> moduleDeclarationSpan;
  std::optional<Token> implementationDeclaration;
  std::optional<SourceSpan> implementationDeclarationSpan;
  std::size_t moduleHeaderEnd = 0;
  if (processed) {
    moduleDeclaration = processed->moduleDeclaration;
    moduleDeclarationSpan = processed->moduleDeclarationSpan;
    implementationDeclaration = processed->implementationDeclaration;
    implementationDeclarationSpan = processed->implementationDeclarationSpan;
  } else if (const auto header = parseModuleHeader(lexed)) {
    if (header->interface) {
      moduleDeclaration = header->declaration;
      moduleDeclarationSpan = header->span;
    } else {
      implementationDeclaration = header->declaration;
      implementationDeclarationSpan = header->span;
    }
    moduleHeaderEnd = header->tokenEnd;
  }
  // `std` names the configured standard library in direct and project
  // compilation alike. A source header outside its roots cannot claim it.
  for (const auto &[declaration, span] :
       {std::pair{&moduleDeclaration, &moduleDeclarationSpan},
        std::pair{&implementationDeclaration,
                  &implementationDeclarationSpan}}) {
    if (!*declaration)
      continue;
    const std::string &name = (*declaration)->lexeme;
    if ((name == "std" || name.starts_with("std.")) && !standardLibraryName &&
        !standardModuleNameForExistingPath(path)) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-I0012", DiagnosticPhase::SourceLoading,
          span->value_or(tokenSpan(**declaration)),
          "The module name 'std' is reserved for the standard library."));
    }
  }
  graph.findUnit(unitId)->implementationDeclaration = implementationDeclaration;
  graph.findUnit(unitId)->implementationDeclarationSpan =
      implementationDeclarationSpan;
  const bool moduleInterface = moduleDeclaration.has_value();
  std::unordered_set<std::string> importerFlags;
  if (moduleInterface) {
    graph.findUnit(unitId)->moduleDeclaration = moduleDeclaration;
    graph.findUnit(unitId)->moduleDeclarationSpan = moduleDeclarationSpan;
    const Token &moduleName = *moduleDeclaration;
    importerFlags = activeFlags;
    activeFlags = initialFlags;
    const auto *owner = packageForPath(path);
    const std::string ownerIdentity = owner ? owner->identity : "direct";
    if (graph.registerModule(unitId, ownerIdentity, moduleName.lexeme) == 0) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-I0012", DiagnosticPhase::SourceLoading,
          moduleDeclarationSpan.value_or(tokenSpan(moduleName)),
          "Module '" + moduleName.lexeme +
              "' has more than one interface in this source owner.");
      for (const auto &other : graph.sourceUnits())
        if (other.id != unitId && other.moduleDeclaration &&
            other.moduleDeclaration->lexeme == moduleName.lexeme &&
            packageForPath(other.path) &&
            packageForPath(other.path)->identity == ownerIdentity) {
          diagnostic.related.push_back(
              {other.moduleDeclarationSpan.value_or(
                   tokenSpan(*other.moduleDeclaration)),
               "First interface is here."});
          break;
        }
      diagnostics.push_back(std::move(diagnostic));
    }
  }

  const auto finishLoad = [&] {
    states[key].state = LoadState::Loaded;
    if (moduleInterface) {
      activeFlags = std::move(importerFlags);
    }
    return unitId;
  };

  std::size_t includeOccurrence = 0;
  // Resolves the import directive or diagnoses retired include syntax at
  // `index` and records its dependency edge, returning the directive's last
  // token. First loading and replay of a processed unit share it; replay
  // presents only the directive's own tokens.
  const auto resolveDirective = [&](const std::vector<Token> &tokens,
                                    std::size_t index, int braceDepth,
                                    int conditionalDepth) {
    const Token &token = tokens[index];
    const bool exportImport = token.kind == TokenKind::EXPORT &&
                              index + 1 < tokens.size() &&
                              tokens[index + 1].kind == TokenKind::IMPORT;
    if (token.kind == TokenKind::IMPORT || exportImport) {
      if (exportImport && !moduleInterface) {
        report(token,
               "Re-exporting an import requires an 'export module name;' "
               "interface.",
               "GTI-I0012");
      }
      const ResolvedImport imported =
          resolveImport(tokens, index + (exportImport ? 1 : 0), path,
                        braceDepth, conditionalDepth, exportImport);
      if (imported.dependency != 0) {
        graph.addDependency({.source = unitId,
                             .target = imported.dependency,
                             .kind = imported.kind,
                             .directive = tokenSpan(token),
                             .includeSpelling = imported.includeSpelling,
                             .includeOccurrence = includeOccurrence++});
      }
      return imported.directiveEnd;
    }
    const bool exported = token.kind == TokenKind::EXPORT;
    const Token &includeToken = tokens[index + (exported ? 1 : 0)];
    Diagnostic diagnostic =
        makeDiagnostic(DiagnosticId::RemovedInclude, includeToken);
    diagnostic.hints.push_back(
        "Import an explicit module interface instead. Use 'import std.name;' "
        "for a standard module, or 'import \"path.gti\";' for a relative "
        "interface; publish its API with 'export module name;' and exports.");
    diagnostics.push_back(std::move(diagnostic));
    // Do not resolve or load a rejected legacy dependency. Keep scanning the
    // following source lines so independent dependency errors remain visible.
    std::size_t end = index + (exported ? 1 : 0);
    while (end + 1 < tokens.size() &&
           tokens[end + 1].kind != TokenKind::END_OF_FILE &&
           tokens[end + 1].line == includeToken.line) {
      ++end;
    }
    return end;
  };

  const auto publish = [&](std::shared_ptr<const ProcessedSourceUnit> unit) {
    SourceUnit *loadedUnit = graph.findUnit(unitId);
    loadedUnit->syntaxDigest = unit->syntaxDigest;
    loadedUnit->tokens = unit->tokens;
    loadedUnit->inactiveSpans = unit->inactiveSpans;
    loadedUnit->configurationTokens = unit->configurationTokens;
    flagIndependentUnits.insert(unitId);
  };

  if (processed) {
    if (workMetrics != nullptr) {
      ++workMetrics->processedSourceUnitReplays;
    }
    for (const ProcessedSourceUnit::Directive &directive :
         processed->directives) {
      (void)resolveDirective(directive.tokens, 0, directive.braceDepth,
                             directive.conditionalDepth);
    }
    publish(processed);
    return finishLoad();
  }

  const bool requiresDirectiveProcessing =
      std::any_of(lexed.begin(), lexed.end(), [](const Token &token) {
        return isDirectiveToken(token.kind) ||
               token.kind == TokenKind::IMPORT ||
               token.kind == TokenKind::MODULE ||
               token.kind == TokenKind::EXPORT ||
               (token.kind == TokenKind::IDENTIFIER &&
                token.lexeme == "include");
      });
  // Defining, undefining, or testing a flag makes the selected syntax depend
  // on the flags reaching this unit; only a unit without them, whose
  // conditionals test at most the target, has a processed form.
  const bool configurationFree =
      lexical->diagnostics.empty() &&
      std::none_of(lexed.begin(), lexed.end(), [](const Token &token) {
        return token.kind == TokenKind::HASH_DEFINE ||
               token.kind == TokenKind::HASH_UNDEF ||
               token.kind == TokenKind::HASH_IFDEF ||
               token.kind == TokenKind::HASH_IFNDEF ||
               token.kind == TokenKind::HASH_ERROR;
      });
  const auto store = [&](ProcessedSourceUnit unit) {
    auto shared = std::make_shared<const ProcessedSourceUnit>(std::move(unit));
    if (cache != nullptr) {
      cache->storeProcessed(lexical->cacheKey, lexical->source, shared);
    }
    publish(std::move(shared));
  };
  if (!requiresDirectiveProcessing) {
    if (configurationFree) {
      store({.tokens = lexedTokens, .syntaxDigest = lexical->sourceDigest});
    } else {
      SourceUnit *loadedUnit = graph.findUnit(unitId);
      loadedUnit->syntaxDigest = lexical->sourceDigest;
      loadedUnit->tokens = lexedTokens;
    }
    states[key].state = LoadState::Loaded;
    return unitId;
  }

  // The directive processor resolves configuration flag references in place.
  std::vector<Token> fileTokens = lexed;
  std::vector<Token> output;
  output.reserve(fileTokens.size());
  std::vector<ProcessedSourceUnit::Directive> directives;
  // Set by any diagnostic of this unit's own loading, which replay would not
  // reproduce; directive resolution re-reports its own on replay.
  bool tokenDiagnostic = false;
  int braceDepth = 0;
  ConfigurationDirectiveProcessor directiveProcessor(
      fileTokens, targetInfo, activeFlags, declaredFlags, diagnostics);
  for (std::size_t index = moduleHeaderEnd; index < fileTokens.size();
       ++index) {
    Token &token = fileTokens[index];
    if (token.kind == TokenKind::END_OF_FILE) {
      output.push_back(std::move(token));
      continue;
    }

    const std::size_t diagnosticsBefore = diagnostics.size();
    const ConfigurationDirectiveStep directive =
        directiveProcessor.process(index);
    tokenDiagnostic =
        tokenDiagnostic || diagnostics.size() != diagnosticsBefore;
    if (directive.consumed) {
      index = directive.lastConsumedIndex;
      continue;
    }

    const bool startsLine =
        index == 0 || fileTokens[index - 1].line != token.line;
    const bool legacyInclude =
        braceDepth == 0 && startsLine && token.kind == TokenKind::IDENTIFIER &&
        token.lexeme == "include" && index + 1 < fileTokens.size() &&
        (fileTokens[index + 1].kind == TokenKind::STRING_LITERAL ||
         fileTokens[index + 1].kind == TokenKind::LESS);
    if (legacyInclude) {
      tokenDiagnostic = true;
      report(
          token,
          "Source inclusion has been removed; use an explicit module import.",
          "GTI-I0009");
    }

    const bool exported =
        token.kind == TokenKind::EXPORT && index + 1 < fileTokens.size();
    const bool import =
        token.kind == TokenKind::IMPORT ||
        (exported && fileTokens[index + 1].kind == TokenKind::IMPORT);
    const bool include =
        token.kind == TokenKind::HASH_INCLUDE ||
        (exported && fileTokens[index + 1].kind == TokenKind::HASH_INCLUDE);
    if (import || include) {
      const std::size_t directiveEnd = resolveDirective(
          fileTokens, index, braceDepth, directiveProcessor.conditionalDepth());
      const std::size_t snapshotEnd =
          std::min(directiveEnd + 2, fileTokens.size());
      directives.push_back(
          {.tokens = std::vector<Token>(
               fileTokens.begin() + static_cast<std::ptrdiff_t>(index),
               fileTokens.begin() + static_cast<std::ptrdiff_t>(snapshotEnd)),
           .braceDepth = braceDepth,
           .conditionalDepth = directiveProcessor.conditionalDepth()});
      if (import) {
        // The loader resolves dependency identity, but the parser owns
        // whether this directive is between complete source-unit
        // declarations. Erasing it here could turn malformed parameters or
        // initializers into valid source. Keep its original tokens and spans
        // in the unit syntax cache.
        for (std::size_t part = index; part <= directiveEnd; ++part) {
          output.push_back(fileTokens[part]);
        }
      }
      index = directiveEnd;
      continue;
    }
    if (token.kind == TokenKind::MODULE ||
        (token.kind == TokenKind::EXPORT && index + 1 < fileTokens.size() &&
         fileTokens[index + 1].kind == TokenKind::MODULE)) {
      tokenDiagnostic = true;
      report(
          token,
          "A module header must be the first declaration: 'export module "
          "name;' "
          "for an interface or 'module name;' for a registered implementation.",
          "GTI-I0012");
      while (index + 1 < fileTokens.size() &&
             fileTokens[index].kind != TokenKind::SEMICOLON &&
             fileTokens[index + 1].kind != TokenKind::END_OF_FILE) {
        ++index;
      }
      continue;
    }

    if (!directiveProcessor.currentPathActive() &&
        !directive.conditionalBoundary) {
      directiveProcessor.retainInactive(token);
    }

    if (token.kind == TokenKind::LEFT_BRACE) {
      ++braceDepth;
    } else if (token.kind == TokenKind::RIGHT_BRACE && braceDepth > 0) {
      --braceDepth;
    }
    output.push_back(std::move(token));
  }

  const std::size_t diagnosticsBeforeFinish = diagnostics.size();
  directiveProcessor.finish();
  tokenDiagnostic =
      tokenDiagnostic || diagnostics.size() != diagnosticsBeforeFinish;

  if (output.empty() || output.back().kind != TokenKind::END_OF_FILE) {
    output.emplace_back(TokenKind::END_OF_FILE, "", std::monostate{}, 0, 1,
                        key);
  }
  SourceUnit *loadedUnit = graph.findUnit(unitId);
  const std::uint64_t selectedSyntax = syntaxDigest(output);
  if (configurationFree && !tokenDiagnostic &&
      std::none_of(output.begin(), output.end(), [](const Token &token) {
        return token.configurationFlagReference;
      })) {
    store(
        {.tokens = std::make_shared<std::vector<Token>>(std::move(output)),
         .syntaxDigest = selectedSyntax,
         .moduleDeclaration = std::move(moduleDeclaration),
         .moduleDeclarationSpan = std::move(moduleDeclarationSpan),
         .implementationDeclaration = std::move(implementationDeclaration),
         .implementationDeclarationSpan =
             std::move(implementationDeclarationSpan),
         .directives = std::move(directives),
         .inactiveSpans = directiveProcessor.takeInactiveSpans(),
         .configurationTokens = directiveProcessor.takeConfigurationTokens()});
  } else {
    loadedUnit->syntaxDigest = selectedSyntax;
    loadedUnit->tokens =
        std::make_shared<std::vector<Token>>(std::move(output));
    loadedUnit->inactiveSpans = directiveProcessor.takeInactiveSpans();
    loadedUnit->configurationTokens =
        directiveProcessor.takeConfigurationTokens();
  }
  return finishLoad();
}

SourceLoader::ResolvedImport
SourceLoader::resolveImport(const std::vector<Token> &tokens, std::size_t index,
                            const std::filesystem::path &includingFile,
                            int braceDepth, int conditionalDepth,
                            bool reexport) {
  const Token &keyword = tokens[index];
  std::size_t current = index + 1;
  std::vector<std::string> names;
  std::vector<SourceSpan> segmentSpans;
  const bool quoted = current < tokens.size() &&
                      tokens[current].kind == TokenKind::STRING_LITERAL;
  if (quoted) {
    ++current;
  } else {
    while (current < tokens.size() &&
           tokens[current].kind == TokenKind::IDENTIFIER) {
      segmentSpans.push_back(tokenSpan(tokens[current]));
      names.push_back(tokens[current++].lexeme);
      if (current >= tokens.size() || tokens[current].kind != TokenKind::DOT)
        break;
      ++current;
    }
  }
  const bool terminated =
      current < tokens.size() && tokens[current].kind == TokenKind::SEMICOLON;
  const std::size_t end = terminated ? current : current - 1;
  ResolvedImport result{.directiveEnd = end,
                        .kind = reexport ? SourceDependencyKind::ExportImport
                                         : SourceDependencyKind::Import};
  for (std::size_t part = index + 1; part < current; ++part)
    result.includeSpelling += tokens[part].lexeme;
  const auto tokenEnd = [](const Token &token) {
    return token.position + token.lexeme.size();
  };
  const auto &begin = reexport && index > 0 ? tokens[index - 1] : keyword;
  const auto nameStart =
      current > index + 1 ? tokens[index + 1].position : tokenEnd(keyword);
  const auto nameEnd =
      current > index + 1 ? tokenEnd(tokens[current - 1]) : nameStart;
  SourceImport sourceImport{
      .requester = graph.sourceUnitForPath(includingFile.string()),
      .declaration = {keyword.source, begin.position, tokenEnd(tokens[end]),
                      begin.line},
      .name = {keyword.source, nameStart, nameEnd,
               current > index + 1 ? tokens[index + 1].line : keyword.line},
      .spelling = result.includeSpelling,
      .segments = names,
      .segmentSpans = std::move(segmentSpans),
      .quoted = quoted,
      .reexport = reexport,
      .resolution = terminated ? SourceImportResolution::Unresolved
                               : SourceImportResolution::Incomplete};
  const auto *requester = graph.findUnit(sourceImport.requester);
  if (!quoted && requester && requester->entry && completionOffset &&
      *completionOffset >= tokenEnd(keyword) &&
      (current >= tokens.size() ||
       tokens[current].kind == TokenKind::END_OF_FILE ||
       *completionOffset <= tokens[current].position)) {
    const bool active =
        std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(index + 1),
                    tokens.begin() + static_cast<std::ptrdiff_t>(current),
                    [](const Token &token) { return token.completion; });
    if (active) {
      std::string prefix;
      for (std::size_t part = index + 1; part < current; ++part) {
        const auto &token = tokens[part];
        if (token.position >= *completionOffset)
          break;
        prefix += token.lexeme.substr(0, *completionOffset - token.position);
      }
      sourceImport.completionPrefix = std::move(prefix);
    }
  }
  const auto reject = [&](std::string message) {
    graph.addImport(sourceImport);
    report(keyword, std::move(message), "GTI-I0012");
    return result;
  };
  if (!terminated || (!quoted && (names.empty() ||
                                  tokens[current - 1].kind == TokenKind::DOT)))
    return reject(
        "Expect a module name or quoted relative .gti path followed by ';'.");
  if (braceDepth != 0 || conditionalDepth != 0)
    return reject(
        "Module imports are unconditional source-unit-level declarations.");

  const auto joinNames = [&](std::size_t first) {
    std::string name;
    for (std::size_t part = first; part < names.size(); ++part) {
      if (!name.empty())
        name += '.';
      name += names[part];
    }
    return name;
  };
  const SourceOwnerDescriptor *owner = packageForPath(includingFile);
  std::filesystem::path path;
  std::optional<std::string> expectedName;
  if (quoted) {
    const auto *spelling = std::get_if<std::string>(&tokens[index + 1].literal);
    if (spelling == nullptr || spelling->empty() ||
        spelling->find('\0') != std::string::npos)
      return reject("A quoted module import requires a nonempty .gti path.");
    const std::filesystem::path relative(*spelling);
    if (relative.is_absolute() || relative.extension() != ".gti")
      return reject("A quoted module import must name a relative .gti file.");
    sourceImport.authoredLookupPath =
        authoredPath(includingFile.parent_path() / relative);
    path = canonicalPath(*sourceImport.authoredLookupPath);
    const auto *targetOwner = packageForPath(path);
    if (owner == nullptr || targetOwner == nullptr ||
        owner->identity != targetOwner->identity)
      return reject("A quoted import cannot cross a source-owner boundary; use "
                    "a declared dependency alias.");
  } else {
    const std::string localName = joinNames(0);
    const auto local =
        owner ? std::find_if(owner->modules.begin(), owner->modules.end(),
                             [&](const auto &module) {
                               return module.first == localName;
                             })
              : std::vector<std::pair<std::string,
                                      std::filesystem::path>>::const_iterator{};
    const bool hasLocal = owner && local != owner->modules.end();
    const SourceOwnerDescriptor *target =
        sourceOwners->findImplicitAlias(names.front());
    if (owner) {
      const auto dependency = std::find_if(
          owner->dependencies.begin(), owner->dependencies.end(),
          [&](const auto &edge) { return edge.alias == names.front(); });
      if (dependency != owner->dependencies.end()) {
        if (target)
          return reject("Dependency alias '" + names.front() +
                        "' conflicts with a configured source owner.");
        target = packageByIdentity(dependency->targetIdentity);
        if (!target)
          return reject(
              "Imported source owner is absent from the resolved graph.");
      }
    }
    const std::string dependencyName =
        target && names.size() == 1 ? target->defaultModule : joinNames(1);
    const std::pair<std::string, std::filesystem::path> *dependencyModule =
        nullptr;
    if (target) {
      const auto candidate = std::find_if(
          target->modules.begin(), target->modules.end(),
          [&](const auto &module) { return module.first == dependencyName; });
      if (candidate != target->modules.end())
        dependencyModule = &*candidate;
    }
    if (hasLocal && dependencyModule)
      return reject(
          "Module name '" + localName +
          "' is ambiguous with a dependency alias; rename the alias.");
    std::string name;
    if (hasLocal) {
      target = owner;
      name = localName;
      path = canonicalPath(local->second);
    } else if (target) {
      name = dependencyName;
      if (!dependencyModule)
        return reject("Source owner '" + target->name +
                      "' does not declare public module '" + name + "'.");
      path = canonicalPath(dependencyModule->second);
    } else {
      if (!owner || owner->kind == SourceOwnerKind::Direct)
        return reject("Named module imports require a registered source owner; "
                      "direct compilation can use a quoted import.");
      return reject("Source owner '" + owner->name +
                    "' does not declare module '" + localName +
                    "' or dependency alias '" + names.front() + "'.");
    }
    const auto authored = std::find_if(
        target->moduleLookupPaths.begin(), target->moduleLookupPaths.end(),
        [&](const auto &entry) { return entry.first == name; });
    sourceImport.authoredLookupPath =
        authored != target->moduleLookupPaths.end() ? authored->second : path;
    if (canonicalPath(*sourceImport.authoredLookupPath) != path)
      return reject("Module lookup changed after its registration was "
                    "resolved; retry with current metadata.");
    const auto *resolvedOwner = packageForPath(path);
    if (!resolvedOwner || resolvedOwner->identity != target->identity ||
        path.extension() != ".gti")
      return reject("Module interface path escapes its owning source root or "
                    "is not a .gti file.");
    expectedName = std::move(name);
  }
  sourceImport.lookupPath = path;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) &&
      !sourceOverrides->contains(path.string())) {
    const auto *targetOwner = packageForPath(path);
    if (targetOwner && targetOwner->kind == SourceOwnerKind::Generated)
      return reject(
          "Generated module interface '" + path.string() +
          "' is unavailable; run 'gti check' or 'gti build' to generate it.");
    return reject("Module interface '" + path.string() + "' was not found.");
  }
  const auto trustedName = standardModuleNameForExistingPath(path);
  const SourceUnitId loaded =
      loadFile(path, false, false, &keyword, trustedName,
               trustedName ? SourceUnitRole::StandardLibrary
                           : SourceUnitRole::Application);
  const SourceUnit *unit = graph.findUnit(loaded);
  if (unit == nullptr || !unit->moduleDeclaration)
    return reject(
        "An import must name an explicit 'export module name;' interface; add "
        "a module header and explicit exports to the dependency.");
  if (expectedName && unit->moduleDeclaration->lexeme != *expectedName) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-I0012", DiagnosticPhase::SourceLoading, keyword,
        "Imported module '" + *expectedName + "' declares itself as '" +
            unit->moduleDeclaration->lexeme + "'.");
    diagnostic.related.push_back({unit->moduleDeclarationSpan.value_or(
                                      tokenSpan(*unit->moduleDeclaration)),
                                  "Interface declaration is here."});
    diagnostics.push_back(std::move(diagnostic));
    graph.addImport(sourceImport);
    return result;
  }
  result.dependency = loaded;
  sourceImport.target = loaded;
  sourceImport.resolution = SourceImportResolution::Resolved;
  graph.addImport(std::move(sourceImport));
  return result;
}

void SourceLoader::report(const Token &token, std::string message,
                          std::string code) {
  diagnostics.push_back(makeDiagnostic(std::move(code),
                                       DiagnosticPhase::SourceLoading, token,
                                       std::move(message)));
}

} // namespace lang
