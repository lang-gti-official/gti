// Test-only parking proposal. Nothing here is installed or used by GTI bodies.
// The raw scalar interface deliberately avoids std::atomic_ref's exclusive-
// access object-lifetime contract. GCC/Clang atomic builtins and LLVM lanes
// share the platform atomic ABI; this probe must run on each supported host.
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
void *gti_test_lane_initialize(unsigned, std::uint64_t);
std::uint64_t gti_test_lane_read(unsigned);
void gti_test_llvm_store8(void *, std::uint64_t);
void gti_test_llvm_store16(void *, std::uint64_t);
void gti_test_llvm_store32(void *, std::uint64_t);
void gti_test_llvm_store64(void *, std::uint64_t);
std::uint64_t gti_test_llvm_load8(void *);
std::uint64_t gti_test_llvm_load16(void *);
std::uint64_t gti_test_llvm_load32(void *);
std::uint64_t gti_test_llvm_load64(void *);
}

namespace {
// Proposed v1 load-order encoding, independent of host and MIR enum ordinals.
constexpr std::uint32_t relaxed = 0, acquire = 1, sequential = 2;
struct Bucket {
  std::mutex mutex;
  std::condition_variable condition;
};
using Buckets = std::array<Bucket, 4>;
// The proposed runtime table must survive late TLS/static cleanup. Reserve
// bounded process-lifetime bytes, initialize once, and deliberately never run
// native bucket teardown. Unloading the runtime while calls/callbacks remain
// live is not allowed; embedding integration still owes that lifetime proof.
alignas(Buckets) constinit std::array<std::byte, sizeof(Buckets)> bucketBytes{};
constinit std::once_flag bucketInitialization;
Buckets &parkingBuckets() {
  auto *result = reinterpret_cast<Buckets *>(bucketBytes.data());
  std::call_once(bucketInitialization, [=] { std::construct_at(result); });
  return *result;
}
struct Probe {
  std::atomic<unsigned> enrolled{0};
  bool pauseBeforeEnrollment = false;
  std::atomic<bool> fastpathChecked{false};
  std::atomic<bool> resume{false};
};
thread_local Probe *probe =
    nullptr; // Deterministic test hook, not proposed ABI.

void require(bool condition) {
  if (!condition)
    std::abort();
}

void validate(const void *address, std::uint32_t width) {
  require(address != nullptr &&
          (width == 1 || width == 2 || width == 4 || width == 8) &&
          reinterpret_cast<std::uintptr_t>(address) % width == 0);
}

std::size_t bucketIndex(const void *address, std::uint32_t width) {
  return ((reinterpret_cast<std::uintptr_t>(address) >> 3) ^ width) %
         std::tuple_size_v<Buckets>;
}

template <typename T> T load(const void *address, std::uint32_t order) {
  static_assert(__atomic_always_lock_free(sizeof(T), nullptr));
  const auto *lane = static_cast<const T *>(address);
  switch (order) {
  case relaxed:
    return __atomic_load_n(lane, __ATOMIC_RELAXED);
  case acquire:
    return __atomic_load_n(lane, __ATOMIC_ACQUIRE);
  case sequential:
    return __atomic_load_n(lane, __ATOMIC_SEQ_CST);
  default:
    std::abort();
  }
}

template <typename T>
void wait(const void *address, std::uint64_t old, std::uint32_t order) {
  require(old <= std::numeric_limits<T>::max());
  // No bucket access, host lock, waiter allocation or implicit notification on
  // the ready path. Every successful return has the requested atomic load.
  if (load<T>(address, order) != static_cast<T>(old))
    return;
  if (probe && probe->pauseBeforeEnrollment) {
    probe->fastpathChecked.store(true);
    probe->fastpathChecked.notify_all();
    probe->resume.wait(false);
  }
  auto &bucket = parkingBuckets()[bucketIndex(address, sizeof(T))];
  std::unique_lock lock(bucket.mutex);
  while (load<T>(address, order) == static_cast<T>(old)) {
    if (probe) {
      probe->enrolled.fetch_add(1);
      probe->enrolled.notify_all();
    }
    // The notifier takes this same mutex. Enrollment and unlock are atomic,
    // so a notification cannot slip between the predicate and sleeping.
    bucket.condition.wait(lock);
  }
}

using Store = void (*)(void *, std::uint64_t);
using Load = std::uint64_t (*)(void *);
constexpr std::array<Store, 4> stores{
    gti_test_llvm_store8, gti_test_llvm_store16, gti_test_llvm_store32,
    gti_test_llvm_store64};
constexpr std::array<Load, 4> loads{gti_test_llvm_load8, gti_test_llvm_load16,
                                    gti_test_llvm_load32, gti_test_llvm_load64};

void await(Probe &state, unsigned count) {
  auto observed = state.enrolled.load();
  while (observed < count) {
    state.enrolled.wait(observed);
    observed = state.enrolled.load();
  }
}
} // namespace

// Candidate fixed-width C signatures. Invalid protocol inputs and host waiting
// failures terminate; no C++ exception or invented GTI failure leaves the ABI.
extern "C" void gti_test_atomic_wait_v1(const void *address, std::uint64_t old,
                                        std::uint32_t width,
                                        std::uint32_t order) noexcept {
  validate(address, width);
  require(order <= sequential);
  switch (width) {
  case 1:
    wait<std::uint8_t>(address, old, order);
    break;
  case 2:
    wait<std::uint16_t>(address, old, order);
    break;
  case 4:
    wait<std::uint32_t>(address, old, order);
    break;
  case 8:
    wait<std::uint64_t>(address, old, order);
    break;
  default:
    std::abort();
  }
}

extern "C" void gti_test_atomic_notify_v1(const void *address,
                                          std::uint32_t width,
                                          std::uint32_t all) noexcept {
  validate(address, width);
  require(all <= 1);
  auto &bucket = parkingBuckets()[bucketIndex(address, width)];
  const std::lock_guard lock(bucket.mutex);
  // Hash collisions make condition.notify_one insufficient: it might choose a
  // waiter for another address and strand the eligible one. Both operations
  // broadcast this bounded bucket. Extra wakes remain internal predicate
  // retries, matching GTI's "at least one" contract for notify_one.
  bucket.condition.notify_all();
}

namespace {
void scalarLanes() {
  for (unsigned index = 0; index < 9; ++index) {
    const unsigned lane = index == 8 ? 0 : index / 2;
    const unsigned width = 1U << lane;
    const auto old = index == 8 ? 0 : UINT64_C(1) << (width * 8 - 1);
    const auto next = old | 1;
    for (const auto order : {relaxed, acquire, sequential}) {
      void *address = gti_test_lane_initialize(index, old);
      require(loads[lane](address) == old);
      Probe state;
      int payload = 0;
      std::thread worker([&] {
        probe = &state;
        gti_test_atomic_wait_v1(address, old, width, order);
        require(loads[lane](address) == next);
        if (order != relaxed)
          require(payload == 42);
      });
      await(state, 1);
      payload = 42;
      stores[lane](address, next);
      gti_test_atomic_notify_v1(address, width, 0);
      worker.join();
      require(gti_test_lane_read(index) == next);
      // Value already changed: no notification is required.
      gti_test_atomic_wait_v1(address, old, width, order);
    }
  }
}

void spuriousAndAba() {
  alignas(8) std::uint64_t value = 0;
  // Notifications before enrollment are not retained as future credit. ABA
  // before enrollment is likewise not a remembered transition.
  gti_test_atomic_notify_v1(&value, 8, 1);
  gti_test_llvm_store64(&value, 1);
  gti_test_llvm_store64(&value, 0);
  Probe state;
  std::atomic<bool> done{false};
  std::thread worker([&] {
    probe = &state;
    gti_test_atomic_wait_v1(&value, 0, 8, acquire);
    done.store(true);
  });
  await(state, 1);
  require(!done.load());
  gti_test_atomic_notify_v1(&value, 8, 0);
  await(state, 2);
  require(!done.load());
  gti_test_llvm_store64(&value, 2);
  gti_test_atomic_notify_v1(&value, 8, 1);
  worker.join();
  require(done.load());
  // Reusing the address after every call has ended retains no waiter/credit.
  gti_test_llvm_store64(&value, 3);
  gti_test_atomic_wait_v1(&value, 2, 8, relaxed);
}

void collidingAddresses() {
  alignas(8) std::array<std::uint32_t, 32> values{};
  auto *a = &values[0];
  std::uint32_t *b = nullptr;
  for (auto &candidate : values)
    if (&candidate != a && bucketIndex(&candidate, 4) == bucketIndex(a, 4)) {
      b = &candidate;
      break;
    }
  require(b != nullptr);
  Probe first, second;
  std::atomic<bool> secondDone{false};
  std::thread left([&] {
    probe = &first;
    gti_test_atomic_wait_v1(a, 0, 4, acquire);
  });
  std::thread right([&] {
    probe = &second;
    gti_test_atomic_wait_v1(b, 0, 4, acquire);
    secondDone.store(true);
  });
  await(first, 1);
  await(second, 1);
  gti_test_llvm_store32(a, 1);
  gti_test_atomic_notify_v1(a, 4, 0);
  left.join();
  await(second, 2);
  require(!secondDone.load());
  gti_test_llvm_store32(b, 1);
  gti_test_atomic_notify_v1(b, 4, 1);
  right.join();
}

void multipleWaiters() {
  alignas(8) std::uint64_t value = 0;
  std::array<Probe, 2> states;
  std::array<std::thread, 2> workers;
  for (std::size_t i = 0; i < workers.size(); ++i)
    workers[i] = std::thread([&, i] {
      probe = &states[i];
      gti_test_atomic_wait_v1(&value, 0, 8, acquire);
    });
  for (auto &state : states)
    await(state, 1);
  gti_test_llvm_store64(&value, 1);
  gti_test_atomic_notify_v1(&value, 8, 0);
  for (auto &worker : workers)
    worker.join();
}

void changeBeforeEnrollment() {
  alignas(8) std::uint64_t value = 0;
  Probe state;
  state.pauseBeforeEnrollment = true;
  std::thread worker([&] {
    probe = &state;
    gti_test_atomic_wait_v1(&value, 0, 8, acquire);
  });
  state.fastpathChecked.wait(false);
  // This notification completes before the worker even takes the bucket
  // mutex. Only the second atomic predicate check prevents a lost wakeup.
  gti_test_llvm_store64(&value, 1);
  gti_test_atomic_notify_v1(&value, 8, 0);
  state.resume.store(true);
  state.resume.notify_all();
  worker.join();
  require(state.enrolled.load() == 0);
}
} // namespace

int main(int argc, char **argv) {
  alignas(8) std::array<std::uint8_t, 16> bytes{};
  if (argc == 2) {
    if (std::strcmp(argv[1], "null") == 0)
      gti_test_atomic_wait_v1(nullptr, 0, 1, relaxed);
    else if (std::strcmp(argv[1], "width") == 0)
      gti_test_atomic_wait_v1(bytes.data(), 0, 3, relaxed);
    else if (std::strcmp(argv[1], "alignment") == 0)
      gti_test_atomic_wait_v1(bytes.data() + 1, 0, 8, relaxed);
    else if (std::strcmp(argv[1], "order") == 0)
      gti_test_atomic_wait_v1(bytes.data(), 0, 1, 3);
    else if (std::strcmp(argv[1], "bits") == 0)
      gti_test_atomic_wait_v1(bytes.data(), 256, 1, relaxed);
    else if (std::strcmp(argv[1], "notify") == 0)
      gti_test_atomic_notify_v1(bytes.data(), 1, 2);
    return 1;
  }
  scalarLanes();
  spuriousAndAba();
  collidingAddresses();
  multipleWaiters();
  changeBeforeEnrollment();
  std::cout
      << "wait proposal: C17/LLVM lanes, 9 domains/3 orders, publication, "
         "spurious/ABA, collisions and multiple waiters passed\n";
}
