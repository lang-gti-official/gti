#include "gti/mir_verifier.h"

int mirModelHeaderSmoke();
int mirEffectsHeaderSmoke();
int mirEditorHeaderSmoke();
int mirLoweringHeaderSmoke();

int main() {
  const lang::MirBody body;
  const lang::MirVerificationResult verification = lang::verifyMirBody(body);
  return (verification.valid() ? 1 : 0) | mirModelHeaderSmoke() |
         mirEffectsHeaderSmoke() | mirEditorHeaderSmoke() |
         mirLoweringHeaderSmoke();
}
