#pragma once

#include "gti/diagnostic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lang {

// The source graph is analyzed either as a hosted executable or as a dormant
// embedding library. Library publication remains a driver capability; this
// value only selects frontend entry/root semantics.
enum class ProgramKind {
  Executable,
  Library,
};

inline constexpr std::uint32_t embeddingAbiVersionV1 = 1;

// One manifest-selected public wrapper. `function` is a qualified GTI source
// name; `symbol` is the exact C identifier the future wrapper will publish.
// The source spans remain driver-owned provenance and are used only to anchor
// diagnostics before native compilation.
struct EmbeddingExportRequest {
  std::string function;
  std::string symbol;
  SourceSpan declaration;
  SourceSpan functionDeclaration;
  SourceSpan symbolDeclaration;

  friend bool operator==(const EmbeddingExportRequest &,
                         const EmbeddingExportRequest &) = default;
};

struct ProgramRequest {
  ProgramKind kind = ProgramKind::Executable;
  std::vector<EmbeddingExportRequest> exports;

  friend bool operator==(const ProgramRequest &,
                         const ProgramRequest &) = default;
};

} // namespace lang
