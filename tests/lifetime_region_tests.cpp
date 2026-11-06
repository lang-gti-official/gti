#include "gti/lifetime.h"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  using lang::Lifetime;
  using lang::LifetimeBounds;
  const auto input = Lifetime::parameter(1);
  const auto result = Lifetime::parameter(2);
  const auto shorter = Lifetime::parameter(3);
  const auto unrelated = Lifetime::parameter(4);
  const auto unknown = Lifetime{};
  const auto forever = Lifetime::staticLifetime();

  LifetimeBounds bounds;
  expect(!bounds.proves(input, result),
         "unrelated universal inputs do not imply a store relationship");
  expect(bounds.proves(input, input), "a valid parameter covers itself");
  expect(!bounds.proves(unknown, unknown),
         "two missing annotations are not a lifetime proof");
  expect(!bounds.proves(forever, unknown),
         "static storage cannot validate an unresolved obligation");
  expect(!bounds.proves(Lifetime::parameter(0), input),
         "an invalid parameter identity cannot prove a bound");
  expect(bounds.proves(forever, input), "static covers any valid region");
  expect(!bounds.proves(input, forever),
         "ordinary inputs are not promoted to static");

  expect(bounds.assume(input, result), "accept checked declaration bound");
  expect(bounds.assume(result, shorter), "accept second declaration bound");
  expect(bounds.proves(input, shorter), "outlives bounds compose transitively");
  expect(!bounds.proves(shorter, input), "outlives is directional");
  expect(!bounds.proves(input, unrelated), "bounds do not cover unrelated inputs");
  expect(bounds.assume(input, result) && bounds.bounds().size() == 2,
         "duplicate bounds do not grow the stored contract");

  expect(bounds.assume(shorter, input), "equal-region cycles are legal");
  expect(bounds.proves(shorter, result), "cycles preserve reachable bounds");
  expect(!bounds.proves(shorter, unrelated),
         "a cyclic contract terminates without inventing a bound");

  const auto inferred = Lifetime::inference(1);
  const auto local = Lifetime::local(1);
  expect(input != inferred && input != local && inferred != local,
         "numeric IDs do not equate universal, inferred and local regions");
  expect(!bounds.assume(inferred, input) && !bounds.assume(input, local),
         "call obligations cannot be inserted as declaration assumptions");
  expect(!bounds.proves(inferred, result) && !bounds.proves(local, result),
         "a parameter's bound cannot be borrowed by another region kind");
  expect(bounds.proves(forever, local), "static validity covers a local region");
  LifetimeBounds staticContract;
  expect(staticContract.assume(input, forever) &&
             staticContract.proves(input, unrelated),
         "an explicit static bound covers every valid requested region");
  const Lifetime malformed{static_cast<Lifetime::Kind>(255), 1};
  expect(!bounds.proves(malformed, malformed) &&
             !bounds.assume(malformed, input),
         "malformed serialized region kinds cannot become proofs");
  return 0;
}
