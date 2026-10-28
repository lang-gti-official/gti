#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/parser.h"
#include "gti/source_graph.h"
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

// The post-directive form of a lexical unit whose loading cannot depend on
// configuration flags: it defines, undefines, and tests no flag, and loading
// it reported no lexical, directive, or token-level diagnostic. Its
// conditionals can still test the target, which the cache context fixes.
// Every configuration domain therefore selects `tokens`, and loading it again
// only re-resolves its import and include directives in source order. Each
// directive keeps its own tokens plus one token of lookahead, which is all
// directive resolution reads.
struct ProcessedSourceUnit {
  struct Directive {
    std::vector<Token> tokens;
    int braceDepth = 0;
    int conditionalDepth = 0;
  };

  std::shared_ptr<const std::vector<Token>> tokens;
  std::uint64_t syntaxDigest = 0;
  std::optional<Token> moduleDeclaration;
  std::vector<Directive> directives;
  std::vector<SourceSpan> inactiveSpans;
  std::vector<ConfigurationToken> configurationTokens;
};

struct CachedLexedSource {
  std::string source;
  std::uint64_t sourceDigest = 0;
  // Null once `processed` is attached; the processed form supersedes it.
  std::shared_ptr<const std::vector<Token>> tokens;
  std::vector<Diagnostic> diagnostics;
  bool hadError = false;
  std::shared_ptr<const ProcessedSourceUnit> processed;
};

struct CachedParsedUnit {
  StmtList declarations;
  std::vector<Diagnostic> diagnostics;
  bool hadError = false;
  std::optional<SyntaxCompletionContext> completionContext;
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

  // Attaches the processed form of the lexical row for `key` with exactly
  // `source`, replacing its raw tokens. A missing row is left absent.
  void storeProcessed(const FrontendUnitCacheKey &key, std::string_view source,
                      std::shared_ptr<const ProcessedSourceUnit> processed);

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
