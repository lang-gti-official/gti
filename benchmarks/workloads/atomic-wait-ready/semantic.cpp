#include <atomic>
#include <cstdint>
#include <cstdio>

int main() {
  std::atomic<std::int32_t> ready{1};
  for (std::int32_t i = 0; i < 50000000; ++i) {
    ready.wait(0, std::memory_order_acquire);
    ready.notify_one();
    ready.notify_all();
  }
  if (ready.load() != 1)
    return 1;
  std::puts("GTI-BENCH-1 digest:0000000000000001 work-units:50000000");
  return 0;
}
