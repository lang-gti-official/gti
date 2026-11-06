#include "gti/lifetime_types.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
lang::SemanticType reference(lang::Lifetime region,
                             lang::AccessMode access = lang::AccessMode::ReadOnly) {
  auto type = lang::SemanticType::referenceTo(lang::SemanticType::Int32, access);
  type.referenceLifetime = region;
  return type;
}
} // namespace

int main() {
  using namespace lang;
  const auto a = Lifetime::parameter(1);
  const auto b = Lifetime::parameter(2);
  const auto x = Lifetime::parameter(31);
  const auto y = Lifetime::parameter(32);
  const std::array renamed{LifetimeBinding{a, x}, LifetimeBinding{b, y}};
  const auto first = SemanticType::functionSignature(reference(a),
                                                     {reference(a), reference(b)});
  const auto second = SemanticType::functionSignature(reference(x),
                                                      {reference(x), reference(y)});
  expect(equivalentLifetimeTypes(first, second, renamed),
         "alpha-renaming preserves the complete signature relationship");
  auto different = second;
  different.arguments.front().referenceLifetime = y;
  expect(!equivalentLifetimeTypes(first, different, renamed),
         "renaming cannot change which input supplies the result");
  expect(!equivalentLifetimeTypes(reference(a), reference(x)),
         "unrelated free universals cannot be silently unified");
  expect(!equivalentLifetimeTypes(reference(a), reference(a, AccessMode::Mutable)),
         "equal regions never erase access permissions");
  expect(!equivalentLifetimeTypes(reference(a), reference(Lifetime::inference(1))),
         "inference variables cannot masquerade as universal binders");
  const std::array collapsed{LifetimeBinding{a, x}, LifetimeBinding{b, x}};
  expect(!equivalentLifetimeTypes(first, second, collapsed),
         "alpha-equivalence requires a bijection");

  auto higherRanked = SemanticType::functionSignature(reference(a),
                                                      {reference(a), reference(b)});
  higherRanked.quantifiedLifetimes = {a.id};
  auto renamedHigher = SemanticType::functionSignature(reference(x),
                                                       {reference(x), reference(b)});
  renamedHigher.quantifiedLifetimes = {x.id};
  expect(equivalentLifetimeTypes(higherRanked, renamedHigher),
         "higher-ranked binders are scoped and alpha-equivalent");
  expect(freeTypeLifetimes(higherRanked) == std::vector<Lifetime>{b},
         "quantified callback inputs are not free caller regions");
  const std::array instantiate{LifetimeBinding{a, Lifetime::staticLifetime()},
                               LifetimeBinding{b, y}};
  const auto substituted = substituteTypeLifetimes(higherRanked, instantiate);
  expect(substituted && substituted->arguments.front().referenceLifetime == a &&
             substituted->arguments.back().referenceLifetime == y,
         "substitution preserves a callback's universality and replaces free regions");
  const std::array capture{LifetimeBinding{b, a}};
  expect(!substituteTypeLifetimes(higherRanked, capture),
         "a substitution cannot capture an outer region in a callback binder");

  auto nominal = SemanticType::classType(5, {reference(a)});
  nominal.lifetimeArguments = {b};
  nominal.lambdaEnclosingClassTypes = {reference(b)};
  nominal.lambdaEnclosingFunctionTypes = {higherRanked};
  const auto transformed = substituteTypeLifetimes(nominal, renamed);
  expect(transformed && transformed->lifetimeArguments == std::vector<Lifetime>{y} &&
             transformed->arguments.front().referenceLifetime == x &&
             transformed->lambdaEnclosingClassTypes.front().referenceLifetime == y &&
             transformed->lambdaEnclosingFunctionTypes.front()
                     .arguments.front().referenceLifetime == a,
         "nested nominal and closure substitutions retain independent regions");
  expect(eraseTypeLifetimes(first) == eraseTypeLifetimes(second),
         "region renaming cannot multiply executable signatures");
  expect(eraseTypeLifetimes(reference(a)) !=
             eraseTypeLifetimes(reference(a, AccessMode::Mutable)),
         "representation erasure changes only region metadata");
  expect(freeTypeLifetimes(eraseTypeLifetimes(nominal)).empty(),
         "representation erasure includes all nested type vectors");

  LifetimeSignatureContract contract{.parameters = {a, b},
                                      .bounds = {{a, b}},
                                      .receiver = a,
                                      .origin = LifetimeContractOrigin::Declared,
                                      .complete = true};
  expect(contract.valid(), "a complete declared region interface is valid");
  contract.bounds.push_back({b, Lifetime::inference(7)});
  expect(!contract.valid(), "public contracts cannot publish local inference holes");
  contract.bounds.pop_back();
  contract.parameters.push_back(a);
  expect(!contract.valid(), "duplicate region identities are malformed metadata");
  contract.parameters.pop_back();
  contract.receiver = x;
  expect(!contract.valid(), "receiver identity must be declared by its contract");
  return 0;
}
