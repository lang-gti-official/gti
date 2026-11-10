// Frozen C++-backend coverage moved from unsafe_function_tests.cpp: native
// emission of an explicitly constructed unsafe-constructor value moved into
// raw storage, which the LLVM backend does not yet lower (O-LLVM-05).
#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

int main() {
  const std::string declaration = R"(
struct ReadByte {
  mut int value = 0;
  unsafe ReadByte(const int* address) {
    unsafe { value = *address; }
  }
public:
  ReadByte() = default;
};
struct Generic<T> {
  T value;
  unsafe Generic(T source) : value(source) {}
};
struct LocalGeneric {
  unsafe LocalGeneric<T>(T source) {}
public:
  LocalGeneric() = default;
};
struct ConstantContract {
  int value;
  unsafe constexpr ConstantContract(int source) : value(source) {}
};
struct NativeFactory {
  unsafe static ReadByte create(const int* address);
public:
  NativeFactory() = default;
};
unsafe ReadByte NativeFactory::create(const int* pointer) {
  unsafe { return ReadByte(pointer); }
}
)";
  const auto completedValue = analyzeWithStandardLibrary(
      "unsafe-function.gti",
      "import std.memory;\n" + declaration +
          "int main() { mut std::uninitialized<Generic<int>> storage{}; "
          "unsafe { std::construct_at(storage.data(), Generic<int>(1)); "
          "std::destroy_at(storage.data()); } return 0; }");
  expect(completedValue.canGenerateCode(),
         "an explicitly constructed value can be moved into raw storage");
  if (completedValue.canGenerateCode())
    expect(!gti_test::emitCppText(completedValue).empty(),
           "completed unsafe-constructor values keep ordinary move semantics");
  return failures == 0 ? 0 : 1;
}
