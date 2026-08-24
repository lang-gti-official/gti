#include "frontend_snapshot_state.h"

#include <utility>

namespace lang::lsp {

FrontendSnapshotState::FrontendSnapshotState(FrontendCacheLimits limits)
    : sharedCache(std::make_shared<FrontendCache>(limits)) {}

std::uint64_t
FrontendSnapshotState::invalidate(std::string_view canonicalPath) {
  const std::lock_guard lock(mutex);
  const std::string path(canonicalPath);
  digests.erase(path);
  return ++generations[path];
}

FrontendSourceSnapshot FrontendSnapshotState::capture() const {
  const std::lock_guard lock(mutex);
  return {.cache = sharedCache, .generations = generations, .digests = digests};
}

bool FrontendSnapshotState::publish(std::string_view canonicalPath,
                                    FrontendSourceIdentity identity) {
  if (identity.digest == 0) {
    return false;
  }
  const std::lock_guard lock(mutex);
  const std::string path(canonicalPath);
  const auto current = generations.find(path);
  const std::uint64_t currentGeneration =
      current == generations.end() ? 0 : current->second;
  if (identity.generation != currentGeneration) {
    return false;
  }
  digests[path] = identity.digest;
  return true;
}

std::uint64_t
FrontendSnapshotState::generation(std::string_view canonicalPath) const {
  const std::lock_guard lock(mutex);
  const auto found = generations.find(std::string(canonicalPath));
  return found == generations.end() ? 0 : found->second;
}

std::optional<std::uint64_t>
FrontendSnapshotState::digest(std::string_view canonicalPath) const {
  const std::lock_guard lock(mutex);
  const auto found = digests.find(std::string(canonicalPath));
  return found == digests.end() ? std::nullopt
                                : std::optional<std::uint64_t>(found->second);
}

std::shared_ptr<FrontendCache> FrontendSnapshotState::cache() const {
  return sharedCache;
}

} // namespace lang::lsp
