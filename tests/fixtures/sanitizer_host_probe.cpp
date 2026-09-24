#include <thread>

int main() {
  int value = 0;
  std::thread worker([&] { value = 42; });
  worker.join();
  return value == 42 ? 0 : 1;
}
