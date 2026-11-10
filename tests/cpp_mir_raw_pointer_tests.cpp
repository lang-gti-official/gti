// C++ representation of raw pointers: native pointer-slot calls, native
// C++ syntax for raw member/index operations and C prototype spelling. The
// backend-neutral MIR facts for the same programs live in
// raw_pointer_tests.cpp.

#include "gti/frontend.h"

#include "cpp_backend_test_support.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testNestedPointerSlotEmission() {
  const auto result = lang::Frontend().analyze("nested-pointer-types.gti", R"(
using Pointer = int*;
using Slot = Pointer*;
[[c_abi]] struct NativeSlots { mut Slot slot; };
struct Holder { mut Slot slot = nullptr;  public: Holder() = default; };
extern "C" {
  void native_write(Slot slot, Pointer value);
  Slot native_return();
  void native_read(int* const* readonly_slot, const int** readonly_value);
}
Slot forward(Slot slot) { return slot; }
int*** deeper(int*** slots) { return slots; }
int main() {
  mut int owner = 7;
  mut Pointer pointer = &owner;
  mut Slot slot = &pointer;
  Holder carrier{.slot = forward(slot)};
  int* const* shared = slot;
  unsafe {
    *slot = &owner;
    native_write(*deeper(&slot), &owner);
    if (*shared != pointer || *carrier.slot != pointer) { return 1; }
  }
  return 0;
}
)");
  expect(result.canGenerateCode(), "nested pointer fixture analyzes");
  if (result.canGenerateCode()) {
    const auto artifact = gti_test::emitCpp(result, result.mir, result.mir,
                                            lang::OptimizationResult{});
    expect(artifact.contents.find("__gti_native_out_") == std::string::npos &&
               artifact.contents.find("union raw_address") != std::string::npos,
           "native calls should use actual pointer slots without contextual "
           "temporary writeback staging");
  }
}

void testRawPointerPipelineEmission() {
  const std::string source = R"(
struct Cell {
  mut int32_t value = 0;

  void set(int32_t next) mut {
    this.value = next;
  }
public:
  Cell() = default;
};

struct PointerHolder {
  mut int32_t* pointer = nullptr;
public:
  PointerHolder() = default;
};

class Box<T> {
  T stored;

public:
  Box(T value) : stored(value) {}

  T get() {
    return this.stored;
  }
};

int32_t identify(int32_t* value) { return 1; }
int32_t identify(const int32_t* value) { return 2; }

int main() {
  mut int32_t values[3] = {10, 20, 30};
  mut int32_t* pointer = nullptr;
  mut Cell cell{};
  mut Cell* cell_pointer = nullptr;
  mut Box<int32_t> box{9};
  mut Box<int32_t>* box_pointer = nullptr;
  mut PointerHolder holder{};
  mut PointerHolder* holder_pointer = nullptr;
  mut int32_t* pointers[2] = {};
  unsafe {
    pointer = &values[0];
    holder.pointer = pointer;
    pointers[0] = pointer;
    holder.pointer += 1;
    pointers[0] += 2;
    holder_pointer = &holder;
    holder_pointer->pointer -= 1;
    pointer[1] = 25;
    *(pointer + 2) = 35;
    int64_t distance = (pointer + 2) - pointer;
    pointer++;
    pointer--;
    cell_pointer = &cell;
    cell_pointer->set(pointer[1]);
    box_pointer = &box;
    int32_t exact = identify(pointer);
    const int32_t* read_only = pointer;
    const int32_t* selected = true ? read_only : nullptr;
    if (distance == 2 and exact == 1 and selected[1] == 25 and
        cell_pointer->value == 25 and box_pointer->get() == 9 and
        *holder.pointer == 10 and *pointers[0] == 35) {
      return 0;
    }
  }
  return 1;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("raw-pointer-pipeline.gti", source);
  expect(frontend.canGenerateCode(), "raw pointer pipeline fixture analyzes");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("owner_access(pointer)") == std::string::npos,
         "raw dereference must not use checked-owner lowering");
  const std::size_t mainMarker =
      artifact.contents.find("scalar-cfg-failure-v1 function-instance");
  const std::string_view emittedMain =
      mainMarker == std::string::npos
          ? std::string_view{}
          : std::string_view(artifact.contents).substr(mainMarker);
  expect(emittedMain.find("(*__gti_mir_v_") != std::string_view::npos &&
             emittedMain.find(").value") != std::string_view::npos &&
             emittedMain.find("[__gti_mir_v_") != std::string_view::npos &&
             emittedMain.find("(&__gti_mir_p_") != std::string_view::npos &&
             emittedMain.find(".get());") != std::string_view::npos,
         "raw member and index operations should remain native C++ syntax");
  expect(emittedMain.find("mir_checked_array_read_v1(") !=
                 std::string_view::npos &&
             emittedMain.find(".get().pointer, __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find("raw_pointer_slot_copy((*__gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find(").pointer, __gti_mir_v_") !=
                 std::string_view::npos &&
             emittedMain.find("[static_cast<std::size_t>(0)] = ") !=
                 std::string_view::npos,
         "raw-pointer compound field and fixed-array element targets should "
         "use ordered pointer arithmetic and byte-preserving slot writes "
         "after any required bounds check");
}

void testPointerBearingCAbiEmission() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("raw-pointer-c-abi.gti", R"(
namespace native {
extern "C" {
  int32_t scalar();
  void* open(int32_t value);
  int32_t inspect(const void* handle);
  void close(void* handle);
}
}

int main() {
  mut int32_t value = native::scalar();
  mut void* handle = nullptr;
  unsafe {
    handle = native::open(value);
    value = native::inspect(handle);
    native::close(handle);
  }
  return value;
}
)");
  expect(valid.canGenerateCode(), "raw pointer C ABI fixture analyzes");
  if (!valid.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  expect(artifact.contents.find("extern \"C\"") != std::string::npos &&
             artifact.contents.find("const void*") != std::string::npos,
         "C prototypes should preserve raw pointer and pointee const types");
}

} // namespace

int main() {
  testNestedPointerSlotEmission();
  testRawPointerPipelineEmission();
  testPointerBearingCAbiEmission();
  if (failures != 0)
    return 1;
  std::cout << "raw pointer C++ representation tests passed\n";
  return 0;
}
