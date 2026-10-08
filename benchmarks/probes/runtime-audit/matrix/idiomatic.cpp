#include <cstdint>
#include <cstdio>
#include <vector>
static void multiply(const std::vector<double>& a, const std::vector<double>& b,
                     std::vector<double>& c, std::size_t n) {
  for (std::size_t i = 0; i < n; i++)
    for (std::size_t k = 0; k < n; k++) {
      double scale = a[i * n + k];
      for (std::size_t j = 0; j < n; j++) c[i * n + j] += scale * b[k * n + j];
    }
}
int main() {
  std::size_t n = 320;
  std::vector<double> a(n * n), b(n * n), c(n * n);
  for (std::size_t index = 0; index < n * n; index++) {
    a[index] = double(index % 17) * 0.25;
    b[index] = double(index % 13) * 0.5;
  }
  for (int repeat = 0; repeat < 3; repeat++) multiply(a, b, c, n);
  double total = 0.0;
  for (double value : c) total += value;
  std::printf("checksum %lld\n", (long long)int64_t(total));
}
