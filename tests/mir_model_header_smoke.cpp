#include "gti/mir.h"

int mirModelHeaderSmoke() {
  const lang::MirBody body;
  return body.instructionCount() == 0 ? 0 : 1;
}
