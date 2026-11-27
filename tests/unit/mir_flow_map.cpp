// The MIR carrier-flow state map agrees with std::map under random inserts,
// erasures, copies, edits and both joins, with keys of either kind it holds.
// Copies share chunks, so each step also checks the maps it did not touch.
#include "../../src/compiler/mir_flow_map.h"
#include "unit_test.h"

#include <iostream>
#include <map>
#include <random>

namespace {

using lang::MirCarrierRoot;
using lang::MirPlaceRootKind;
using lang::mir_flow::FlatMap;
using lang::mir_flow::join;
using lang::mir_flow::joinChanges;
using Fact = std::optional<std::vector<std::size_t>>;

std::size_t keyOf(std::size_t number, std::size_t *) { return number; }
MirCarrierRoot keyOf(std::size_t number, MirCarrierRoot *) {
  MirCarrierRoot root{.kind = number % 3 == 0 ? MirPlaceRootKind::Binding
                                              : MirPlaceRootKind::This,
                      .identity = number / 3};
  if (number % 7 == 0)
    root.packElement = number % 5;
  return root;
}

template <class Key> bool agrees(std::uint64_t seed) {
  using Map = FlatMap<Key, Fact>;
  using Reference = std::map<Key, Fact>;
  const auto joinValues = [](Fact &into, const Fact &from) {
    return join(into, from);
  };
  const auto changes = [](const Fact &into, const Fact &from) {
    return joinChanges(into, from);
  };
  const auto same = [](const Map &map, const Reference &reference) {
    if (map.size() != reference.size() || map.empty() != reference.empty())
      return false;
    auto entry = map.begin();
    for (const auto &[key, fact] : reference) {
      if (entry == map.end() || entry->first != key || entry->second != fact)
        return false;
      const auto found = map.find(key);
      if (found == map.end() || found->second != fact || !map.contains(key))
        return false;
      ++entry;
    }
    return entry == map.end();
  };
  std::mt19937_64 random(seed);
  for (int round = 0; round < 60; ++round) {
    std::vector<Map> maps(4);
    std::vector<Reference> references(4);
    // Small ranges exercise one chunk, large ones many chunks and groups.
    const std::size_t range = 1 + random() % (round % 3 == 0 ? 60 : 6000);
    for (int step = 0; step < 2500; ++step) {
      const std::size_t target = random() % 4;
      const std::size_t source = random() % 4;
      const Key key = keyOf(random() % range, static_cast<Key *>(nullptr));
      Fact fact;
      if (const auto kind = random() % 3; kind == 1)
        fact = std::vector<std::size_t>{};
      else if (kind == 2)
        fact = std::vector<std::size_t>{random() % 5, 5 + random() % 5};
      Map &map = maps[target];
      Reference &reference = references[target];
      switch (random() % 12) {
      case 0:
      case 1:
      case 2:
        map[key] = fact;
        reference[key] = fact;
        break;
      case 3: {
        const auto [entry, inserted] = map.emplace(key, fact);
        const auto [expected, expectedInserted] = reference.emplace(key, fact);
        if (inserted != expectedInserted || entry->second != expected->second)
          return false;
        break;
      }
      case 4:
        if (map.erase(key) != (reference.erase(key) != 0))
          return false;
        break;
      case 5: {
        // The map may ask about an entry more than once, so the answer
        // depends on the key alone.
        const std::size_t stride = 2 + random() % 7;
        const auto goes = [&](const auto &entry) {
          return lang::mir_flow::flowKeyBits(entry.first) % stride == 0;
        };
        map.eraseIf(goes);
        std::erase_if(reference, goes);
        break;
      }
      case 6:
        map = maps[source];
        reference = references[source];
        break;
      case 7: {
        const Reference before = reference;
        const bool changed = map.unionWith(maps[source], joinValues, changes);
        for (const auto &[other, held] : references[source]) {
          const auto [entry, inserted] = reference.emplace(other, held);
          if (!inserted)
            join(entry->second, held);
        }
        if (changed != (before != reference))
          return false;
        break;
      }
      case 8: {
        const Reference before = reference;
        const bool changed = map.joinPaths(maps[source], joinValues, changes);
        for (auto &[own, held] : reference) {
          const auto other = references[source].find(own);
          if (other == references[source].end())
            held.reset();
          else
            join(held, other->second);
        }
        for (const auto &[other, held] : references[source])
          reference.emplace(other, Fact{});
        if (changed != (before != reference))
          return false;
        break;
      }
      case 9: {
        const auto entry = map.edit(key);
        const auto expected = reference.find(key);
        if ((expected == reference.end()) !=
            (entry == std::as_const(map).end()))
          return false;
        if (expected != reference.end()) {
          entry->second = fact;
          expected->second = fact;
        }
        break;
      }
      case 10: {
        const auto bound = std::as_const(map).lowerBound(key);
        const auto expected = reference.lower_bound(key);
        if ((expected == reference.end()) != (bound == map.end()) ||
            (expected != reference.end() && bound->first != expected->first))
          return false;
        break;
      }
      default: {
        // A digest is kept per chunk until that chunk is written: a scan
        // must still see every unknown fact after any of the steps above.
        std::size_t seen = 0;
        std::size_t unknown = 0;
        for (const auto &[other, held] : reference)
          unknown += !held;
        map.scan(
            [](const auto &entries) {
              std::uint64_t digest = 0;
              for (const auto &entry : entries)
                digest |= !entry.second;
              return digest;
            },
            [](std::uint64_t digest) { return digest != 0; },
            [&](const auto &entry) { seen += !entry.second; });
        if (seen != unknown)
          return false;
        break;
      }
      }
      if (step % 40 == 0)
        for (std::size_t index = 0; index < maps.size(); ++index)
          if (!same(maps[index], references[index]))
            return false;
    }
    for (std::size_t index = 0; index < maps.size(); ++index)
      if (!same(maps[index], references[index]))
        return false;
  }
  return true;
}

} // namespace

GTI_UNIT_TEST(mir_flow_map_value_keys) {
  if (!agrees<std::size_t>(20261127)) {
    std::cerr << "flow map over value keys disagrees with std::map\n";
    return 1;
  }
  return 0;
}

GTI_UNIT_TEST(mir_flow_map_root_keys) {
  if (!agrees<MirCarrierRoot>(704)) {
    std::cerr << "flow map over root keys disagrees with std::map\n";
    return 1;
  }
  return 0;
}
