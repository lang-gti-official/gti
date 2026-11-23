// Wall-clock samples use a separate executable from the macOS allocation probe.
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#define SAMPLES 100000
static uint64_t started, previous, samples[SAMPLES];
static size_t used;
static uint64_t mach_calls, unix_calls;
static void calls(uint64_t *m, uint64_t *u) {
#ifdef __APPLE__
  task_events_info_data_t stats;
  mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&stats, &n) != KERN_SUCCESS) abort();
  *m = (uint32_t)stats.syscalls_mach;
  *u = (uint32_t)stats.syscalls_unix;
#else
  *m = *u = 0;
#endif
}
uint64_t probe_option(int index) {
  const char *names[] = {"HTTP_CASE", "HTTP_ROUNDS", "HTTP_UPLOAD", "HTTP_ADAPTER"};
  uint64_t defaults[] = {0, 10000, 1048576, 0};
  const char *value = getenv(names[index]);
  uint64_t n = value ? strtoull(value, NULL, 10) : defaults[index];
  if ((index == 0 && n > 4) || (index == 1 && (!n || n > SAMPLES)) ||
      (index == 2 && n > 1048576) || (index == 3 && n > 1)) abort();
  return n;
}
static uint64_t now(void) {
#ifdef __APPLE__
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
  return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}
extern void http_heap_start(void);
extern void http_heap_finish(void);
void probe_start(void) {
  http_heap_start();
  calls(&mach_calls, &unix_calls);
  started = previous = now();
}
void probe_sample(void) {
  uint64_t end = now();
  if (used == SAMPLES) abort();
  samples[used++] = end - previous;
  previous = end;
}
static int compare(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
void probe_finish(uint64_t digest, uint64_t reads, uint64_t writes, uint64_t flushes) {
  uint64_t duration = now() - started, m, u;
  calls(&m, &u);
  http_heap_finish();
  if (!used) abort();
  qsort(samples, used, sizeof(samples[0]), compare);
  printf("requests=%zu ns=%" PRIu64 " p99_ns=%" PRIu64 " digest=%" PRIu64
         " reads=%" PRIu64 " writes=%" PRIu64 " flushes=%" PRIu64
         " mach_calls=%" PRIu64 " unix_calls=%" PRIu64 "\n",
         used, duration, samples[(used * 99 - 1) / 100], digest,
         reads, writes, flushes, m - mach_calls, u - unix_calls);
}
#ifndef HTTP_TRACK_ALLOCATIONS
void http_heap_start(void) {}
void http_heap_finish(void) {}
#endif
