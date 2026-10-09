#include <climits>

namespace {
int *retained = nullptr;
}
extern "C" void opaque_pointer_effect(int *pointer) { *pointer = INT_MAX; }
extern "C" void retain_cell_pointer(int *pointer) { retained = pointer; }
extern "C" void overwrite_retained_cell() { *retained = INT_MAX; }
