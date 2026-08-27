#include "gti/mir_lowering.h"

int mirLoweringHeaderSmoke() {
  const lang::MirLoweringResult result;
  return result.valid() ? 0 : 1;
}
