// Nested temporary receiver contracts must designate a stored reference, not
// its owning shell.
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

GTI_UNIT_TEST(nested_temporary_region_contract) {
  using namespace lang;
  const auto frontend = Frontend().analyze("nested.gti", R"(
class Inner<T, lifetime owner> {
  T&<owner> value;
public:
  Inner(T&<owner> value) : value(value) {}
  T&<owner> get() { return this.value; }
};
class Outer<T, lifetime owner> {
  Inner<T, owner> inner;
public:
  Outer(T&<owner> value) : inner(Inner<T>(value)) {}
  T&<owner> get() { return this.inner.get(); }
};
Outer<T> make<T>(T& value) { return Outer<T>(value); }
int main() {
  int value = 7;
  int& result = make(value).get();
  int& constructed = Outer<int>(value).get();
  return result + constructed - 14;
}

)");
  if (!frontend.canGenerateCode())
    return 1;
  auto forged = frontend.mir;
  bool changed = false;
  auto &functions = const_cast<std::vector<MirFunctionInstance> &>(
      forged.functionInstances());
  for (auto &function : functions) {
    if (function.returnBorrowOrigin != BorrowOriginKind::Receiver ||
        function.returnBorrowFields.size() != 1 ||
        function.returnBorrowFields.front().sourceFields.size() != 2)
      continue;
    // The prefix is the owned Inner subobject, not its external int referent.
    function.returnBorrowFields.front().sourceFields.pop_back();
    changed = true;
  }
  return changed && !verifyMirProgram(forged).valid() ? 0 : 1;
}
