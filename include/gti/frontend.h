#pragma once

#include "gti/diagnostic.h"
#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/mir.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_graph.h"
#include "gti/target.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang {

enum class FrontendPhase {
  Semantics,
  Hir,
  Mir,
};

struct FrontendOptions {
  TargetInfo target = TargetInfo::host();
  bool analyzeRecoveredProgram = false;
  std::optional<std::size_t> completionOffset;
  FrontendPhase stopAfter = FrontendPhase::Mir;
  bool toolingOccurrences = true;
};

struct FrontendResult {
  Program program;
  SemanticModel semantics;
  HirProgram hir;
  FailureMetadata failureMetadata;
  MirProgram mir;
  SourceGraph sourceGraph;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  bool sourceValid = false;
  bool syntaxValid = false;
  bool semanticValid = false;
  bool hirValid = false;
  bool failureMetadataValid = false;
  bool mirValid = false;

  [[nodiscard]] bool canGenerateCode() const {
    return sourceValid && syntaxValid && semanticValid && hirValid &&
           failureMetadataValid && mirValid;
  }
};

class Frontend {
public:
  explicit Frontend(FrontendOptions options = {});

  [[nodiscard]] FrontendResult analyze(
      const std::filesystem::path &entryPath,
      std::optional<std::string> entrySource = std::nullopt,
      const std::vector<std::filesystem::path> &preludePaths = {},
      const std::unordered_map<std::string, std::string> &sourceOverrides = {},
      const std::vector<std::filesystem::path> &standardLibraryRoots = {},
      const std::vector<PackageSourceRoot> &packageSourceRoots = {}) const;

  [[nodiscard]] FrontendResult
  analyzeLoaded(const std::filesystem::path &entryPath, SourceGraph sourceGraph,
                SourceManager sources,
                std::vector<Diagnostic> sourceDiagnostics = {},
                bool sourceValid = true) const;

private:
  [[nodiscard]] FrontendResult
  finishAnalysis(const std::filesystem::path &entryPath,
                 FrontendResult result) const;
  static void
  appendParserDiagnostics(std::vector<Diagnostic> &destination,
                          const std::vector<ParseDiagnostic> &diagnostics,
                          const SourceGraph &sourceGraph, SourceUnitId unit);

  template <typename DiagnosticType>
  static void append(std::vector<Diagnostic> &destination,
                     const std::vector<DiagnosticType> &source);

  FrontendOptions options;
};

} // namespace lang
