#pragma once

#include "gti/source_graph.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <map>
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
template <class Value> class DeclarationIndex {
  using Storage = std::map<OwnedDeclarationName, Value>;
  Storage values;
  std::map<std::string, std::vector<Value>> alternatives;
  // Changes whenever this index's membership may have changed: on creation,
  // copy, move and every insertion or clear. Generations come from one
  // process-wide sequence, so a copied or reassigned index never reuses a
  // value another index state had. Lookups that memoise a miss across the
  // index key on it; in-place edits of a stored value do not change it.
  std::uint64_t generation_ = nextGeneration();

  static std::uint64_t nextGeneration() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }
  void touch() { generation_ = nextGeneration(); }

public:
  DeclarationIndex() = default;
  DeclarationIndex(const DeclarationIndex &other)
      : values(other.values), alternatives(other.alternatives) {}
  DeclarationIndex(DeclarationIndex &&other) noexcept
      : values(std::move(other.values)),
        alternatives(std::move(other.alternatives)) {
    other.touch();
  }
  DeclarationIndex &operator=(const DeclarationIndex &other) {
    values = other.values;
    alternatives = other.alternatives;
    touch();
    return *this;
  }
  DeclarationIndex &operator=(DeclarationIndex &&other) noexcept {
    values = std::move(other.values);
    alternatives = std::move(other.alternatives);
    touch();
    other.touch();
    return *this;
  }
  ~DeclarationIndex() = default;

  std::uint64_t generation() const { return generation_; }

  using iterator = typename Storage::iterator;
  using const_iterator = typename Storage::const_iterator;

  auto begin() { return values.begin(); }
  auto end() { return values.end(); }
  auto begin() const { return values.begin(); }
  auto end() const { return values.end(); }
  auto size() const { return values.size(); }
  bool empty() const { return values.empty(); }
  void clear() {
    touch();
    values.clear();
    alternatives.clear();
  }
  template <class Visitor> void forEachValue(Visitor &&visitor) {
    for (auto &[_, value] : values)
      visitor(value);
    for (auto &[_, candidates] : alternatives)
      for (auto &candidate : candidates)
        visitor(candidate);
  }
  void addAlternative(const std::string &name, const Value &value) {
    touch();
    auto &candidates = alternatives[name];
    if (candidates.empty())
      candidates.push_back(values.find({name, {}})->second);
    candidates.push_back(value);
  }
  const std::vector<Value> *ambiguousCandidates(const std::string &name) const {
    const auto found = alternatives.find(name);
    return found == alternatives.end() ? nullptr : &found->second;
  }

  auto find(const std::string &name, DeclarationOwner owner) {
    return values.find({name, owner});
  }
  auto find(const std::string &name, DeclarationOwner owner) const {
    return values.find({name, owner});
  }
  auto find(const OwnedDeclarationName &name) { return values.find(name); }
  auto find(const OwnedDeclarationName &name) const {
    return values.find(name);
  }
  auto find(const std::string &name) {
    if (alternatives.contains(name))
      return end();
    const auto found = values.lower_bound({name, {}});
    return found != values.end() && found->first.spelling == name ? found
                                                                  : end();
  }
  auto find(const std::string &name) const {
    if (alternatives.contains(name))
      return end();
    const auto found = values.lower_bound({name, {}});
    return found != values.end() && found->first.spelling == name ? found
                                                                  : end();
  }
  bool contains(const std::string &name) const { return find(name) != end(); }
  bool contains(const std::string &name, DeclarationOwner owner) const {
    return find(name, owner) != end();
  }
  auto emplace(std::string name, Value value, DeclarationOwner owner = {}) {
    touch();
    return values.emplace(OwnedDeclarationName{std::move(name), owner},
                          std::move(value));
  }
  auto emplace(OwnedDeclarationName name, Value value) {
    touch();
    return values.emplace(std::move(name), std::move(value));
  }
  auto try_emplace(OwnedDeclarationName name, const Value &value) {
    touch();
    return values.try_emplace(std::move(name), value);
  }
  auto try_emplace(std::string name, const Value &value) {
    touch();
    return values.try_emplace(OwnedDeclarationName{std::move(name), {}}, value);
  }
  auto insert_or_assign(std::string name, Value value,
                        DeclarationOwner owner = {}) {
    touch();
    return values.insert_or_assign(OwnedDeclarationName{std::move(name), owner},
                                   std::move(value));
  }
  Value &operator[](const std::string &name) {
    touch();
    return values[{name, {}}];
  }
};

} // namespace lang
