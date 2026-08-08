#include <memory>
#include <utility>

class Item {
public:
  int value = 7;
};

int main() {
  std::unique_ptr<Item> original = std::make_unique<Item>();
  std::unique_ptr<Item> transferred = std::move(original);
  return original == nullptr && transferred != nullptr ? 0 : 1;
}
