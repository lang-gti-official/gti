#include <cstdlib>
#include <cstring>

extern "C" int gti_test_read(int *, int);
extern "C" void gti_test_write(int *, int, int);

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  auto *allocation = static_cast<int *>(std::malloc(2 * sizeof(int)));
  if (!allocation)
    return 3;
  allocation[0] = 41;
  allocation[1] = 42;
  if (!std::strcmp(argv[1], "safe")) {
    gti_test_write(allocation, 1, 43);
    const bool valid = gti_test_read(allocation, 0) == 41 &&
                       gti_test_read(allocation, 1) == 43;
    std::free(allocation);
    return valid ? 0 : 4;
  }
  int *pointer = allocation;
  int offset = 0;
  if (!std::strncmp(argv[1], "null", 4))
    pointer = nullptr;
  else if (!std::strncmp(argv[1], "misaligned", 10))
    pointer = reinterpret_cast<int *>(reinterpret_cast<char *>(allocation) + 1);
  else if (!std::strncmp(argv[1], "out-of-bounds", 13))
    offset = 2;
  else if (!std::strncmp(argv[1], "use-after-free", 14))
    std::free(allocation);
  else {
    std::free(allocation);
    return 2;
  }
  // Only generated GTI code performs the deliberately invalid access.
  if (std::strstr(argv[1], "-write"))
    gti_test_write(pointer, offset, 99);
  else {
    volatile int observed = gti_test_read(pointer, offset);
    static_cast<void>(observed);
  }
  if (std::strncmp(argv[1], "use-after-free", 14))
    std::free(allocation);
  return 0;
}
