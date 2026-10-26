#include <stdio.h>
#include <stdlib.h>

static unsigned long long expected(int family, int count) {
  unsigned long long value = 7;
  unsigned long long array[7] = {1, 2, 3, 4, 5, 6, 7};
  const unsigned char text[] = "abcdefghijklmnop";
  for (int index = 0; index < count; ++index) {
    switch (family) {
    case 0:
    case 5:
      value = (value * 33 + (unsigned)index) % 1000003;
      break;
    case 1:
      value = (value * (value % 2 ? 17 : 33) + (unsigned)index) % 1000003;
      break;
    case 2:
      array[index % 7] = (array[index % 7] * 3 + (unsigned)index) % 1009;
      break;
    case 3:
      value = (value * 33 + text[index % 16]) % 1000003;
      break;
    case 4:
      value = (value * 33 + (unsigned)(index % 31)) % 1000003;
      break;
    case 6:
      value = (value * 33 + (unsigned)(index % 31)) % 1000003;
      break;
    case 7:
      // Seven identical float lanes compute (input + 2) * 2 - input.
      // Every intermediate and the reduction are exactly representable.
      value = (value * 33 + 7 * (unsigned)(index % 31 + 5)) % 1000003;
      break;
    default:
      exit(2);
    }
  }
  if (family == 2) {
    value = 0;
    for (int index = 0; index < 7; ++index)
      value += array[index];
  }
  return value;
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  int family = atoi(argv[1]);
  int iterations = atoi(argv[2]);
  if (family < 0 || family > 7 || iterations < 1 || iterations > 1000000000)
    return 2;
  unsigned long long wanted = expected(family, iterations);
  unsigned long long wanted_effects = family == 4   ? 1
                                      : family == 6 ? (unsigned)iterations
                                                    : 0;
  printf("{\"checksum\":%llu,\"effects\":%llu}\n", wanted, wanted_effects);
  return 0;
}
