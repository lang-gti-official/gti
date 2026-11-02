#include "gti/driver/compilation.h"

#include <algorithm>
#include <utility>

namespace lang::driver {

AnalysisConfiguration::AnalysisConfiguration(
    TargetInfo target, ExecutionProfile executionProfile,
    RuntimeProfile runtimeProfile, ConfigurationFlags configurationFlags,
    ConfigurationFlags declaredConfigurationFlags)
    : targetInfo(std::move(target)), selectedRuntimeProfile(runtimeProfile),
      configuredFlags(std::move(configurationFlags)),
      declaredFlags(std::move(declaredConfigurationFlags)) {
  targetInfo.executionProfile = executionProfile;
  targetInfo.runtimeCapabilities = {};
  if (selectedRuntimeProfile == RuntimeProfile::Hosted &&
      targetInfo.supported()) {
    targetInfo.runtimeCapabilities.threads = true;
  }
  normalizeConfigurationFlags(configuredFlags);
  normalizeConfigurationFlags(declaredFlags);
}

const TargetInfo &AnalysisConfiguration::target() const { return targetInfo; }

RuntimeProfile AnalysisConfiguration::runtimeProfile() const {
  return selectedRuntimeProfile;
}

const ConfigurationFlags &AnalysisConfiguration::configurationFlags() const {
  return configuredFlags;
}

const ConfigurationFlags &
AnalysisConfiguration::declaredConfigurationFlags() const {
  return declaredFlags;
}

PackageWarningPolicy::PackageWarningPolicy(
    bool dependencyWarnings, std::string selectedPackageIdentity,
    std::vector<std::string> dependencyPackageIdentities)
    : emitDependencyWarnings(dependencyWarnings),
      selectedIdentity(std::move(selectedPackageIdentity)),
      dependencyIdentities(std::move(dependencyPackageIdentities)) {
  std::erase_if(dependencyIdentities, [&](const std::string &identity) {
    return identity.empty() || identity == selectedIdentity;
  });
  std::ranges::sort(dependencyIdentities);
  const auto duplicate = std::ranges::unique(dependencyIdentities).begin();
  dependencyIdentities.erase(duplicate, dependencyIdentities.end());
}

bool PackageWarningPolicy::dependencyWarnings() const {
  return emitDependencyWarnings;
}

const std::string &PackageWarningPolicy::selectedPackage() const {
  return selectedIdentity;
}

const std::vector<std::string> &
PackageWarningPolicy::dependencyPackages() const {
  return dependencyIdentities;
}

bool PackageWarningPolicy::suppresses(const Diagnostic &diagnostic,
                                      const SourceGraph &sources) const {
  if (emitDependencyWarnings || selectedIdentity.empty() ||
      diagnostic.severity != DiagnosticSeverity::Warning) {
    return false;
  }
  const SourceUnit *unit =
      sources.findUnit(sources.sourceUnitForPath(diagnostic.primary.source));
  return unit != nullptr && unit->role == SourceUnitRole::Application &&
         unit->packageIdentity && *unit->packageIdentity != selectedIdentity &&
         std::ranges::binary_search(dependencyIdentities,
                                    *unit->packageIdentity);
}

} // namespace lang::driver
