#include <climits>

extern "C" int *iterator_sanitizer_control_allocate(int count);

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  if (argv[1][0] == 'a') {
    // A separate native TU keeps the allocation extent opaque to UBSan's
    // object-size check so ASan independently diagnoses the invalid access.
    volatile int *memory = iterator_sanitizer_control_allocate(argc - 1);
    memory[argc + 1] = argc;
    delete[] memory;
    return 0;
  }
  volatile int maximum = INT_MAX;
  volatile int overflow = maximum + argc;
  return overflow;
}
