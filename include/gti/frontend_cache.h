#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/token.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct FrontendCacheLimits {
  std::size_t maximumEntries = 256;
  std::size_t maximumEstimatedBytes = 64U * 1024U * 1024U;
};

struct FrontendCacheStats {
  std::size_t entries = 0;
  std::size_t estimatedBytes = 0;
  FrontendCacheLimits limits;
};

// Identity shared by lexical and parsed-unit cache rows. `context` is the
// normalized target, initial configuration-flag, and artifact program/export
// identity;
// `sourceGeneration` is supplied by the snapshot coordinator and changes
// whenever disk or an overlay changes. Parsed rows additionally use the exact
// post-directive `syntaxDigest` and the transitive `dependencyDigest` for the
// source graph visible to that unit. Overlay lexical lookups compare exact
// bytes.
struct FrontendUnitCacheKey {
  std::string canonicalPath;
  std::uint64_t sourceGeneration = 0;
  std::uint64_t sourceDigest = 0;
  // Zero for raw lexical rows; the exact post-directive token identity for
  // parsed rows.
  std::uint64_t syntaxDigest = 0;
  // Zero for raw lexical rows; a deterministic identity of every transitive
  // dependency's canonical source, generation, contents, and processed token
  // stream for parsed rows.
  std::uint64_t dependencyDigest = 0;
  std::string context;
  std::optional<std::size_t> completionOffset;

  friend bool operator==(const FrontendUnitCacheKey &,
                         const FrontendUnitCacheKey &) = default;
};

struct CachedLexedSource {
  std::string source;
  std::uint64_t sourceDigest = 0;
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;
  bool hadError = false;
};

struct CachedParsedUnit {
  StmtList declarations;
  std::vector<Diagnostic> diagnostics;
  bool hadError = false;
};

// A process-local, thread-safe LRU shared by compatible frontend invocations.
// Rows contain source text/tokens or immutable parsed statement trees only;
// semantic IDs, semantic records, and snapshot-owned pointers never enter
// this cache.
class FrontendCache final {
public:
  explicit FrontendCache(FrontendCacheLimits limits = {});
  ~FrontendCache();

  FrontendCache(const FrontendCache &) = delete;
  FrontendCache &operator=(const FrontendCache &) = delete;

  [[nodiscard]] std::optional<CachedLexedSource>
  lookupLexed(const FrontendUnitCacheKey &key,
              std::optional<std::string_view> exactSource = std::nullopt);

  void storeLexed(FrontendUnitCacheKey key, CachedLexedSource source);

  [[nodiscard]] std::optional<CachedParsedUnit>
  lookupParsed(const FrontendUnitCacheKey &key);

  void storeParsed(FrontendUnitCacheKey key, std::size_t estimatedInputBytes,
                   CachedParsedUnit parsed);

  void invalidate(std::string_view canonicalPath);

  [[nodiscard]] FrontendCacheStats stats() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
