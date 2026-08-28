#include "gti/mir_effects.h"

int mirEffectsHeaderSmoke() {
  const lang::MirEffectTraits traits =
      lang::effects(lang::MirInstructionKind::Compute);
  return traits.mayTrap ? 1 : 0;
}
