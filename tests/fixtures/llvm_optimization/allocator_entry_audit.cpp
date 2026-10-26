#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Test-only public allocator entry observation. This does not replace a GTI
// byte provider. Counters include attempts, not successful/live allocations.
// No allocation, logging or pointer map is performed by an active wrapper.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static std::atomic<bool> active{false};
static std::atomic<std::uint64_t> calls[5];
static std::atomic<unsigned> begins{0}, ends{0};

enum Entry { Malloc, Calloc, Realloc, PosixMemalign, Free };
static void observe(Entry entry) noexcept {
  if (active.load(std::memory_order_relaxed))
    calls[entry].fetch_add(1, std::memory_order_relaxed);
}

#if defined(__APPLE__)
#define AUDIT_ENTRY(name) audit_##name
#define REAL_ENTRY(name) name
#else
#define AUDIT_ENTRY(name) __wrap_##name
#define REAL_ENTRY(name) __real_##name
extern "C" void *__real_malloc(std::size_t) noexcept;
extern "C" void *__real_calloc(std::size_t, std::size_t) noexcept;
extern "C" void *__real_realloc(void *, std::size_t) noexcept;
extern "C" int __real_posix_memalign(void **, std::size_t,
                                     std::size_t) noexcept;
extern "C" void __real_free(void *) noexcept;
#endif

extern "C" void *AUDIT_ENTRY(malloc)(std::size_t bytes) noexcept {
  observe(Malloc);
  return REAL_ENTRY(malloc)(bytes);
}
extern "C" void *AUDIT_ENTRY(calloc)(std::size_t count,
                                     std::size_t bytes) noexcept {
  observe(Calloc);
  return REAL_ENTRY(calloc)(count, bytes);
}
extern "C" void *AUDIT_ENTRY(realloc)(void *address,
                                      std::size_t bytes) noexcept {
  observe(Realloc);
  return REAL_ENTRY(realloc)(address, bytes);
}
extern "C" int AUDIT_ENTRY(posix_memalign)(void **address,
                                           std::size_t alignment,
                                           std::size_t bytes) noexcept {
  observe(PosixMemalign);
  return REAL_ENTRY(posix_memalign)(address, alignment, bytes);
}
extern "C" void AUDIT_ENTRY(free)(void *address) noexcept {
  observe(Free);
  REAL_ENTRY(free)(address);
}

#if defined(__APPLE__)
// Calls from this test dylib retain the original bindings. The participating
// executable and its loaded images use dyld's interpose tuple mechanism.
#define INTERPOSE(name)                                                        \
  {reinterpret_cast<const void *>(&AUDIT_ENTRY(name)),                         \
   reinterpret_cast<const void *>(&name)}
__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void *replacement;
  const void *original;
} interpositions[] = {INTERPOSE(malloc), INTERPOSE(calloc), INTERPOSE(realloc),
                      INTERPOSE(posix_memalign), INTERPOSE(free)};
#endif

static unsigned long long input(const char *name) noexcept {
  const char *value = std::getenv(name);
  return value ? std::strtoull(value, nullptr, 10) : 0;
}
extern "C" int gti_cost_iterations() noexcept {
  return static_cast<int>(input("GTI_ALLOCATOR_AUDIT_ITERATIONS"));
}
extern "C" void gti_cost_begin() noexcept {
  active.store(false, std::memory_order_relaxed);
  for (auto &entry : calls)
    entry.store(0, std::memory_order_relaxed);
  begins.fetch_add(1, std::memory_order_relaxed);
  active.store(true, std::memory_order_relaxed);
}
extern "C" int gti_cost_end(unsigned long long checksum,
                            unsigned long long effects) noexcept {
  active.store(false, std::memory_order_relaxed);
  ends.fetch_add(1, std::memory_order_relaxed);
  // Parsing and printing occur outside the observation phase.
  const bool correct = checksum == input("GTI_ALLOCATOR_AUDIT_CHECKSUM") &&
                       effects == input("GTI_ALLOCATOR_AUDIT_EFFECTS");
  std::printf("{\"kind\":\"allocator-entry-observation\","
              "\"checksum\":%llu,\"effects\":%llu,\"begins\":%u,\"ends\":%u,"
              "\"malloc\":%llu,\"calloc\":%llu,\"realloc\":%llu,"
              "\"posix_memalign\":%llu,\"free\":%llu}\n",
              checksum, effects, begins.load(std::memory_order_relaxed),
              ends.load(std::memory_order_relaxed),
              static_cast<unsigned long long>(calls[Malloc].load()),
              static_cast<unsigned long long>(calls[Calloc].load()),
              static_cast<unsigned long long>(calls[Realloc].load()),
              static_cast<unsigned long long>(calls[PosixMemalign].load()),
              static_cast<unsigned long long>(calls[Free].load()));
  return correct ? 0 : 1;
}
