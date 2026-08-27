#pragma once

#include "gti/diagnostic.h"
#include "gti/frontend_cache.h"
#include "gti/frontend_work.h"
#include "gti/token.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct LexicalUnitRequest {
  std::filesystem::path path;
  std::string canonicalPath;
  std::optional<std::string_view> exactSource;
  std::optional<std::size_t> completionOffset;
  std::uint64_t sourceGeneration = 0;
  std::uint64_t knownSourceDigest = 0;
  std::string_view cacheContext;
};

struct LoadedLexicalUnit {
  std::string source;
  std::uint64_t sourceDigest = 0;
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;
  bool hadError = false;
  bool cacheHit = false;
};

// Owns one unit's byte acquisition, stable content digest, lexical cache
// interaction, lexing/completion lexing, and exact lexical diagnostics. It has
// no source-graph, include-resolution, or package-policy authority.
class LoadedLexicalUnitLoader final {
public:
  LoadedLexicalUnitLoader(FrontendWorkMetrics *workMetrics,
                          std::shared_ptr<FrontendCache> cache)
      : workMetrics(workMetrics), cache(std::move(cache)) {}

  [[nodiscard]] LoadedLexicalUnit load(const LexicalUnitRequest &request) const;

private:
  FrontendWorkMetrics *workMetrics = nullptr;
  std::shared_ptr<FrontendCache> cache;
};

} // namespace lang
