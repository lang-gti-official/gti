#include "gti/instance_analysis_cache.h"

#include <algorithm>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>

namespace lang {
namespace {

void combine(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

void hashValue(std::size_t &seed, const CompileTimeValue &value) {
  combine(seed, static_cast<std::size_t>(value.kind));
  combine(seed, static_cast<std::size_t>(value.value));
  combine(seed, value.parameterId);
  combine(seed, value.expression ? 1U : 0U);
}

void hashType(std::size_t &seed, const SemanticType &type) {
  combine(seed, static_cast<std::size_t>(type.kind));
  combine(seed, static_cast<std::size_t>(type.nativeCScalar));
  combine(seed, type.classId);
  combine(seed, type.enumId);
  combine(seed, type.genericParameterId);
  combine(seed, type.functionId);
  combine(seed, type.lambdaId);
  combine(seed, static_cast<std::size_t>(type.arrayLength));
  combine(seed, static_cast<std::size_t>(type.referenceAccess));
  combine(seed, static_cast<std::size_t>(type.pointerAccess));
  combine(seed, type.arguments.size());
  for (const SemanticType &argument : type.arguments)
    hashType(seed, argument);
  combine(seed, type.valueArguments.size());
  for (const CompileTimeValue &value : type.valueArguments)
    hashValue(seed, value);
}

struct KeyHash {
  std::size_t operator()(const InstanceAnalysisKey &key) const {
    std::size_t seed = static_cast<std::size_t>(key.kind);
    combine(seed, key.declaration);
    for (const auto *types : {&key.classTypes, &key.types}) {
      combine(seed, types->size());
      for (const SemanticType &type : *types)
        hashType(seed, type);
    }
    for (const auto *values : {&key.classValues, &key.values}) {
      combine(seed, values->size());
      for (const CompileTimeValue &value : *values)
        hashValue(seed, value);
    }
    return seed;
  }
};

} // namespace

class InstanceAnalysisGeneration final {
public:
  InstanceAnalysisGeneration(InstanceAnalysisGenerationKey key,
                             std::vector<StmtPtr> syntax)
      : key(std::move(key)), syntax(std::move(syntax)) {}

  InstanceAnalysisGenerationKey key;
  // Keeps every trusted unit's parsed declarations (and so every AST
  // identity an entry keys on) alive for the generation's lifetime.
  std::vector<StmtPtr> syntax;
};

class InstanceAnalysisCache::Impl final {
public:
  explicit Impl(InstanceAnalysisCacheLimits limits) : limits(limits) {}

  struct Entry {
    std::shared_ptr<const InstanceAnalysisGeneration> generation;
    InstanceAnalysisKey key;
    std::shared_ptr<const InstanceAnalysisCacheEntry> value;
  };

  struct GenerationEntries {
    std::shared_ptr<const InstanceAnalysisGeneration> generation;
    std::unordered_map<InstanceAnalysisKey, std::list<Entry>::iterator, KeyHash>
        index;
    std::uint64_t lastUse = 0;
  };

  GenerationEntries *find(const InstanceAnalysisGeneration *generation) {
    for (GenerationEntries &entries : generations)
      if (entries.generation.get() == generation)
        return &entries;
    return nullptr;
  }

  void erase(std::list<Entry>::iterator entry) {
    if (GenerationEntries *owner = find(entry->generation.get()))
      owner->index.erase(entry->key);
    estimatedBytes -= entry->value->estimatedBytes;
    entries.erase(entry);
  }

  void eraseGeneration(std::size_t position) {
    const InstanceAnalysisGeneration *generation =
        generations[position].generation.get();
    for (auto entry = entries.begin(); entry != entries.end();) {
      if (entry->generation.get() == generation) {
        estimatedBytes -= entry->value->estimatedBytes;
        entry = entries.erase(entry);
      } else {
        ++entry;
      }
    }
    generations.erase(generations.begin() +
                      static_cast<std::ptrdiff_t>(position));
  }

  void evict() {
    while (!entries.empty() && (entries.size() > limits.maximumEntries ||
                                estimatedBytes > limits.maximumEstimatedBytes))
      erase(entries.begin());
  }

  InstanceAnalysisCacheLimits limits;
  mutable std::mutex mutex;
  std::unordered_map<const VariableDecl *, std::shared_ptr<const ExprPtr>>
      anchors;
  std::uint64_t clock = 0;
  std::size_t estimatedBytes = 0;
  // Least recently used first.
  std::list<Entry> entries;
  std::vector<GenerationEntries> generations;
};

InstanceAnalysisCache::InstanceAnalysisCache(InstanceAnalysisCacheLimits limits)
    : impl(std::make_unique<Impl>(limits)) {}

InstanceAnalysisCache::~InstanceAnalysisCache() = default;

InstanceAnalysisCacheStats InstanceAnalysisCache::stats() const {
  const std::lock_guard lock(impl->mutex);
  return {.entries = impl->entries.size(),
          .estimatedBytes = impl->estimatedBytes,
          .generations = impl->generations.size(),
          .limits = impl->limits};
}

void InstanceAnalysisCache::clear() {
  const std::lock_guard lock(impl->mutex);
  impl->entries.clear();
  impl->generations.clear();
  impl->anchors.clear();
  impl->estimatedBytes = 0;
}

std::shared_ptr<const ExprPtr>
InstanceAnalysisCache::implicitFieldInitializer(const VariableDecl &field) {
  const std::lock_guard lock(impl->mutex);
  const auto same = [](const Token &left, const Token &right) {
    return left.kind == right.kind && left.lexeme == right.lexeme &&
           left.position == right.position && left.line == right.line &&
           left.source == right.source && left.expansion == right.expansion &&
           left.generated == right.generated;
  };
  if (const auto found = impl->anchors.find(&field);
      found != impl->anchors.end()) {
    const auto *anchor = exprAs<DirectInitializer>(found->second->get());
    if (anchor != nullptr && same(anchor->brace(), field.name()) &&
        same(anchor->closingBrace(), field.name()))
      return found->second;
  }
  // Fields of superseded syntax are never presented again; bound the table
  // rather than tracking each one's lifetime.
  if (impl->anchors.size() >= 4 * impl->limits.maximumEntries)
    impl->anchors.clear();
  auto anchor =
      std::make_shared<const ExprPtr>(std::make_unique<DirectInitializer>(
          field.name(), ExprList{}, field.name()));
  impl->anchors.insert_or_assign(&field, anchor);
  return anchor;
}

InstanceAnalysisCache::Generation
InstanceAnalysisCache::generation(InstanceAnalysisGenerationKey key,
                                  std::vector<StmtPtr> syntax) {
  const std::lock_guard lock(impl->mutex);
  for (Impl::GenerationEntries &entries : impl->generations) {
    if (entries.generation->key == key) {
      entries.lastUse = ++impl->clock;
      return entries.generation;
    }
  }
  if (impl->limits.maximumGenerations == 0)
    return nullptr;
  // A generation whose units have the same paths but other syntax has been
  // superseded: its syntax left the parse cache or changed, so no later
  // snapshot can present it again.
  const auto sameShape = [&](const InstanceAnalysisGenerationKey &other) {
    if (other.context != key.context || other.units.size() != key.units.size())
      return false;
    for (std::size_t index = 0; index < key.units.size(); ++index)
      if (other.units[index].id != key.units[index].id ||
          other.units[index].path != key.units[index].path)
        return false;
    return true;
  };
  for (std::size_t index = impl->generations.size(); index > 0; --index)
    if (sameShape(impl->generations[index - 1].generation->key))
      impl->eraseGeneration(index - 1);
  while (impl->generations.size() >= impl->limits.maximumGenerations) {
    const auto oldest =
        std::min_element(impl->generations.begin(), impl->generations.end(),
                         [](const auto &left, const auto &right) {
                           return left.lastUse < right.lastUse;
                         });
    impl->eraseGeneration(
        static_cast<std::size_t>(oldest - impl->generations.begin()));
  }
  auto created = std::make_shared<const InstanceAnalysisGeneration>(
      std::move(key), std::move(syntax));
  impl->generations.push_back(
      {.generation = created, .index = {}, .lastUse = ++impl->clock});
  return created;
}

std::shared_ptr<const InstanceAnalysisCacheEntry>
InstanceAnalysisCache::lookup(const Generation &generation,
                              const InstanceAnalysisKey &key) {
  if (generation == nullptr)
    return nullptr;
  const std::lock_guard lock(impl->mutex);
  Impl::GenerationEntries *entries = impl->find(generation.get());
  if (entries == nullptr)
    return nullptr;
  entries->lastUse = ++impl->clock;
  const auto found = entries->index.find(key);
  if (found == entries->index.end())
    return nullptr;
  impl->entries.splice(impl->entries.end(), impl->entries, found->second);
  return found->second->value;
}

void InstanceAnalysisCache::store(const Generation &generation,
                                  InstanceAnalysisKey key,
                                  InstanceAnalysisCacheEntry entry) {
  if (generation == nullptr)
    return;
  const std::lock_guard lock(impl->mutex);
  Impl::GenerationEntries *entries = impl->find(generation.get());
  // A generation evicted while its analysis ran stores nothing.
  if (entries == nullptr)
    return;
  if (const auto found = entries->index.find(key);
      found != entries->index.end())
    impl->erase(found->second);
  entries = impl->find(generation.get());
  auto value =
      std::make_shared<const InstanceAnalysisCacheEntry>(std::move(entry));
  impl->estimatedBytes += value->estimatedBytes;
  impl->entries.push_back(
      {.generation = generation, .key = key, .value = std::move(value)});
  entries->index.emplace(std::move(key), std::prev(impl->entries.end()));
  impl->evict();
}

} // namespace lang
