#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t begin_ns;
static uint64_t env_number(const char *name, uint64_t fallback,
                           uint64_t maximum) {
  const char *text = getenv(name);
  if (!text)
    return fallback;
  char *end;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || end == text || *end || value < 1 || value > maximum) {
    fprintf(stderr, "invalid %s\n", name);
    exit(2);
  }
  return (uint64_t)value;
}
uint64_t bench_rounds(void) { return env_number("BENCH_ROUNDS", 1, 1000000); }
uint64_t bench_seed(void) { return env_number("BENCH_SEED", 17, 1000000); }
static uint64_t now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    abort();
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
void bench_begin(void) { begin_ns = now(); }
void bench_end(uint64_t digest) {
  uint64_t elapsed = now() - begin_ns;
  printf("GTI-COMPARE-1 %llu %llu\n", (unsigned long long)digest,
         (unsigned long long)elapsed);
}
