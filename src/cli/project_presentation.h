#pragma once

#include "gti/driver/project.h"

#include <iosfwd>
#include <string_view>

namespace lang::cli {

inline constexpr int projectMetadataSchemaVersion = 5;

[[nodiscard]] int optimizationNumber(OptimizationLevel optimization);
[[nodiscard]] std::string_view cppStandardName(CppStandard standard);

void writeProjectMetadata(std::ostream &stream,
                          const driver::ProjectMetadata &metadata);

} // namespace lang::cli
