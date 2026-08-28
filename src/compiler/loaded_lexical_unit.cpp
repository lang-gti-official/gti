#include "loaded_lexical_unit.h"

#include "gti/lexer.h"

#include <chrono>
#include <utility>

namespace lang {
namespace {

std::uint64_t sourceDigest(std::string_view source) {
  // Stable FNV-1a identity. Overlay cache hits also compare the exact source
  // bytes, so a digest collision cannot silently select another overlay.
  std::uint64_t result = 14695981039346656037ULL;
  for (const unsigned char byte : source) {
    result ^= byte;
    result *= 1099511628211ULL;
  }
  return result;
}

} // namespace

LoadedLexicalUnit
LoadedLexicalUnitLoader::load(const LexicalUnitRequest &request) const {
  const std::uint64_t requestedDigest = request.exactSource
                                            ? sourceDigest(*request.exactSource)
                                            : request.knownSourceDigest;
  FrontendUnitCacheKey cacheKey{
      .canonicalPath = request.canonicalPath,
      .sourceGeneration = request.sourceGeneration,
      .sourceDigest = requestedDigest,
      .context = std::string(request.cacheContext),
      .completionOffset = request.completionOffset,
  };

  std::optional<CachedLexedSource> cached =
      cache ? cache->lookupLexed(cacheKey, request.exactSource) : std::nullopt;
  if (cached) {
    if (workMetrics != nullptr) {
      workMetrics->lexCacheHits.push_back(request.canonicalPath);
      workMetrics->sourceBytes += cached->source.size();
    }
    return {.source = std::move(cached->source),
            .sourceDigest = cached->sourceDigest,
            .tokens = std::move(cached->tokens),
            .diagnostics = std::move(cached->diagnostics),
            .hadError = cached->hadError,
            .cacheHit = true};
  }

  if (workMetrics != nullptr) {
    workMetrics->loadedUnits.push_back(request.canonicalPath);
    workMetrics->lexedUnits.push_back(request.canonicalPath);
  }
  Lexer lexer;
  const auto lexStart = std::chrono::steady_clock::now();
  std::vector<Token> tokens;
  if (request.exactSource) {
    tokens = request.completionOffset
                 ? lexer.scanForCompletion(std::string(*request.exactSource),
                                           *request.completionOffset,
                                           request.canonicalPath)
                 : lexer.scan(std::string(*request.exactSource),
                              request.canonicalPath);
  } else {
    tokens = lexer.consume(request.path);
  }
  if (workMetrics != nullptr) {
    workMetrics->lexMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lexStart)
            .count());
  }

  std::string source = lexer.sourceText();
  const std::uint64_t loadedDigest = sourceDigest(source);
  std::vector<Diagnostic> diagnostics = lexer.errors();
  const bool hadError = lexer.hadError();
  if (workMetrics != nullptr) {
    workMetrics->sourceBytes += source.size();
  }
  if (cache != nullptr) {
    cacheKey.sourceDigest = loadedDigest;
    cache->storeLexed(cacheKey, {.source = source,
                                 .sourceDigest = loadedDigest,
                                 .tokens = tokens,
                                 .diagnostics = diagnostics,
                                 .hadError = hadError});
  }
  return {.source = std::move(source),
          .sourceDigest = loadedDigest,
          .tokens = std::move(tokens),
          .diagnostics = std::move(diagnostics),
          .hadError = hadError};
}

} // namespace lang
