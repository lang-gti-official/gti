#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {

struct EntityState {
  std::int32_t position_x = 0;
  std::int32_t position_y = 0;
  std::int32_t velocity_x = 0;
  std::int32_t velocity_y = 0;
};

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::int32_t checked_add_i32(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int32_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int32_t>::min() - right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left + right);
}

std::int64_t checked_add_i64(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    checked_failure();
  }
  return left + right;
}

std::int32_t checked_sub_i32(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left < std::numeric_limits<std::int32_t>::min() + right) ||
      (right < 0 && left > std::numeric_limits<std::int32_t>::max() + right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left - right);
}

std::int32_t checked_mul_i32(std::int32_t left, std::int32_t right) {
  const std::int64_t product =
      static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
  if (product < std::numeric_limits<std::int32_t>::min() ||
      product > std::numeric_limits<std::int32_t>::max()) {
    checked_failure();
  }
  return static_cast<std::int32_t>(product);
}

std::int32_t checked_rem_i32(std::int32_t left, std::int32_t right) {
  if (right == 0 ||
      (left == std::numeric_limits<std::int32_t>::min() && right == -1)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left % right);
}

std::uint64_t checked_add_u64(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    checked_failure();
  }
  return left + right;
}

std::uint64_t checked_mul_u64(std::uint64_t left, std::uint64_t right) {
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
    checked_failure();
  }
  return left * right;
}

std::size_t checked_add_size(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    checked_failure();
  }
  return left + right;
}

EntityState &checked_at(std::vector<EntityState> &entities, std::size_t index) {
  if (index >= entities.size()) {
    checked_failure();
  }
  return entities[index];
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:51200000\n";
}

} // namespace

int main() {
  constexpr std::size_t entity_count = 1024;
  std::vector<EntityState> entities(entity_count);
  for (std::size_t index = 0; index < entity_count;
       index = checked_add_size(index, 1)) {
    EntityState &entity = checked_at(entities, index);
    entity.position_x = static_cast<std::int32_t>(index % 257) - 128;
    entity.position_y = static_cast<std::int32_t>(index % 193) - 96;
    entity.velocity_x = static_cast<std::int32_t>(index % 11) - 5;
    entity.velocity_y = static_cast<std::int32_t>(index % 7) - 3;
  }

  for (std::int32_t tick = 0; tick < 50000; tick = checked_add_i32(tick, 1)) {
    const std::int32_t phase = checked_rem_i32(tick, 17);
    for (std::size_t index = 0; index < entity_count;
         index = checked_add_size(index, 1)) {
      EntityState &entity = checked_at(entities, index);
      const std::int32_t lane = static_cast<std::int32_t>(index % 13);
      entity.velocity_x = checked_sub_i32(
          checked_rem_i32(
              checked_add_i32(
                  checked_add_i32(checked_mul_i32(entity.velocity_x, 3), phase),
                  lane),
              61),
          30);
      entity.velocity_y = checked_sub_i32(
          checked_rem_i32(
              checked_add_i32(
                  checked_add_i32(checked_mul_i32(entity.velocity_y, 5), phase),
                  lane),
              67),
          33);
      entity.position_x = checked_sub_i32(
          checked_rem_i32(
              checked_add_i32(
                  checked_add_i32(entity.position_x, entity.velocity_x), 4096),
              8191),
          4095);
      entity.position_y = checked_sub_i32(
          checked_rem_i32(
              checked_add_i32(
                  checked_add_i32(entity.position_y, entity.velocity_y), 4096),
              8191),
          4095);
    }
  }

  std::uint64_t digest = 146959810;
  for (std::size_t index = 0; index < entity_count;
       index = checked_add_size(index, 1)) {
    const EntityState &entity = checked_at(entities, index);
    const std::int64_t values[] = {
        checked_add_i64(entity.position_x, 16384),
        checked_add_i64(entity.position_y, 16384),
        checked_add_i64(entity.velocity_x, 100),
        checked_add_i64(entity.velocity_y, 100),
    };
    for (const std::int64_t value : values) {
      digest = checked_add_u64(checked_mul_u64(digest, 131),
                               static_cast<std::uint64_t>(value)) %
               1000000007;
    }
  }
  if (digest != 0x39624947) {
    return 1;
  }
  print_record(digest);
  return 0;
}
