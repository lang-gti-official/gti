#include "gti/driver/manifest.h"
#include "gti/driver/registry.h"
#include "gti/driver/semver.h"

#include "gti/driver/dependencies.h"
#include "manifest_source.h"

#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

constexpr std::string_view manifestFilename = "gti.toml";

std::string pathString(const std::filesystem::path &path) {
  return path.lexically_normal().string();
}

Diagnostic buildDiagnostic(std::string code, SourceSpan span,
                           std::string message) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        std::move(span), std::move(message));
}

SourceSpan pointSpan(std::string_view source, std::size_t offset, int line) {
  return {std::string(source), offset, offset + 1, line};
}

std::size_t sourceOffset(std::string_view source,
                         const toml::source_position &position) {
  return detail::manifestSourceOffset(source, position.line, position.column);
}

SourceSpan sourceSpan(std::string_view sourceName, std::string_view source,
                      const toml::source_region &region) {
  const std::size_t start = sourceOffset(source, region.begin);
  const std::size_t rawEnd = sourceOffset(source, region.end);
  const std::size_t end = std::min(
      source.size(), std::max(rawEnd, std::min(start + 1, source.size())));
  return {std::string(sourceName), start, end,
          static_cast<int>(std::max<std::uint32_t>(region.begin.line, 1))};
}

SourceSpan sourceSpan(std::string_view sourceName, std::string_view source,
                      const toml::node &node) {
  return sourceSpan(sourceName, source, node.source());
}

SourceSpan sourceSpan(std::string_view sourceName, std::string_view source,
                      const toml::key &key) {
  return sourceSpan(sourceName, source, key.source());
}

std::size_t editDistance(std::string_view left, std::string_view right) {
  std::vector<std::size_t> previous(right.size() + 1);
  std::vector<std::size_t> current(right.size() + 1);
  for (std::size_t index = 0; index <= right.size(); ++index) {
    previous[index] = index;
  }

  for (std::size_t leftIndex = 0; leftIndex < left.size(); ++leftIndex) {
    current[0] = leftIndex + 1;
    for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
      const std::size_t substitution =
          previous[rightIndex] +
          (left[leftIndex] == right[rightIndex] ? 0U : 1U);
      current[rightIndex + 1] =
          std::min({current[rightIndex] + 1, previous[rightIndex + 1] + 1,
                    substitution});
    }
    previous.swap(current);
  }
  return previous.back();
}

std::optional<std::string_view>
nearestName(std::string_view name,
            const std::vector<std::string_view> &allowedNames) {
  std::optional<std::string_view> nearest;
  std::size_t nearestDistance = std::numeric_limits<std::size_t>::max();
  for (const std::string_view candidate : allowedNames) {
    const std::size_t distance = editDistance(name, candidate);
    if (distance < nearestDistance) {
      nearest = candidate;
      nearestDistance = distance;
    }
  }
  return nearestDistance <= 3 ? nearest : std::nullopt;
}

void validateFields(const toml::table &table,
                    const std::vector<std::string_view> &allowedNames,
                    std::string_view context, std::string_view sourceName,
                    std::string_view source,
                    std::vector<Diagnostic> &diagnostics) {
  for (const auto &[key, value] : table) {
    const std::string_view name = key.str();
    if (std::find(allowedNames.begin(), allowedNames.end(), name) !=
        allowedNames.end()) {
      continue;
    }

    Diagnostic diagnostic =
        buildDiagnostic("GTI-B1001", sourceSpan(sourceName, source, key),
                        "Unknown " + std::string(context) + " field '" +
                            std::string(name) + "'.");
    if (const std::optional<std::string_view> nearest =
            nearestName(name, allowedNames)) {
      diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) +
                                 "'?");
    }
    diagnostics.push_back(std::move(diagnostic));
  }
}

const toml::node *requiredField(const toml::table &table, std::string_view name,
                                std::string_view context,
                                std::string_view sourceName,
                                std::string_view source,
                                std::vector<Diagnostic> &diagnostics) {
  const toml::node *node = table.get(name);
  if (node != nullptr) {
    return node;
  }

  const toml::source_region &region = table.source();
  const std::size_t offset = sourceOffset(source, region.begin);
  diagnostics.push_back(buildDiagnostic(
      "GTI-B1002",
      pointSpan(
          sourceName, offset,
          static_cast<int>(std::max<std::uint32_t>(region.begin.line, 1))),
      "Missing required " + std::string(context) + " field '" +
          std::string(name) + "'."));
  return nullptr;
}

const toml::table *
requiredTable(const toml::table &table, std::string_view name,
              std::string_view context, std::string_view sourceName,
              std::string_view source, std::vector<Diagnostic> &diagnostics) {
  const toml::node *node =
      requiredField(table, name, context, sourceName, source, diagnostics);
  if (node == nullptr) {
    return nullptr;
  }
  if (const toml::table *nested = node->as_table()) {
    return nested;
  }
  diagnostics.push_back(buildDiagnostic(
      "GTI-B1004", sourceSpan(sourceName, source, *node),
      "Manifest field '" + std::string(name) + "' must be a table."));
  return nullptr;
}

std::optional<std::string>
requiredString(const toml::table &table, std::string_view name,
               std::string_view context, std::string_view sourceName,
               std::string_view source, std::vector<Diagnostic> &diagnostics) {
  const toml::node *node =
      requiredField(table, name, context, sourceName, source, diagnostics);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (const std::optional<std::string> value = node->value<std::string>()) {
    return value;
  }
  diagnostics.push_back(buildDiagnostic(
      "GTI-B1004", sourceSpan(sourceName, source, *node),
      "Manifest field '" + std::string(name) + "' must be a string."));
  return std::nullopt;
}

} // namespace

std::string portableProjectNameKey(std::string_view name) {
  std::string key(name);
  std::transform(key.begin(), key.end(), key.begin(), [](char character) {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
  });
  return key;
}

bool isPortableProjectName(std::string_view name) {
  const auto alphabetic = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  const auto digit = [](char character) {
    return character >= '0' && character <= '9';
  };
  if (name.empty() || !alphabetic(name.front())) {
    return false;
  }
  if (!std::all_of(name.begin() + 1, name.end(), [&](char character) {
        return alphabetic(character) || digit(character) || character == '_' ||
               character == '-';
      })) {
    return false;
  }
  const std::string key = portableProjectNameKey(name);
  if (key == "con" || key == "prn" || key == "aux" || key == "nul") {
    return false;
  }
  return !(key.size() == 4 &&
           (key.starts_with("com") || key.starts_with("lpt")) &&
           key.back() >= '1' && key.back() <= '9');
}

namespace {

bool isSemanticVersion(std::string_view version) {
  return parsePackageVersion(version).has_value();
}

bool pathIsWithin(const std::filesystem::path &root,
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

bool isAbsoluteOnSupportedHost(std::string_view spelling,
                               const std::filesystem::path &path) {
  const bool windowsDrive =
      spelling.size() >= 2 && (((spelling[0] >= 'A' && spelling[0] <= 'Z') ||
                                (spelling[0] >= 'a' && spelling[0] <= 'z')) &&
                               spelling[1] == ':');
  return path.is_absolute() || windowsDrive || spelling.starts_with('\\');
}

std::vector<std::string>
stringArray(const toml::table &table, std::string_view name,
            std::string_view context, std::string_view sourceName,
            std::string_view source, std::vector<Diagnostic> &diagnostics,
            std::vector<SourceSpan> *declarations = nullptr) {
  std::vector<std::string> values;
  const toml::node *node = table.get(name);
  if (node == nullptr) {
    return values;
  }
  const toml::array *array = node->as_array();
  if (array == nullptr) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        std::string(context) + " field '" + std::string(name) +
                            "' must be an array of strings."));
    return values;
  }

  values.reserve(array->size());
  for (const toml::node &element : *array) {
    const std::optional<std::string> value = element.value<std::string>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' must contain only strings."));
      continue;
    }
    if (value->empty()) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' cannot contain an empty string."));
      continue;
    }
    if (std::any_of(value->begin(), value->end(), [](unsigned char character) {
          return character < 0x20U || character == 0x7FU;
        })) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' cannot contain ASCII control characters."));
      continue;
    }
    values.push_back(*value);
    if (declarations != nullptr) {
      declarations->push_back(sourceSpan(sourceName, source, element));
    }
  }
  return values;
}

std::vector<std::filesystem::path>
containedPaths(const toml::table &table, std::string_view name,
               const std::filesystem::path &packageRoot,
               std::string_view context, std::string_view kind,
               std::string_view sourceName, std::string_view source,
               std::vector<SourceSpan> &declarations,
               std::vector<Diagnostic> &diagnostics) {
  std::vector<std::filesystem::path> paths;
  const toml::node *node = table.get(name);
  if (node == nullptr) {
    return paths;
  }
  const toml::array *array = node->as_array();
  if (array == nullptr) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        std::string(context) + " field '" + std::string(name) +
                            "' must be an array of package-relative " +
                            std::string(kind) + " strings."));
    return paths;
  }

  paths.reserve(array->size());
  declarations.reserve(array->size());
  for (const toml::node &element : *array) {
    const std::optional<std::string> value = element.value<std::string>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' must contain only " + std::string(kind) + " strings."));
      continue;
    }
    if (std::any_of(value->begin(), value->end(), [](unsigned char character) {
          return character < 0x20U || character == 0x7FU;
        })) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' cannot contain ASCII control characters."));
      continue;
    }

    const std::filesystem::path declared(*value);
    if (declared.empty() || isAbsoluteOnSupportedHost(*value, declared)) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", sourceSpan(sourceName, source, element),
          std::string(context) + " field '" + std::string(name) +
              "' must contain non-empty paths relative to gti.toml."));
      continue;
    }

    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(packageRoot / declared, error);
    if (error) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", sourceSpan(sourceName, source, element),
          "Failed to resolve native " + std::string(kind) + " '" +
              pathString(declared) + "': " + error.message() + "."));
      continue;
    }
    if (!pathIsWithin(packageRoot, resolved)) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1104", sourceSpan(sourceName, source, element),
          "Native " + std::string(kind) +
              " escapes the package directory: " + pathString(declared) + "."));
      continue;
    }

    paths.push_back(resolved);
    declarations.push_back(sourceSpan(sourceName, source, element));
  }
  return paths;
}

void rejectReservedArguments(std::vector<std::string> &arguments,
                             std::vector<SourceSpan> &declarations,
                             std::string_view field,
                             std::vector<Diagnostic> &diagnostics) {
  for (std::size_t index = 0; index < arguments.size();) {
    if (!isReservedNativeBuildArgument(arguments[index])) {
      ++index;
      continue;
    }
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1005", declarations[index],
        "Native field '" + std::string(field) +
            "' cannot override the resolved language standard, optimization, "
            "target/data layout, output, response-file inputs, or executable "
            "build mode."));
    arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
    declarations.erase(declarations.begin() +
                       static_cast<std::ptrdiff_t>(index));
  }
}

void rejectNonCSources(std::vector<std::filesystem::path> &sources,
                       std::vector<SourceSpan> &declarations,
                       std::vector<Diagnostic> &diagnostics) {
  for (std::size_t index = 0; index < sources.size();) {
    if (sources[index].extension() == ".c") {
      ++index;
      continue;
    }
    diagnostics.push_back(
        buildDiagnostic("GTI-B1005", declarations[index],
                        "Native field 'c-sources' accepts only files with the "
                        "'.c' extension."));
    sources.erase(sources.begin() + static_cast<std::ptrdiff_t>(index));
    declarations.erase(declarations.begin() +
                       static_cast<std::ptrdiff_t>(index));
  }
}

void rejectNonCppSources(std::vector<std::filesystem::path> &sources,
                         std::vector<SourceSpan> &declarations,
                         std::vector<Diagnostic> &diagnostics) {
  for (std::size_t index = 0; index < sources.size();) {
    const std::filesystem::path extension = sources[index].extension();
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
        extension == ".mm") {
      ++index;
      continue;
    }
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1005", declarations[index],
        "Native field 'cpp-sources' accepts only files with the '.cpp', "
        "'.cc', '.cxx', or Objective-C++ '.mm' extension."));
    sources.erase(sources.begin() + static_cast<std::ptrdiff_t>(index));
    declarations.erase(declarations.begin() +
                       static_cast<std::ptrdiff_t>(index));
  }
}

std::optional<CStandard>
optionalCStandard(const toml::table &table, std::string_view context,
                  std::string_view sourceName, std::string_view source,
                  std::vector<Diagnostic> &diagnostics) {
  const toml::node *node = table.get("c-standard");
  if (node == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::string> value = node->value<std::string>();
  if (!value) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", sourceSpan(sourceName, source, *node),
        std::string(context) + " field 'c-standard' must be a string."));
    return std::nullopt;
  }
  if (*value == "c11") {
    return CStandard::C11;
  }
  if (*value == "c17") {
    return CStandard::C17;
  }
  if (*value == "c23") {
    return CStandard::C23;
  }
  diagnostics.push_back(buildDiagnostic(
      "GTI-B1005", sourceSpan(sourceName, source, *node),
      std::string(context) +
          " field 'c-standard' must be 'c11', 'c17', or 'c23'."));
  return std::nullopt;
}

template <typename Settings>
void parseNativeInputs(Settings &settings, const toml::table &table,
                       const std::filesystem::path &packageRoot,
                       std::string_view context, std::string_view sourceName,
                       std::string_view source,
                       std::vector<Diagnostic> &diagnostics) {
  NativeInputs &inputs = settings.inputs;
  inputs.includeDirectories = containedPaths(
      table, "include-dirs", packageRoot, context, "directory", sourceName,
      source, settings.includeDirectoryDeclarations, diagnostics);
  inputs.cSources = containedPaths(table, "c-sources", packageRoot, context,
                                   "C source", sourceName, source,
                                   settings.cSourceDeclarations, diagnostics);
  rejectNonCSources(inputs.cSources, settings.cSourceDeclarations, diagnostics);
  inputs.cppSources = containedPaths(
      table, "cpp-sources", packageRoot, context, "C++ source", sourceName,
      source, settings.cppSourceDeclarations, diagnostics);
  rejectNonCppSources(inputs.cppSources, settings.cppSourceDeclarations,
                      diagnostics);
  inputs.libraryDirectories = containedPaths(
      table, "library-dirs", packageRoot, context, "directory", sourceName,
      source, settings.libraryDirectoryDeclarations, diagnostics);
  inputs.libraryFiles = containedPaths(
      table, "link-files", packageRoot, context, "file", sourceName, source,
      settings.libraryFileDeclarations, diagnostics);
  std::vector<SourceSpan> libraryDeclarations;
  inputs.libraries = stringArray(table, "libraries", context, sourceName,
                                 source, diagnostics, &libraryDeclarations);
  for (std::size_t index = 0; index < inputs.libraries.size();) {
    const std::string &library = inputs.libraries[index];
    const bool hasWhitespaceOrControl = std::any_of(
        library.begin(), library.end(), [](unsigned char character) {
          return character <= 0x20U || character == 0x7FU;
        });
    if (!library.starts_with('-') && library.find('/') == std::string::npos &&
        library.find('\\') == std::string::npos && !hasWhitespaceOrControl) {
      ++index;
      continue;
    }
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1005", libraryDeclarations[index],
        "Native libraries must be names without '-l', path separators, ASCII "
        "whitespace, or control characters."));
    inputs.libraries.erase(inputs.libraries.begin() +
                           static_cast<std::ptrdiff_t>(index));
    libraryDeclarations.erase(libraryDeclarations.begin() +
                              static_cast<std::ptrdiff_t>(index));
  }
  inputs.frameworks =
      stringArray(table, "frameworks", context, sourceName, source, diagnostics,
                  &settings.frameworkDeclarations);
  for (std::size_t index = 0; index < inputs.frameworks.size();) {
    const bool hasWhitespaceOrControl = std::any_of(
        inputs.frameworks[index].begin(), inputs.frameworks[index].end(),
        [](unsigned char character) {
          return character <= 0x20U || character == 0x7FU;
        });
    if (!inputs.frameworks[index].starts_with('-') &&
        inputs.frameworks[index].find('/') == std::string::npos &&
        inputs.frameworks[index].find('\\') == std::string::npos &&
        !hasWhitespaceOrControl) {
      ++index;
      continue;
    }
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1005", settings.frameworkDeclarations[index],
        "Native framework names must not begin with '-', use path separators, "
        "or contain ASCII whitespace or control characters."));
    inputs.frameworks.erase(inputs.frameworks.begin() +
                            static_cast<std::ptrdiff_t>(index));
    settings.frameworkDeclarations.erase(
        settings.frameworkDeclarations.begin() +
        static_cast<std::ptrdiff_t>(index));
  }
  std::vector<SourceSpan> compilerArgumentDeclarations;
  inputs.compilerArguments =
      stringArray(table, "compile-args", context, sourceName, source,
                  diagnostics, &compilerArgumentDeclarations);
  rejectReservedArguments(inputs.compilerArguments,
                          compilerArgumentDeclarations, "compile-args",
                          diagnostics);
  std::vector<SourceSpan> cCompilerArgumentDeclarations;
  inputs.cCompilerArguments =
      stringArray(table, "c-compile-args", context, sourceName, source,
                  diagnostics, &cCompilerArgumentDeclarations);
  rejectReservedArguments(inputs.cCompilerArguments,
                          cCompilerArgumentDeclarations, "c-compile-args",
                          diagnostics);
  std::vector<SourceSpan> linkerArgumentDeclarations;
  inputs.linkerArguments =
      stringArray(table, "link-args", context, sourceName, source, diagnostics,
                  &linkerArgumentDeclarations);
  rejectReservedArguments(inputs.linkerArguments, linkerArgumentDeclarations,
                          "link-args", diagnostics);
  std::vector<SourceSpan> rawArgumentDeclarations;
  inputs.trailingArguments =
      stringArray(table, "raw-args", context, sourceName, source, diagnostics,
                  &rawArgumentDeclarations);
  rejectReservedArguments(inputs.trailingArguments, rawArgumentDeclarations,
                          "raw-args", diagnostics);
}

std::optional<std::string>
optionalSelector(const toml::table &table, std::string_view name,
                 std::string_view context, std::string_view sourceName,
                 std::string_view source,
                 std::vector<Diagnostic> &diagnostics) {
  const toml::node *node = table.get(name);
  if (node == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::string> value = node->value<std::string>();
  if (!value) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        std::string(context) + " selector '" +
                            std::string(name) + "' must be a string."));
    return std::nullopt;
  }
  if (value->empty()) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1005", sourceSpan(sourceName, source, *node),
                        std::string(context) + " selector '" +
                            std::string(name) + "' cannot be empty."));
    return std::nullopt;
  }
  if (value->find('\0') != std::string::npos ||
      std::any_of(value->begin(), value->end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1005", sourceSpan(sourceName, source, *node),
        std::string(context) + " selector '" + std::string(name) +
            "' cannot contain ASCII control characters."));
    return std::nullopt;
  }
  return value;
}

ProjectNativeSettings parseNativeSettings(
    const toml::table &table, const std::filesystem::path &packageRoot,
    std::string_view context, std::string_view sourceName,
    std::string_view source, std::vector<Diagnostic> &diagnostics) {
  static const std::vector<std::string_view> nativeFields{
      "include-dirs", "c-sources",    "c-standard", "c-compile-args",
      "cpp-sources",  "library-dirs", "link-files", "libraries",
      "frameworks",   "compile-args", "link-args",  "raw-args",
      "platforms"};
  validateFields(table, nativeFields, context, sourceName, source, diagnostics);

  ProjectNativeSettings settings;
  settings.declaration = sourceSpan(sourceName, source, table);
  parseNativeInputs(settings, table, packageRoot, context, sourceName, source,
                    diagnostics);
  settings.inputs.cStandard =
      optionalCStandard(table, context, sourceName, source, diagnostics);
  const toml::node *platformsNode = table.get("platforms");
  if (platformsNode == nullptr) {
    return settings;
  }
  const toml::array *platforms = platformsNode->as_array();
  if (platforms == nullptr) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", sourceSpan(sourceName, source, *platformsNode),
        std::string(context) +
            " field 'platforms' must be an array of platform tables."));
    return settings;
  }

  const std::vector<std::string_view> platformFields{
      "os",         "vendor",         "arch",        "include-dirs",
      "c-sources",  "c-compile-args", "cpp-sources", "library-dirs",
      "link-files", "libraries",      "frameworks",  "compile-args",
      "link-args",  "raw-args"};
  settings.platforms.reserve(platforms->size());
  for (const toml::node &platformNode : *platforms) {
    const toml::table *platformTable = platformNode.as_table();
    if (platformTable == nullptr) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, platformNode),
          std::string(context) +
              " field 'platforms' must contain only tables."));
      continue;
    }
    validateFields(*platformTable, platformFields, "native platform",
                   sourceName, source, diagnostics);
    ProjectNativePlatform platform;
    platform.os = optionalSelector(*platformTable, "os", "Native platform",
                                   sourceName, source, diagnostics);
    platform.vendor =
        optionalSelector(*platformTable, "vendor", "Native platform",
                         sourceName, source, diagnostics);
    platform.arch = optionalSelector(*platformTable, "arch", "Native platform",
                                     sourceName, source, diagnostics);
    platform.declaration = sourceSpan(sourceName, source, platformNode);
    const bool hasOs = platformTable->get("os") != nullptr;
    const bool hasVendor = platformTable->get("vendor") != nullptr;
    const bool hasArch = platformTable->get("arch") != nullptr;
    const bool hasSelector = hasOs || hasVendor || hasArch;
    const bool validSelectors = (!hasOs || platform.os.has_value()) &&
                                (!hasVendor || platform.vendor.has_value()) &&
                                (!hasArch || platform.arch.has_value());
    if (!hasSelector) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", platform.declaration,
          "Native platform entries must select at least one of 'os', 'vendor', "
          "or 'arch'."));
    }
    parseNativeInputs(platform, *platformTable, packageRoot, "native platform",
                      sourceName, source, diagnostics);
    if (!hasSelector || !validSelectors) {
      continue;
    }
    const auto duplicate =
        std::find_if(settings.platforms.begin(), settings.platforms.end(),
                     [&platform](const ProjectNativePlatform &existing) {
                       return existing.os == platform.os &&
                              existing.vendor == platform.vendor &&
                              existing.arch == platform.arch;
                     });
    if (duplicate != settings.platforms.end()) {
      Diagnostic diagnostic = buildDiagnostic(
          "GTI-B1005", platform.declaration,
          "Duplicate native platform selector in the same native table.");
      diagnostic.related.push_back(
          {duplicate->declaration, "The selector was first declared here."});
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }
    settings.platforms.push_back(std::move(platform));
  }
  return settings;
}

ProjectNativeSettings parseOptionalNative(
    const toml::table &owner, const std::filesystem::path &packageRoot,
    std::string_view context, std::string_view sourceName,
    std::string_view source, std::vector<Diagnostic> &diagnostics) {
  const toml::node *nativeNode = owner.get("native");
  if (nativeNode == nullptr) {
    return {};
  }
  const toml::table *nativeTable = nativeNode->as_table();
  if (nativeTable == nullptr) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", sourceSpan(sourceName, source, *nativeNode),
        std::string(context) + " field 'native' must be a table."));
    return {};
  }
  return parseNativeSettings(*nativeTable, packageRoot,
                             std::string(context) + " native", sourceName,
                             source, diagnostics);
}

ProjectProfile defaultProfile(std::string name, SourceSpan declaration = {}) {
  const bool release = name == "release";
  return {
      .name = std::move(name),
      .optimization = release ? OptimizationLevel::O3 : OptimizationLevel::O0,
      .cppStandard = CppStandard::Cpp23,
      .keepCpp = false,
      .declaration = std::move(declaration),
      .defines = release ? ConfigurationFlags{"NDEBUG"} : ConfigurationFlags{}};
}

OptimizationLevel optimizationLevel(std::int64_t value) {
  switch (value) {
  case 1:
    return OptimizationLevel::O1;
  case 2:
    return OptimizationLevel::O2;
  case 3:
    return OptimizationLevel::O3;
  default:
    return OptimizationLevel::O0;
  }
}

ConfigurationFlags
parseConfigurationDefines(const toml::table &table, std::string_view owner,
                          std::string_view sourceName, std::string_view source,
                          std::vector<Diagnostic> &diagnostics,
                          std::vector<SourceSpan> &declarations) {
  const auto flags = stringArray(table, "defines", owner, sourceName, source,
                                 diagnostics, &declarations);
  std::vector<std::pair<std::string, SourceSpan>> valid;
  for (std::size_t index = 0; index < flags.size(); ++index) {
    if (!isConfigurationFlagName(flags[index])) {
      diagnostics.push_back(
          buildDiagnostic("GTI-B1005", declarations[index],
                          std::string(owner) + " define '" + flags[index] +
                              "' is not an ordinary GTI identifier."));
      continue;
    }
    valid.emplace_back(flags[index], declarations[index]);
  }
  std::sort(valid.begin(), valid.end(),
            [](const auto &left, const auto &right) {
              if (left.first != right.first) {
                return left.first < right.first;
              }
              return left.second.start < right.second.start;
            });
  valid.erase(std::unique(valid.begin(), valid.end(),
                          [](const auto &left, const auto &right) {
                            return left.first == right.first;
                          }),
              valid.end());
  ConfigurationFlags result;
  declarations.clear();
  for (auto &[name, declaration] : valid) {
    result.push_back(std::move(name));
    declarations.push_back(std::move(declaration));
  }
  return result;
}

void applyProfileFields(ProjectProfile &profile, const toml::table &table,
                        std::string_view sourceName, std::string_view source,
                        std::vector<Diagnostic> &diagnostics) {
  validateFields(table,
                 {"optimization", "cpp-standard", "execution-profile",
                  "raw-pointer-audit", "raw-pointer-audit-library-findings",
                  "disabled-warnings", "dependency-warnings", "keep-cpp",
                  "debug-info", "native", "defines"},
                 "profile", sourceName, source, diagnostics);
  if (table.contains("defines"))
    profile.defines =
        parseConfigurationDefines(table, "Profile", sourceName, source,
                                  diagnostics, profile.defineDeclarations);

  if (const toml::node *optimization = table.get("optimization")) {
    const std::optional<std::int64_t> value =
        optimization->value<std::int64_t>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *optimization),
          "Profile field 'optimization' must be an integer."));
    } else if (*value < 0 || *value > 3) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, *optimization),
          "Profile optimization must be 0, 1, 2, or 3."));
    } else {
      profile.optimization = optimizationLevel(*value);
    }
  }

  if (const toml::node *standard = table.get("cpp-standard")) {
    const std::optional<std::string> value = standard->value<std::string>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *standard),
          "Profile field 'cpp-standard' must be a string."));
    } else if (*value == "c++20") {
      profile.cppStandard = CppStandard::Cpp20;
    } else if (*value == "c++23") {
      profile.cppStandard = CppStandard::Cpp23;
    } else {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, *standard),
          "Profile C++ standard must be 'c++20' or 'c++23'."));
    }
  }

  if (const toml::node *execution = table.get("execution-profile")) {
    const std::optional<std::string> value = execution->value<std::string>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *execution),
          "Profile field 'execution-profile' must be a string."));
    } else if (const std::optional<ExecutionProfile> parsed =
                   parseExecutionProfile(*value)) {
      profile.executionProfile = *parsed;
    } else {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, *execution),
          "Profile execution profile must be 'single-threaded' or "
          "'concurrent'."));
    }
  }

  if (const toml::node *audit = table.get("raw-pointer-audit")) {
    const auto value = audit->value<std::string>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *audit),
          "Profile field 'raw-pointer-audit' must be a string."));
    } else if (const auto parsed = parseRawPointerAuditMode(*value)) {
      profile.rawPointerAudit = *parsed;
    } else {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", sourceSpan(sourceName, source, *audit),
          "Profile raw pointer audit must be 'off', 'warn', or 'strict'."));
    }
  }

  if (const toml::node *libraryFindings =
          table.get("raw-pointer-audit-library-findings")) {
    if (const auto value = libraryFindings->value<bool>()) {
      profile.rawPointerAuditLibraryFindings = *value;
    } else {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *libraryFindings),
          "Profile field 'raw-pointer-audit-library-findings' must be a "
          "boolean."));
    }
  }

  if (table.get("disabled-warnings") != nullptr) {
    std::vector<SourceSpan> declarations;
    std::vector<std::string> disabledWarnings =
        stringArray(table, "disabled-warnings", "Profile", sourceName, source,
                    diagnostics, &declarations);
    std::vector<std::string> accepted;
    accepted.reserve(disabledWarnings.size());
    for (std::size_t index = 0; index < disabledWarnings.size(); ++index) {
      if (!isKnownWarningGroup(disabledWarnings[index])) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1005", declarations[index],
            "Profile disabled-warnings contains unknown warning group '" +
                disabledWarnings[index] + "'."));
        continue;
      }
      accepted.push_back(std::move(disabledWarnings[index]));
    }
    profile.warningPolicy = WarningPolicy(std::move(accepted));
  }

  if (const toml::node *dependencyWarnings = table.get("dependency-warnings")) {
    const std::optional<bool> value = dependencyWarnings->value_exact<bool>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *dependencyWarnings),
          "Profile field 'dependency-warnings' must be a Boolean."));
    } else {
      profile.dependencyWarnings = *value;
    }
  }

  if (const toml::node *debugInfo = table.get("debug-info")) {
    const std::optional<bool> value = debugInfo->value<bool>();
    if (!value) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *debugInfo),
          "Profile field 'debug-info' must be a Boolean."));
    } else {
      profile.debugInfo = *value;
    }
  }

  if (const toml::node *keepCpp = table.get("keep-cpp")) {
    const std::optional<bool> value = keepCpp->value<bool>();
    if (!value) {
      diagnostics.push_back(
          buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *keepCpp),
                          "Profile field 'keep-cpp' must be a Boolean."));
    } else {
      profile.keepCpp = *value;
    }
  }
}

bool isImportAlias(std::string_view alias) {
  const auto alphabetic = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_';
  };
  const auto digit = [](char character) {
    return character >= '0' && character <= '9';
  };
  return !alias.empty() && alphabetic(alias.front()) &&
         std::all_of(alias.begin() + 1, alias.end(), [&](char character) {
           return alphabetic(character) || digit(character);
         });
}

std::optional<std::filesystem::path> resolvePackageDirectory(
    std::string_view spelling, const std::filesystem::path &base,
    const SourceSpan &declaration, bool contained, std::string_view kind,
    std::vector<Diagnostic> &diagnostics) {
  const std::filesystem::path declared(spelling);
  if (declared.empty() || isAbsoluteOnSupportedHost(spelling, declared)) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1103", declaration,
        std::string(kind) +
            " must be a non-empty path relative to its gti.toml."));
    return std::nullopt;
  }

  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::weakly_canonical(base / declared, error);
  if (error || !std::filesystem::is_directory(resolved, error)) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1103", declaration,
        std::string(kind) + " does not name an existing directory: " +
            pathString(declared) + "."));
    return std::nullopt;
  }
  if (contained && !pathIsWithin(base, resolved)) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1104", declaration,
        std::string(kind) +
            " escapes the package directory: " + pathString(declared) + "."));
    return std::nullopt;
  }
  return resolved;
}

std::vector<ProjectDependency>
parseDependencies(const toml::table &document,
                  const std::filesystem::path &packageRoot,
                  std::string_view sourceName, std::string_view source,
                  std::vector<Diagnostic> &diagnostics) {
  std::vector<ProjectDependency> dependencies;
  const toml::node *node = document.get("dependencies");
  if (node == nullptr) {
    return dependencies;
  }
  const toml::table *table = node->as_table();
  if (table == nullptr) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        "Manifest field 'dependencies' must be a table."));
    return dependencies;
  }

  for (const auto &[key, dependencyNode] : *table) {
    const std::string alias(key.str());
    const SourceSpan aliasSpan = sourceSpan(sourceName, source, key);
    if (!isImportAlias(alias) || alias == "std") {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", aliasSpan,
          "Dependency aliases must match [A-Za-z_][A-Za-z0-9_]* and cannot "
          "be 'std'."));
    }
    const toml::table *dependencyTable = dependencyNode.as_table();
    const auto range = dependencyNode.value<std::string>();
    const bool hasRange = dependencyTable && dependencyTable->get("version");
    if (range || hasRange) {
      const auto requirement =
          range ? range : dependencyTable->get("version")->value<std::string>();
      const SourceSpan span = sourceSpan(sourceName, source, dependencyNode);
      std::string registry = "official", package = alias;
      bool valid = requirement &&
                   PackageVersionRequirement::parse(*requirement).has_value();
      if (dependencyTable) {
        validateFields(*dependencyTable, {"version", "registry", "package"},
                       "registry dependency", sourceName, source, diagnostics);
        for (auto field : {"registry", "package"}) {
          if (const auto *value = dependencyTable->get(field)) {
            auto text = value->value<std::string>();
            if (!text || !validRegistryName(*text)) {
              valid = false;
            } else if (std::string_view(field) == "registry") {
              registry = *text;
            } else {
              package = *text;
            }
          }
        }
      }
      if (!valid || !validRegistryName(package)) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1710", span,
            "Registry dependency '" + alias +
                "' needs a valid package/registry name and a caret, tilde or "
                "exact SemVer requirement (for example '^0.2.1')."));
        continue;
      }
      dependencies.push_back(
          {.alias = alias,
           .registry =
               ProjectRegistryDependencySource{.registry = registry,
                                               .package = package,
                                               .requirement = *requirement},
           .declaration = aliasSpan,
           .pathDeclaration = span});
      continue;
    }
    if (dependencyTable == nullptr) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, dependencyNode),
          "Dependency '" + alias +
              "' must be an inline or ordinary table containing 'path', or "
              "'git' with 'rev'."));
      continue;
    }
    validateFields(*dependencyTable, {"path", "git", "rev"}, "dependency",
                   sourceName, source, diagnostics);
    const bool hasPath = dependencyTable->get("path") != nullptr;
    const bool hasGit = dependencyTable->get("git") != nullptr;
    const bool hasRevision = dependencyTable->get("rev") != nullptr;
    if (hasPath && (hasGit || hasRevision)) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1006", aliasSpan,
          "Dependency '" + alias +
              "' declares both 'path' and a git source; declare exactly "
              "one."));
      continue;
    }
    if (!hasPath && !hasGit) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1006", aliasSpan,
          "Dependency '" + alias +
              "' must declare 'path', or 'git' with a pinned 'rev'."));
      continue;
    }

    if (hasGit) {
      const std::optional<std::string> url =
          requiredString(*dependencyTable, "git", "dependency", sourceName,
                         source, diagnostics);
      if (!url) {
        continue;
      }
      const SourceSpan urlSpan =
          sourceSpan(sourceName, source, *dependencyTable->get("git"));
      if (std::any_of(url->begin(), url->end(), [](const char character) {
            return static_cast<unsigned char>(character) <= 0x20U;
          })) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1006", urlSpan,
            "Dependency '" + alias +
                "' has a git URL containing whitespace or control "
                "characters."));
        continue;
      }
      if (!hasRevision) {
        Diagnostic diagnostic = buildDiagnostic(
            "GTI-B1006", urlSpan,
            "Git dependency '" + alias +
                "' must pin 'rev' to a full 40-character commit.");
        diagnostic.hints.push_back(
            "Branches and tags are mutable; record the exact commit so the "
            "build plan cannot change under an unchanged manifest.");
        diagnostics.push_back(std::move(diagnostic));
        continue;
      }
      const std::optional<std::string> revision =
          requiredString(*dependencyTable, "rev", "dependency", sourceName,
                         source, diagnostics);
      if (!revision) {
        continue;
      }
      const SourceSpan revisionSpan =
          sourceSpan(sourceName, source, *dependencyTable->get("rev"));
      std::string normalized = *revision;
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                     });
      if (!isFullGitRevision(normalized)) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1006", revisionSpan,
            "Git dependency '" + alias +
                "' must pin 'rev' to a full 40-character hexadecimal "
                "commit."));
        continue;
      }
      dependencies.push_back(
          {.alias = alias,
           .packageRoot = {},
           .git =
               ProjectGitDependencySource{.url = *url,
                                          .revision = std::move(normalized),
                                          .urlDeclaration = urlSpan,
                                          .revisionDeclaration = revisionSpan},
           .declaration = aliasSpan,
           .pathDeclaration = urlSpan});
      continue;
    }

    const std::optional<std::string> path =
        requiredString(*dependencyTable, "path", "dependency", sourceName,
                       source, diagnostics);
    if (!path) {
      continue;
    }
    const toml::node &pathNode = *dependencyTable->get("path");
    const SourceSpan pathSpan = sourceSpan(sourceName, source, pathNode);
    const std::optional<std::filesystem::path> resolved =
        resolvePackageDirectory(*path, packageRoot, pathSpan, false,
                                "Dependency path", diagnostics);
    if (!resolved) {
      continue;
    }
    std::error_code error;
    const std::filesystem::path manifest = *resolved / manifestFilename;
    if (!std::filesystem::is_regular_file(manifest, error) || error) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", pathSpan,
          "Dependency path does not contain a regular gti.toml: " +
              pathString(std::filesystem::path(*path)) + "."));
      continue;
    }
    dependencies.push_back({.alias = alias,
                            .packageRoot = *resolved,
                            .declaration = aliasSpan,
                            .pathDeclaration = pathSpan});
  }
  std::sort(dependencies.begin(), dependencies.end(),
            [](const ProjectDependency &left, const ProjectDependency &right) {
              return left.alias < right.alias;
            });
  return dependencies;
}

// `[bindgen.<alias>]` import tables. Aliases share the per-package include
// namespace with dependency aliases, so a collision is rejected here rather
// than surfacing later as an ambiguous package include.
[[nodiscard]] bool bindgenMacroName(std::string_view name) {
  if (name.empty() ||
      (std::isalpha(static_cast<unsigned char>(name.front())) == 0 &&
       name.front() != '_')) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '_';
  });
}

std::vector<ProjectBindgenImport>
parseBindgenImports(const toml::table &document,
                    const std::filesystem::path &packageRoot,
                    const std::vector<ProjectDependency> &dependencies,
                    std::string_view sourceName, std::string_view source,
                    std::vector<Diagnostic> &diagnostics) {
  std::vector<ProjectBindgenImport> imports;
  const toml::node *node = document.get("bindgen");
  if (node == nullptr) {
    return imports;
  }
  const toml::table *table = node->as_table();
  if (table == nullptr) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", sourceSpan(sourceName, source, *node),
        "Manifest field 'bindgen' must be a table of [bindgen.<alias>] "
        "import tables."));
    return imports;
  }

  for (const auto &[key, importNode] : *table) {
    ProjectBindgenImport import;
    import.alias = std::string(key.str());
    import.declaration = sourceSpan(sourceName, source, key);
    if (!isImportAlias(import.alias) || import.alias == "std") {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1800", import.declaration,
          "Bindgen aliases must match [A-Za-z_][A-Za-z0-9_]* and cannot be "
          "'std'."));
      continue;
    }
    const auto dependencyCollision =
        std::find_if(dependencies.begin(), dependencies.end(),
                     [&import](const ProjectDependency &dependency) {
                       return dependency.alias == import.alias;
                     });
    if (dependencyCollision != dependencies.end()) {
      Diagnostic diagnostic = buildDiagnostic(
          "GTI-B1801", import.declaration,
          "Bindgen import '" + import.alias +
              "' collides with a dependency alias; both resolve package "
              "includes under one alias namespace.");
      diagnostic.related.push_back({dependencyCollision->declaration,
                                    "The conflicting dependency is declared "
                                    "here."});
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }
    const toml::table *importTable = importNode.as_table();
    if (importTable == nullptr) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, importNode),
          "Bindgen import '" + import.alias +
              "' must be a table declaring 'header'."));
      continue;
    }
    validateFields(*importTable,
                   {"header", "namespace", "include-dirs", "defines",
                    "undefines", "allow", "deny", "rename", "array-returns"},
                   "bindgen import", sourceName, source, diagnostics);

    const std::optional<std::string> header =
        requiredString(*importTable, "header", "bindgen import", sourceName,
                       source, diagnostics);
    if (!header) {
      continue;
    }
    import.headerDeclaration =
        sourceSpan(sourceName, source, *importTable->get("header"));
    const std::filesystem::path declared(*header);
    if (declared.empty() || isAbsoluteOnSupportedHost(*header, declared)) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", import.headerDeclaration,
          "Bindgen header must be a non-empty path relative to gti.toml."));
      continue;
    }
    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(packageRoot / declared, error);
    if (error) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", import.headerDeclaration,
          "Failed to resolve bindgen header '" + pathString(declared) +
              "': " + error.message() + "."));
      continue;
    }
    if (!pathIsWithin(packageRoot, resolved)) {
      diagnostics.push_back(
          buildDiagnostic("GTI-B1104", import.headerDeclaration,
                          "Bindgen header escapes the package directory: " +
                              pathString(declared) + "."));
      continue;
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
      diagnostics.push_back(
          buildDiagnostic("GTI-B1802", import.headerDeclaration,
                          "Bindgen header does not name an existing file: " +
                              pathString(declared) + "."));
      continue;
    }
    import.header = resolved;
    import.declaredHeader = *header;

    if (const toml::node *namespaceNode = importTable->get("namespace")) {
      const std::optional<std::string> value =
          namespaceNode->value<std::string>();
      if (!value || !isImportAlias(*value)) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1803", sourceSpan(sourceName, source, *namespaceNode),
            "Bindgen namespace must be an ordinary GTI identifier."));
        continue;
      }
      import.namespaceName = *value;
    }

    std::vector<SourceSpan> includeDeclarations;
    import.includeDirectories = containedPaths(
        *importTable, "include-dirs", packageRoot, "Bindgen import",
        "directory", sourceName, source, includeDeclarations, diagnostics);

    bool macrosValid = true;
    std::vector<SourceSpan> macroDeclarations;
    const std::vector<std::string> defines =
        stringArray(*importTable, "defines", "Bindgen import", sourceName,
                    source, diagnostics, &macroDeclarations);
    for (std::size_t index = 0; index < defines.size(); ++index) {
      const std::string &value = defines[index];
      const std::size_t equals = value.find('=');
      if (!bindgenMacroName(value.substr(0, std::min(equals, value.size())))) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1804", macroDeclarations[index],
            "Bindgen defines must be '<name>' or '<name>=<value>' with an "
            "ordinary macro name."));
        macrosValid = false;
        continue;
      }
      import.macroArguments.push_back("-D" + value);
    }
    macroDeclarations.clear();
    const std::vector<std::string> undefines =
        stringArray(*importTable, "undefines", "Bindgen import", sourceName,
                    source, diagnostics, &macroDeclarations);
    for (std::size_t index = 0; index < undefines.size(); ++index) {
      if (!bindgenMacroName(undefines[index])) {
        diagnostics.push_back(
            buildDiagnostic("GTI-B1804", macroDeclarations[index],
                            "Bindgen undefines must name an ordinary macro."));
        macrosValid = false;
        continue;
      }
      import.macroArguments.push_back("-U" + undefines[index]);
    }
    if (!macrosValid) {
      continue;
    }

    // Opt-in policy (C-07). Patterns are exact names or one trailing '*';
    // renames and array returns are ordinary tables so a typo is a manifest
    // diagnostic rather than a silent no-op.
    bool policyValid = true;
    std::vector<SourceSpan> patternDeclarations;
    const auto readPatterns = [&](std::string_view field,
                                  std::vector<std::string> &into) {
      patternDeclarations.clear();
      const std::vector<std::string> values =
          stringArray(*importTable, field, "Bindgen import", sourceName, source,
                      diagnostics, &patternDeclarations);
      for (std::size_t index = 0; index < values.size(); ++index) {
        std::string_view name(values[index]);
        if (!name.empty() && name.back() == '*') {
          name.remove_suffix(1);
        }
        if (name.empty() || !bindgenMacroName(name)) {
          diagnostics.push_back(buildDiagnostic(
              "GTI-B1808", patternDeclarations[index],
              "Bindgen '" + std::string(field) +
                  "' entries must be an identifier optionally followed by "
                  "'*'."));
          policyValid = false;
          continue;
        }
        into.push_back(values[index]);
      }
    };
    readPatterns("allow", import.allowPatterns);
    readPatterns("deny", import.denyPatterns);

    if (const toml::node *renameNode = importTable->get("rename")) {
      const toml::table *renameTable = renameNode->as_table();
      if (renameTable == nullptr) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1004", sourceSpan(sourceName, source, *renameNode),
            "Bindgen import field 'rename' must be a table of "
            "<c-name> = <gti-name> entries."));
        policyValid = false;
      } else {
        for (const auto &[from, toNode] : *renameTable) {
          const SourceSpan entrySpan = sourceSpan(sourceName, source, from);
          const std::optional<std::string> to = toNode.value<std::string>();
          if (!bindgenMacroName(from.str()) || !to || !bindgenMacroName(*to)) {
            diagnostics.push_back(buildDiagnostic(
                "GTI-B1808", entrySpan,
                "Bindgen 'rename' entries must map an identifier to an "
                "identifier."));
            policyValid = false;
            continue;
          }
          import.renames.emplace_back(std::string(from.str()), *to);
        }
      }
    }

    if (const toml::node *arrayNode = importTable->get("array-returns")) {
      const toml::array *arrayEntries = arrayNode->as_array();
      if (arrayEntries == nullptr) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1004", sourceSpan(sourceName, source, *arrayNode),
            "Bindgen import field 'array-returns' must be an array of "
            "{ function, count } tables."));
        policyValid = false;
      } else {
        for (const toml::node &element : *arrayEntries) {
          const SourceSpan entrySpan = sourceSpan(sourceName, source, element);
          const toml::table *entry = element.as_table();
          if (entry == nullptr) {
            diagnostics.push_back(buildDiagnostic(
                "GTI-B1004", entrySpan,
                "Each 'array-returns' entry must be a table declaring "
                "'function' and 'count'."));
            policyValid = false;
            continue;
          }
          validateFields(*entry, {"function", "count"}, "bindgen array return",
                         sourceName, source, diagnostics);
          const toml::node *functionNode = entry->get("function");
          const toml::node *countNode = entry->get("count");
          const std::optional<std::string> function =
              functionNode == nullptr ? std::nullopt
                                      : functionNode->value<std::string>();
          const std::optional<std::string> count =
              countNode == nullptr ? std::nullopt
                                   : countNode->value<std::string>();
          if (!function || !count || !bindgenMacroName(*function) ||
              !bindgenMacroName(*count)) {
            diagnostics.push_back(buildDiagnostic(
                "GTI-B1808", entrySpan,
                "Each 'array-returns' entry must name a 'function' and its "
                "'count' parameter as identifiers."));
            policyValid = false;
            continue;
          }
          import.arrayReturns.emplace_back(*function, *count);
        }
      }
    }
    if (!policyValid) {
      continue;
    }
    // Deterministic identity: the manifest's own table ordering must not
    // reach the build plan's model identity.
    std::sort(import.allowPatterns.begin(), import.allowPatterns.end());
    std::sort(import.denyPatterns.begin(), import.denyPatterns.end());
    std::sort(import.renames.begin(), import.renames.end());
    std::sort(import.arrayReturns.begin(), import.arrayReturns.end());
    imports.push_back(std::move(import));
  }
  std::sort(
      imports.begin(), imports.end(),
      [](const ProjectBindgenImport &left, const ProjectBindgenImport &right) {
        return left.alias < right.alias;
      });
  return imports;
}

// A declared native compiler must be an executable name or path, never an
// option: the value is passed to the process layer exactly as spelled, in
// the same position `--cxx` would occupy.
bool isExecutableSpelling(std::string_view value) {
  return !value.empty() && value.front() != '-';
}

std::optional<std::string>
toolchainExecutable(const toml::table &table, std::string_view field,
                    std::string_view sourceName, std::string_view source,
                    SourceSpan &declaration,
                    std::vector<Diagnostic> &diagnostics) {
  const toml::node *node = table.get(field);
  if (node == nullptr) {
    return std::nullopt;
  }
  declaration = sourceSpan(sourceName, source, *node);
  const std::optional<std::string> value = node->value<std::string>();
  if (!value) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", declaration,
        "Toolchain field '" + std::string(field) + "' must be a string."));
    return std::nullopt;
  }
  if (!isExecutableSpelling(*value)) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1902", declaration,
        "Toolchain field '" + std::string(field) +
            "' must name a native compiler executable, not an option."));
    return std::nullopt;
  }
  return value;
}

// `[toolchains.<triple>]` declares the native toolchain a cross build uses.
// The table key is normalized through the compiler's own triple vocabulary,
// so the manifest cannot introduce a second spelling table, and two keys
// selecting one normalized target are rejected rather than silently ordered.
std::vector<ProjectToolchain>
parseToolchains(const toml::table &document,
                const std::filesystem::path &packageRoot,
                std::string_view sourceName, std::string_view source,
                std::vector<Diagnostic> &diagnostics) {
  std::vector<ProjectToolchain> toolchains;
  const toml::node *node = document.get("toolchains");
  if (node == nullptr) {
    return toolchains;
  }
  const toml::table *table = node->as_table();
  if (table == nullptr) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        "Manifest field 'toolchains' must be a table of "
                        "[toolchains.<triple>] tables."));
    return toolchains;
  }

  for (const auto &[key, toolchainNode] : *table) {
    ProjectToolchain toolchain;
    toolchain.declaredTriple = std::string(key.str());
    toolchain.declaration = sourceSpan(sourceName, source, key);
    const TargetTripleParseResult parsed =
        parseTargetTripleResult(toolchain.declaredTriple);
    if (!parsed.succeeded()) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1900", toolchain.declaration,
          "Toolchain table key '" + toolchain.declaredTriple +
              "' is not a supported target triple: " +
              std::string(targetTripleErrorMessage(parsed.error)) + "."));
      continue;
    }
    toolchain.target = *parsed.target;
    const auto collision = std::find_if(
        toolchains.begin(), toolchains.end(),
        [&toolchain](const ProjectToolchain &candidate) {
          return candidate.target.os == toolchain.target.os &&
                 candidate.target.vendor == toolchain.target.vendor &&
                 candidate.target.arch == toolchain.target.arch;
        });
    if (collision != toolchains.end()) {
      Diagnostic diagnostic = buildDiagnostic(
          "GTI-B1901", toolchain.declaration,
          "Toolchain '" + toolchain.declaredTriple +
              "' selects the same normalized target as another declared "
              "toolchain.");
      diagnostic.related.push_back({collision->declaration,
                                    "The conflicting toolchain is declared "
                                    "here."});
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const toml::table *toolchainTable = toolchainNode.as_table();
    if (toolchainTable == nullptr) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, toolchainNode),
          "Toolchain '" + toolchain.declaredTriple +
              "' must be a table declaring 'cxx'."));
      continue;
    }
    validateFields(*toolchainTable, {"cxx", "cc", "sysroot", "native-target"},
                   "toolchain", sourceName, source, diagnostics);

    const toml::node *cxxNode = toolchainTable->get("cxx");
    if (cxxNode == nullptr) {
      Diagnostic diagnostic = buildDiagnostic(
          "GTI-B1902", toolchain.declaration,
          "Toolchain '" + toolchain.declaredTriple +
              "' must declare 'cxx', the native compiler and linker for "
              "that target.");
      diagnostic.hints.push_back(
          "Without it the build would fall back to host compiler discovery "
          "and link host objects into a cross build.");
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }
    const std::optional<std::string> cxx =
        toolchainExecutable(*toolchainTable, "cxx", sourceName, source,
                            toolchain.cxxDeclaration, diagnostics);
    if (!cxx) {
      continue;
    }
    toolchain.cxx = *cxx;
    if (const std::optional<std::string> cc =
            toolchainExecutable(*toolchainTable, "cc", sourceName, source,
                                toolchain.ccDeclaration, diagnostics)) {
      toolchain.cc = *cc;
    }
    if (const toml::node *nativeTargetNode =
            toolchainTable->get("native-target")) {
      toolchain.nativeTargetDeclaration =
          sourceSpan(sourceName, source, *nativeTargetNode);
      const std::optional<std::string> value =
          nativeTargetNode->value<std::string>();
      if (!value) {
        diagnostics.push_back(
            buildDiagnostic("GTI-B1004", toolchain.nativeTargetDeclaration,
                            "Toolchain field 'native-target' must be a "
                            "string."));
        continue;
      }
      if (value->empty() || value->front() == '-') {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1902", toolchain.nativeTargetDeclaration,
            "Toolchain field 'native-target' must be a native triple "
            "spelling, not an option."));
        continue;
      }
      toolchain.nativeTarget = *value;
    }

    if (const toml::node *sysrootNode = toolchainTable->get("sysroot")) {
      toolchain.sysrootDeclaration =
          sourceSpan(sourceName, source, *sysrootNode);
      const std::optional<std::string> spelling =
          sysrootNode->value<std::string>();
      if (!spelling) {
        diagnostics.push_back(
            buildDiagnostic("GTI-B1004", toolchain.sysrootDeclaration,
                            "Toolchain field 'sysroot' must be a string."));
        continue;
      }
      const std::filesystem::path declared(*spelling);
      if (declared.empty()) {
        diagnostics.push_back(
            buildDiagnostic("GTI-B1903", toolchain.sysrootDeclaration,
                            "Toolchain field 'sysroot' must not be empty."));
        continue;
      }
      // A cross sysroot normally lives outside the package, so an absolute
      // spelling is accepted as an explicit machine-specific declaration. A
      // relative spelling stays package-contained like every other declared
      // manifest path.
      const bool absolute = isAbsoluteOnSupportedHost(*spelling, declared);
      std::error_code error;
      const std::filesystem::path resolved =
          absolute ? std::filesystem::weakly_canonical(declared, error)
                   : std::filesystem::weakly_canonical(packageRoot / declared,
                                                       error);
      if (error) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1903", toolchain.sysrootDeclaration,
            "Failed to resolve toolchain sysroot '" + pathString(declared) +
                "': " + error.message() + "."));
        continue;
      }
      if (!absolute && !pathIsWithin(packageRoot, resolved)) {
        Diagnostic diagnostic = buildDiagnostic(
            "GTI-B1903", toolchain.sysrootDeclaration,
            "Relative toolchain sysroot escapes the package directory: " +
                pathString(declared) + ".");
        diagnostic.hints.push_back(
            "Declare a sysroot outside the package with an absolute path.");
        diagnostics.push_back(std::move(diagnostic));
        continue;
      }
      if (!std::filesystem::is_directory(resolved, error) || error) {
        diagnostics.push_back(buildDiagnostic(
            "GTI-B1903", toolchain.sysrootDeclaration,
            "Toolchain sysroot does not name an existing directory: " +
                pathString(declared) + "."));
        continue;
      }
      toolchain.sysroot = resolved;
      toolchain.declaredSysroot = *spelling;
    }
    toolchains.push_back(std::move(toolchain));
  }
  // Deterministic identity: the manifest's own table ordering must not reach
  // the build plan's model identity.
  std::sort(toolchains.begin(), toolchains.end(),
            [](const ProjectToolchain &left, const ProjectToolchain &right) {
              return left.declaredTriple < right.declaredTriple;
            });
  return toolchains;
}

std::optional<ProjectWorkspaceManifest>
parseWorkspace(const toml::table &document,
               const std::filesystem::path &packageRoot,
               std::string_view sourceName, std::string_view source,
               std::vector<Diagnostic> &diagnostics) {
  const toml::node *node = document.get("workspace");
  if (node == nullptr) {
    return std::nullopt;
  }
  const toml::table *table = node->as_table();
  if (table == nullptr) {
    diagnostics.push_back(
        buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                        "Manifest field 'workspace' must be a table."));
    return std::nullopt;
  }
  validateFields(*table, {"members"}, "workspace", sourceName, source,
                 diagnostics);
  const toml::node *membersNode = requiredField(
      *table, "members", "workspace", sourceName, source, diagnostics);
  ProjectWorkspaceManifest workspace;
  workspace.declaration = sourceSpan(sourceName, source, *node);
  if (membersNode == nullptr) {
    return workspace;
  }
  const toml::array *members = membersNode->as_array();
  if (members == nullptr) {
    diagnostics.push_back(buildDiagnostic(
        "GTI-B1004", sourceSpan(sourceName, source, *membersNode),
        "Workspace field 'members' must be an array of package paths."));
    return workspace;
  }

  for (const toml::node &memberNode : *members) {
    const SourceSpan memberSpan = sourceSpan(sourceName, source, memberNode);
    const std::optional<std::string> spelling = memberNode.value<std::string>();
    if (!spelling) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", memberSpan,
          "Workspace field 'members' must contain only strings."));
      continue;
    }
    const std::optional<std::filesystem::path> resolved =
        resolvePackageDirectory(*spelling, packageRoot, memberSpan, true,
                                "Workspace member", diagnostics);
    if (!resolved) {
      continue;
    }
    if (*resolved == packageRoot) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1104", memberSpan,
          "The workspace root package is implicit and cannot also be a "
          "member."));
      continue;
    }
    std::error_code error;
    const std::filesystem::path manifest = *resolved / manifestFilename;
    if (!std::filesystem::is_regular_file(manifest, error) || error) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1103", memberSpan,
          "Workspace member does not contain a regular gti.toml: " +
              pathString(std::filesystem::path(*spelling)) + "."));
      continue;
    }
    if (std::find(workspace.members.begin(), workspace.members.end(),
                  *resolved) != workspace.members.end()) {
      diagnostics.push_back(buildDiagnostic(
          "GTI-B1005", memberSpan,
          "Workspace member resolves to a package already listed."));
      continue;
    }
    workspace.members.push_back(*resolved);
    workspace.memberDeclarations.push_back(memberSpan);
  }
  return workspace;
}

} // namespace

ProjectManifest::ProjectManifest(
    std::filesystem::path path, ProjectPackage package,
    std::vector<ProjectTarget> targets, std::vector<ProjectProfile> profiles,
    std::vector<ProjectDependency> dependencies,
    std::optional<ProjectWorkspaceManifest> workspace,
    ProjectBuildSettings build,
    std::vector<ProjectBindgenImport> bindgenImports,
    std::vector<ProjectToolchain> toolchains)
    : manifestPath(std::move(path)), rootPath(manifestPath.parent_path()),
      packageIdentity(std::move(package)), projectTargets(std::move(targets)),
      buildProfiles(std::move(profiles)),
      packageDependencies(std::move(dependencies)),
      workspaceManifest(std::move(workspace)),
      projectBuildSettings(std::move(build)),
      projectBindgenImports(std::move(bindgenImports)),
      projectToolchains(std::move(toolchains)) {}

std::string_view projectTargetKindName(ProjectTargetKind kind) {
  switch (kind) {
  case ProjectTargetKind::Executable:
    return "executable";
  case ProjectTargetKind::Test:
    return "test";
  case ProjectTargetKind::Library:
    return "library";
  }
  return "executable";
}

std::string_view projectTargetLinkageName(ProjectTargetLinkage linkage) {
  switch (linkage) {
  case ProjectTargetLinkage::None:
    return "none";
  case ProjectTargetLinkage::Shared:
    return "shared";
  }
  return "none";
}

const std::filesystem::path &ProjectManifest::path() const {
  return manifestPath;
}

const std::filesystem::path &ProjectManifest::packageRoot() const {
  return rootPath;
}

const ProjectPackage &ProjectManifest::package() const {
  return packageIdentity;
}

const std::vector<ProjectTarget> &ProjectManifest::targets() const {
  return projectTargets;
}

const std::vector<ProjectProfile> &ProjectManifest::profiles() const {
  return buildProfiles;
}

const std::vector<ProjectDependency> &ProjectManifest::dependencies() const {
  return packageDependencies;
}

const std::optional<ProjectWorkspaceManifest> &
ProjectManifest::workspace() const {
  return workspaceManifest;
}

const ProjectBuildSettings &ProjectManifest::build() const {
  return projectBuildSettings;
}

const std::vector<ProjectToolchain> &ProjectManifest::toolchains() const {
  return projectToolchains;
}

const ProjectToolchain *
ProjectManifest::findToolchain(const TargetInfo &target) const {
  for (const ProjectToolchain &toolchain : projectToolchains) {
    if (toolchain.target.os == target.os &&
        toolchain.target.vendor == target.vendor &&
        toolchain.target.arch == target.arch) {
      return &toolchain;
    }
  }
  return nullptr;
}

const std::vector<ProjectBindgenImport> &ProjectManifest::bindgen() const {
  return projectBindgenImports;
}

const ProjectTarget *ProjectManifest::findTarget(std::string_view name) const {
  const auto found = std::find_if(
      projectTargets.begin(), projectTargets.end(),
      [name](const ProjectTarget &target) { return target.name == name; });
  return found == projectTargets.end() ? nullptr : &*found;
}

const ProjectProfile *
ProjectManifest::findProfile(std::string_view name) const {
  const auto found = std::find_if(
      buildProfiles.begin(), buildProfiles.end(),
      [name](const ProjectProfile &profile) { return profile.name == name; });
  return found == buildProfiles.end() ? nullptr : &*found;
}

ManifestDiscoveryResult
discoverProjectManifest(const std::filesystem::path &startDirectory) {
  ManifestDiscoveryResult result;
  std::error_code error;
  std::filesystem::path current =
      std::filesystem::absolute(startDirectory, error);
  if (error) {
    result.status = ManifestDiscoveryStatus::FilesystemFailure;
    result.diagnostics.push_back(
        buildDiagnostic("GTI-B1101", {pathString(startDirectory), 0, 1, 1},
                        "Failed to resolve the project discovery directory: " +
                            error.message() + "."));
    return result;
  }

  current = std::filesystem::weakly_canonical(current, error);
  if (error || !std::filesystem::is_directory(current, error)) {
    result.status = ManifestDiscoveryStatus::FilesystemFailure;
    result.diagnostics.push_back(buildDiagnostic(
        "GTI-B1101", {pathString(current), 0, 1, 1},
        "Project discovery must start from an existing directory."));
    return result;
  }

  while (true) {
    const std::filesystem::path candidate = current / manifestFilename;
    const std::filesystem::file_status candidateEntry =
        std::filesystem::symlink_status(candidate, error);
    const bool missing = error == std::errc::no_such_file_or_directory ||
                         (!error && !std::filesystem::exists(candidateEntry));
    if (missing) {
      error.clear();
    }
    if (error) {
      result.status = ManifestDiscoveryStatus::FilesystemFailure;
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1101", {pathString(candidate), 0, 1, 1},
          "Failed while searching for gti.toml: " + error.message() + "."));
      return result;
    }
    if (!missing) {
      if (std::filesystem::is_symlink(candidateEntry)) {
        result.status = ManifestDiscoveryStatus::FilesystemFailure;
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1101", {pathString(candidate), 0, 1, 1},
            "Discovered gti.toml must not be a symbolic link."));
        return result;
      }
      error.clear();
      if (!std::filesystem::is_regular_file(candidate, error) || error) {
        result.status = ManifestDiscoveryStatus::FilesystemFailure;
        result.diagnostics.push_back(
            buildDiagnostic("GTI-B1101", {pathString(candidate), 0, 1, 1},
                            "Discovered gti.toml is not a regular file."));
        return result;
      }
      result.status = ManifestDiscoveryStatus::Found;
      result.path = std::filesystem::canonical(candidate, error);
      if (error) {
        result.status = ManifestDiscoveryStatus::FilesystemFailure;
        result.path.reset();
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1101", {pathString(candidate), 0, 1, 1},
            "Failed to resolve gti.toml: " + error.message() + "."));
      }
      return result;
    }

    const std::filesystem::path parent = current.parent_path();
    if (parent == current || parent.empty()) {
      break;
    }
    current = parent;
  }

  result.status = ManifestDiscoveryStatus::NotFound;
  result.diagnostics.push_back(buildDiagnostic(
      "GTI-B1100", {pathString(startDirectory / manifestFilename), 0, 1, 1},
      "Could not find gti.toml in this directory or any parent directory."));
  return result;
}

ManifestLoadResult
loadProjectManifest(const std::filesystem::path &requestedManifestPath,
                    const std::optional<std::string> &textOverride,
                    ManifestFeaturePolicy features) {
  ManifestLoadResult result;
  std::error_code error;
  std::filesystem::path manifestPath =
      std::filesystem::absolute(requestedManifestPath, error);
  if (!error) {
    manifestPath = std::filesystem::weakly_canonical(manifestPath, error);
  }
  if (error) {
    result.status = ManifestLoadStatus::IoFailure;
    result.diagnostics.push_back(buildDiagnostic(
        "GTI-B1102", {pathString(requestedManifestPath), 0, 1, 1},
        "Failed to resolve the project manifest: " + error.message() + "."));
    return result;
  }

  const std::string sourceName = pathString(manifestPath);
  std::ifstream input(manifestPath, std::ios::binary);
  if (!input && !textOverride) {
    result.status = ManifestLoadStatus::IoFailure;
    result.diagnostics.push_back(
        buildDiagnostic("GTI-B1102", {sourceName, 0, 1, 1},
                        "Failed to open the project manifest."));
    return result;
  }
  const std::string source =
      textOverride ? *textOverride
                   : std::string{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
  if (!textOverride && !input.good() && !input.eof()) {
    result.status = ManifestLoadStatus::IoFailure;
    result.diagnostics.push_back(
        buildDiagnostic("GTI-B1102", {sourceName, 0, 1, 1},
                        "Failed to read the project manifest."));
    return result;
  }
  result.sources.set(sourceName, source);

  toml::parse_result parsed = toml::parse(source, sourceName);
  if (!parsed) {
    const toml::parse_error &parseError = parsed.error();
    result.status = ManifestLoadStatus::ParseFailure;
    result.diagnostics.push_back(buildDiagnostic(
        "GTI-B1000", sourceSpan(sourceName, source, parseError.source()),
        "Invalid TOML: " + std::string(parseError.description()) + "."));
    return result;
  }

  toml::table document = std::move(parsed).table();
  validateFields(document,
                 {"manifest-version", "package", "targets", "profiles",
                  "dependencies", "workspace", "build", "bindgen", "toolchains",
                  "modules", "module-implementations"},
                 "top-level manifest", sourceName, source, result.diagnostics);

  if (const toml::node *version =
          requiredField(document, "manifest-version", "top-level", sourceName,
                        source, result.diagnostics)) {
    const std::optional<std::int64_t> value = version->value<std::int64_t>();
    if (!value) {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *version),
          "Manifest field 'manifest-version' must be an integer."));
    } else if (*value != currentManifestVersion) {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1003", sourceSpan(sourceName, source, *version),
          "Unsupported manifest version " + std::to_string(*value) +
              "; this GTI toolchain supports version " +
              std::to_string(currentManifestVersion) + "."));
    }
  }

  const std::filesystem::path packageRoot = manifestPath.parent_path();
  ProjectPackage package;
  package.sourceRoot = packageRoot / "src";
  {
    std::error_code sourceRootError;
    const std::filesystem::path canonicalSourceRoot =
        std::filesystem::weakly_canonical(package.sourceRoot, sourceRootError);
    if (!sourceRootError) {
      package.sourceRoot = canonicalSourceRoot;
    }
  }
  if (const toml::table *packageTable =
          requiredTable(document, "package", "top-level", sourceName, source,
                        result.diagnostics)) {
    validateFields(*packageTable,
                   {"name", "version", "source-root", "native", "build-script"},
                   "package", sourceName, source, result.diagnostics);
    package.sourceRootDeclaration =
        sourceSpan(sourceName, source, *packageTable);
    const std::optional<std::string> name =
        requiredString(*packageTable, "name", "package", sourceName, source,
                       result.diagnostics);
    const std::optional<std::string> version =
        requiredString(*packageTable, "version", "package", sourceName, source,
                       result.diagnostics);
    if (name) {
      package.name = *name;
      if (!isPortableProjectName(*name)) {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1005",
            sourceSpan(sourceName, source, *packageTable->get("name")),
            "Package names must match [A-Za-z][A-Za-z0-9_-]* and cannot use "
            "a reserved portable device name."));
      }
    }
    if (version) {
      package.version = *version;
      if (!isSemanticVersion(*version)) {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1005",
            sourceSpan(sourceName, source, *packageTable->get("version")),
            "Package version must be a Semantic Version such as '0.1.0'."));
      }
    }
    if (const toml::node *sourceRootNode = packageTable->get("source-root")) {
      package.sourceRootDeclaration =
          sourceSpan(sourceName, source, *sourceRootNode);
      const std::optional<std::string> spelling =
          sourceRootNode->value<std::string>();
      if (!spelling) {
        result.diagnostics.push_back(
            buildDiagnostic("GTI-B1004", package.sourceRootDeclaration,
                            "Package field 'source-root' must be a string."));
      } else if (const std::optional<std::filesystem::path> resolved =
                     resolvePackageDirectory(
                         *spelling, packageRoot, package.sourceRootDeclaration,
                         true, "Package source root", result.diagnostics)) {
        package.sourceRoot = *resolved;
      }
    }
    package.native =
        parseOptionalNative(*packageTable, packageRoot, "Package", sourceName,
                            source, result.diagnostics);
    if (const toml::node *scriptNode = packageTable->get("build-script")) {
      ProjectBuildScript script;
      script.declaration = sourceSpan(sourceName, source, *scriptNode);
      const toml::table *scriptTable = scriptNode->as_table();
      if (scriptTable == nullptr) {
        result.diagnostics.push_back(
            buildDiagnostic("GTI-B1004", script.declaration,
                            "Package field 'build-script' must be a table."));
      } else {
        validateFields(*scriptTable, {"command", "source", "inputs"},
                       "package build-script", sourceName, source,
                       result.diagnostics);
        if (scriptTable->contains("command")) {
          script.command = stringArray(
              *scriptTable, "command", "Package build-script", sourceName,
              source, result.diagnostics, &script.commandDeclarations);
        }
        const toml::node *sourceNode = scriptTable->get("source");
        if (sourceNode != nullptr) {
          script.sourceDeclaration =
              sourceSpan(sourceName, source, *sourceNode);
          const std::optional<std::string> spelling =
              sourceNode->value<std::string>();
          if (!spelling) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1004", script.sourceDeclaration,
                "Package build-script field 'source' must be a string."));
          } else {
            const std::filesystem::path declared(*spelling);
            if (declared.empty() ||
                isAbsoluteOnSupportedHost(*spelling, declared)) {
              result.diagnostics.push_back(buildDiagnostic(
                  "GTI-B1103", script.sourceDeclaration,
                  "Package build-script source must be a non-empty path "
                  "relative to gti.toml."));
            } else {
              std::error_code sourceError;
              const std::filesystem::path resolved =
                  std::filesystem::weakly_canonical(packageRoot / declared,
                                                    sourceError);
              if (sourceError ||
                  !std::filesystem::is_regular_file(resolved, sourceError)) {
                result.diagnostics.push_back(buildDiagnostic(
                    "GTI-B2001", script.sourceDeclaration,
                    "Package build-script source does not name an existing "
                    "regular file: " +
                        *spelling + "."));
              } else if (!pathIsWithin(packageRoot, resolved)) {
                result.diagnostics.push_back(buildDiagnostic(
                    "GTI-B1104", script.sourceDeclaration,
                    "Package build-script source escapes the package "
                    "directory: " +
                        *spelling + "."));
              } else if (resolved.extension() != ".gti") {
                result.diagnostics.push_back(buildDiagnostic(
                    "GTI-B2001", script.sourceDeclaration,
                    "Package build-script source must use the '.gti' "
                    "extension: " +
                        *spelling + "."));
              } else {
                script.source = resolved;
                script.declaredSource = *spelling;
              }
            }
          }
        }
        if (script.command.empty() == !script.source.has_value()) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1002", script.declaration,
              "Package build-script requires exactly one of a non-empty "
              "'command' array or a GTI 'source' file."));
        }

        std::vector<SourceSpan> inputDeclarations;
        script.declaredInputs = stringArray(
            *scriptTable, "inputs", "Package build-script", sourceName, source,
            result.diagnostics, &inputDeclarations);
        for (std::size_t index = 0; index < script.declaredInputs.size();
             ++index) {
          const std::string &spelling = script.declaredInputs[index];
          const std::filesystem::path declared(spelling);
          if (declared.empty() ||
              isAbsoluteOnSupportedHost(spelling, declared)) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1103", inputDeclarations[index],
                "Package build-script inputs must be non-empty paths relative "
                "to gti.toml."));
            continue;
          }
          std::error_code inputError;
          const std::filesystem::path resolved =
              std::filesystem::weakly_canonical(packageRoot / declared,
                                                inputError);
          if (inputError || !std::filesystem::exists(resolved, inputError)) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B2001", inputDeclarations[index],
                "Package build-script input does not exist: " + spelling +
                    "."));
            continue;
          }
          if (!pathIsWithin(packageRoot, resolved)) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1104", inputDeclarations[index],
                "Package build-script input escapes the package directory: " +
                    spelling + "."));
            continue;
          }
          script.inputs.push_back(resolved);
          script.inputDeclarations.push_back(inputDeclarations[index]);
        }
        if (script.source &&
            std::find(script.inputs.begin(), script.inputs.end(),
                      *script.source) == script.inputs.end()) {
          script.inputs.push_back(*script.source);
          script.inputDeclarations.push_back(script.sourceDeclaration);
        }
        if (script.inputs.size() != script.declaredInputs.size()) {
          script.declaredInputs.clear();
          for (const std::filesystem::path &input : script.inputs) {
            script.declaredInputs.push_back(
                std::filesystem::relative(input, packageRoot).generic_string());
          }
        }
        package.buildScript = std::move(script);
      }
    }
  }

  if (const toml::node *moduleNode = document.get("modules")) {
    const toml::table *modules = moduleNode->as_table();
    if (modules == nullptr) {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *moduleNode),
          "Manifest field 'modules' must map module names to interface "
          "paths."));
    } else {
      for (const auto &[moduleKey, pathNode] : *modules) {
        const std::string name(moduleKey.str());
        const SourceSpan declaration = sourceSpan(sourceName, source, pathNode);
        if (!isImportAlias(name) || keywords.contains(name) ||
            isCppReservedIdentifier(name) || name.starts_with("__gti_")) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", sourceSpan(sourceName, source, moduleKey),
              "Module names must be ordinary ASCII GTI identifiers."));
          continue;
        }
        if (name == "std") {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", sourceSpan(sourceName, source, moduleKey),
              "The module name 'std' is reserved for the standard library."));
          continue;
        }
        const auto spelling = pathNode.value<std::string>();
        if (!spelling) {
          result.diagnostics.push_back(
              buildDiagnostic("GTI-B1004", declaration,
                              "A module interface path must be a string."));
          continue;
        }
        const std::filesystem::path relative(*spelling);
        if (spelling->empty() || spelling->find('\0') != std::string::npos ||
            isAbsoluteOnSupportedHost(*spelling, relative) ||
            relative.extension() != ".gti") {
          result.diagnostics.push_back(
              buildDiagnostic("GTI-B1103", declaration,
                              "A module interface must be a relative .gti path "
                              "inside its package."));
          continue;
        }
        std::error_code pathError;
        const auto resolved =
            std::filesystem::canonical(packageRoot / relative, pathError);
        if (pathError ||
            !std::filesystem::is_regular_file(resolved, pathError)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1103", declaration,
              "Module interface does not name an existing regular file: " +
                  *spelling + "."));
          continue;
        }
        if (!pathIsWithin(packageRoot, resolved)) {
          result.diagnostics.push_back(
              buildDiagnostic("GTI-B1104", declaration,
                              "Module interface escapes its owning package."));
          continue;
        }
        if (std::any_of(package.modules.begin(), package.modules.end(),
                        [&](const auto &module) {
                          return module.second == resolved ||
                                 portableProjectNameKey(module.first) ==
                                     portableProjectNameKey(name);
                        })) {
          result.diagnostics.push_back(
              buildDiagnostic("GTI-B1005", declaration,
                              "Module registrations must have distinct "
                              "interface files and portable names."));
          continue;
        }
        package.modules.emplace_back(name, resolved);
      }
    }
  }

  if (const auto *node = document.get("module-implementations")) {
    const auto *table = node->as_table();
    if (table == nullptr) {
      result.diagnostics.push_back(
          buildDiagnostic("GTI-B1004", sourceSpan(sourceName, source, *node),
                          "'module-implementations' must map registered module "
                          "names to arrays of .gti paths."));
    } else {
      std::set<std::filesystem::path> seen;
      for (const auto &[key, pathsNode] : *table) {
        const std::string name(key.str());
        const auto declaration = sourceSpan(sourceName, source, pathsNode);
        const auto module = std::find_if(
            package.modules.begin(), package.modules.end(),
            [&](const auto &entry) { return entry.first == name; });
        if (module == package.modules.end()) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", declaration,
              "An implementation must name a module registered in [modules]."));
          continue;
        }
        const auto *paths = pathsNode.as_array();
        if (paths == nullptr) {
          result.diagnostics.push_back(
              buildDiagnostic("GTI-B1004", declaration,
                              "Module implementations must be an array of "
                              "relative .gti paths."));
          continue;
        }
        std::vector<std::filesystem::path> implementations;
        for (const auto &pathNode : *paths) {
          const auto spelling = pathNode.value<std::string>();
          const auto where = sourceSpan(sourceName, source, pathNode);
          if (!spelling) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1004", where,
                "A module implementation path must be a string."));
            continue;
          }
          const std::filesystem::path relative(*spelling);
          if (spelling->empty() || spelling->find('\0') != std::string::npos ||
              isAbsoluteOnSupportedHost(*spelling, relative) ||
              relative.extension() != ".gti") {
            result.diagnostics.push_back(
                buildDiagnostic("GTI-B1103", where,
                                "A module implementation must be a relative "
                                ".gti path inside its package."));
            continue;
          }
          std::error_code error;
          const auto resolved =
              std::filesystem::canonical(packageRoot / relative, error);
          if (error || !std::filesystem::is_regular_file(resolved, error)) {
            result.diagnostics.push_back(
                buildDiagnostic("GTI-B1103", where,
                                "Module implementation does not name an "
                                "existing regular file: " +
                                    *spelling + "."));
          } else if (!pathIsWithin(packageRoot, resolved)) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1104", where,
                "Module implementation escapes its owning package."));
          } else if (!seen.insert(resolved).second ||
                     std::any_of(package.modules.begin(), package.modules.end(),
                                 [&](const auto &entry) {
                                   return entry.second == resolved;
                                 })) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1005", where,
                "An implementation file must be distinct from every interface "
                "and other implementation registration."));
          } else {
            implementations.push_back(resolved);
          }
        }
        package.moduleImplementations.emplace_back(name,
                                                   std::move(implementations));
      }
    }
  }

  std::vector<ProjectDependency> dependencies = parseDependencies(
      document, packageRoot, sourceName, source, result.diagnostics);
  std::optional<ProjectWorkspaceManifest> workspace = parseWorkspace(
      document, packageRoot, sourceName, source, result.diagnostics);
  std::vector<ProjectBindgenImport> bindgenImports =
      parseBindgenImports(document, packageRoot, dependencies, sourceName,
                          source, result.diagnostics);
  std::vector<ProjectToolchain> toolchains = parseToolchains(
      document, packageRoot, sourceName, source, result.diagnostics);
  ProjectBuildSettings build;
  if (const toml::node *buildNode = document.get("build")) {
    build.declaration = sourceSpan(sourceName, source, *buildNode);
    const toml::table *buildTable = buildNode->as_table();
    if (buildTable == nullptr) {
      result.diagnostics.push_back(
          buildDiagnostic("GTI-B1004", build.declaration,
                          "Manifest field 'build' must be a table."));
    } else {
      validateFields(*buildTable, {"defines", "allow-build-scripts"}, "build",
                     sourceName, source, result.diagnostics);
      build.defines = parseConfigurationDefines(
          *buildTable, "Build", sourceName, source, result.diagnostics,
          build.defineDeclarations);
      build.allowedBuildScripts = stringArray(
          *buildTable, "allow-build-scripts", "Build", sourceName, source,
          result.diagnostics, &build.allowedBuildScriptDeclarations);
      std::vector<std::pair<std::string, SourceSpan>> permissions;
      permissions.reserve(build.allowedBuildScripts.size());
      for (std::size_t index = 0; index < build.allowedBuildScripts.size();
           ++index) {
        permissions.emplace_back(build.allowedBuildScripts[index],
                                 build.allowedBuildScriptDeclarations[index]);
      }
      std::sort(permissions.begin(), permissions.end(),
                [](const auto &left, const auto &right) {
                  return left.first < right.first;
                });
      permissions.erase(std::unique(permissions.begin(), permissions.end(),
                                    [](const auto &left, const auto &right) {
                                      return left.first == right.first;
                                    }),
                        permissions.end());
      build.allowedBuildScripts.clear();
      build.allowedBuildScriptDeclarations.clear();
      for (auto &[identity, declaration] : permissions) {
        build.allowedBuildScripts.push_back(std::move(identity));
        build.allowedBuildScriptDeclarations.push_back(std::move(declaration));
      }
    }
  }

  std::vector<ProjectTarget> targets;
  if (const toml::node *targetsNode = document.get("targets")) {
    const toml::table *targetsTable = targetsNode->as_table();
    if (targetsTable == nullptr) {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *targetsNode),
          "Manifest field 'targets' must be a table."));
    } else {
      std::vector<std::pair<std::string, SourceSpan>> portableTargetNames;
      for (const auto &[targetKey, targetNode] : *targetsTable) {
        const std::string targetName(targetKey.str());
        const SourceSpan targetNameSpan =
            sourceSpan(sourceName, source, targetKey);
        if (!isPortableProjectName(targetName)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", targetNameSpan,
              "Target names must match [A-Za-z][A-Za-z0-9_-]* and cannot use "
              "a reserved portable device name."));
        }
        const std::string portableName = portableProjectNameKey(targetName);
        const auto collision =
            std::find_if(portableTargetNames.begin(), portableTargetNames.end(),
                         [&portableName](const auto &candidate) {
                           return candidate.first == portableName;
                         });
        if (collision != portableTargetNames.end()) {
          Diagnostic diagnostic = buildDiagnostic(
              "GTI-B1005", targetNameSpan,
              "Target name '" + targetName +
                  "' collides with another target under portable "
                  "case-insensitive artifact identity.");
          diagnostic.related.push_back(
              {collision->second, "The conflicting target is declared here."});
          result.diagnostics.push_back(std::move(diagnostic));
        } else {
          portableTargetNames.emplace_back(portableName, targetNameSpan);
        }
        const toml::table *targetTable = targetNode.as_table();
        if (targetTable == nullptr) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1004", sourceSpan(sourceName, source, targetNode),
              "Target '" + targetName + "' must be a table."));
          continue;
        }
        validateFields(
            *targetTable,
            {"kind", "linkage", "root", "exports", "native", "defines"},
            "target", sourceName, source, result.diagnostics);
        const std::optional<std::string> kind =
            requiredString(*targetTable, "kind", "target", sourceName, source,
                           result.diagnostics);
        const std::optional<std::string> root =
            requiredString(*targetTable, "root", "target", sourceName, source,
                           result.diagnostics);
        ProjectTargetKind targetKind = ProjectTargetKind::Executable;
        if (kind) {
          if (*kind == "test") {
            targetKind = ProjectTargetKind::Test;
          } else if (*kind == "library") {
            targetKind = ProjectTargetKind::Library;
            if (!features.dormantLibraryTargets) {
              Diagnostic diagnostic = buildDiagnostic(
                  "GTI-B2000",
                  sourceSpan(sourceName, source, *targetTable->get("kind")),
                  "Shared-library targets are not enabled in this GTI "
                  "toolchain yet.");
              diagnostic.hints.push_back(
                  "Use an executable or test target until the complete "
                  "embedding artifact gate is released.");
              result.diagnostics.push_back(std::move(diagnostic));
            }
          } else if (*kind != "executable") {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1005",
                sourceSpan(sourceName, source, *targetTable->get("kind")),
                "Target kind must be 'executable' or 'test' in manifest "
                "version 1."));
          }
        }

        ProjectTargetLinkage targetLinkage = ProjectTargetLinkage::None;
        std::vector<ProjectExport> targetExports;
        const toml::node *linkageNode = targetTable->get("linkage");
        const toml::node *exportsNode = targetTable->get("exports");
        if (targetKind == ProjectTargetKind::Library) {
          const std::optional<std::string> linkage =
              requiredString(*targetTable, "linkage", "library target",
                             sourceName, source, result.diagnostics);
          if (linkage) {
            if (*linkage == "shared") {
              targetLinkage = ProjectTargetLinkage::Shared;
            } else {
              result.diagnostics.push_back(buildDiagnostic(
                  "GTI-B2000", sourceSpan(sourceName, source, *linkageNode),
                  "Library target linkage must be 'shared' in embedding ABI "
                  "version 1."));
            }
          }
          if (exportsNode == nullptr) {
            (void)requiredField(*targetTable, "exports", "library target",
                                sourceName, source, result.diagnostics);
          } else if (const toml::array *entries = exportsNode->as_array()) {
            for (const toml::node &element : *entries) {
              const SourceSpan declaration =
                  sourceSpan(sourceName, source, element);
              const toml::table *entry = element.as_table();
              if (entry == nullptr) {
                result.diagnostics.push_back(buildDiagnostic(
                    "GTI-B1004", declaration,
                    "Each library export must be a table declaring "
                    "'function' and 'symbol'."));
                continue;
              }
              validateFields(*entry, {"function", "symbol"}, "library export",
                             sourceName, source, result.diagnostics);
              const std::optional<std::string> function =
                  requiredString(*entry, "function", "library export",
                                 sourceName, source, result.diagnostics);
              const std::optional<std::string> symbol =
                  requiredString(*entry, "symbol", "library export", sourceName,
                                 source, result.diagnostics);
              if (!function || !symbol) {
                continue;
              }
              targetExports.push_back(
                  {.function = *function,
                   .symbol = *symbol,
                   .declaration = declaration,
                   .functionDeclaration =
                       sourceSpan(sourceName, source, *entry->get("function")),
                   .symbolDeclaration =
                       sourceSpan(sourceName, source, *entry->get("symbol"))});
            }
            if (entries->empty()) {
              result.diagnostics.push_back(buildDiagnostic(
                  "GTI-B2000", sourceSpan(sourceName, source, *exportsNode),
                  "A library target must declare at least one export."));
            }
          } else {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1004", sourceSpan(sourceName, source, *exportsNode),
                "Library target field 'exports' must be an array of tables."));
          }
        } else {
          if (linkageNode != nullptr) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1005", sourceSpan(sourceName, source, *linkageNode),
                "Only a library target may declare 'linkage'."));
          }
          if (exportsNode != nullptr) {
            result.diagnostics.push_back(buildDiagnostic(
                "GTI-B1005", sourceSpan(sourceName, source, *exportsNode),
                "Only a library target may declare 'exports'."));
          }
        }
        if (!root) {
          continue;
        }

        const toml::node &rootNode = *targetTable->get("root");
        const std::filesystem::path declaredRoot(*root);
        if (declaredRoot.empty() ||
            isAbsoluteOnSupportedHost(*root, declaredRoot)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1103", sourceSpan(sourceName, source, rootNode),
              "Target root must be a non-empty path relative to gti.toml."));
          continue;
        }
        if (declaredRoot.extension() != ".gti") {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1103", sourceSpan(sourceName, source, rootNode),
              "Target root must use the .gti extension."));
          continue;
        }

        const std::filesystem::path candidate = packageRoot / declaredRoot;
        std::error_code pathError;
        const std::filesystem::path resolvedRoot =
            std::filesystem::canonical(candidate, pathError);
        if (pathError ||
            !std::filesystem::is_regular_file(resolvedRoot, pathError)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1103", sourceSpan(sourceName, source, rootNode),
              "Target root does not name an existing regular file: " +
                  pathString(declaredRoot) + "."));
          continue;
        }
        if (!pathIsWithin(packageRoot, resolvedRoot)) {
          Diagnostic diagnostic = buildDiagnostic(
              "GTI-B1104", sourceSpan(sourceName, source, rootNode),
              "Target root escapes the package directory.");
          diagnostic.hints.push_back(
              "Dependency source roots must be declared before they can be "
              "used.");
          result.diagnostics.push_back(std::move(diagnostic));
          continue;
        }
        targets.push_back(
            {.name = targetName,
             .kind = targetKind,
             .linkage = targetLinkage,
             .root = resolvedRoot,
             .declaration = sourceSpan(sourceName, source, targetKey),
             .exports = std::move(targetExports),
             .defines = {},
             .defineDeclarations = {},
             .native =
                 parseOptionalNative(*targetTable, packageRoot, "Target",
                                     sourceName, source, result.diagnostics)});
        ProjectTarget &parsedTarget = targets.back();
        parsedTarget.defines = parseConfigurationDefines(
            *targetTable, "Target", sourceName, source, result.diagnostics,
            parsedTarget.defineDeclarations);
      }
    }
  }

  std::vector<ProjectProfile> profiles{defaultProfile("dev"),
                                       defaultProfile("release")};
  std::vector<std::pair<std::string, SourceSpan>> portableProfileNames;
  if (const toml::node *profilesNode = document.get("profiles")) {
    if (const toml::table *profilesTable = profilesNode->as_table()) {
      for (const auto &[profileKey, profileNode] : *profilesTable) {
        const std::string profileName(profileKey.str());
        const SourceSpan profileNameSpan =
            sourceSpan(sourceName, source, profileKey);
        if (!isPortableProjectName(profileName)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", profileNameSpan,
              "Profile names must match [A-Za-z][A-Za-z0-9_-]* and cannot "
              "use a reserved portable device name."));
        }
        const std::string portableName = portableProjectNameKey(profileName);
        const bool builtinCollision =
            (portableName == "dev" && profileName != "dev") ||
            (portableName == "release" && profileName != "release");
        const auto collision = std::find_if(
            portableProfileNames.begin(), portableProfileNames.end(),
            [&portableName](const auto &candidate) {
              return candidate.first == portableName;
            });
        if (builtinCollision || collision != portableProfileNames.end()) {
          Diagnostic diagnostic = buildDiagnostic(
              "GTI-B1005", profileNameSpan,
              "Profile name '" + profileName +
                  "' collides with another profile under portable "
                  "case-insensitive artifact identity.");
          if (collision != portableProfileNames.end()) {
            diagnostic.related.push_back(
                {collision->second,
                 "The conflicting profile is declared here."});
          } else {
            diagnostic.hints.push_back("The built-in profile name is '" +
                                       portableName + "'.");
          }
          result.diagnostics.push_back(std::move(diagnostic));
        } else if (profileName != "dev" && profileName != "release") {
          portableProfileNames.emplace_back(portableName, profileNameSpan);
        }
        const toml::table *profileTable = profileNode.as_table();
        if (profileTable == nullptr) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1004", sourceSpan(sourceName, source, profileNode),
              "Profile '" + profileName + "' must be a table."));
          continue;
        }

        const auto existing =
            std::find_if(profiles.begin(), profiles.end(),
                         [&profileName](const ProjectProfile &profile) {
                           return profile.name == profileName;
                         });
        ProjectProfile profile =
            existing == profiles.end()
                ? defaultProfile(profileName,
                                 sourceSpan(sourceName, source, profileKey))
                : *existing;
        profile.declaration = sourceSpan(sourceName, source, profileKey);
        applyProfileFields(profile, *profileTable, sourceName, source,
                           result.diagnostics);
        profile.native =
            parseOptionalNative(*profileTable, packageRoot, "Profile",
                                sourceName, source, result.diagnostics);
        if (existing == profiles.end()) {
          profiles.push_back(std::move(profile));
        } else {
          *existing = std::move(profile);
        }
      }
    } else {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1004", sourceSpan(sourceName, source, *profilesNode),
          "Manifest field 'profiles' must be a table."));
    }
  }

  if (!result.diagnostics.empty()) {
    result.status = ManifestLoadStatus::ValidationFailure;
    return result;
  }

  std::sort(targets.begin(), targets.end(),
            [](const ProjectTarget &left, const ProjectTarget &right) {
              return left.name < right.name;
            });
  std::sort(profiles.begin(), profiles.end(),
            [](const ProjectProfile &left, const ProjectProfile &right) {
              return left.name < right.name;
            });
  result.status = ManifestLoadStatus::Success;
  result.manifest.emplace(manifestPath, std::move(package), std::move(targets),
                          std::move(profiles), std::move(dependencies),
                          std::move(workspace), std::move(build),
                          std::move(bindgenImports), std::move(toolchains));
  return result;
}

} // namespace lang::driver
