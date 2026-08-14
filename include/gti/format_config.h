#pragma once

#include "gti/formatter.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct FormatConfigIssue {
  std::size_t line = 0;
  std::string message;
};

struct FormatConfigResult {
  FormatOptions options;
  std::optional<std::filesystem::path> configPath;
  std::vector<FormatConfigIssue> issues;
};

[[nodiscard]] std::string_view defaultFormatConfig();

FormatConfigResult parseFormatConfig(std::string_view source,
                                     FormatOptions baseOptions = {});

FormatConfigResult loadFormatConfig(const std::filesystem::path &documentPath,
                                    FormatOptions baseOptions = {});

} // namespace lang
