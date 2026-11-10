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
  expect(!bounds.proves(input, unrelated),
         "bounds do not cover unrelated inputs");
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
  expect(bounds.proves(forever, local),
         "static validity covers a local region");
  LifetimeBounds staticContract;
  expect(staticContract.assume(input, forever) &&
             staticContract.proves(input, unrelated),
         "an explicit static bound covers every valid requested region");
  const Lifetime malformed{static_cast<Lifetime::Kind>(255), 1};
  expect(!bounds.proves(malformed, malformed) &&
             !bounds.assume(malformed, input),
         "malformed serialized region kinds cannot become proofs");
  lang::LifetimeInferenceConstraints intersection;
  expect(
      intersection.require(input, inferred) &&
          intersection.require(unrelated, inferred) && !intersection.failure(),
      "a local result may infer the intersection of independent input regions");
  expect(
      intersection.require(inferred, input) &&
          intersection.failure().has_value(),
      "the intersection cannot escape as either unrestricted original input");
  expect(!intersection.proves(inferred, input),
         "an inconsistent inference graph never becomes a declaration proof");
  lang::LifetimeInferenceConstraints shortening(bounds);
  const auto child = Lifetime::inference(2);
  expect(
      shortening.require(input, inferred) &&
          shortening.require(inferred, child) &&
          shortening.require(child, shorter) && !shortening.failure(),
      "checked declaration bounds discharge transitive inference obligations");
  expect(shortening.proves(inferred, shorter),
         "a solved inferred region preserves its checked lower requirement");
  lang::LifetimeInferenceConstraints cycle;
  expect(
      cycle.require(inferred, child) && cycle.require(child, inferred) &&
          !cycle.failure(),
      "inference cycles terminate without equating unrelated universal inputs");
  lang::LifetimeInferenceConstraints localUse;
  expect(localUse.require(input, inferred) &&
             localUse.require(inferred, local) &&
             localUse.failure().has_value(),
         "local scope ordering is not inferred from numeric region identities");
  expect(
      !localUse.failure(
          [&](Lifetime a, Lifetime b) { return a == input && b == local; }),
      "body-local containment must come from independently checked flow facts");
  expect(!localUse.require({}, input),
         "unsolved syntax is not an inference region");
  const auto projected = intersection.fixedRequirements();
  expect(std::find(projected.begin(), projected.end(),
                   lang::LifetimeBound{unrelated, input}) != projected.end() &&
             intersection.failure().has_value(),
         "fixed-anchor projection exposes a requirement without proving it");
  expect(localUse.fixedRequirements() ==
             std::vector{lang::LifetimeBound{input, local}},
         "projection retains local anchors so declaration inference can reject "
         "escape");
  expect(cycle.fixedRequirements().empty(),
         "an existential cycle alone invents no declaration requirement");
  return 0;
}
