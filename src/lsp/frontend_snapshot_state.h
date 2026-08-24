#pragma once

#include "gti/frontend_cache.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lang::lsp {

struct FrontendSourceIdentity {
  std::uint64_t generation = 0;
  std::uint64_t digest = 0;
};

struct FrontendSourceSnapshot {
  std::shared_ptr<FrontendCache> cache;
  std::unordered_map<std::string, std::uint64_t> generations;
  std::unordered_map<std::string, std::uint64_t> digests;
};

// Owns the process-wide reusable frontend cache and the exact source
// identities that make its rows safe to reuse. LSP document state remains in
// the protocol adapter; this class only coordinates immutable frontend input
// snapshots. Its own lock keeps worker capture and protocol-thread
// invalidation independently testable.
class FrontendSnapshotState final {
public:
  explicit FrontendSnapshotState(FrontendCacheLimits limits = {});

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

private:
  std::shared_ptr<FrontendCache> sharedCache;
  mutable std::mutex mutex;
  std::unordered_map<std::string, std::uint64_t> generations;
  std::unordered_map<std::string, std::uint64_t> digests;
};

} // namespace lang::lsp
