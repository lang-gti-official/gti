#pragma once

#include "gti/ast.h"
#include "gti/semantic_model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lang {

// Concrete instance analyses of compiler-trusted declarations whose
// arguments name only compiler-trusted types are re-derived identically by
// every editor snapshot that shares the same standard library. This cache lets
// a later snapshot reuse such an analysis (the semantic delta HIR lowers from)
// instead of repeating it. It is owned by a frontend caller (the LSP) like
// FrontendCache, passed through FrontendOptions, bounded, and consulted only
// by bounded (`TrustedBodyAnalysis::Reachable`) analysis; complete analysis
// (`gti check`) and artifact builds never use it. See "Concrete Generic
// Reanalysis" in docs/architecture/semantic-analysis.md for the identity
// contract.
struct InstanceAnalysisCacheLimits {
  std::size_t maximumEntries = 8192;
  std::size_t maximumEstimatedBytes = 192U * 1024U * 1024U;
  // Distinct library generations (trusted source identity plus registration
  // identities) kept at once; one per concurrently edited project shape.
  std::size_t maximumGenerations = 4;
};

struct InstanceAnalysisCacheStats {
  std::size_t entries = 0;
  std::size_t estimatedBytes = 0;
  std::size_t generations = 0;
  InstanceAnalysisCacheLimits limits;
};

enum class InstanceAnalysisKind {
  Function,
  Constructor,
  Destructor,
  ClassFieldInitializers,
};

// One concrete instance request. Identities are snapshot-local, but every
// entry belongs to a generation whose trusted registration identities are
// proved equal before any lookup, so equal keys denote the same request.
struct InstanceAnalysisKey {
  InstanceAnalysisKind kind = InstanceAnalysisKind::Function;
  std::size_t declaration = 0;
  std::vector<SemanticType> classTypes;
  std::vector<CompileTimeValue> classValues;
  std::vector<SemanticType> types;
  std::vector<CompileTimeValue> values;

  friend bool operator==(const InstanceAnalysisKey &,
                         const InstanceAnalysisKey &) = default;
};

struct InstanceAnalysisKeyHash {
  std::size_t operator()(const InstanceAnalysisKey &key) const;
};

// Everything that must be identical for two snapshots to share entries: the
// frontend/analysis configuration, every compiler-trusted unit by its exact
// immutable parsed declarations (shared through FrontendCache), and a
// fingerprint of the trusted declaration identities registration assigned.
struct InstanceAnalysisGenerationKey {
  struct TrustedUnit {
    SourceUnitId id = 0;
    std::string path;
    std::uint64_t sourceDigest = 0;
    std::uint64_t syntaxDigest = 0;
    std::vector<const Stmt *> declarations;

    friend bool operator==(const TrustedUnit &, const TrustedUnit &) = default;
  };

  std::string context;
  std::vector<TrustedUnit> units;
  std::uint64_t registrationFingerprint = 0;

  friend bool operator==(const InstanceAnalysisGenerationKey &,
                         const InstanceAnalysisGenerationKey &) = default;
};

// A reusable analysis: the instance delta (detached from any base, with its
// reuse journal frozen), the declaration syntax it analysed, and the functions
// whose facts it consulted.
struct InstanceAnalysisCacheEntry {
  SemanticModel model;
  const Stmt *declaration = nullptr;
  std::vector<std::pair<FunctionId, const FunctionDecl *>> consulted;
  std::size_t estimatedBytes = 0;
};

class InstanceAnalysisGeneration;

class InstanceAnalysisCache final {
public:
  using Generation = std::shared_ptr<const InstanceAnalysisGeneration>;

  explicit InstanceAnalysisCache(InstanceAnalysisCacheLimits limits = {});
  ~InstanceAnalysisCache();

  InstanceAnalysisCache(const InstanceAnalysisCache &) = delete;
  InstanceAnalysisCache &operator=(const InstanceAnalysisCache &) = delete;

  [[nodiscard]] InstanceAnalysisCacheStats stats() const;
  void clear();

  // ---- Semantic analysis interface ------------------------------------
  // Returns the generation equal to `key`, creating it when absent. A new
  // generation retains `syntax` (the trusted units' parsed declarations) so
  // AST identities its entries key on can never be reused by another
  // allocation while an entry is alive.
  [[nodiscard]] Generation generation(InstanceAnalysisGenerationKey key,
                                      std::vector<StmtPtr> syntax);
  [[nodiscard]] std::shared_ptr<const InstanceAnalysisCacheEntry>
  lookup(const Generation &generation, const InstanceAnalysisKey &key);
  void store(const Generation &generation, InstanceAnalysisKey key,
             InstanceAnalysisCacheEntry entry);
  // The implicit initializer anchor (a source-free empty construction) of a
  // compiler-trusted field, shared by every snapshot that presents the same
  // field so instance analyses keyed on it stay reusable. An anchor holds
  // only the field's name token, so anchors with equal tokens are
  // interchangeable; one with different tokens is replaced.
  [[nodiscard]] std::shared_ptr<const ExprPtr>
  implicitFieldInitializer(const VariableDecl &field);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
