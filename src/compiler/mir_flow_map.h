#pragma once

// The ordered map MIR carrier flow keeps its per-block state in, and the join
// of the loan-group sets it holds. They live here, apart from the analysis,
// so that tests/unit/mir_flow_map.cpp can check them against std::map.

#include "mir_carrier_flow.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace lang::mir_flow {

// The bits that decide where a flow map starts a new chunk at a key.
template <class K>
  requires std::is_integral_v<K>
[[nodiscard]] std::uint64_t flowKeyBits(K key) {
  return static_cast<std::uint64_t>(key);
}
[[nodiscard]] inline std::uint64_t flowKeyBits(const MirCarrierRoot &root) {
  return static_cast<std::uint64_t>(root.identity) * 31 +
         static_cast<std::uint64_t>(root.kind) * 0x85EBCA77C2B2AE63ull +
         (root.packElement ? *root.packElement + 1 : 0) * 0xC2B2AE3D27D4EB4Full;
}

// A sorted map kept as a shallow tree: a spine of groups, each a run of
// sorted chunks of entries. Flow states are copied for every block visit and
// retained for every block, while one block changes few entries. Copies share
// the spine until one is modified, and a modification replaces only the
// spine, one group and one chunk, so a long body does not pay for its whole
// state at every block. Most facts are empty sets, and contiguous chunks
// avoid one heap node per entry.
//
// Where a chunk starts depends on the keys alone: about one key in
// thirty-two is a boundary key, a boundary key always starts a chunk, and no
// other key does except the map's first. Two maps that hold the same keys
// over a range therefore hold the same chunks over it, however each was
// built, which lets a join carry over every chunk the two still share.
template <class K, class V> class FlatMap {
public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<K, V>;
  using Items = std::vector<value_type>;

private:
  // A chunk's entries, with room for a caller's digest of them (see scan()).
  // Handing the entries out for writing discards the digest.
  struct Block : Items {
    using Items::Items;
    Block() = default;
    // A copy is made to be changed, so it starts without a digest.
    Block(const Block &other) : Items(other) {}
    Block &operator=(const Block &) = delete;
    mutable std::uint64_t digest = 0;
    mutable bool digested = false;
  };
  using Chunk = std::shared_ptr<Block>; // Never null; empty only in passing.
  using Chunks = std::vector<Chunk>;
  using Group = std::shared_ptr<Chunks>; // Never null, never empty.
  struct Spine {
    std::vector<Group> groups;
    std::size_t count = 0;  // Entries.
    std::size_t chunks = 0; // Chunks across every group.
  };
  // A group splits once it holds more than twice this many chunks.
  static constexpr std::size_t groupLimit = 32;
  [[nodiscard]] static bool boundary(const K &key) {
    return (flowKeyBits(key) * 0x9E3779B97F4A7C15ull) >> 59 == 0;
  }

public:
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = FlatMap::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type *;
    using reference = const value_type &;
    const_iterator() = default;
    reference operator*() const { return (*(*(*groups)[group])[chunk])[index]; }
    pointer operator->() const { return &**this; }
    const_iterator &operator++() {
      const Chunks &chunks = *(*groups)[group];
      if (++index == chunks[chunk]->size()) {
        index = 0;
        if (++chunk == chunks.size()) {
          chunk = 0;
          ++group;
        }
      }
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator previous = *this;
      ++*this;
      return previous;
    }
    friend bool operator==(const const_iterator &left,
                           const const_iterator &right) {
      return left.group == right.group && left.chunk == right.chunk &&
             left.index == right.index;
    }

  private:
    friend class FlatMap;
    const_iterator(const std::vector<Group> &groups, std::size_t group,
                   std::size_t chunk, std::size_t index)
        : groups(&groups), group(group), chunk(chunk), index(index) {}
    [[nodiscard]] bool atEnd() const { return group == groups->size(); }
    const std::vector<Group> *groups = nullptr;
    std::size_t group = 0;
    std::size_t chunk = 0;
    std::size_t index = 0;
  };
  // One writable entry, or none. It compares equal to end() when it names
  // none, and does not step: stepping would reach chunks other copies share.
  class iterator {
  public:
    iterator() = default;
    value_type &operator*() const { return *item; }
    value_type *operator->() const { return item; }
    friend bool operator==(const iterator &left, const iterator &right) {
      return left.item == right.item;
    }
    friend bool operator==(const iterator &left, const const_iterator &right) {
      return left.item ? !right.atEnd() && &*right == left.item : right.atEnd();
    }

  private:
    friend class FlatMap;
    explicit iterator(value_type *item) : item(item) {}
    value_type *item = nullptr;
  };

  [[nodiscard]] const_iterator begin() const { return {view(), 0, 0, 0}; }
  [[nodiscard]] const_iterator end() const {
    return {view(), view().size(), 0, 0};
  }
  [[nodiscard]] std::size_t size() const { return shared ? shared->count : 0; }
  [[nodiscard]] bool empty() const { return size() == 0; }
  // A writable entry copies the chunk holding it when that chunk is shared,
  // so lookups that only read go through find() instead.
  [[nodiscard]] iterator edit(const K &key) {
    const auto position = locate(key);
    if (!position.found)
      return iterator{};
    return iterator{&ownChunk(position.group, position.chunk)[position.index]};
  }
  [[nodiscard]] const_iterator find(const K &key) const {
    const auto position = locate(key);
    return position.found ? const_iterator{view(), position.group,
                                           position.chunk, position.index}
                          : end();
  }
  [[nodiscard]] bool contains(const K &key) const { return locate(key).found; }
  // The first entry whose key is not less than `key`.
  [[nodiscard]] const_iterator lowerBound(const K &key) const {
    const auto position = locate(key);
    return position.group == view().size()
               ? end()
               : const_iterator{view(), position.group, position.chunk,
                                position.index};
  }
  // Visits the entries of every chunk whose digest `wanted` accepts.
  // `digest(entries)` summarizes a chunk's entries; it is computed once per
  // chunk and kept until the chunk is next written, so a pass that concerns
  // few entries of a large map need not read the rest.
  template <class Digest, class Wanted, class Visit>
  void scan(Digest &&digest, Wanted &&wanted, Visit &&visit) const {
    for (const Group &group : view())
      for (const Chunk &chunk : *group) {
        if (!chunk->digested) {
          chunk->digest = digest(static_cast<const Items &>(*chunk));
          chunk->digested = true;
        }
        if (wanted(chunk->digest))
          for (const value_type &entry : *chunk)
            visit(entry);
      }
  }
  template <class... Arguments>
  std::pair<iterator, bool> emplace(const K &key, Arguments &&...arguments) {
    auto position = locate(key);
    if (position.found)
      return {
          iterator{&ownChunk(position.group, position.chunk)[position.index]},
          false};
    if (view().empty()) {
      ownSpine().groups.push_back(
          std::make_shared<Chunks>(1, std::make_shared<Block>()));
      ++shared->chunks;
      Items &items = *shared->groups.front()->front();
      items.emplace_back(key, V(std::forward<Arguments>(arguments)...));
      ++shared->count;
      return {iterator{&items.front()}, true};
    }
    if (position.group == view().size()) {
      // Past every key: the end of the last chunk.
      position.group = view().size() - 1;
      position.chunk = view()[position.group]->size() - 1;
      position.index = view()[position.group]->back()->size();
    } else if (position.index == 0 &&
               (position.group != 0 || position.chunk != 0)) {
      // Before a chunk's first key: the end of the chunk before it.
      if (position.chunk != 0) {
        --position.chunk;
      } else {
        --position.group;
        position.chunk = view()[position.group]->size() - 1;
      }
      position.index = (*view()[position.group])[position.chunk]->size();
    }
    Items *items = &ownChunk(position.group, position.chunk);
    ++shared->count;
    if (position.index == 0 && boundary(items->front().first)) {
      // Ahead of a first chunk that starts at a boundary key: a first chunk
      // of its own.
      Chunks &chunks = *shared->groups.front();
      chunks.insert(chunks.begin(), std::make_shared<Block>());
      ++shared->chunks;
      items = chunks.front().get();
      items->emplace_back(key, V(std::forward<Arguments>(arguments)...));
      return {iterator{&items->front()}, true};
    }
    if (!boundary(key) || position.index == 0) {
      // An ordinary key joins the chunk it falls in, and so does a boundary
      // key ahead of every other key.
      const auto inserted = items->emplace(
          items->begin() + static_cast<std::ptrdiff_t>(position.index), key,
          V(std::forward<Arguments>(arguments)...));
      return {iterator{&*inserted}, true};
    }
    // A boundary key starts a chunk that takes the entries after it.
    auto upper = std::make_shared<Block>();
    upper->reserve(items->size() - position.index + 1);
    upper->emplace_back(key, V(std::forward<Arguments>(arguments)...));
    upper->insert(
        upper->end(),
        std::make_move_iterator(items->begin() +
                                static_cast<std::ptrdiff_t>(position.index)),
        std::make_move_iterator(items->end()));
    items->erase(items->begin() + static_cast<std::ptrdiff_t>(position.index),
                 items->end());
    items = upper.get();
    Chunks &chunks = *shared->groups[position.group];
    chunks.insert(chunks.begin() +
                      static_cast<std::ptrdiff_t>(position.chunk + 1),
                  std::move(upper));
    ++shared->chunks;
    if (chunks.size() > 2 * groupLimit) {
      const std::size_t half = chunks.size() / 2;
      auto upperGroup = std::make_shared<Chunks>(
          std::make_move_iterator(chunks.begin() +
                                  static_cast<std::ptrdiff_t>(half)),
          std::make_move_iterator(chunks.end()));
      chunks.resize(half);
      auto &groups = shared->groups;
      groups.insert(groups.begin() +
                        static_cast<std::ptrdiff_t>(position.group + 1),
                    std::move(upperGroup));
    }
    return {iterator{&items->front()}, true};
  }
  V &operator[](const K &key) { return emplace(key).first->second; }
  bool erase(const K &key) {
    const auto position = locate(key);
    if (!position.found)
      return false;
    Items &owned = ownChunk(position.group, position.chunk);
    owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(position.index));
    --shared->count;
    if (!settle(position.group, position.chunk) &&
        shared->groups[position.group]->empty())
      shared->groups.erase(shared->groups.begin() +
                           static_cast<std::ptrdiff_t>(position.group));
    return true;
  }
  template <class Predicate> void eraseIf(Predicate predicate) {
    for (std::size_t group = 0; group < view().size();) {
      for (std::size_t chunk = 0; chunk < view()[group]->size();) {
        const Items &items = *(*view()[group])[chunk];
        if (std::none_of(items.begin(), items.end(), predicate)) {
          ++chunk;
          continue;
        }
        shared->count -= std::erase_if(ownChunk(group, chunk), predicate);
        if (settle(group, chunk))
          ++chunk;
      }
      if (view()[group]->empty())
        shared->groups.erase(shared->groups.begin() +
                             static_cast<std::ptrdiff_t>(group));
      else
        ++group;
    }
  }
  // Union in one ordered pass: keys only in `incoming` are added with their
  // value, and keys on both sides join. Returns whether this map changed.
  // `changes(into, from)` tells whether `joinValues(into, from)` would
  // change `into`, without changing it.
  template <class Join, class Changes>
  bool unionWith(const FlatMap &incoming, Join &&joinValues,
                 Changes &&changes) {
    return merge(incoming, joinValues, changes, false);
  }
  // Path join in one ordered pass: a key absent from either side becomes
  // unknown (a default value, which every join keeps unknown), and keys on
  // both sides join their values. Returns whether this map changed.
  template <class Join, class Changes>
  bool joinPaths(const FlatMap &incoming, Join &&joinValues,
                 Changes &&changes) {
    return merge(incoming, joinValues, changes, true);
  }

private:
  struct Position {
    std::size_t group = 0;
    std::size_t chunk = 0;
    std::size_t index = 0;
    bool found = false;
  };
  [[nodiscard]] const std::vector<Group> &view() const {
    static const std::vector<Group> none;
    return shared ? shared->groups : none;
  }
  // The first entry whose key is not less than `key`; group == size() when
  // every key is less.
  [[nodiscard]] Position locate(const K &key) const {
    const auto &groups = view();
    const auto group = std::partition_point(
        groups.begin(), groups.end(), [&](const Group &chunks) {
          return chunks->back()->back().first < key;
        });
    if (group == groups.end())
      return {.group = groups.size()};
    const auto chunk = std::partition_point(
        (*group)->begin(), (*group)->end(),
        [&](const Chunk &items) { return items->back().first < key; });
    const auto item =
        std::lower_bound((*chunk)->begin(), (*chunk)->end(), key,
                         [](const value_type &entry, const K &probe) {
                           return entry.first < probe;
                         });
    return {.group = static_cast<std::size_t>(group - groups.begin()),
            .chunk = static_cast<std::size_t>(chunk - (*group)->begin()),
            .index = static_cast<std::size_t>(item - (*chunk)->begin()),
            .found = !(key < item->first)};
  }
  [[nodiscard]] Spine &ownSpine() {
    if (!shared)
      shared = std::make_shared<Spine>();
    else if (shared.use_count() > 1)
      shared = std::make_shared<Spine>(*shared);
    return *shared;
  }
  [[nodiscard]] Chunks &ownGroup(std::size_t index) {
    Group &group = ownSpine().groups[index];
    if (group.use_count() > 1)
      group = std::make_shared<Chunks>(*group);
    return *group;
  }
  [[nodiscard]] Items &ownChunk(std::size_t group, std::size_t index) {
    Chunk &chunk = ownGroup(group)[index];
    if (chunk.use_count() > 1)
      chunk = std::make_shared<Block>(*chunk);
    chunk->digested = false;
    return *chunk;
  }
  // Restores the chunk form after entries left an owned chunk: a chunk that
  // lost its boundary key gives what is left to the chunk before it, and an
  // emptied chunk goes. Returns whether the chunk is still there; the caller
  // removes a group this empties.
  bool settle(std::size_t group, std::size_t chunk) {
    Chunks &chunks = *shared->groups[group];
    Items &owned = *chunks[chunk];
    const bool first = group == 0 && chunk == 0;
    if (!owned.empty() && (first || boundary(owned.front().first)))
      return true;
    if (!owned.empty()) {
      const std::size_t beforeGroup = chunk != 0 ? group : group - 1;
      const std::size_t beforeChunk =
          chunk != 0 ? chunk - 1 : view()[beforeGroup]->size() - 1;
      Items &before = ownChunk(beforeGroup, beforeChunk);
      before.insert(before.end(), std::make_move_iterator(owned.begin()),
                    std::make_move_iterator(owned.end()));
    }
    chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(chunk));
    --shared->chunks;
    return false;
  }
  [[nodiscard]] static std::vector<Group> grouped(std::vector<Chunk> chunks) {
    std::vector<Group> groups;
    for (std::size_t first = 0; first < chunks.size(); first += groupLimit) {
      const std::size_t last = std::min(chunks.size(), first + groupLimit);
      groups.push_back(std::make_shared<Chunks>(
          std::make_move_iterator(chunks.begin() +
                                  static_cast<std::ptrdiff_t>(first)),
          std::make_move_iterator(chunks.begin() +
                                  static_cast<std::ptrdiff_t>(last))));
    }
    return groups;
  }
  // The ordered merge behind both joins. A group or chunk both maps share
  // holds equal entries, which no join changes, so it is carried over whole;
  // a union also carries over a chunk whose key range the other map does not
  // reach. Nothing is copied or counted until the merge is known to change
  // this map: most joins change nothing.
  template <class Join, class Changes>
  bool merge(const FlatMap &incoming, Join &joinValues, Changes &changes,
             bool paths) {
    if (shared == incoming.shared)
      return false;
    const auto &mineGroups = view();
    const auto &theirGroups = incoming.view();
    std::size_t prefix = 0;
    while (prefix < mineGroups.size() && prefix < theirGroups.size() &&
           mineGroups[prefix] == theirGroups[prefix])
      ++prefix;
    if (prefix == mineGroups.size() && prefix == theirGroups.size())
      return false;
    const auto flat = [&](const std::vector<Group> &groups) {
      std::vector<const Chunk *> chunks;
      for (std::size_t group = prefix; group < groups.size(); ++group)
        for (const Chunk &chunk : *groups[group])
          chunks.push_back(&chunk);
      return chunks;
    };
    const auto mine = flat(mineGroups);
    const auto theirs = flat(theirGroups);
    bool changed = false;
    // The result past the shared prefix, in the same chunk form: a chunk
    // carried over as it is, or one built here.
    struct Piece {
      const Chunk *carried = nullptr;
      Chunk fresh;
    };
    std::vector<Piece> pieces;
    const auto append = [&](const K &key, const V &value) -> V & {
      if (pieces.empty() || boundary(key)) {
        pieces.push_back({.fresh = std::make_shared<Block>()});
      } else if (Piece &last = pieces.back(); last.carried) {
        last.fresh = std::make_shared<Block>(**last.carried);
        last.carried = nullptr;
      }
      return pieces.back().fresh->emplace_back(key, value).second;
    };
    const auto carry = [&](const Chunk *chunk) {
      if (pieces.empty() || boundary((*chunk)->front().first)) {
        pieces.push_back({.carried = chunk});
        return;
      }
      for (const auto &[key, value] : **chunk)
        append(key, value);
    };
    std::size_t left = 0, leftItem = 0, right = 0, rightItem = 0;
    const auto step = [](const std::vector<const Chunk *> &chunks,
                         std::size_t &chunk, std::size_t &item) {
      if (++item == (*chunks[chunk])->size()) {
        ++chunk;
        item = 0;
      }
    };
    while (left != mine.size() || right != theirs.size()) {
      const bool leftDone = left == mine.size();
      const bool rightDone = right == theirs.size();
      if (!leftDone && !rightDone && leftItem == 0 && rightItem == 0 &&
          *mine[left] == *theirs[right]) {
        carry(mine[left]);
        ++left;
        ++right;
        continue;
      }
      // Two chunks over the same keys that the join leaves as they are:
      // a join into a block many paths reach meets these on every path,
      // and they need no copy.
      if (!leftDone && !rightDone && leftItem == 0 && rightItem == 0 &&
          (*mine[left])->size() == (*theirs[right])->size() &&
          std::equal((*mine[left])->begin(), (*mine[left])->end(),
                     (*theirs[right])->begin(),
                     [&](const value_type &into, const value_type &from) {
                       return !(into.first < from.first) &&
                              !(from.first < into.first) &&
                              !changes(into.second, from.second);
                     })) {
        carry(mine[left]);
        ++left;
        ++right;
        continue;
      }
      if (!paths && !leftDone && leftItem == 0 &&
          (rightDone ||
           (*mine[left])->back().first < (**theirs[right])[rightItem].first)) {
        carry(mine[left++]);
        continue;
      }
      if (!paths && !rightDone && rightItem == 0 &&
          (leftDone ||
           (*theirs[right])->back().first < (**mine[left])[leftItem].first)) {
        carry(theirs[right++]);
        changed = true;
        continue;
      }
      const value_type *mineItem =
          leftDone ? nullptr : &(**mine[left])[leftItem];
      const value_type *theirItem =
          rightDone ? nullptr : &(**theirs[right])[rightItem];
      if (!theirItem || (mineItem && mineItem->first < theirItem->first)) {
        V &value = append(mineItem->first, mineItem->second);
        if (paths && value) {
          value.reset();
          changed = true;
        }
        step(mine, left, leftItem);
      } else if (!mineItem || theirItem->first < mineItem->first) {
        append(theirItem->first, paths ? V{} : theirItem->second);
        changed = true;
        step(theirs, right, rightItem);
      } else {
        V &value = append(mineItem->first, mineItem->second);
        changed = joinValues(value, theirItem->second) || changed;
        step(mine, left, leftItem);
        step(theirs, right, rightItem);
      }
    }
    if (!changed)
      return false;
    Spine merged;
    merged.groups.assign(mineGroups.begin(),
                         mineGroups.begin() +
                             static_cast<std::ptrdiff_t>(prefix));
    for (const Group &group : merged.groups) {
      merged.chunks += group->size();
      for (const Chunk &chunk : *group)
        merged.count += chunk->size();
    }
    std::vector<Chunk> rest;
    rest.reserve(pieces.size());
    for (Piece &piece : pieces) {
      rest.push_back(piece.carried ? *piece.carried : std::move(piece.fresh));
      merged.count += rest.back()->size();
    }
    merged.chunks += rest.size();
    for (Group &group : grouped(std::move(rest)))
      merged.groups.push_back(std::move(group));
    shared = std::make_shared<Spine>(std::move(merged));
    return true;
  }
  std::shared_ptr<Spine> shared;
};

// Adds `from` to the set `into`. Unknown (no value) absorbs everything, and
// an unknown `from` makes `into` unknown. Returns whether `into` changed.
template <class T>
bool join(std::optional<std::vector<T>> &into,
          const std::optional<std::vector<T>> &from) {
  if (!into)
    return false;
  if (!from) {
    into.reset();
    return true;
  }
  if (from->empty())
    return false;
  // Facts are kept as sorted unique sets; adding elements is a change
  // exactly when the normalized set grows.
  bool changed = false;
  if (!std::is_sorted(into->begin(), into->end()) ||
      std::adjacent_find(into->begin(), into->end()) != into->end()) {
    std::sort(into->begin(), into->end());
    into->erase(std::unique(into->begin(), into->end()), into->end());
    changed = true;
  }
  const auto before = into->size();
  into->insert(into->end(), from->begin(), from->end());
  std::sort(into->begin(), into->end());
  into->erase(std::unique(into->begin(), into->end()), into->end());
  return changed || into->size() != before;
}
// Whether join(into, from) would change `into`.
template <class T>
bool joinChanges(const std::optional<std::vector<T>> &into,
                 const std::optional<std::vector<T>> &from) {
  if (!into)
    return false;
  if (!from)
    return true;
  if (from->empty())
    return false;
  if (!std::is_sorted(into->begin(), into->end()) ||
      std::adjacent_find(into->begin(), into->end()) != into->end())
    return true;
  return std::any_of(from->begin(), from->end(), [&](const T &element) {
    return !std::binary_search(into->begin(), into->end(), element);
  });
}

} // namespace lang::mir_flow
