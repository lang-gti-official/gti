// ARCHIVED: excluded from the GTI build. See archive/compiler/README.md.

#include "gti/target.h"

namespace lang {

std::optional<TargetInfo> parseTargetTriple(std::string_view) {
  return std::nullopt;
}

bool targetTripleParsingAvailable() { return false; }

} // namespace lang
