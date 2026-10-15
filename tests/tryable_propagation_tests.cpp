#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// Prefix `?` through `std::tryable`: an operand other than built-in
// `expected` propagates through the selected conformance block. Every rule is
// exercised through unrelated user types.
int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze(root / "tests" / "tryable-propagation.gti",
                                    source, {root / "stdlib" / "prelude.gti"},
                                    {}, {root / "stdlib"});
  };
  const auto dump = [](const lang::FrontendResult &result) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  };
  const auto accepted = [&](const std::string &source,
                            const std::string &message,
                            std::initializer_list<std::string> fragments = {}) {
    const auto result = analyze(source);
    expect(result.canGenerateCode(), message);
    if (!result.canGenerateCode()) {
      dump(result);
      return;
    }
    const std::string cpp = gti_test::emitCpp(result).contents;
    for (const std::string &fragment : fragments) {
      expect(cpp.find(fragment) != std::string::npos,
             message + " (emitted C++ lacks '" + fragment + "')");
    }
  };
  const auto rejected = [&](const std::string &source, const char *code,
                            const std::string &fragment) {
    const auto result = analyze(source);
    const bool located = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &d) {
          const bool hinted = std::any_of(
              d.hints.begin(), d.hints.end(), [&](const std::string &hint) {
                return hint.find(fragment) != std::string::npos;
              });
          return d.code == code && !d.primary.source.empty() &&
                 d.primary.end > d.primary.start &&
                 (d.message.find(fragment) != std::string::npos || hinted);
        });
    if (!located)
      dump(result);
    expect(!result.canGenerateCode(),
           "invalid source must not reach codegen: " + fragment);
    expect(located,
           std::string("missing ") + code + " with '" + fragment + "'");
  };

  const std::string types = R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
};
class Checked<T> {
public:
  T value;
  int code;
  Checked(T payload, int failure) : value(payload), code(failure) {}
  Checked(Checked<T>& other) = delete;
  Checked(Checked<T>&& other) = default;
};
for <std::copyable T> concept std::tryable<Checked<T>> {
  using value_type = T;
  using residual_type = Rejected;
  bool has_value(Checked<T>& result) { return result.code == 0; }
  T take_value(Checked<T> result) { return result.value; }
  Rejected take_residual(Checked<T> result) { return Rejected(result.code); }
};
class Status {
public:
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected rejected) : code(rejected.code) {}
};
Checked<int> make(int value, int failure) {
  return Checked<int>(value, failure);
}
)gti";

  accepted(types + R"gti(
Status total(int failure) {
  Checked<int> held = make(2, 0);
  int value = ?make(1, failure) + ?std::move(held);
  return Status(value);
}
int measure<V>(V value) { return 4; }
Status forward<T>(T result) requires std::tryable<T> {
  std::tryable<T>::value_type value = ?std::move(result);
  return Status(measure(value));
}
int main() {
  return total(0).code + forward(make(4, 0)).code - 7;
}
)gti",
           "temporary, moved and generic operands should propagate through "
           "their block",
           {"__gti_conformance_"});

  rejected(types + R"gti(
struct Plain {
  int value;
};
Status plain() {
  Plain value{1};
  int copy = ?value;
  return Status(copy);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "no 'for concept std::tryable<Plain>' block");
  rejected(types + R"gti(
Status named() {
  Checked<int> held = make(1, 0);
  int value = ?held;
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "std::move(name)");
  rejected(types + R"gti(
int scalar() {
  int value = ?make(1, 0);
  return value;
}
int main() { return 0; }
)gti",
           "GTI-S2086", "cannot be initialized from it");
  rejected(types + R"gti(
class Other {
public:
  int code;
  Other(int value) : code(value) {}
};
Other other() {
  int value = ?make(1, 0);
  return Other(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "declares no implicit constructor");
  rejected(types + R"gti(
void nothing() {
  int value = ?make(1, 0);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "declared return type accepts it");
  rejected(types + R"gti(
Status unconstrained<T>(T result) {
  int value = ?std::move(result);
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "requires 'requires std::tryable<T>'");
  rejected(types + R"gti(
constexpr Status constant() {
  int value = ?make(1, 0);
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "constexpr function");
  // Built-in expected keeps its own rule.
  rejected(types + R"gti(
expected<int, int> load() { return 1; }
Status mixed() {
  int value = ?load();
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "returning 'expected<U, E>'");

  if (failures != 0) {
    std::cerr << failures << " tryable propagation checks failed\n";
    return 1;
  }
  std::cout << "tryable propagation tests passed\n";
  return 0;
}
