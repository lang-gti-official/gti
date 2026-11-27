#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lang::driver {
class ProjectBuildPlan;
std::optional<std::string>
publishMacosBundle(const ProjectBuildPlan &plan,
                   std::filesystem::path &destination);
} // namespace lang::driver
