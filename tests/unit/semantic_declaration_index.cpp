// A declaration index that shares a suspended source's storage reads it in
// place, copies it before any non-const access, and never writes the source;
// ordinary copies stay deep.
#include "../../src/compiler/semantic_declaration_index.h"
#include "unit_test.h"

#include <stdexcept>
#include <string>
#include <utility>

GTI_UNIT_TEST(semantic_declaration_index) {
  using Index = lang::DeclarationIndex<int>;
  const auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  Index source;
  source.emplace("a", 1);
  source.emplace("b", 2);
  source.addAlternative("b", 3);
  int *sourceValue = &source.find("a", {})->second;

  Index copy = source;
  require(!copy.sharesStorage() && !source.sharesStorage(),
          "an ordinary copy shared storage");
  copy.find("a", {})->second = 10;
  require(*sourceValue == 1, "an ordinary copy wrote its source");

  Index shared;
  shared.shareFrom(source);
  require(shared.sharesStorage() && source.sharesStorage(),
          "sharing did not share storage");
  const auto version = shared.sharedVersion();
  require(version && version == source.sharedVersion(),
          "holders of one storage report different versions");
  const Index &reader = shared;
  require(&reader.find("a", {})->second == sourceValue,
          "a read-only lookup copied shared storage");
  require(reader.ambiguousCandidates("b") != nullptr &&
              reader.ambiguousCandidates("b")->size() == 2,
          "a shared index lost an ambiguity");
  std::size_t visited = 0;
  reader.forEachValue([&](const int &) { ++visited; });
  require(visited == 4 && shared.sharesStorage(),
          "a read-only visit copied shared storage");

  shared.editValuesIf([](const int &) { return false; }, [](int &) {});
  require(shared.sharesStorage(), "an empty edit copied shared storage");

  const auto generation = shared.generation();
  shared.find("a", {})->second = 20;
  require(!shared.sharesStorage() && !source.sharesStorage(),
          "a non-const lookup did not take a private copy");
  require(shared.generation() != generation,
          "taking a private copy kept the index generation");
  require(*sourceValue == 1 && reader.find("a", {})->second == 20,
          "an edit through a shared index reached its source");

  Index nested;
  nested.shareFrom(source);
  Index inner;
  inner.shareFrom(nested);
  require(inner.sharedVersion() == nested.sharedVersion(),
          "sharing already shared storage started a new version");
  inner.clear();
  nested = Index{};
  require(!source.sharesStorage() && source.find("b", {})->second == 2,
          "releasing sharers changed their source");

  Index later;
  later.shareFrom(source);
  require(later.sharedVersion() != version,
          "storage shared again after it was unshared kept its version");
  return 0;
}
