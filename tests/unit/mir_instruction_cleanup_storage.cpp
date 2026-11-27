// Sparse instruction cleanup recipes stay independent across MIR snapshots.
#include "gti/mir.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(mir_instruction_cleanup_storage) {
  using namespace lang;
  MirInstruction source;
  if (sizeof(source.elementCleanup) >= sizeof(MirDropType) ||
      sizeof(source.replacementCleanup) >= sizeof(MirDropType)) {
    std::cerr << "each instruction reserves a full absent cleanup recipe\n";
    return 1;
  }
  MirDropType recipe{.type = SemanticType::Int64,
                     .aggregate = {{.type = SemanticType::Int32}}};
  source.elementCleanup = recipe;
  source.replacementCleanup = recipe;
  MirInstruction copy = source;
  if (copy != source)
    return 1;
  copy.elementCleanup->aggregate.front().type = SemanticType::Bool;
  copy.replacementCleanup.reset();
  if (source.elementCleanup->aggregate.front().type != SemanticType::Int32 ||
      !source.replacementCleanup || source == copy)
    return 1;
  copy = source;
  copy.replacementCleanup->recursiveCleanup = true;
  return source.replacementCleanup->recursiveCleanup || copy == source;
}
