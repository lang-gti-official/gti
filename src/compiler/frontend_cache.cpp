#include "gti/frontend_cache.h"

#include <algorithm>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>

namespace lang {
namespace {

void combineHash(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct UnitKeyHash {
  std::size_t operator()(const FrontendUnitCacheKey &key) const {
    std::size_t result = std::hash<std::string>{}(key.canonicalPath);
    combineHash(result, std::hash<std::uint64_t>{}(key.sourceGeneration));
    combineHash(result, std::hash<std::uint64_t>{}(key.sourceDigest));
    combineHash(result, std::hash<std::uint64_t>{}(key.syntaxDigest));
    combineHash(result, std::hash<std::uint64_t>{}(key.dependencyDigest));
    combineHash(result, std::hash<std::string>{}(key.context));
    combineHash(result, key.completionOffset
                            ? std::hash<std::size_t>{}(*key.completionOffset)
                            : 0x51f15eU);
    return result;
  }
};

std::size_t tokenBytes(const std::vector<Token> &tokens) {
  std::size_t result = tokens.size() * sizeof(Token);
  for (const Token &token : tokens) {
    result += token.lexeme.size() + token.source.size();
    if (const auto *text = std::get_if<std::string>(&token.literal)) {
      result += text->size();
    }
    if (token.leadingDocumentationMarkdown) {
      result += token.leadingDocumentationMarkdown->size();
    }
  }
  return result;
}

std::size_t diagnosticBytes(const std::vector<Diagnostic> &diagnostics) {
  std::size_t result = diagnostics.size() * sizeof(Diagnostic);
  for (const Diagnostic &diagnostic : diagnostics) {
    result += diagnostic.code.size() + diagnostic.message.size() +
              diagnostic.primary.source.size();
    result += diagnostic.related.size() * sizeof(RelatedDiagnostic);
    for (const RelatedDiagnostic &related : diagnostic.related) {
      result += related.span.source.size() + related.message.size();
    }
    result += diagnostic.fixes.size() * sizeof(FixIt);
    for (const FixIt &fix : diagnostic.fixes) {
      result +=
          fix.span.source.size() + fix.replacement.size() + fix.message.size();
    }
    result += diagnostic.hints.size() * sizeof(std::string);
    for (const std::string &hint : diagnostic.hints) {
      result += hint.size();
    }
  }
  return result;
}

std::size_t keyBytes(const FrontendUnitCacheKey &key) {
  return sizeof(FrontendUnitCacheKey) + key.canonicalPath.size() +
         key.context.size();
}

} // namespace

class FrontendCache::Impl final {
public:
  explicit Impl(FrontendCacheLimits limits) : limits(limits) {}

  struct LexEntry {
    FrontendUnitCacheKey key;
    CachedLexedSource value;
    std::size_t estimatedBytes = 0;
    std::uint64_t lastUse = 0;
  };

  struct ParseEntry {
    FrontendUnitCacheKey key;
    CachedParsedUnit value;
    std::size_t estimatedBytes = 0;
    std::uint64_t lastUse = 0;
  };

  void eraseLex(std::list<LexEntry>::iterator entry) {
    estimatedBytes -= entry->estimatedBytes;
    lexIndex.erase(entry->key);
    lexEntries.erase(entry);
  }

  void eraseParse(std::list<ParseEntry>::iterator entry) {
    estimatedBytes -= entry->estimatedBytes;
    parseIndex.erase(entry->key);
    parseEntries.erase(entry);
  }

  void evict() {
    while (lexEntries.size() + parseEntries.size() > limits.maximumEntries ||
           estimatedBytes > limits.maximumEstimatedBytes) {
      const bool evictLex =
          parseEntries.empty() ||
          (!lexEntries.empty() &&
           lexEntries.front().lastUse <= parseEntries.front().lastUse);
      if (evictLex) {
        eraseLex(lexEntries.begin());
      } else {
        eraseParse(parseEntries.begin());
      }
    }
  }

  FrontendCacheLimits limits;
  mutable std::mutex mutex;
  std::uint64_t clock = 0;
  std::size_t estimatedBytes = 0;
  std::list<LexEntry> lexEntries;
  std::list<ParseEntry> parseEntries;
  std::unordered_map<FrontendUnitCacheKey, std::list<LexEntry>::iterator,
                     UnitKeyHash>
      lexIndex;
  std::unordered_map<FrontendUnitCacheKey, std::list<ParseEntry>::iterator,
                     UnitKeyHash>
      parseIndex;
};

FrontendCache::FrontendCache(FrontendCacheLimits limits)
    : impl(std::make_unique<Impl>(limits)) {}

FrontendCache::~FrontendCache() = default;

std::optional<CachedLexedSource>
FrontendCache::lookupLexed(const FrontendUnitCacheKey &key,
                           std::optional<std::string_view> exactSource) {
  if (key.sourceDigest == 0) {
    return std::nullopt;
  }
  const std::lock_guard lock(impl->mutex);
  const auto found = impl->lexIndex.find(key);
  if (found == impl->lexIndex.end() ||
      (exactSource && found->second->value.source != *exactSource)) {
    return std::nullopt;
  }
  found->second->lastUse = ++impl->clock;
  impl->lexEntries.splice(impl->lexEntries.end(), impl->lexEntries,
                          found->second);
  return found->second->value;
}

void FrontendCache::storeLexed(FrontendUnitCacheKey key,
                               CachedLexedSource source) {
  if (key.sourceDigest == 0 || source.sourceDigest != key.sourceDigest) {
    return;
  }
  const std::lock_guard lock(impl->mutex);
  if (const auto found = impl->lexIndex.find(key);
      found != impl->lexIndex.end()) {
    impl->eraseLex(found->second);
  }
  const std::size_t bytes = sizeof(Impl::LexEntry) + keyBytes(key) +
                            source.source.size() + tokenBytes(source.tokens) +
                            diagnosticBytes(source.diagnostics);
  impl->lexEntries.push_back({.key = std::move(key),
                              .value = std::move(source),
                              .estimatedBytes = bytes,
                              .lastUse = ++impl->clock});
  auto inserted = std::prev(impl->lexEntries.end());
  impl->estimatedBytes += bytes;
  impl->lexIndex.emplace(inserted->key, inserted);
  impl->evict();
}

std::optional<CachedParsedUnit>
FrontendCache::lookupParsed(const FrontendUnitCacheKey &key) {
  if (key.sourceDigest == 0 || key.syntaxDigest == 0 ||
      key.dependencyDigest == 0) {
    return std::nullopt;
  }
  const std::lock_guard lock(impl->mutex);
  const auto found = impl->parseIndex.find(key);
  if (found == impl->parseIndex.end()) {
    return std::nullopt;
  }
  found->second->lastUse = ++impl->clock;
  impl->parseEntries.splice(impl->parseEntries.end(), impl->parseEntries,
                            found->second);
  return found->second->value;
}

void FrontendCache::storeParsed(FrontendUnitCacheKey key,
                                std::size_t estimatedInputBytes,
                                CachedParsedUnit parsed) {
  if (key.sourceDigest == 0 || key.syntaxDigest == 0 ||
      key.dependencyDigest == 0) {
    return;
  }
  const std::lock_guard lock(impl->mutex);
  if (const auto found = impl->parseIndex.find(key);
      found != impl->parseIndex.end()) {
    impl->eraseParse(found->second);
  }
  // AST nodes own nested trees whose allocator size is not directly exposed.
  // Charge four times the exact processed-token footprint plus declaration
  // handles; this deliberately overestimates small units for predictable LRU
  // pressure rather than pretending the cache has byte-perfect accounting.
  const std::size_t bytes = sizeof(Impl::ParseEntry) + keyBytes(key) +
                            estimatedInputBytes * 4U +
                            parsed.declarations.size() * sizeof(StmtPtr) +
                            diagnosticBytes(parsed.diagnostics);
  impl->parseEntries.push_back({.key = std::move(key),
                                .value = std::move(parsed),
                                .estimatedBytes = bytes,
                                .lastUse = ++impl->clock});
  auto inserted = std::prev(impl->parseEntries.end());
  impl->estimatedBytes += bytes;
  impl->parseIndex.emplace(inserted->key, inserted);
  impl->evict();
}

void FrontendCache::invalidate(std::string_view canonicalPath) {
  const std::lock_guard lock(impl->mutex);
  for (auto entry = impl->lexEntries.begin();
       entry != impl->lexEntries.end();) {
    if (entry->key.canonicalPath == canonicalPath) {
      auto current = entry++;
      impl->eraseLex(current);
    } else {
      ++entry;
    }
  }
  for (auto entry = impl->parseEntries.begin();
       entry != impl->parseEntries.end();) {
    if (entry->key.canonicalPath == canonicalPath) {
      auto current = entry++;
      impl->eraseParse(current);
    } else {
      ++entry;
    }
  }
}

FrontendCacheStats FrontendCache::stats() const {
  const std::lock_guard lock(impl->mutex);
  return {.entries = impl->lexEntries.size() + impl->parseEntries.size(),
          .estimatedBytes = impl->estimatedBytes,
          .limits = impl->limits};
}

} // namespace lang
