#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lang {

class FunctionDecl;

// ---- Body readiness ------------------------------------------------------
// Bounded analysis may leave a compiler-trusted body unanalysed until
// something demands it. Every analysis context answers "may a fact derived
// from this body be read here?" from one table: the body's state in the
// context. A body the table does not track was analysed in place (or is not
// subject to deferral), so it is always Analysed.
//
// The key names a body independently of its syntax address, so later body
// kinds (constructors, destructors) and stored records can share the table.

enum class BodyKind : std::uint8_t { Function, Constructor, Destructor };

struct BodyKey {
  BodyKind kind = BodyKind::Function;
  std::size_t id = 0;

  friend bool operator==(const BodyKey &, const BodyKey &) = default;
};

struct BodyKeyHash {
  [[nodiscard]] std::size_t operator()(const BodyKey &key) const {
    return key.id * 4 + static_cast<std::size_t>(key.kind);
  }
};

enum class BodyState : std::uint8_t {
  // Facts are final in this context.
  Analysed,
  // Being analysed (recursion): its facts are not available yet.
  InProgress,
  // Not analysed; the context's owner may analyse it on demand.
  Deferred,
  // Analysed in an adopted preamble but not yet reached by this analysis:
  // reaching it reports what it held back.
  HeldBack,
};

enum class BodyReadiness : std::uint8_t { Ready, Unavailable };

// One per body-analysis context. The request's root analyzer owns one; a
// worker that completes bodies in a private context of its own (the program
// effect worker) owns a fork; every other worker shares its creator's. Only
// the owner may change a body's state: a worker that is not the owner
// records a need instead of completing the body privately.
class BodyContext {
public:
  explicit BodyContext(const void *owner) : owner(owner) {}

  [[nodiscard]] bool ownedBy(const void *candidate) const {
    return owner == candidate;
  }

  // Starts tracking `declaration` in `state` (Deferred or HeldBack). Tracking
  // order is the resolution order, so it follows declaration order.
  void track(const FunctionDecl &declaration, BodyKey key, BodyState state) {
    const auto [entry, inserted] = entries.try_emplace(
        &declaration, Entry{.key = key, .state = state, .order = nextOrder});
    if (!inserted)
      return;
    ++nextOrder;
    keys.emplace(key, &declaration);
    if (state != BodyState::Analysed)
      ++unfinished;
  }
  // Whether any tracked body is not final yet; with none, nothing in this
  // context can need one.
  [[nodiscard]] bool anyUnfinished() const { return unfinished != 0; }

  [[nodiscard]] BodyState state(const FunctionDecl *declaration) const {
    if (declaration == nullptr)
      return BodyState::Analysed;
    const auto found = entries.find(declaration);
    return found == entries.end() ? BodyState::Analysed : found->second.state;
  }
  [[nodiscard]] BodyState state(BodyKey key) const {
    const auto found = keys.find(key);
    return found == keys.end() ? BodyState::Analysed : state(found->second);
  }
  // Deferred or held back: a fact derived from it is not available yet, but
  // the context's owner can make it available.
  [[nodiscard]] bool pending(const FunctionDecl *declaration) const {
    const BodyState current = state(declaration);
    return current == BodyState::Deferred || current == BodyState::HeldBack;
  }
  [[nodiscard]] bool tracked(const FunctionDecl *declaration) const {
    return declaration != nullptr && entries.contains(declaration);
  }
  [[nodiscard]] std::optional<BodyKey>
  key(const FunctionDecl *declaration) const {
    const auto found = entries.find(declaration);
    if (found == entries.end())
      return std::nullopt;
    return found->second.key;
  }
  [[nodiscard]] const FunctionDecl *declaration(BodyKey key) const {
    const auto found = keys.find(key);
    return found == keys.end() ? nullptr : found->second;
  }

  // The tracked bodies that are not final yet, in resolution order.
  [[nodiscard]] std::vector<const FunctionDecl *> unfinishedBodies() const {
    std::vector<const FunctionDecl *> bodies;
    for (const auto &[declaration, entry] : entries)
      if (entry.state != BodyState::Analysed)
        bodies.push_back(declaration);
    return inResolutionOrder(std::move(bodies));
  }

  // Revision advances whenever a pending body becomes Analysed, so an
  // attempt that began at an older revision may now succeed.
  [[nodiscard]] std::uint64_t revision() const { return currentRevision; }

  void markInProgress(const FunctionDecl &declaration) {
    const auto found = entries.find(&declaration);
    if (found != entries.end() && found->second.state == BodyState::Deferred)
      found->second.state = BodyState::InProgress;
  }
  // Returns whether the body was pending or in progress before.
  bool markAnalysed(const FunctionDecl &declaration) {
    const auto found = entries.find(&declaration);
    if (found == entries.end() || found->second.state == BodyState::Analysed)
      return false;
    found->second.state = BodyState::Analysed;
    ++currentRevision;
    --unfinished;
    return true;
  }

  // The declarations in `bodies` sorted into resolution (tracking) order,
  // without repeats.
  [[nodiscard]] std::vector<const FunctionDecl *>
  inResolutionOrder(std::vector<const FunctionDecl *> bodies) const {
    std::erase_if(bodies, [&](const FunctionDecl *body) {
      return !entries.contains(body);
    });
    std::sort(bodies.begin(), bodies.end(),
              [&](const FunctionDecl *left, const FunctionDecl *right) {
                return entries.at(left).order < entries.at(right).order;
              });
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    return bodies;
  }

  // A child context for a worker that completes bodies privately. It starts
  // from this context's states; its completions never change this context.
  [[nodiscard]] std::shared_ptr<BodyContext> fork(const void *child) const {
    auto forked = std::make_shared<BodyContext>(*this);
    forked->owner = child;
    forked->recordedNeeds.clear();
    forked->openAttempts.clear();
    forked->resolvingAttempts = 0;
    return forked;
  }

  // ---- Needs ---------------------------------------------------------------
  // A demand that could not be met where it arose. It is recorded in the
  // innermost open attempt (see BodyAttemptNeeds), or, with none open, in the
  // request-level record that the frontend's net examines. Every need is
  // recorded, repeats included.
  void need(const FunctionDecl &declaration) {
    ++needsRecorded;
    if (!openAttempts.empty())
      openAttempts.back()->needs.push_back(&declaration);
    else
      recordedNeeds.push_back(&declaration);
  }
  // An attempt whose result may be discarded and repeated collects the needs
  // recorded while it is open, by any analyzer sharing this context.
  struct AttemptNeeds {
    std::vector<const FunctionDecl *> needs;
    // Whether the attempt's owner can resolve its needs and repeat it. While
    // such an attempt is open, a sharing worker records a pending body
    // instead of analysing it privately.
    bool resolving = false;
    // Cache writes made while the attempt is open. They take effect only if
    // it ends clean (see commitWrites), so a result computed while a needed
    // body was unavailable never enters a cache.
    std::vector<std::function<void()>> writes;
  };
  // A write to a cache that outlives the current attempt: journaled in the
  // innermost open attempt, or performed now when none is open.
  void write(std::function<void()> cacheWrite) {
    if (openAttempts.empty())
      cacheWrite();
    else
      openAttempts.back()->writes.push_back(std::move(cacheWrite));
  }
  // A closed attempt that ended clean passes its writes to the enclosing
  // attempt, or performs them.
  void commitWrites(AttemptNeeds &attempt) {
    for (auto &cacheWrite : attempt.writes)
      write(std::move(cacheWrite));
    attempt.writes.clear();
  }
  // Every need ever recorded in this context, monotonically. A computation
  // during which it changed depended on an unavailable body, wherever the
  // need was recorded.
  [[nodiscard]] std::uint64_t needSerial() const { return needsRecorded; }
  void openAttempt(AttemptNeeds &attempt) {
    openAttempts.push_back(&attempt);
    if (attempt.resolving)
      ++resolvingAttempts;
  }
  void closeAttempt(AttemptNeeds &attempt) {
    if (!openAttempts.empty() && openAttempts.back() == &attempt)
      openAttempts.pop_back();
    if (attempt.resolving && resolvingAttempts != 0)
      --resolvingAttempts;
  }
  [[nodiscard]] bool resolvingAttemptOpen() const {
    return resolvingAttempts != 0;
  }
  [[nodiscard]] std::size_t needCount() const { return recordedNeeds.size(); }
  [[nodiscard]] const std::vector<const FunctionDecl *> &needs() const {
    return recordedNeeds;
  }
  [[nodiscard]] std::vector<const FunctionDecl *>
  needsSince(std::size_t mark) const {
    return {recordedNeeds.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(mark, recordedNeeds.size())),
            recordedNeeds.end()};
  }
  void dropNeedsFrom(std::size_t mark) {
    if (mark < recordedNeeds.size())
      recordedNeeds.resize(mark);
  }
  // The needs still pending here, without repeats, in resolution order.
  [[nodiscard]] std::vector<const FunctionDecl *> outstandingNeeds() const {
    std::vector<const FunctionDecl *> outstanding;
    for (const FunctionDecl *body : recordedNeeds)
      if (pending(body) || state(body) == BodyState::InProgress)
        outstanding.push_back(body);
    return inResolutionOrder(std::move(outstanding));
  }

private:
  struct Entry {
    BodyKey key;
    BodyState state = BodyState::Analysed;
    std::size_t order = 0;
  };

  const void *owner = nullptr;
  std::unordered_map<const FunctionDecl *, Entry> entries;
  std::unordered_map<BodyKey, const FunctionDecl *, BodyKeyHash> keys;
  std::vector<const FunctionDecl *> recordedNeeds;
  std::vector<AttemptNeeds *> openAttempts;
  std::size_t resolvingAttempts = 0;
  std::size_t unfinished = 0;
  std::uint64_t needsRecorded = 0;
  std::size_t nextOrder = 0;
  std::uint64_t currentRevision = 0;
};

} // namespace lang
