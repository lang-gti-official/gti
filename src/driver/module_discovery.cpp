#include "module_discovery.h"

#include "gti/lexer.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>

namespace lang::driver::detail {
namespace {

bool within(const std::filesystem::path &root,
            const std::filesystem::path &path) {
  auto first = root.begin();
  auto second = path.begin();
  for (; first != root.end() && second != path.end(); ++first, ++second)
    if (*first != *second)
      return false;
  return first == root.end();
}

bool nestedPackage(const std::filesystem::path &packageRoot,
                   std::filesystem::path directory) {
  for (; directory != packageRoot && within(packageRoot, directory);
       directory = directory.parent_path()) {
    std::error_code error;
    if (std::filesystem::exists(directory / "gti.toml", error))
      return true;
  }
  return false;
}

void conflict(std::vector<Diagnostic> &diagnostics, const SourceSpan &span,
              std::string message, const std::filesystem::path &other) {
  auto diagnostic = makeDiagnostic("GTI-B1005", DiagnosticPhase::Driver, span,
                                   std::move(message));
  diagnostic.related.push_back(
      {{other.string(), 0, 1, 1}, "The other module interface is here."});
  diagnostic.hints.push_back(
      "Select an interface explicitly in [modules], or exclude the unwanted "
      "file in [module-discovery].");
  diagnostics.push_back(std::move(diagnostic));
}

} // namespace

std::vector<DiscoveredModule>
discoverModules(const std::filesystem::path &packageRoot,
                const ModuleDiscoverySettings &settings, SourceManager &sources,
                std::vector<Diagnostic> &diagnostics) {
  std::set<std::filesystem::path> files;
  std::set<std::filesystem::path> visited;
  const auto report = [&](std::string code, std::string message) {
    diagnostics.push_back(
        makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                       settings.declaration, std::move(message)));
  };
  const auto excluded = [&](const std::filesystem::path &path) {
    return std::any_of(
        settings.exclusions.begin(), settings.exclusions.end(),
        [&](const auto &exclude) { return within(exclude, path); });
  };
  std::function<void(const std::filesystem::path &)> visit;
  visit = [&](const std::filesystem::path &directory) {
    if (excluded(directory) || !visited.insert(directory).second ||
        nestedPackage(packageRoot, directory))
      return;
    std::error_code error;
    std::vector<std::filesystem::directory_entry> entries;
    std::filesystem::directory_iterator cursor(directory, error), end;
    for (; !error && cursor != end; cursor.increment(error))
      entries.push_back(*cursor);
    if (error) {
      report("GTI-B1102", "Failed to discover modules in '" +
                              directory.string() + "': " + error.message() +
                              ".");
      return;
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
      return a.path().generic_string() < b.path().generic_string();
    });
    for (const auto &entry : entries) {
      const auto path = entry.path().lexically_normal();
      if (excluded(path) || path.filename().string().starts_with('.'))
        continue;
      const auto status = entry.symlink_status(error);
      if (error) {
        report("GTI-B1102", "Failed to inspect module candidate '" +
                                path.string() + "': " + error.message() + ".");
        continue;
      }
      if (std::filesystem::is_directory(status)) {
        if (path.filename() != "build")
          visit(path);
      } else if (path.extension() == ".gti" &&
                 (std::filesystem::is_regular_file(status) ||
                  std::filesystem::is_symlink(status))) {
        files.insert(path);
      }
      // Directory symlinks are not traversed: no cycles or implicit extra
      // roots.
    }
  };
  for (const auto &root : settings.roots) {
    std::error_code error;
    const bool exists = std::filesystem::exists(root, error);
    if (!exists && !error && !settings.explicitRoots)
      continue; // Existing projects need not have the default src directory.
    const auto canonical = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonical, error)) {
      report("GTI-B1103", "A module discovery root must name an existing "
                          "directory: " +
                              root.string() + ".");
    } else if (!within(packageRoot, canonical)) {
      report("GTI-B1104", "Module discovery root escapes its owning package: " +
                              root.string() + ".");
    } else if (nestedPackage(packageRoot, canonical)) {
      report("GTI-B1104",
             "Module discovery root belongs to a nested package: " +
                 root.string() + ".");
    } else {
      visit(canonical);
    }
  }

  std::vector<DiscoveredModule> result;
  std::map<std::filesystem::path, std::filesystem::path> physicalFiles;
  for (const auto &path : files) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical, error)) {
      report("GTI-B1103", "Module discovery candidate is not a regular file: " +
                              path.string() + ".");
      continue;
    }
    if (!within(packageRoot, canonical) ||
        nestedPackage(packageRoot, canonical.parent_path())) {
      report("GTI-B1104", "Module discovery candidate escapes its owning "
                          "package: " +
                              path.string() + ".");
      continue;
    }
    if (excluded(canonical))
      continue;
    if (const auto found = physicalFiles.find(canonical);
        found != physicalFiles.end()) {
      report("GTI-B1005", "Module discovery reaches one file through multiple "
                          "paths: " +
                              found->second.string() + " and " + path.string() +
                              ".");
      continue;
    }
    physicalFiles.emplace(canonical, path);
    std::ifstream input(canonical, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(input), {}};
    if (!input || input.bad()) {
      report("GTI-B1102",
             "Failed to read module candidate: " + path.string() + ".");
      continue;
    }
    Lexer lexer;
    const auto tokens = lexer.scanModuleHeader(text, canonical.string());
    const bool beginsModule =
        !tokens.empty() &&
        (tokens[0].kind == TokenKind::MODULE ||
         (tokens.size() > 1 && tokens[0].kind == TokenKind::EXPORT &&
          tokens[1].kind == TokenKind::MODULE));
    if (!beginsModule)
      continue;
    const auto prefixEnd = tokens.back().position;
    sources.set(canonical.string(), text.substr(0, prefixEnd));
    const auto header = parseModuleHeader(tokens);
    if (lexer.hadError()) {
      diagnostics.insert(diagnostics.end(), lexer.errors().begin(),
                         lexer.errors().end());
    } else if (!header) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-B1005", DiagnosticPhase::Driver, tokenSpan(tokens.front()),
          "Module discovery requires a leading 'export module name;' or "
          "'module name;' declaration."));
    } else if (header->declaration.lexeme == "std" ||
               header->declaration.lexeme.starts_with("std.")) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-B1005", DiagnosticPhase::Driver, header->span,
          "The module name 'std' is reserved for the standard library."));
    } else {
      result.push_back({*header, canonical, path});
    }
  }
  return result;
}

void addDiscoveredInterfaces(ProjectPackage &package,
                             const std::vector<DiscoveredModule> &discovered,
                             std::vector<Diagnostic> &diagnostics) {
  const auto explicitModules = package.modules;
  for (const auto &unit : discovered) {
    if (!unit.header.interface)
      continue;
    const auto &name = unit.header.declaration.lexeme;
    if (std::any_of(explicitModules.begin(), explicitModules.end(),
                    [&](const auto &entry) {
                      return entry.first == name || entry.second == unit.path;
                    }))
      continue;
    const auto collision = std::find_if(
        package.modules.begin(), package.modules.end(), [&](const auto &entry) {
          return portableProjectNameKey(entry.first) ==
                 portableProjectNameKey(name);
        });
    if (collision != package.modules.end()) {
      conflict(
          diagnostics, unit.header.span,
          "Discovered module interfaces must have distinct portable names: '" +
              name + "'.",
          collision->second);
      continue;
    }
    package.modules.emplace_back(name, unit.path);
    package.moduleLookupPaths.emplace_back(name, unit.lookupPath);
  }
}

void addDiscoveredImplementations(
    ProjectPackage &package, const std::vector<DiscoveredModule> &discovered,
    std::vector<Diagnostic> &diagnostics) {
  std::set<std::string> explicitNames;
  std::set<std::filesystem::path> explicitFiles;
  for (const auto &[name, files] : package.moduleImplementations) {
    explicitNames.insert(name);
    explicitFiles.insert(files.begin(), files.end());
  }
  for (const auto &unit : discovered) {
    const auto &name = unit.header.declaration.lexeme;
    if (unit.header.interface || explicitNames.contains(name) ||
        explicitFiles.contains(unit.path) ||
        std::any_of(
            package.modules.begin(), package.modules.end(),
            [&](const auto &entry) { return entry.second == unit.path; }))
      continue;
    if (std::none_of(package.modules.begin(), package.modules.end(),
                     [&](const auto &entry) { return entry.first == name; })) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-B1005", DiagnosticPhase::Driver, unit.header.span,
          "Discovered implementation has no interface for module '" + name +
              "'."));
      continue;
    }
    auto implementation =
        std::find_if(package.moduleImplementations.begin(),
                     package.moduleImplementations.end(),
                     [&](const auto &entry) { return entry.first == name; });
    if (implementation == package.moduleImplementations.end())
      package.moduleImplementations.emplace_back(name, std::vector{unit.path});
    else
      implementation->second.push_back(unit.path);
  }
}

} // namespace lang::driver::detail
