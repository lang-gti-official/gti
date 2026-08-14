# Game engine port

This directory pairs the cloned C++ Psych engine with a bounded GTI port:

- [`gti/`](gti/README.md) is a runnable headless project that preserves the
  core `Application`/`App` polymorphism, layer contracts, ordered layer stack,
  frame loop, stop request, and explicit shutdown lifecycle that current GTI
  can express honestly.

The GTI README records every deliberate substitution and remaining language or
standard-library gap. This port uses the newly supported polymorphic unique
ownership and indexed vector mutation directly, without pretending native
vendor integrations already exist.
