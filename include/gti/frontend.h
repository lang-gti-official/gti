#pragma once

#include "gti/configuration_flags.h"
#include "gti/diagnostic.h"
#include "gti/failure_metadata.h"
#include "gti/frontend_cache.h"
#include "gti/frontend_work.h"
#include "gti/hir.h"
#include "gti/include_suggestions.h"
#include "gti/mir.h"
#include "gti/parser.h"
#include "gti/program_request.h"
#include "gti/progress.h"
#include "gti/semantic_model.h"
#include "gti/source_graph.h"
#include "gti/target.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang {

enum class FrontendPhase {
  Parsing,
  Semantics,
  Hir,
  Mir,
};

struct FrontendOptions {
  TargetInfo target = TargetInfo::host();
  ProgramRequest program;
  ConfigurationFlags configurationFlags;
  RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off;
  bool analyzeRecoveredProgram = false;
  std::optional<std::size_t> completionOffset;
  FrontendPhase stopAfter = FrontendPhase::Mir;
  bool toolingOccurrences = true;
  // Set only for an executable root, never inferred from a source filename.
  bool requireEntryPoint = false;
  ProgressObserver *progress = nullptr;
  // Optional synchronous work accounting. The caller owns this object for the
  // complete Frontend invocation.
  FrontendWorkMetrics *workMetrics = nullptr;
  std::shared_ptr<FrontendCache> cache;
  std::unordered_map<std::string, std::uint64_t> sourceGenerations;
  // Exact digests learned from a completed snapshot for the corresponding
  // source generation. Missing identities force SourceLoader to read and
  // digest the file before it may populate or reuse a cache row.
  std::unordered_map<std::string, std::uint64_t> sourceDigests;
  bool suggestIncludes = true;
  std::vector<IncludeSearchRoot> includeSearchRoots;
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
  // Files consulted while discovering fixes, including negative candidates.
  // Editors must invalidate suggestions when any of these inputs changes.
  std::vector<std::filesystem::path> includeSuggestionInputs;
  bool includeSuggestionsRequested = false;

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
