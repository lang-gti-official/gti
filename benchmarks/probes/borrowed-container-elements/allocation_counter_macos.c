// Diagnostic allocation counting only. Use comparative/support.c for timing:
// interposition adds overhead and is not a runtime-performance measurement.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static _Thread_local int counting;
static _Thread_local uint64_t allocations, deallocations, requested_bytes;
static void record(void *pointer, size_t bytes) {
  if (counting && pointer) {
    ++allocations;
    requested_bytes += bytes;
  }
}
static void *counted_malloc(size_t bytes) {
  void *pointer = malloc(bytes);
  record(pointer, bytes);
  return pointer;
}
static void *counted_calloc(size_t count, size_t bytes) {
  void *pointer = calloc(count, bytes);
  record(pointer, count * bytes);
  return pointer;
}
static void *counted_realloc(void *previous, size_t bytes) {
  void *pointer = realloc(previous, bytes);
  record(pointer, bytes);
  return pointer;
}
static int counted_posix_memalign(void **pointer, size_t alignment,
                                  size_t bytes) {
  int status = posix_memalign(pointer, alignment, bytes);
  if (!status)
    record(*pointer, bytes);
  return status;
}
static void counted_free(void *pointer) {
  if (counting && pointer)
    ++deallocations;
  free(pointer);
}
#define INTERPOSE(replacement, original)                                       \
  __attribute__((used)) static struct {                                        \
    const void *replace;                                                       \
    const void *original;                                                      \
  } pair_##original __attribute__((section("__DATA,__interpose"))) = {         \
      (const void *)&replacement, (const void *)&original}
INTERPOSE(counted_malloc, malloc);
INTERPOSE(counted_calloc, calloc);
INTERPOSE(counted_realloc, realloc);
INTERPOSE(counted_posix_memalign, posix_memalign);
INTERPOSE(counted_free, free);

uint64_t bench_rounds(void) { return 1; }
void bench_begin(void) {
  allocations = deallocations = requested_bytes = 0;
  counting = 1;
}
void bench_end(uint64_t digest) {
  counting = 0;
  printf("GTI-ALLOC-1 %llu %llu %llu %llu\n", (unsigned long long)digest,
         (unsigned long long)allocations, (unsigned long long)deallocations,
         (unsigned long long)requested_bytes);
}
