#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

struct EntityState {
  std::int32_t position_x = 0;
  std::int32_t position_y = 0;
  std::int32_t velocity_x = 0;
  std::int32_t velocity_y = 0;
};

int main() {
  constexpr std::size_t entity_count = 1024;
  std::vector<EntityState> entities(entity_count);
  for (std::size_t index = 0; index < entity_count; ++index) {
    EntityState &entity = entities[index];
    entity.position_x = static_cast<std::int32_t>(index % 257) - 128;
    entity.position_y = static_cast<std::int32_t>(index % 193) - 96;
    entity.velocity_x = static_cast<std::int32_t>(index % 11) - 5;
    entity.velocity_y = static_cast<std::int32_t>(index % 7) - 3;
  }

  for (std::int32_t tick = 0; tick < 60000; ++tick) {
    const std::int32_t phase = tick % 17;
    for (std::size_t index = 0; index < entity_count; ++index) {
      EntityState &entity = entities[index];
      const std::int32_t lane = static_cast<std::int32_t>(index % 13);
      entity.velocity_x = (entity.velocity_x * 3 + phase + lane) % 61 - 30;
      entity.velocity_y = (entity.velocity_y * 5 + phase + lane) % 67 - 33;
      entity.position_x =
          (entity.position_x + entity.velocity_x + 4096) % 8191 - 4095;
      entity.position_y =
          (entity.position_y + entity.velocity_y + 4096) % 8191 - 4095;
    }
  }

  std::uint64_t digest = 146959810;
  for (const EntityState &entity : entities) {
    const std::int64_t values[] = {
        static_cast<std::int64_t>(entity.position_x) + 16384,
        static_cast<std::int64_t>(entity.position_y) + 16384,
        static_cast<std::int64_t>(entity.velocity_x) + 100,
        static_cast<std::int64_t>(entity.velocity_y) + 100,
    };
    for (const std::int64_t value : values) {
      digest = (digest * 131 + static_cast<std::uint64_t>(value)) % 1000000007;
    }
  }
  if (digest != 0x12e84ad4) {
    return 1;
  }
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:61440000\n";
  return 0;
}
