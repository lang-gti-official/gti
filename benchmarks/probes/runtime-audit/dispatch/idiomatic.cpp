#include <cstdint>
#include <cstdio>
struct shape {
  virtual double area() const = 0;
  virtual ~shape() = default;
};
struct square : shape {
  double side;
  explicit square(double s) : side(s) {}
  double area() const override { return side * side; }
};
struct circle : shape {
  double radius;
  explicit circle(double r) : radius(r) {}
  double area() const override { return 3.0 * radius * radius; }
};
static double sum_areas(const shape &item, int32_t count) {
  double total = 0.0;
  for (int32_t i = 0; i < count; i++)
    total += item.area();
  return total;
}
int main() {
  square s{1.5};
  circle c{0.5};
  double total = 0.0;
  for (int32_t round = 0; round < 100000; round++) {
    total += sum_areas(s, 1000);
    total += sum_areas(c, 1000);
  }
  std::printf("checksum %lld\n", (long long)int64_t(total));
}
