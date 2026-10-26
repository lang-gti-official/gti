#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

struct Pair {
  std::uint64_t a, b;
};
struct Large {
  std::uint64_t a, b, c;
};
struct Hfa {
  double values[4];
};
struct Text {
  const char *data;
  std::uint64_t length;
};
using ScalarCallback = std::int32_t (*)(std::int32_t);
using PairCallback = Pair (*)(Pair);
using LargeCallback = Large (*)(Large);
using HfaCallback = Hfa (*)(Hfa);
extern "C" {
ScalarCallback get_callback_scalar();
PairCallback get_callback_pair();
LargeCallback get_callback_large();
HfaCallback get_callback_hfa();
std::uint64_t (*get_callback_view())(Text);
ScalarCallback get_callback_exception();
ScalarCallback get_callback_failure();
std::int32_t native_throw(std::int32_t value) {
  struct Guard {
    ~Guard() { std::fputs("foreign-cleanup\n", stderr); }
  } guard;
  if (value < 0)
    throw value;
  return value;
}
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::set_terminate([] { std::_Exit(91); });
  const std::string_view mode(argv[1]);
  if (mode == "exception") {
    std::signal(SIGABRT, [](int) { std::_Exit(86); });
    try {
      get_callback_exception()(-1);
    } catch (...) {
      return 92;
    }
    return 93;
  }
  if (mode == "failure") {
    get_callback_failure()(0);
    return 94;
  }
  if (mode != "success")
    return 3;
  if (get_callback_scalar()(-1234567) != -1234567)
    return 4;
  if (get_callback_view()({"a\0bc", 4}) != 4)
    return 8;
  const auto pair =
      get_callback_pair()({0x8123456789abcdef, 0xabcdef0123456789});
  if (pair.a != 0x8123456789abcdef || pair.b != 0xabcdef0123456789)
    return 5;
  const auto large = get_callback_large()({17, 0xfedcba9876543210, 99});
  if (large.a != 17 || large.b != 0xfedcba9876543210 || large.c != 99)
    return 6;
  const auto hfa = get_callback_hfa()({{1.25, -17.5, 1024.125, -0.25}});
  if (hfa.values[0] != 1.25 || hfa.values[1] != -17.5 ||
      hfa.values[2] != 1024.125 || hfa.values[3] != -0.25)
    return 7;
  std::puts("native callback boundary oracle");
}
