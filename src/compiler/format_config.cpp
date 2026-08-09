#include "gti/format_config.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

struct Entry {
  std::string key;
  std::string value;
  std::size_t line = 0;
};

std::string_view trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

void issue(FormatConfigResult &result, std::size_t line, std::string message) {
  result.issues.push_back({.line = line, .message = std::move(message)});
}

std::optional<long long> parseInteger(std::string_view value) {
  long long parsed = 0;
  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto [cursor, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc{} || cursor != end) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<bool> parseBoolean(std::string_view value) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return std::nullopt;
}

FormatOptions llvmStyle() {
  FormatOptions options;
  options.referenceAlignment = ReferenceAlignment::Right;
  options.accessModifierOffset = -2;
  return options;
}

std::vector<Entry> parseEntries(std::string_view source,
                                FormatConfigResult &result) {
  std::vector<Entry> entries;
  std::unordered_map<std::string, std::size_t> firstLines;
  std::size_t lineNumber = 1;

  while (!source.empty()) {
    const std::size_t newline = source.find('\n');
    std::string_view line = source.substr(0, newline);
    source = newline == std::string_view::npos ? std::string_view{}
                                               : source.substr(newline + 1);

    if (const std::size_t comment = line.find('#');
        comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (line.empty() || line == "---" || line == "...") {
      ++lineNumber;
      continue;
    }

    const std::size_t separator = line.find(':');
    if (separator == std::string_view::npos) {
      issue(result, lineNumber, "expected a top-level 'Option: Value' entry");
      ++lineNumber;
      continue;
    }

    const std::string_view key = trim(line.substr(0, separator));
    const std::string_view value = trim(line.substr(separator + 1));
    if (key.empty() || value.empty()) {
      issue(result, lineNumber, "format option name and value cannot be empty");
      ++lineNumber;
      continue;
    }

    const auto [previous, inserted] =
        firstLines.emplace(std::string(key), lineNumber);
    if (!inserted) {
      issue(result, lineNumber,
            "duplicate option '" + std::string(key) + "' (first set on line " +
                std::to_string(previous->second) +
                "); the later value is used");
    }
    entries.push_back({.key = std::string(key),
                       .value = std::string(value),
                       .line = lineNumber});
    ++lineNumber;
  }
  return entries;
}

void applyBasedOnStyle(const std::vector<Entry> &entries,
                       FormatConfigResult &result) {
  const Entry *selected = nullptr;
  for (const Entry &entry : entries) {
    if (entry.key == "BasedOnStyle") {
      selected = &entry;
    }
  }
  if (selected == nullptr) {
    return;
  }
  if (selected->value == "GTI") {
    result.options = FormatOptions{};
  } else if (selected->value == "LLVM") {
    result.options = llvmStyle();
  } else {
    issue(result, selected->line, "BasedOnStyle must be GTI or LLVM");
  }
}

bool applyUnsigned(std::string_view key, std::string_view value,
                   std::size_t line, std::size_t minimum, std::size_t maximum,
                   std::size_t &destination, FormatConfigResult &result) {
  const std::optional<long long> parsed = parseInteger(value);
  if (!parsed || *parsed < static_cast<long long>(minimum) ||
      *parsed > static_cast<long long>(maximum)) {
    issue(result, line,
          std::string(key) + " must be an integer from " +
              std::to_string(minimum) + " through " + std::to_string(maximum));
    return false;
  }
  destination = static_cast<std::size_t>(*parsed);
  return true;
}

void applyEntry(const Entry &entry, FormatConfigResult &result) {
  FormatOptions &options = result.options;
  const std::string_view key = entry.key;
  const std::string_view value = entry.value;

  if (key == "BasedOnStyle") {
    return;
  }
  if (key == "IndentWidth") {
    applyUnsigned(key, value, entry.line, 1, 16, options.indentWidth, result);
    return;
  }
  if (key == "UseTab") {
    if (value == "Never") {
      options.insertSpaces = true;
    } else if (value == "Always" || value == "ForIndentation") {
      options.insertSpaces = false;
    } else {
      issue(result, entry.line,
            "UseTab must be Never, ForIndentation, or Always");
    }
    return;
  }
  if (key == "BreakBeforeBraces") {
    if (value == "Attach") {
      options.breakBeforeBraces = BraceBreakingStyle::Attach;
    } else if (value == "Allman") {
      options.breakBeforeBraces = BraceBreakingStyle::Allman;
    } else {
      issue(result, entry.line,
            "BreakBeforeBraces currently supports Attach or Allman");
    }
    return;
  }
  if (key == "SpaceBeforeParens") {
    if (value == "Never") {
      options.spaceBeforeParens = SpaceBeforeParensStyle::Never;
    } else if (value == "ControlStatements") {
      options.spaceBeforeParens = SpaceBeforeParensStyle::ControlStatements;
    } else if (value == "Always") {
      options.spaceBeforeParens = SpaceBeforeParensStyle::Always;
    } else {
      issue(result, entry.line,
            "SpaceBeforeParens must be Never, ControlStatements, or Always");
    }
    return;
  }
  if (key == "IndentCaseLabels" || key == "SpaceBeforeAssignmentOperators" ||
      key == "DisableFormat") {
    const std::optional<bool> parsed = parseBoolean(value);
    if (!parsed) {
      issue(result, entry.line, std::string(key) + " must be true or false");
      return;
    }
    if (key == "IndentCaseLabels") {
      options.indentCaseLabels = *parsed;
    } else if (key == "SpaceBeforeAssignmentOperators") {
      options.spaceBeforeAssignmentOperators = *parsed;
    } else {
      options.disableFormat = *parsed;
    }
    return;
  }
  if (key == "AccessModifierOffset") {
    const std::optional<long long> parsed = parseInteger(value);
    if (!parsed || *parsed < -64 || *parsed > 64) {
      issue(result, entry.line,
            "AccessModifierOffset must be an integer from -64 through 64");
    } else {
      options.accessModifierOffset = static_cast<int>(*parsed);
    }
    return;
  }
  if (key == "MaxEmptyLinesToKeep") {
    applyUnsigned(key, value, entry.line, 0, 16, options.maxEmptyLinesToKeep,
                  result);
    return;
  }
  if (key == "SpacesBeforeTrailingComments") {
    applyUnsigned(key, value, entry.line, 0, 16,
                  options.spacesBeforeTrailingComments, result);
    return;
  }
  if (key == "ReferenceAlignment") {
    if (value == "Left") {
      options.referenceAlignment = ReferenceAlignment::Left;
    } else if (value == "Right") {
      options.referenceAlignment = ReferenceAlignment::Right;
    } else if (value == "Middle") {
      options.referenceAlignment = ReferenceAlignment::Middle;
    } else {
      issue(result, entry.line,
            "ReferenceAlignment must be Left, Right, or Middle");
    }
    return;
  }

  issue(result, entry.line,
        "unsupported .gti-format option '" + entry.key + "'");
}

} // namespace

FormatConfigResult parseFormatConfig(std::string_view source,
                                     FormatOptions baseOptions) {
  FormatConfigResult result{.options = std::move(baseOptions)};
  const std::vector<Entry> entries = parseEntries(source, result);
  applyBasedOnStyle(entries, result);
  for (const Entry &entry : entries) {
    applyEntry(entry, result);
  }
  return result;
}

FormatConfigResult loadFormatConfig(const std::filesystem::path &documentPath,
                                    FormatOptions baseOptions) {
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(documentPath, error);
  std::filesystem::path directory =
      (error ? documentPath : absolute).parent_path();
  while (!directory.empty()) {
    const std::filesystem::path candidate = directory / ".gti-format";
    std::ifstream stream(candidate);
    if (stream) {
      std::string source((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
      FormatConfigResult result =
          parseFormatConfig(source, std::move(baseOptions));
      result.configPath = candidate;
      return result;
    }

    const std::filesystem::path parent = directory.parent_path();
    if (parent == directory) {
      break;
    }
    directory = parent;
  }
  return {.options = std::move(baseOptions)};
}

} // namespace lang
