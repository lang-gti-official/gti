#include "gti/semantic_core.h"

int semanticCoreHeaderSmoke() {
  const lang::SemanticType type = lang::SemanticType::UInt64;
  return type == lang::SemanticType::UInt64 ? 0 : 1;
}
