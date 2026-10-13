#pragma once

#include "gti/frontend_cache.h"
#include "gti/instance_analysis_cache.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lang::lsp {

// An editor keeps every unit of an open project's include graph parsed: the
// shared library alone is several hundred kilobytes of source, whose lexical
// and parsed rows exceed the compiler's generic 64 MiB estimate. Evicting
// them makes every keystroke reload, re-lex and re-parse the standard
// library, and gives each snapshot fresh syntax identities, so no concrete
// instance analysis could be reused either.
[[nodiscard]] constexpr FrontendCacheLimits editorFrontendCacheLimits() {
  return {.maximumEntries = 1024,
          .maximumEstimatedBytes = 512U * 1024U * 1024U};
}

struct FrontendSourceIdentity {
  std::uint64_t generation = 0;
  std::uint64_t digest = 0;
};

struct FrontendSourceSnapshot {
  std::shared_ptr<FrontendCache> cache;
  std::shared_ptr<InstanceAnalysisCache> instanceCache;
  std::unordered_map<std::string, std::uint64_t> generations;
  std::unordered_map<std::string, std::uint64_t> digests;
};

// Owns the process-wide reusable frontend caches (parsed units and reusable
// concrete instance analyses) and the exact source identities that make
// their rows safe to reuse. LSP document state remains in
// the protocol adapter; this class only coordinates immutable frontend input
// snapshots. Its own lock keeps worker capture and protocol-thread
// invalidation independently testable.
class FrontendSnapshotState final {
public:
  explicit FrontendSnapshotState(
      FrontendCacheLimits limits = editorFrontendCacheLimits(),
      InstanceAnalysisCacheLimits instanceLimits = {});

  // Advances the identity before an overlay or disk change is analysed.
  // Previously learned digests for the path become unreachable immediately;
  // their physical rows are reclaimed by the bounded cache's LRU.
  std::uint64_t invalidate(std::string_view canonicalPath);

  [[nodiscard]] FrontendSourceSnapshot capture() const;

  // Learns identities only from a completed analysis that observed the same
  // generation still held by the coordinator. Returns false for stale or
  // unspecified identities.
  bool publish(std::string_view canonicalPath, FrontendSourceIdentity identity);

  [[nodiscard]] std::uint64_t generation(std::string_view canonicalPath) const;

  [[nodiscard]] std::optional<std::uint64_t>
  digest(std::string_view canonicalPath) const;

  [[nodiscard]] std::shared_ptr<FrontendCache> cache() const;
  [[nodiscard]] std::shared_ptr<InstanceAnalysisCache> instanceCache() const;

private:
  std::shared_ptr<FrontendCache> sharedCache;
  // Entries are keyed by exact trusted-unit syntax and registration
  // identities, so source invalidation needs no explicit eviction here: a
  // changed library unit or project shape simply selects another generation.
  std::shared_ptr<InstanceAnalysisCache> sharedInstanceCache;
  mutable std::mutex mutex;
  std::unordered_map<std::string, std::uint64_t> generations;
  std::unordered_map<std::string, std::uint64_t> digests;
};

} // namespace lang::lsp
