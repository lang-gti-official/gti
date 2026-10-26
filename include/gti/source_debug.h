#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace lang {

enum class SourceDebugProfile : std::uint8_t { None, LexicalV1 };

enum class SourceDebugCollection : std::uint8_t {
  Uncollected,
  CollectedLexicalV1,
  NotApplicable,
};

[[nodiscard]] constexpr bool
isValidSourceDebugProfile(SourceDebugProfile profile) {
  switch (profile) {
  case SourceDebugProfile::None:
  case SourceDebugProfile::LexicalV1:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view
sourceDebugProfileName(SourceDebugProfile profile) {
  switch (profile) {
  case SourceDebugProfile::None:
    return "none";
  case SourceDebugProfile::LexicalV1:
    return "lexical-v1";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
sourceDebugCollectionName(SourceDebugCollection collection) {
  switch (collection) {
  case SourceDebugCollection::Uncollected:
    return "uncollected";
  case SourceDebugCollection::CollectedLexicalV1:
    return "collected-lexical-v1";
  case SourceDebugCollection::NotApplicable:
    return "not-applicable";
  }
  return "invalid";
}

// One phase body's descriptive source facts. Collection state is independent
// of row count. Copies own their tables; absent collection embeds no table
// vectors and allocates no tables. This state alone proves no source identity,
// lexical membership, or execution availability.
template <typename Tables> struct SourceDebugRecord {
  SourceDebugCollection collection = SourceDebugCollection::Uncollected;
  std::unique_ptr<Tables> tables;

  SourceDebugRecord() = default;

  SourceDebugRecord(const SourceDebugRecord &other)
      : collection(other.collection),
        tables(other.tables ? std::make_unique<Tables>(*other.tables) : nullptr) {}

  SourceDebugRecord &operator=(const SourceDebugRecord &other) {
    if (this != &other) {
      SourceDebugRecord copy(other);
      swap(copy);
    }
    return *this;
  }

  SourceDebugRecord(SourceDebugRecord &&other) noexcept
      : collection(std::exchange(other.collection,
                                 SourceDebugCollection::Uncollected)),
        tables(std::move(other.tables)) {}

  SourceDebugRecord &operator=(SourceDebugRecord &&other) noexcept {
    if (this != &other) {
      collection = std::exchange(other.collection,
                                 SourceDebugCollection::Uncollected);
      tables = std::move(other.tables);
    }
    return *this;
  }

  void swap(SourceDebugRecord &other) noexcept {
    std::swap(collection, other.collection);
    tables.swap(other.tables);
  }

  friend void swap(SourceDebugRecord &left, SourceDebugRecord &right) noexcept {
    left.swap(right);
  }

  friend bool operator==(const SourceDebugRecord &left,
                         const SourceDebugRecord &right) {
    if (left.collection != right.collection ||
        static_cast<bool>(left.tables) != static_cast<bool>(right.tables))
      return false;
    return !left.tables || *left.tables == *right.tables;
  }
};

// This is only the collection/table invariant. Phase verifiers separately
// check the requested profile, exact body role, provenance, and table contents.
template <typename Tables>
[[nodiscard]] bool
isValidSourceDebugRecord(const SourceDebugRecord<Tables> &record) {
  switch (record.collection) {
  case SourceDebugCollection::CollectedLexicalV1:
    return record.tables != nullptr;
  case SourceDebugCollection::Uncollected:
  case SourceDebugCollection::NotApplicable:
    return record.tables == nullptr;
  }
  return false;
}

} // namespace lang
