#include "allocation_probe.h"

#include <atomic>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <new>

namespace {
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> newCalls{}, newBytes{}, deleteCalls{};
std::atomic<std::uint64_t> exceptionCalls{}, exceptionBytes{};
using AllocateException = void *(*)(std::size_t) noexcept;
#if !defined(__APPLE__)
AllocateException allocateException = nullptr;
#endif

void allocated(std::size_t size) noexcept {
  if (enabled.load(std::memory_order_relaxed)) {
    newCalls.fetch_add(1, std::memory_order_relaxed);
    newBytes.fetch_add(size, std::memory_order_relaxed);
  }
}
void released(void *pointer) noexcept {
  if (pointer && enabled.load(std::memory_order_relaxed))
    deleteCalls.fetch_add(1, std::memory_order_relaxed);
  std::free(pointer);
}
} // namespace

extern "C" void c04_allocation_begin() noexcept {
  if (enabled.load())
    std::abort();
#if !defined(__APPLE__)
  // Resolve while disabled, before any measured throw or worker starts.
  allocateException = reinterpret_cast<AllocateException>(
      dlsym(RTLD_NEXT, "__cxa_allocate_exception"));
  if (!allocateException)
    std::abort();
#endif
  newCalls = newBytes = deleteCalls = 0;
  exceptionCalls = exceptionBytes = 0;
  enabled.store(true);
}

extern "C" C04AllocationCounts c04_allocation_end() noexcept {
  enabled.store(false);
  return {newCalls.load(), newBytes.load(), deleteCalls.load(),
          exceptionCalls.load(), exceptionBytes.load()};
}

void *operator new(std::size_t size) {
  if (void *pointer = std::malloc(size ? size : 1)) {
    allocated(size);
    return pointer;
  }
  throw std::bad_alloc{};
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { released(pointer); }
void operator delete[](void *pointer) noexcept { released(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { released(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept {
  released(pointer);
}
void *operator new(std::size_t size, std::align_val_t alignment) {
  void *pointer = nullptr;
  if (posix_memalign(&pointer, static_cast<std::size_t>(alignment),
                     size ? size : 1))
    throw std::bad_alloc{};
  allocated(size);
  return pointer;
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void *pointer, std::align_val_t) noexcept {
  released(pointer);
}
void operator delete[](void *pointer, std::align_val_t) noexcept {
  released(pointer);
}
void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept {
  released(pointer);
}
void operator delete[](void *pointer, std::size_t, std::align_val_t) noexcept {
  released(pointer);
}

#if defined(__APPLE__)
#define C04_ALLOCATE c04_observed_allocate_exception
#else
#define C04_ALLOCATE __cxa_allocate_exception
#endif

extern "C" void *C04_ALLOCATE(std::size_t size) noexcept {
#if defined(__APPLE__)
  void *pointer = __cxxabiv1::__cxa_allocate_exception(size);
#else
  if (!allocateException)
    allocateException = reinterpret_cast<AllocateException>(
        dlsym(RTLD_NEXT, "__cxa_allocate_exception"));
  if (!allocateException)
    std::abort();
  void *pointer = allocateException(size);
#endif
  if (enabled.load(std::memory_order_relaxed)) {
    exceptionCalls.fetch_add(1, std::memory_order_relaxed);
    exceptionBytes.fetch_add(size, std::memory_order_relaxed);
  }
  return pointer;
}

#if defined(__APPLE__)
// dyld does not replace references from the interposing image itself.
__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void *replacement;
  const void *original;
} interpose[] = {
    {reinterpret_cast<const void *>(&C04_ALLOCATE),
     reinterpret_cast<const void *>(&__cxxabiv1::__cxa_allocate_exception)}};
#endif
