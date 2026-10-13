#pragma once

#include "gti/configuration_flags.h"
#include "gti/diagnostic.h"
#include "gti/failure_metadata.h"
#include "gti/frontend_cache.h"
#include "gti/frontend_work.h"
#include "gti/hir.h"
#include "gti/include_suggestions.h"
#include "gti/instance_analysis_cache.h"
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
  // Declared but possibly inactive switches; this set never selects a branch.
  ConfigurationFlags declaredConfigurationFlags;
  RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off;
  bool analyzeRecoveredProgram = false;
  std::optional<std::size_t> completionOffset;
  FrontendPhase stopAfter = FrontendPhase::Mir;
  bool toolingOccurrences = true;
  // Set only for an executable root, never inferred from a source filename.
  bool requireEntryPoint = false;
  // Executable representation requests require selected GTI call definitions
  // even when they intentionally do not require a hosted entry point.
  bool requireFunctionDefinitions = false;
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
  // Lower to MIR only the instances reachable from the program roots. The
  // complete program is still analyzed, so diagnostics are unchanged; artifact
  // builds set this, while inspection of every declaration's lowering does
  // not.
  bool lowerReachableOnly = false;
  // With `lowerReachableOnly`, lower compiler-trusted (prelude and
  // standard-library) declarations only when the program roots reach them. The
  // entry unit and every application or package unit are still lowered
  // completely, so their diagnostics are unchanged. Library instances that
  // nothing reaches are not concretely analyzed. Artifact builds set this; `gti
  // check` does not, so its diagnostics remain complete.
  bool lowerLibraryReachableOnly = false;
  // Lower HIR only for the program roots and the entry source unit's own
  // declarations, discovering everything they reach. Concrete-instance
  // diagnostics are then reported for the entry unit's code and the library
  // code it instantiates, not for library instances nothing in the entry unit
  // reaches. Editor snapshots set this; `gti check` and builds never do, so
  // their diagnostics remain complete. Meaningful only with `stopAfter == Hir`.
  bool lowerEntryUnitOnly = false;
  // When false, HIR analyses but does not lower the bodies of library-only
  // instances (compiler-trusted declarations whose arguments name only
  // compiler-trusted types); see HirLoweringScope::omitLibraryOnlyBodies.
  // Editor snapshots clear it because the LSP never reads HIR bodies.
  bool lowerLibraryOnlyInstanceBodies = true;
  // Report raw-pointer audit findings located inside compiler-trusted units
  // (the prelude and the standard library) to an application. Off by default:
  // such a finding describes the library's own implementation, which the
  // library checks with that unit as the entry. The audit still analyses
  // those bodies, because application provenance flows through them.
  bool reportLibraryAuditFindings = false;
  // When compiler-trusted function bodies are analysed; see
  // TrustedBodyAnalysis. `Deferred` is a test mode; artifacts and editor
  // snapshots may use `Reachable`. Full checks retain `Complete`.
  TrustedBodyAnalysis trustedBodies = TrustedBodyAnalysis::Complete;
  // Concrete instance analyses of compiler-trusted declarations shared
  // across snapshots (see InstanceAnalysisCache). Consulted only by
  // `Reachable` analysis; editor snapshots set it, builds and checks do not.
  std::shared_ptr<InstanceAnalysisCache> instanceCache;
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
