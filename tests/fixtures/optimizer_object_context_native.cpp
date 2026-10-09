// Opaque to GTI's contextual analysis; native variants explicitly select each
// hazardous tail. Keeping the discriminator outside the proved comparisons
// makes all negative contexts genuinely reachable to the GTI analysis.
extern "C" int gti_optimizer_case() { return 0; }
