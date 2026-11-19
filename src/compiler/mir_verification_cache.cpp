#include "mir_verification_session.h"

#include <array>
#include <mutex>
#include <variant>

namespace lang {
namespace {
// A collision replaces a slot. Only exact equality can answer a query.
// Cap both the number and size of rows; bigger inputs are always checked.
template <class Value> class FactTable {
  struct Row {
    std::vector<std::size_t> key;
    Value value;
  };
  static constexpr std::size_t slots = 512;
  static constexpr std::size_t maximumWords = 4096;
  std::array<Row, slots> rows;
  std::mutex mutex;

  static std::size_t slot(const std::vector<std::size_t> &key) {
    std::size_t hash = 14695981039346656037ULL;
    for (auto word : key)
      hash = (hash ^ word) * 1099511628211ULL;
    return hash % slots;
  }

public:
  std::optional<Value> lookup(const std::vector<std::size_t> &key) {
    if (key.empty() || key.size() > maximumWords)
      return std::nullopt;
    auto index = slot(key);
    std::lock_guard lock(mutex);
    if (rows[index].key != key)
      return std::nullopt;
    return rows[index].value;
  }
  void store(std::vector<std::size_t> key, Value value) {
    if (key.empty() || key.size() > maximumWords)
      return;
    auto index = slot(key);
    std::lock_guard lock(mutex);
    rows[index] = {std::move(key), std::move(value)};
  }
};
} // namespace
struct MirVerificationCache::Storage {
  FactTable<MirDominanceInfo> dominance;
  FactTable<std::monostate> boundaries;
};
MirVerificationCache::MirVerificationCache()
    : storage(std::make_unique<Storage>()) {}
MirVerificationCache::~MirVerificationCache() = default;

namespace mir_detail {
VerificationSession::VerificationSession(MirVerificationCache *cache,
                                         FrontendWorkMetrics *work)
    : cache(cache), work(work) {}
void VerificationSession::recordRoundBody(bool reused) {
  if (!work)
    return;
  if (reused)
    ++work->mirRoundBodyReuses;
  else
    ++work->mirRoundBodyLowerings;
}
std::optional<MirDominanceInfo>
VerificationSession::dominance(const std::vector<std::size_t> &key) {
  auto result = cache->storage->dominance.lookup(key);
  if (work && result)
    ++work->mirDominanceReuses;
  return result;
}
void VerificationSession::rememberDominance(std::vector<std::size_t> key,
                                            const MirDominanceInfo &facts) {
  cache->storage->dominance.store(std::move(key), facts);
}
bool VerificationSession::knowsExclusiveBoundaries(
    const std::vector<std::size_t> &key) {
  bool hit = cache->storage->boundaries.lookup(key).has_value();
  if (work) {
    ++work->mirBoundaryQueries;
    if (hit)
      ++work->mirBoundaryReuses;
  }
  return hit;
}
void VerificationSession::rememberExclusiveBoundaries(
    std::vector<std::size_t> key) {
  cache->storage->boundaries.store(std::move(key), {});
}
} // namespace mir_detail
} // namespace lang
