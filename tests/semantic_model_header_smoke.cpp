#include "gti/semantic_model.h"

int semanticModelHeaderSmoke() {
  const lang::SemanticModel model;
  return model.placeSnapshot() == 0 ? 0 : 1;
}
