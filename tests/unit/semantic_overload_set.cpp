// Sealed overload snapshots share candidate storage and isolate every later
// edit.
#include "../../src/compiler/semantic_overload_set.h"
#include "unit_test.h"

#include <stdexcept>
#include <utility>

GTI_UNIT_TEST(semantic_overload_set) {
  lang::SemanticOverloadSet<int> original{7};
  auto &registered = original.edit().front();
  auto duringRegistration = original;
  registered = 11;
  if (duringRegistration.front() != 7)
    throw std::runtime_error("in-progress registration leaked into a copy");

  original.freeze();
  auto snapshot = original;
  if (&snapshot.front() != &original.front())
    throw std::runtime_error("a read-only snapshot copied a sealed signature");
  snapshot.edit().front() = 19;
  if (snapshot.front() != 19 || original.front() != 11)
    throw std::runtime_error(
        "editing a snapshot changed the catalog signature");

  snapshot.freeze();
  auto moved = std::move(snapshot);
  snapshot.edit().push_back(23);
  if (moved.front() != 19 || moved.size() != 1 || snapshot.front() != 23)
    throw std::runtime_error("reusing a moved snapshot changed its signatures");
  return 0;
}
