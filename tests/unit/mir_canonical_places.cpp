// Canonical places match the first MustAlias in each finalized relation row.
#include "gti/optimization/alias.h"
#include "unit_test.h"

GTI_UNIT_TEST(mir_canonical_places) {
  using namespace lang;
  MirProgram program;
  const MirBodyAddress address{.kind = MirBodyKind::Module};
  auto *body = findMirBody(program, address);
  body->places = {{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 1},
                  {.id = 2, .root = MirPlaceRootKind::Binding, .binding = 2},
                  {.id = 3, .root = MirPlaceRootKind::Binding, .binding = 1},
                  {.id = 4, .root = MirPlaceRootKind::Loan, .loan = 1},
                  {.id = 5, .root = MirPlaceRootKind::Binding, .binding = 1}};
  const auto aliases = analyzeMirPlaceAliasEscape(program, address);
  if (aliases.placeCount != body->places.size() ||
      aliases.canonicalPlace(5) != 1 || aliases.canonicalPlace(0) != 0 ||
      aliases.canonicalPlace(6) != 6 ||
      aliases.relation(2, 4) != MirAliasRelation::NoAlias)
    return 1;
  for (const auto &place : body->places) {
    auto first = place.id;
    for (const auto &candidate : body->places)
      if (aliases.relation(place.id, candidate.id) ==
          MirAliasRelation::MustAlias) {
        first = candidate.id;
        break;
      }
    if (aliases.canonicalPlace(place.id) != first)
      return 1;
  }
  return 0;
}
