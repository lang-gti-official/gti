#pragma once

#include <cstdint>

struct C04AllocationCounts {
  std::uint64_t newCalls, newBytes, deleteCalls;
  std::uint64_t exceptionCalls, exceptionBytes;
};

extern "C" void c04_allocation_begin() noexcept;
extern "C" C04AllocationCounts c04_allocation_end() noexcept;
