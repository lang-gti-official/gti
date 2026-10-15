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

  // The residual returns as an ordinary `return residual;` of a fresh value
  // would: through an implicit conversion operator of the residual, to a
  // class or a scalar return type, or an implicit constructor with a trailing
  // default or a satisfied requirement, under ADR 027's one shared
  // selection.
  accepted(R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
  implicit operator int() { return -this.code; }
};
class Attempt {
public:
  int code;
  Attempt(int failure) : code(failure) {}
  Attempt(Attempt& other) = delete;
  Attempt(Attempt&& other) = default;
};
for concept std::tryable<Attempt> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Attempt& attempt) { return attempt.code == 0; }
  int take_value(Attempt attempt) { return 1; }
  Rejected take_residual(Attempt attempt) { return Rejected(attempt.code); }
};
struct Outcome {
  int code;
  Outcome(int value) : code(value) {}
};
struct Ledger {
  int code;
  int bias;
  implicit Ledger(Rejected rejected, int extra = 100)
      : code(rejected.code), bias(extra) {}
};
struct Report<T> {
  T code;
  Report(T value) : code(value) {}
  implicit Report(Rejected rejected)
    requires std::integral<T>
      : code(T(rejected.code)) {}
};
class Lease {
public:
  int code;
  Lease(int value) : code(value) {}
  Lease(Lease& other) = delete;
  Lease(Lease&& other) = default;
  implicit operator Outcome() && { return Outcome(this.code * 10); }
};
class Claim {
public:
  int code;
  Claim(int failure) : code(failure) {}
  Claim(Claim& other) = delete;
  Claim(Claim&& other) = default;
};
for concept std::tryable<Claim> {
  using value_type = int;
  using residual_type = Lease;
  bool has_value(Claim& claim) { return claim.code == 0; }
  int take_value(Claim claim) { return 2; }
  Lease take_residual(Claim claim) { return Lease(claim.code); }
};
int scalar(int failure) {
  int value = ?Attempt(failure);
  return value;
}
Ledger defaulted(int failure) {
  int value = ?Attempt(failure);
  return Ledger(Rejected(value), 0);
}
Outcome consumed(int failure) {
  int value = ?Claim(failure);
  return Outcome(value);
}
Report<int> constrained(int failure) {
  int value = ?Attempt(failure);
  return Report<int>(value);
}
int main() {
  return scalar(0) + defaulted(3).bias + consumed(4).code +
         constrained(5).code;
}
)gti",
           "a residual must return through an implicit operator or a "
           "defaulted implicit constructor",
           {"__gti_operator_conversion"});
  rejected(R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
  operator int() { return -this.code; }
};
class Attempt {
public:
  int code;
  Attempt(int failure) : code(failure) {}
  Attempt(Attempt& other) = delete;
  Attempt(Attempt&& other) = default;
};
for concept std::tryable<Attempt> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Attempt& attempt) { return attempt.code == 0; }
  int take_value(Attempt attempt) { return 1; }
  Rejected take_residual(Attempt attempt) { return Rejected(attempt.code); }
};
int scalar(int failure) {
  int value = ?Attempt(failure);
  return value;
}
int main() { return 0; }
)gti",
           "GTI-S2086", "'implicit operator int32_t()' in 'Rejected'");
  rejected(R"gti(
struct Outcome {
  int code;
  Outcome(int value) : code(value) {}
  implicit Outcome(Rejected rejected) : code(rejected.code) {}
};
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
  implicit operator Outcome() { return Outcome(this.code); }
};
class Attempt {
public:
  int code;
  Attempt(int failure) : code(failure) {}
  Attempt(Attempt& other) = delete;
  Attempt(Attempt&& other) = default;
};
for concept std::tryable<Attempt> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Attempt& attempt) { return attempt.code == 0; }
  int take_value(Attempt attempt) { return 1; }
  Rejected take_residual(Attempt attempt) { return Rejected(attempt.code); }
};
Outcome ambiguous(int failure) {
  int value = ?Attempt(failure);
  return Outcome(value);
}
int main() { return 0; }
)gti",
           "GTI-S2122",
           "Initialization of 'Outcome' from 'Rejected' is "
           "ambiguous");

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
  rejected(types + R"gti(
enum class Parse {
  ok(int value),
  failed(int code),
};
for concept std::tryable<Parse> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Parse& parse) { return true; }
  int take_value(Parse parse) { return 1; }
  Rejected take_residual(Parse parse) { return Rejected(1); }
};
Status enumerated() {
  int value = ?Parse::ok(1);
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "class or struct operand");
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
