#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lang {

// Per-invocation frontend work accounting. Callers provide one instance to a
// synchronous Frontend invocation; no compiler state or snapshot-local
// identity is retained here. The LSP benchmark uses the exact unit lists to
// distinguish useful cache hits from unchanged end-to-end behavior.
struct FrontendWorkMetrics {
  std::vector<std::string> loadedUnits;
  std::vector<std::string> lexedUnits;
  std::vector<std::string> parsedUnits;
  std::vector<std::string> semanticallyAnalyzedUnits;
  std::vector<std::string> lexCacheHits;
  std::vector<std::string> parseCacheHits;
  std::size_t sourceBytes = 0;
  std::size_t sourceLoadMicroseconds = 0;
  std::size_t lexMicroseconds = 0;
  std::size_t parseMicroseconds = 0;
  std::size_t semanticMicroseconds = 0;
  std::size_t semanticRegistrationMicroseconds = 0;
  std::size_t semanticBodyMicroseconds = 0;
  std::size_t semanticTrustedBodyMicroseconds = 0;
  // Trusted bodies the lexical pass deferred, and how many were analysed on
  // demand; see TrustedBodyAnalysis.
  std::size_t semanticDeferredBodies = 0;
  std::size_t semanticAnalyzedDeferredBodies = 0;
  std::size_t semanticDemandRounds = 0;
  std::size_t semanticDemandFallbacks = 0;
  std::size_t semanticApplicationBodyMicroseconds = 0;
  std::size_t semanticFinalizationMicroseconds = 0;
  std::size_t hirMicroseconds = 0;
  // Function instances HIR lowered, including any beyond the reachable prefix.
  std::size_t hirFunctionInstances = 0;
  std::size_t semanticBorrowSummaryCandidateInspections = 0;
  std::size_t semanticBorrowSummaryFunctions = 0;
  std::size_t constexprMirExecutions = 0;
  std::size_t semanticRawAuditQueries = 0;

  void clear() {
    loadedUnits.clear();
    lexedUnits.clear();
    parsedUnits.clear();
    semanticallyAnalyzedUnits.clear();
    lexCacheHits.clear();
    parseCacheHits.clear();
    sourceBytes = 0;
    sourceLoadMicroseconds = 0;
    lexMicroseconds = 0;
    parseMicroseconds = 0;
    semanticMicroseconds = 0;
    semanticRegistrationMicroseconds = 0;
    semanticBodyMicroseconds = 0;
    semanticTrustedBodyMicroseconds = 0;
    semanticDeferredBodies = 0;
    semanticAnalyzedDeferredBodies = 0;
    semanticDemandRounds = 0;
    semanticDemandFallbacks = 0;
    semanticApplicationBodyMicroseconds = 0;
    semanticFinalizationMicroseconds = 0;
    hirMicroseconds = 0;
    hirFunctionInstances = 0;
    semanticBorrowSummaryCandidateInspections = 0;
    semanticBorrowSummaryFunctions = 0;
    constexprMirExecutions = 0;
    semanticRawAuditQueries = 0;
  }
};

} // namespace lang
