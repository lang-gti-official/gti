#pragma once

#include "gti/diagnostic.h"
#include "gti/source_graph.h"
#include "gti/token.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lang {

using SourceDiagnostic = Diagnostic;

class SourceLoader {
public:
  SourceGraph
  load(const std::filesystem::path &entryPath,
       std::optional<std::string> entrySource = std::nullopt,
       const std::vector<std::filesystem::path> &preludePaths = {},
       const std::unordered_map<std::string, std::string> &sourceOverrides = {},
       const std::vector<std::filesystem::path> &standardLibraryRoots = {},
       std::optional<std::size_t> completionOffset = std::nullopt,
       const std::vector<PackageSourceRoot> &packageSourceRoots = {});

  [[nodiscard]] bool hadError() const;

  [[nodiscard]] const std::vector<SourceDiagnostic> &errors() const;

  [[nodiscard]] const SourceManager &sources() const;

private:
  enum class LoadState {
    Visiting,
    Loaded,
  };

  struct FileState {
    LoadState state = LoadState::Visiting;
    SourceUnitId unit = 0;
  };

  struct ResolvedInclude {
    std::size_t directiveEnd = 0;
    SourceUnitId dependency = 0;
    SourceDependencyKind kind = SourceDependencyKind::Include;
  };

  static std::filesystem::path canonicalPath(const std::filesystem::path &path);

  static bool isImportPathSegment(std::string_view segment);

  static bool isImportPathSegment(const Token &token);

  static bool pathIsWithin(const std::filesystem::path &root,
                           const std::filesystem::path &candidate);

  [[nodiscard]] const PackageSourceRoot *
  packageForPath(const std::filesystem::path &path) const;

  [[nodiscard]] const PackageSourceRoot *
  packageByIdentity(std::string_view identity) const;

  [[nodiscard]] std::optional<std::string>
  standardLibraryNameForExistingPath(const std::filesystem::path &path) const;

  SourceUnitId
  loadFile(const std::filesystem::path &path, bool isEntry, bool isPrelude,
           const Token *includeToken,
           std::optional<std::string> standardLibraryName = std::nullopt,
           SourceUnitRole role = SourceUnitRole::Application);

  ResolvedInclude resolveInclude(std::vector<Token> &tokens, std::size_t index,
                                 const std::filesystem::path &includingFile,
                                 int braceDepth, int conditionalDepth);

  ResolvedInclude
  resolveAngleInclude(std::vector<Token> &tokens, std::size_t index,
                      const Token &includeToken,
                      const std::filesystem::path &includingFile);

  ResolvedInclude
  resolvePackageInclude(const std::vector<std::string> &segments,
                        std::size_t directiveEnd, const Token &includeToken,
                        const std::filesystem::path &includingFile);

  void report(const Token &token, std::string message,
              std::string code = "GTI-I0000");

  std::vector<SourceDiagnostic> diagnostics;
  SourceGraph graph;
  SourceManager sourceManager;
  std::unordered_map<std::string, FileState> states;
  std::optional<std::string> entrySource;
  const std::unordered_map<std::string, std::string> *sourceOverrides = nullptr;
  std::vector<std::filesystem::path> standardLibraryRoots;
  std::vector<PackageSourceRoot> packageSourceRoots;
  bool entrySourceConsumed = false;
  std::optional<std::size_t> completionOffset;
};

} // namespace lang
