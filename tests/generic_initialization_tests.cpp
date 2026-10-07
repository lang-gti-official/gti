#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(std::string_view source, bool valid, std::string_view label,
           std::string_view code = "GTI-S2068", bool closed = false) {
  lang::FrontendOptions options;
  options.requireEntryPoint = closed;
  auto result = lang::Frontend(options).analyze("generic-initialization.gti",
                                         std::string(source));
  const bool diagnostic = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.code == code && d.primary.end > d.primary.start &&
               d.primary.line != 0;
      });
  if (result.canGenerateCode() != valid || (!valid && !diagnostic)) {
    std::cerr << "FAIL " << label << '\n';
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    ++failures;
  }
}
} // namespace

int main() {
  check(R"gti(
int read<uint64_t N>() {
  if constexpr (N == 0) { int local = 4; return local; }
  else { return later; }
}
int first = read<0>();
int later = 8;
int main() { return first - 4; }
)gti",
        true, "a selected pure branch has an exact empty effect set");
  check(R"gti(
int read<uint64_t N>() {
  if constexpr (N == 0) { return 4; }
  else { return later; }
}
int first = read<1>();
int later = 8;
int main() { return first; }
)gti",
        false, "the other instance retains its real early read");
  check(R"gti(
int later = 8;
int read<T>() { return later; }
int first = read<int>();
int main() { return first - 8; }
)gti",
        true, "an exact generic read of earlier storage is safe");
  check(R"gti(
int read<T>() { return later; }
int bridge() { return read<int>(); }
int first = bridge();
int later = 8;
int main() { return first; }
)gti",
        false, "a nongeneric bridge cannot hide an early generic read");
  constexpr std::string_view owner = R"gti(
class Owner<uint64_t N> {
public:
  Owner() { if constexpr (N == 1) { later = 9; } }
  ~Owner() { if constexpr (N == 2) { later = 10; } }
};
int make<uint64_t N>() { Owner<N> owner{}; return 3; }
)gti";
  check(std::string(owner) + R"gti(
int first = make<0>();
mut int later = 0;
int main() { return first - 3; }
)gti",
        true, "closed selected construction and cleanup are pure");
  check(std::string(owner) + R"gti(
int first = make<1>();
mut int later = 0;
int main() { return first; }
)gti",
        false, "a concrete constructor write remains an early access");
  check(std::string(owner) + R"gti(
int first = make<2>();
mut int later = 0;
int main() { return first; }
)gti",
        false, "a concrete destructor write remains an early access");
  check(R"gti(
class Values<uint64_t N> {
public:
  Values() {}
  int read() { if constexpr (N == 0) { return 7; } else { return later; } }
};
int fetch() { Values<0> values{}; return values.read(); }
int first = fetch();
int later = 9;
int main() { return first - 7; }
)gti",
        true, "generic-owner methods use exact owner arguments");
  check(R"gti(
int grow<uint64_t N>() { return grow<N + 1>(); }
int first = grow<0>();
int main() { return first; }
)gti",
        false, "initializer-reachable generic growth has a deterministic bound",
        "GTI-S2105");
  check(
      R"gti(
int leaf<uint64_t N>() {
  if constexpr (N == 0) { return 5; }
  else { return leaf<N - 1>(); }
}
int first = leaf<63>();
int main() { return first - 5; }
)gti",
      true,
      "a finite initializer effect chain reaches the positive depth boundary");
  check(R"gti(
constexpr int later = 8;
int read<T>(int& value) { return value; }
int first = read<int>(later);
int main() { return first - 8; }
)gti",
        true,
        "reference boundaries to earlier storage retain exact generic proof");
  check(R"gti(
int first = read<int>();
constexpr int later = 8;
int read<T>() { int& bound = later; return bound; }
int main() { return first; }
)gti",
        false, "a reference boundary cannot fold away later constexpr storage");
  check(R"gti(
int missing();
int select<uint64_t N>() {
  if constexpr (N == 0) { return 7; }
  else { return missing(); }
}
int first = select<0>();
int main() { return first - 7; }
)gti", true, "closed definition policy ignores discarded generic calls", "GTI-S2106", true);
  check(R"gti(
int missing();
int select<uint64_t N>() {
  if constexpr (N == 0) { return 7; }
  else { return missing(); }
}
int first = select<1>();
int main() { return first; }
)gti", false, "instance effect worker retains closed executable definition policy", "GTI-S2106", true);
  if (failures)
    return 1;
  std::cout << "generic initialization tests passed\n";
  return 0;
}
