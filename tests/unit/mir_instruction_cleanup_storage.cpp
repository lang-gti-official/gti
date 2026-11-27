// Large optional instruction payloads stay sparse and independent across MIR
// snapshots.
#include "gti/mir.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(mir_instruction_cleanup_storage) {
  using namespace lang;
  MirInstruction source;
  if (sizeof(source.elementCleanup) >= sizeof(MirDropType) ||
      sizeof(source.replacementCleanup) >= sizeof(MirDropType) ||
      sizeof(source.nativeCalleeType) >= sizeof(SemanticType) ||
      sizeof(source.objectLayoutType) >= sizeof(SemanticType)) {
    std::cerr << "each instruction reserves a full absent payload\n";
    return 1;
  }
  MirDropType recipe{.type = SemanticType::Int64,
                     .aggregate = {{.type = SemanticType::Int32}}};
  source.elementCleanup = recipe;
  source.replacementCleanup = recipe;
  source.objectLayoutType = SemanticType::Int64;
  source.nativeCalleeType =
      SemanticType::nativeFunctionType(SemanticType::Int32, {});
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
  copy.objectLayoutType.reset();
  copy.nativeCalleeType->arguments.front() = SemanticType::Void;
  return source.replacementCleanup->recursiveCleanup ||
         !source.objectLayoutType ||
         source.nativeCalleeType->arguments.front() != SemanticType::Int32 ||
         copy == source;
}
