#include <array>
#include <cstddef>

int main(int argc, char **) {
  std::array<int, 3> values = {4, 5, 6};
  std::size_t index = static_cast<std::size_t>(argc + 2);
  return values[index];
}
