#include <cstdint>
#include <cstdio>
#include <vector>
struct particle { float x, y, vx, vy; };
int main() {
  std::vector<particle> particles;
  for (int32_t i = 0; i < 100000; i++) {
    float f = float(i % 100);
    particles.push_back(particle{f, f * 0.5f, 1.0f, -0.5f});
  }
  for (int32_t frame = 0; frame < 2000; frame++)
    for (particle& p : particles) {
      p.vy += 0.01f;
      p.x += p.vx * 0.016f;
      p.y += p.vy * 0.016f;
      if (p.y > 100.0f) p.vy = -p.vy * 0.5f;
    }
  double total = 0.0;
  for (const particle& p : particles) total += double(p.x) + double(p.y);
  std::printf("checksum %lld\n", (long long)int64_t(total));
}
