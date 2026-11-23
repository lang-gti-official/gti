// Diagnostic only. Measure live allocator block sizes inside the single-thread
// workload.
#include <inttypes.h>
#include <malloc/malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int active;
static uint64_t allocations, live, peak;
static void record(void *p) {
  if (p && active) {
    ++allocations;
    live += malloc_size(p);
    if (live > peak)
      peak = live;
  }
}
static void retire(size_t bytes) {
  if (bytes > live)
    abort();
  live -= bytes;
}
static void *observed_malloc(size_t bytes) {
  void *p = malloc(bytes);
  record(p);
  return p;
}
static void *observed_calloc(size_t n, size_t bytes) {
  void *p = calloc(n, bytes);
  record(p);
  return p;
}
static int observed_align(void **p, size_t align, size_t bytes) {
  int rc = posix_memalign(p, align, bytes);
  if (!rc)
    record(*p);
  return rc;
}
static void *observed_realloc(void *p, size_t bytes) {
  size_t previous = active && p ? malloc_size(p) : 0;
  void *result = realloc(p, bytes);
  if (active && result) {
    retire(previous);
    record(result);
  }
  return result;
}
static void observed_free(void *p) {
  if (active && p)
    retire(malloc_size(p));
  free(p);
}
#define INTERPOSE(fn, original)                                                \
  __attribute__((used, section("__DATA,__interpose"))) static const struct {   \
    const void *replacement, *symbol;                                          \
  } entry_##original = {fn, original}
INTERPOSE(observed_malloc, malloc);
INTERPOSE(observed_calloc, calloc);
INTERPOSE(observed_realloc, realloc);
INTERPOSE(observed_align, posix_memalign);
INTERPOSE(observed_free, free);
void http_heap_start(void) { active = 1; }
void http_heap_finish(void) {
  active = 0;
  printf("allocations=%" PRIu64 " peak_live_bytes=%" PRIu64
         " final_live_bytes=%" PRIu64 "\n",
         allocations, peak, live);
}
