#pragma once

#include "gti/ast.h"
#include "gti/source_debug.h"
#include "gti/source_graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lang {

// The analysed state of a program's leading compiler-trusted units (its
// trusted prefix): registration and every trusted body, analysed once as a
// program of their own. A later analysis of a program with the same prefix
// adopts a copy of that state and analyses only the declarations after it.
// Built and consumed by SemanticVisitor (buildPreamble/adoptPreamble); owned
// by a frontend caller (the LSP) through SemanticPreambleCache and
// FrontendOptions::preambleCache. See "Trusted Prefix Preamble" in
// docs/architecture/semantic-analysis.md.
class SemanticPreamble;
// Distinct for every preamble built in this process; reusable records that
// refer to a preamble's synthesized syntax are keyed by it.
[[nodiscard]] std::uint64_t semanticPreambleIdentity(const SemanticPreamble &);
[[nodiscard]] bool semanticPreambleSupportsSourceDebug(const SemanticPreamble &,
                                                       SourceDebugProfile);

// Everything two programs must share for one preamble to serve both: the
// analysis configuration, every prefix unit by identity, path, digests and
// exact parsed declarations (shared through FrontendCache), and the whole
// source graph by unit identity and include edges, because registration
// publishes a declaration's visibility per consuming unit.
struct SemanticPreambleKey {
  struct Unit {
    SourceUnitId id = 0;
    std::string path;
    std::uint64_t sourceDigest = 0;
    std::uint64_t syntaxDigest = 0;
    std::vector<const Stmt *> declarations;

    friend bool operator==(const Unit &, const Unit &) = default;
  };

  std::string context;
  std::vector<Unit> units;
  // Every unit of the graph in compilation order, and its include edges.
  std::vector<std::pair<SourceUnitId, std::string>> graphUnits;
  std::vector<std::pair<SourceUnitId, SourceUnitId>> graphEdges;

  friend bool operator==(const SemanticPreambleKey &,
                         const SemanticPreambleKey &) = default;
};

struct SemanticPreambleCacheLimits {
  // Distinct prefixes kept at once (least recently used evicted); one per
  // concurrently edited include graph.
  std::size_t maximumEntries = 4;
};

struct SemanticPreambleCacheStats {
  std::size_t entries = 0;
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t stores = 0;
  SemanticPreambleCacheLimits limits;
};

class SemanticPreambleCache final {
public:
  explicit SemanticPreambleCache(SemanticPreambleCacheLimits limits = {});
  ~SemanticPreambleCache();

  SemanticPreambleCache(const SemanticPreambleCache &) = delete;
  SemanticPreambleCache &operator=(const SemanticPreambleCache &) = delete;

  [[nodiscard]] std::shared_ptr<const SemanticPreamble>
  lookup(const SemanticPreambleKey &key);
  void store(SemanticPreambleKey key,
             std::shared_ptr<const SemanticPreamble> preamble);
  [[nodiscard]] SemanticPreambleCacheStats stats() const;
  void clear();

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
