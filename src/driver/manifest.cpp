#include "gti/driver/manifest.h"

#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
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

std::size_t utf8CodePointLength(unsigned char byte) {
  if ((byte & 0x80U) == 0) {
    return 1;
  }
  if ((byte & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((byte & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((byte & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

std::size_t sourceOffset(std::string_view source,
                         const toml::source_position &position) {
  if (position.line == 0 || position.column == 0) {
    return 0;
  }

  std::size_t offset = 0;
  for (std::uint32_t line = 1; line < position.line && offset < source.size();
       ++line) {
    const std::size_t newline = source.find('\n', offset);
    if (newline == std::string_view::npos) {
      return source.size();
    }
    offset = newline + 1;
  }

  for (std::uint32_t column = 1;
       column < position.column && offset < source.size() &&
       source[offset] != '\n';
       ++column) {
    const std::size_t length =
        utf8CodePointLength(static_cast<unsigned char>(source[offset]));
    offset += std::min(length, source.size() - offset);
  }
  return offset;
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

bool isPortableName(std::string_view name) {
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
  return std::all_of(name.begin() + 1, name.end(), [&](char character) {
    return alphabetic(character) || digit(character) || character == '_' ||
           character == '-';
  });
}

bool isIdentifierText(std::string_view identifier, bool numericLeadingZero) {
  if (identifier.empty()) {
    return false;
  }
  const bool numeric =
      std::all_of(identifier.begin(), identifier.end(), [](char character) {
        return character >= '0' && character <= '9';
      });
  if (numeric && numericLeadingZero && identifier.size() > 1 &&
      identifier.front() == '0') {
    return false;
  }
  return std::all_of(identifier.begin(), identifier.end(), [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-';
  });
}

bool validIdentifierList(std::string_view text, bool numericLeadingZero) {
  if (text.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t separator = text.find('.', start);
    const std::size_t end =
        separator == std::string_view::npos ? text.size() : separator;
    if (!isIdentifierText(text.substr(start, end - start),
                          numericLeadingZero)) {
      return false;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return true;
}

bool isSemanticVersion(std::string_view version) {
  const std::size_t buildSeparator = version.find('+');
  const std::string_view beforeBuild = version.substr(0, buildSeparator);
  if (buildSeparator != std::string_view::npos &&
      !validIdentifierList(version.substr(buildSeparator + 1), false)) {
    return false;
  }

  const std::size_t prereleaseSeparator = beforeBuild.find('-');
  const std::string_view core = beforeBuild.substr(0, prereleaseSeparator);
  if (prereleaseSeparator != std::string_view::npos &&
      !validIdentifierList(beforeBuild.substr(prereleaseSeparator + 1), true)) {
    return false;
  }

  std::size_t start = 0;
  for (int component = 0; component < 3; ++component) {
    const std::size_t separator = core.find('.', start);
    const bool final = component == 2;
    if ((final && separator != std::string_view::npos) ||
        (!final && separator == std::string_view::npos)) {
      return false;
    }
    const std::size_t end = final ? core.size() : separator;
    const std::string_view value = core.substr(start, end - start);
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(),
                     [](char character) {
                       return character >= '0' && character <= '9';
                     }) ||
        (value.size() > 1 && value.front() == '0')) {
      return false;
    }
    start = end + 1;
  }
  return true;
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

ProjectProfile defaultProfile(std::string name, SourceSpan declaration = {}) {
  const bool release = name == "release";
  return {.name = std::move(name),
          .optimization =
              release ? OptimizationLevel::O3 : OptimizationLevel::O0,
          .cppStandard = CppStandard::Cpp23,
          .keepCpp = false,
          .declaration = std::move(declaration)};
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

void applyProfileFields(ProjectProfile &profile, const toml::table &table,
                        std::string_view sourceName, std::string_view source,
                        std::vector<Diagnostic> &diagnostics) {
  validateFields(table, {"optimization", "cpp-standard", "keep-cpp"}, "profile",
                 sourceName, source, diagnostics);

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

} // namespace

ProjectManifest::ProjectManifest(std::filesystem::path path,
                                 ProjectPackage package,
                                 std::vector<ProjectTarget> targets,
                                 std::vector<ProjectProfile> profiles)
    : manifestPath(std::move(path)), rootPath(manifestPath.parent_path()),
      packageIdentity(std::move(package)),
      executableTargets(std::move(targets)),
      buildProfiles(std::move(profiles)) {}

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
  return executableTargets;
}

const std::vector<ProjectProfile> &ProjectManifest::profiles() const {
  return buildProfiles;
}

const ProjectTarget *ProjectManifest::findTarget(std::string_view name) const {
  const auto found = std::find_if(
      executableTargets.begin(), executableTargets.end(),
      [name](const ProjectTarget &target) { return target.name == name; });
  return found == executableTargets.end() ? nullptr : &*found;
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
    const bool exists = std::filesystem::exists(candidate, error);
    if (error) {
      result.status = ManifestDiscoveryStatus::FilesystemFailure;
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1101", {pathString(candidate), 0, 1, 1},
          "Failed while searching for gti.toml: " + error.message() + "."));
      return result;
    }
    if (exists) {
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
loadProjectManifest(const std::filesystem::path &requestedManifestPath) {
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
  if (!input) {
    result.status = ManifestLoadStatus::IoFailure;
    result.diagnostics.push_back(
        buildDiagnostic("GTI-B1102", {sourceName, 0, 1, 1},
                        "Failed to open the project manifest."));
    return result;
  }
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  if (!input.good() && !input.eof()) {
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
                 {"manifest-version", "package", "targets", "profiles"},
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

  ProjectPackage package;
  if (const toml::table *packageTable =
          requiredTable(document, "package", "top-level", sourceName, source,
                        result.diagnostics)) {
    validateFields(*packageTable, {"name", "version"}, "package", sourceName,
                   source, result.diagnostics);
    const std::optional<std::string> name =
        requiredString(*packageTable, "name", "package", sourceName, source,
                       result.diagnostics);
    const std::optional<std::string> version =
        requiredString(*packageTable, "version", "package", sourceName, source,
                       result.diagnostics);
    if (name) {
      package.name = *name;
      if (!isPortableName(*name)) {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1005",
            sourceSpan(sourceName, source, *packageTable->get("name")),
            "Package names must match [A-Za-z][A-Za-z0-9_-]*."));
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
  }

  const std::filesystem::path packageRoot = manifestPath.parent_path();
  std::vector<ProjectTarget> targets;
  if (const toml::table *targetsTable =
          requiredTable(document, "targets", "top-level", sourceName, source,
                        result.diagnostics)) {
    if (targetsTable->empty()) {
      result.diagnostics.push_back(buildDiagnostic(
          "GTI-B1002", sourceSpan(sourceName, source, *targetsTable),
          "Manifest table 'targets' must declare at least one executable "
          "target."));
    }
    for (const auto &[targetKey, targetNode] : *targetsTable) {
      const std::string targetName(targetKey.str());
      if (!isPortableName(targetName)) {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1005", sourceSpan(sourceName, source, targetKey),
            "Target names must match [A-Za-z][A-Za-z0-9_-]*."));
      }
      const toml::table *targetTable = targetNode.as_table();
      if (targetTable == nullptr) {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1004", sourceSpan(sourceName, source, targetNode),
            "Target '" + targetName + "' must be a table."));
        continue;
      }
      validateFields(*targetTable, {"kind", "root"}, "target", sourceName,
                     source, result.diagnostics);
      const std::optional<std::string> kind =
          requiredString(*targetTable, "kind", "target", sourceName, source,
                         result.diagnostics);
      const std::optional<std::string> root =
          requiredString(*targetTable, "root", "target", sourceName, source,
                         result.diagnostics);
      if (kind && *kind != "executable") {
        result.diagnostics.push_back(buildDiagnostic(
            "GTI-B1005",
            sourceSpan(sourceName, source, *targetTable->get("kind")),
            "Target kind must be 'executable' in manifest version 1."));
      }
      if (!root) {
        continue;
      }

      const toml::node &rootNode = *targetTable->get("root");
      const std::filesystem::path declaredRoot(*root);
      if (declaredRoot.empty() || declaredRoot.is_absolute()) {
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
        diagnostic.hints.push_back("Dependency source roots must be declared "
                                   "before they can be used.");
        result.diagnostics.push_back(std::move(diagnostic));
        continue;
      }
      targets.push_back({targetName, resolvedRoot,
                         sourceSpan(sourceName, source, targetKey)});
    }
  }

  std::vector<ProjectProfile> profiles{defaultProfile("dev"),
                                       defaultProfile("release")};
  if (const toml::node *profilesNode = document.get("profiles")) {
    if (const toml::table *profilesTable = profilesNode->as_table()) {
      for (const auto &[profileKey, profileNode] : *profilesTable) {
        const std::string profileName(profileKey.str());
        if (!isPortableName(profileName)) {
          result.diagnostics.push_back(buildDiagnostic(
              "GTI-B1005", sourceSpan(sourceName, source, profileKey),
              "Profile names must match [A-Za-z][A-Za-z0-9_-]*."));
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
                          std::move(profiles));
  return result;
}

} // namespace lang::driver
