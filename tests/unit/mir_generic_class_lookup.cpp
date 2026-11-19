// Generic instances sharing a class ID keep their own drop and allocation
// recipes.
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(mir_generic_class_lookup) {
  using namespace lang;
  const auto frontend = Frontend().analyze("class-lookup.gti", R"(
struct Box<T> { T value; Box(T& x) : value(x) {} };
int main() {
  int n = 1;
  Box<int> plain{n};
  bool flag = false;
  Box<bool> other{flag};
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << "\n";
    return 1;
  }

  const auto &program = frontend.mir;
  bool sawInt = false;
  bool sawBool = false;
  for (const auto &instance : program.classInstances()) {
    const auto &type = instance.type;
    if (type.arguments.size() != 1)
      continue;
    const auto cleanup =
        mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(type));
    if (!cleanup || cleanup->uniquePointeeClass != instance.id ||
        !isMirStructuralValueType(program, type,
                                  MirStructuralValueOperation::Drop)) {
      std::cerr << "Box instance selected another specialization's cleanup\n";
      return 1;
    }
    sawInt |= type.arguments.front().kind == SemanticType::Int32;
    sawBool |= type.arguments.front().kind == SemanticType::Bool;
  }
  return sawInt && sawBool ? 0 : 1;
}
