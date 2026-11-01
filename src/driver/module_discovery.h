#pragma once

#include "gti/driver/manifest.h"
#include "gti/module_name.h"

namespace lang::driver::detail {

struct ModuleDiscoverySettings {
  std::vector<std::filesystem::path> roots;
  std::vector<std::filesystem::path> exclusions;
  SourceSpan declaration;
  bool explicitRoots = false;
};

struct DiscoveredModule {
  ModuleHeader header;
  std::filesystem::path path;
  std::filesystem::path lookupPath;
};

std::vector<DiscoveredModule> discoverModules(
    const std::filesystem::path &packageRoot,
    const ModuleDiscoverySettings &settings, SourceManager &sources,
    std::vector<Diagnostic> &diagnostics);

void addDiscoveredInterfaces(ProjectPackage &package,
                            const std::vector<DiscoveredModule> &discovered,
                            std::vector<Diagnostic> &diagnostics);
void addDiscoveredImplementations(
    ProjectPackage &package, const std::vector<DiscoveredModule> &discovered,
    std::vector<Diagnostic> &diagnostics);

} // namespace lang::driver::detail
