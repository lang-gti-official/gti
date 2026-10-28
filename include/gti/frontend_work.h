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
  // Units loaded from their cached flag-independent processed form.
  std::size_t processedSourceUnitReplays = 0;
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
  // Deferred bodies concrete discovery demanded after the pass, analysed in
  // the same analysis (late demand), and the time that took. The time is
  // part of semantic (and trusted-body) time, not HIR time.
  std::size_t semanticLateDemandBodies = 0;
  std::size_t semanticLateDemandMicroseconds = 0;
  std::size_t semanticApplicationBodyMicroseconds = 0;
  std::size_t semanticFinalizationMicroseconds = 0;
  // Trusted prefix preamble (see SemanticPreambleCache): whether this
  // analysis built one (and how long that took), whether it resumed from
  // one (and how long adopting its state took), the trusted bodies the
  // preamble holds and how many this analysis reached and reported.
  std::size_t semanticPreambleBuilds = 0;
  std::size_t semanticPreambleBuildMicroseconds = 0;
  std::size_t semanticPreambleAdoptions = 0;
  std::size_t semanticPreambleAdoptMicroseconds = 0;
  std::size_t semanticPreambleBodies = 0;
  std::size_t semanticPreambleReachedBodies = 0;
  std::size_t hirMicroseconds = 0;
  // Function instances HIR lowered, including any beyond the reachable prefix.
  std::size_t hirFunctionInstances = 0;
  // Concrete instance analyses with an InstanceAnalysisCache: reused from an
  // earlier snapshot (hits), analysed because no reusable entry existed or an
  // entry failed validation (misses; `rejected` counts the latter), not
  // eligible for reuse (an argument names a non-trusted type, or the
  // declaration is not compiler-trusted), and entries this analysis stored.
  std::size_t semanticInstanceReuseHits = 0;
  std::size_t semanticInstanceReuseMisses = 0;
  std::size_t semanticInstanceReuseRejected = 0;
  std::size_t semanticInstanceReuseIneligible = 0;
  std::size_t semanticInstanceReuseStores = 0;
  // Time spent looking up, validating and copying reusable entries (part of
  // HIR time, like the analyses they replace).
  std::size_t semanticInstanceReuseMicroseconds = 0;
  // Requirements answered from, and proofs added to, the adopted preamble's
  // shared concept proofs (see SemanticPreamble::ConceptProofKey).
  std::size_t semanticConceptProofReuses = 0;
  std::size_t semanticConceptProofRecords = 0;
  // Concrete initializer-effect queries answered from, and added to, the
  // adopted preamble's shared effect results.
  std::size_t semanticEffectQueryReuses = 0;
  std::size_t semanticEffectQueryRecords = 0;
  // Instance bodies HIR omitted as library-only; see
  // FrontendOptions::lowerLibraryOnlyInstanceBodies.
  std::size_t hirOmittedInstanceBodies = 0;
  std::size_t semanticBorrowSummaryCandidateInspections = 0;
  std::size_t semanticBorrowSummaryFunctions = 0;
  std::size_t constexprMirExecutions = 0;
  // Staged constant evaluations replayed from an earlier success of the same
  // request instead of executing again.
  std::size_t constexprStagedReuses = 0;
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
    processedSourceUnitReplays = 0;
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
    semanticLateDemandBodies = 0;
    semanticLateDemandMicroseconds = 0;
    semanticApplicationBodyMicroseconds = 0;
    semanticFinalizationMicroseconds = 0;
    semanticPreambleBuilds = 0;
    semanticPreambleBuildMicroseconds = 0;
    semanticPreambleAdoptions = 0;
    semanticPreambleAdoptMicroseconds = 0;
    semanticPreambleBodies = 0;
    semanticPreambleReachedBodies = 0;
    hirMicroseconds = 0;
    hirFunctionInstances = 0;
    semanticInstanceReuseHits = 0;
    semanticInstanceReuseMisses = 0;
    semanticInstanceReuseRejected = 0;
    semanticInstanceReuseIneligible = 0;
    semanticInstanceReuseStores = 0;
    semanticInstanceReuseMicroseconds = 0;
    semanticConceptProofReuses = 0;
    semanticConceptProofRecords = 0;
    semanticEffectQueryReuses = 0;
    semanticEffectQueryRecords = 0;
    hirOmittedInstanceBodies = 0;
    semanticBorrowSummaryCandidateInspections = 0;
    semanticBorrowSummaryFunctions = 0;
    constexprMirExecutions = 0;
    constexprStagedReuses = 0;
    semanticRawAuditQueries = 0;
  }
};

} // namespace lang
