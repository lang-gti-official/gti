#pragma once

#include "gti/source_graph.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

// Declaration ownership is independent of whether an API is exported. Files
// without a named module retain their own domain; source-local static names
// continue to use the separate SourceUnitId indexes.
struct DeclarationOwner {
  ModuleId module = 0;
  SourceUnitId source = 0;
  auto operator<=>(const DeclarationOwner &) const = default;
};

struct OwnedDeclarationName {
  std::string spelling;
  DeclarationOwner owner;
  auto operator<=>(const OwnedDeclarationName &) const = default;
  operator const std::string &() const { return spelling; }
  operator std::string_view() const { return spelling; }
};

// One authoritative owner/name index. An unqualified find is intentionally a
// catalog query (for diagnostics); accepted lookup uses a requester-visible
// index or an explicit owner. Visible indexes use the empty owner and retain
// their existing name lookup interface.
//
// Copies are deep. shareFrom() instead makes this index read the source's
// storage until its first non-const access, which takes a private copy. That
// is only sound while the source is not accessed at all (a nested analysis
// worker whose demanding analyzer is suspended): the source's storage, and
// every pointer into it, is then never written through this index, and the
// source becomes its storage's only holder again once this index is
// released, cleared or reassigned. Every non-const member detaches first,
// including begin(), end() and find(), because their results can be written
// through.
template <class Value> class DeclarationIndex {
  using Storage = std::map<OwnedDeclarationName, Value>;
  using Alternatives = std::map<std::string, std::vector<Value>>;
  struct Data {
    Storage values;
    Alternatives alternatives;
    // Names one period in which this storage is shared and so unchanged;
    // see sharedVersion().
    std::uint64_t version = nextGeneration();
    Data() = default;
    Data(const Data &other)
        : values(other.values), alternatives(other.alternatives) {}
    Data &operator=(const Data &other) {
      values = other.values;
      alternatives = other.alternatives;
      version = nextGeneration();
      return *this;
    }
  };
  // Null means empty.
  std::shared_ptr<Data> data;
  // Changes whenever this index's membership may have changed: on creation,
  // copy, move, sharing, detaching and every insertion or clear. Generations
  // come from one process-wide sequence, so a copied or reassigned index
  // never reuses a value another index state had. Lookups that memoise a
  // miss or a binding pointer across the index key on it; in-place edits of
  // a stored value do not change it.
  std::uint64_t generation_ = nextGeneration();

  static std::uint64_t nextGeneration() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }
  void touch() { generation_ = nextGeneration(); }
  static const Data &emptyData() {
    static const Data empty;
    return empty;
  }
  const Data &read() const { return data ? *data : emptyData(); }
  Data &write() {
    if (!data) {
      data = std::make_shared<Data>();
    } else if (data.use_count() > 1) {
      data = std::make_shared<Data>(*data);
      // Pointers memoised from the shared storage do not designate this
      // index's private copy.
      touch();
    }
    return *data;
  }
  static std::shared_ptr<Data> copyOf(const DeclarationIndex &other) {
    return other.data && !(other.data->values.empty() &&
                           other.data->alternatives.empty())
               ? std::make_shared<Data>(*other.data)
               : nullptr;
  }

public:
  DeclarationIndex() = default;
  DeclarationIndex(const DeclarationIndex &other) : data(copyOf(other)) {}
  DeclarationIndex(DeclarationIndex &&other) noexcept
      : data(std::move(other.data)) {
    other.touch();
  }
  DeclarationIndex &operator=(const DeclarationIndex &other) {
    if (this != &other) {
      // Reuse a private copy's nodes, as a map assignment does.
      if (data && data.use_count() == 1 && other.data)
        *data = *other.data;
      else
        data = copyOf(other);
    }
    touch();
    return *this;
  }
  DeclarationIndex &operator=(DeclarationIndex &&other) noexcept {
    data = std::move(other.data);
    touch();
    other.touch();
    return *this;
  }
  ~DeclarationIndex() = default;

  // Read the source's storage until the first non-const access; see above.
  void shareFrom(const DeclarationIndex &source) {
    // Storage that only the source held may have been written through
    // pointers the source kept, so its sharing starts a new version.
    if (source.data && source.data.use_count() == 1)
      source.data->version = nextGeneration();
    data = source.data;
    touch();
  }
  // Whether this index still reads storage another index holds.
  bool sharesStorage() const { return data && data.use_count() > 1; }
  // While storage is shared nothing writes it: every holder but the
  // innermost is suspended, and the innermost copies before writing. Facts
  // derived from the stored values hold for as long as this version is
  // reported; versions are never reused. Unshared storage has no version.
  std::optional<std::uint64_t> sharedVersion() const {
    if (!sharesStorage())
      return std::nullopt;
    return data->version;
  }

  std::uint64_t generation() const { return generation_; }

  using iterator = typename Storage::iterator;
  using const_iterator = typename Storage::const_iterator;

  auto begin() { return write().values.begin(); }
  auto end() { return write().values.end(); }
  auto begin() const { return read().values.begin(); }
  auto end() const { return read().values.end(); }
  auto size() const { return read().values.size(); }
  bool empty() const { return read().values.empty(); }
  void clear() {
    touch();
    // Releasing shared storage leaves its other holders untouched.
    data.reset();
  }
  template <class Visitor> void forEachValue(Visitor &&visitor) {
    auto &current = write();
    for (auto &[_, value] : current.values)
      visitor(value);
    for (auto &[_, candidates] : current.alternatives)
      for (auto &candidate : candidates)
        visitor(candidate);
  }
  template <class Visitor> void forEachValue(Visitor &&visitor) const {
    const auto &current = read();
    for (const auto &[_, value] : current.values)
      visitor(value);
    for (const auto &[_, candidates] : current.alternatives)
      for (const auto &candidate : candidates)
        visitor(candidate);
  }
  // Edit exactly the values the predicate selects. The predicate sees every
  // value read-only, so storage shared by shareFrom() is copied only when
  // some value is actually edited.
  template <class Predicate, class Editor>
  void editValuesIf(Predicate &&selected, Editor &&edit) {
    bool any = false;
    static_cast<const DeclarationIndex &>(*this).forEachValue(
        [&](const Value &value) { any = any || selected(value); });
    if (!any)
      return;
    forEachValue([&](Value &value) {
      if (selected(static_cast<const Value &>(value)))
        edit(value);
    });
  }
  void addAlternative(const std::string &name, const Value &value) {
    touch();
    auto &current = write();
    auto &candidates = current.alternatives[name];
    if (candidates.empty())
      candidates.push_back(current.values.find({name, {}})->second);
    candidates.push_back(value);
  }
  const std::vector<Value> *ambiguousCandidates(const std::string &name) const {
    const auto &alternatives = read().alternatives;
    const auto found = alternatives.find(name);
    return found == alternatives.end() ? nullptr : &found->second;
  }

  auto find(const std::string &name, DeclarationOwner owner) {
    return write().values.find({name, owner});
  }
  auto find(const std::string &name, DeclarationOwner owner) const {
    return read().values.find({name, owner});
  }
  auto find(const OwnedDeclarationName &name) {
    return write().values.find(name);
  }
  auto find(const OwnedDeclarationName &name) const {
    return read().values.find(name);
  }
  auto find(const std::string &name) {
    auto &current = write();
    if (current.alternatives.contains(name))
      return current.values.end();
    const auto found = current.values.lower_bound({name, {}});
    return found != current.values.end() && found->first.spelling == name
               ? found
               : current.values.end();
  }
  auto find(const std::string &name) const {
    const auto &current = read();
    if (current.alternatives.contains(name))
      return current.values.end();
    const auto found = current.values.lower_bound({name, {}});
    return found != current.values.end() && found->first.spelling == name
               ? found
               : current.values.end();
  }
  bool contains(const std::string &name) const { return find(name) != end(); }
  bool contains(const std::string &name, DeclarationOwner owner) const {
    return find(name, owner) != end();
  }
  auto emplace(std::string name, Value value, DeclarationOwner owner = {}) {
    touch();
    return write().values.emplace(OwnedDeclarationName{std::move(name), owner},
                                  std::move(value));
  }
  auto emplace(OwnedDeclarationName name, Value value) {
    touch();
    return write().values.emplace(std::move(name), std::move(value));
  }
  auto try_emplace(OwnedDeclarationName name, const Value &value) {
    touch();
    return write().values.try_emplace(std::move(name), value);
  }
  auto try_emplace(std::string name, const Value &value) {
    touch();
    return write().values.try_emplace(OwnedDeclarationName{std::move(name), {}},
                                      value);
  }
  auto insert_or_assign(std::string name, Value value,
                        DeclarationOwner owner = {}) {
    touch();
    return write().values.insert_or_assign(
        OwnedDeclarationName{std::move(name), owner}, std::move(value));
  }
  Value &operator[](const std::string &name) {
    touch();
    return write().values[{name, {}}];
  }
};

} // namespace lang
