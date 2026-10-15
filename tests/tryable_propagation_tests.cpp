#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
} // namespace lang

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
      if (result.hirValid && !result.mirValid) {
        std::cerr << lang::MirPrinter().print(result.mir);
      }
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
  [[borrowed_from(result)]] T take_value(Checked<T> result) { return result.value; }
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
  const std::string borrowed = R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
};
class Reading {
public:
  int& external;
  int code;
  Reading(int& owner, int failure) : external(owner), code(failure) {}
};
for concept std::tryable<Reading> {
  using value_type = int&;
  using residual_type = Rejected;
  bool has_value(Reading& value) { return value.code == 0; }
  [[borrowed_from(value)]] int& take_value(Reading value) {
    return value.external;
  }
  Rejected take_residual(Reading value) { return Rejected(value.code); }
};
class Status {
public:
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected error) : code(error.code) {}
};
)gti";
  accepted(borrowed + R"gti(
Status read(int& owner, int failure) {
  Reading source{owner, failure};
  int& value = ?std::move(source);
  return Status(value);
}
int main() {
  int owner = 7;
  return read(owner, 0).code - 7;
}
)gti",
           "propagation keeps a by-value carrier's external reference");

  const std::string mutableBorrowed = R"gti(
struct Rejected { int code; Rejected(int value) : code(value) {} };
class Editing {
public:
  mut int& external;
  int code;
  Editing(mut int& owner, int failure) : external(owner), code(failure) {}
};
for concept std::tryable<Editing> {
  using value_type = mut int&;
  using residual_type = Rejected;
  bool has_value(Editing& value) { return value.code == 0; }
  [[borrowed_from(value)]] mut int& take_value(Editing value) {
    return value.external;
  }
  Rejected take_residual(Editing value) { return Rejected(value.code); }
};
struct Status {
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected error) : code(error.code) {}
};
)gti";
  accepted(mutableBorrowed + R"gti(
Status edit(mut int& owner) {
  Editing source{owner, 0};
  mut int& value = ?std::move(source);
  value = 8;
  return Status(value);
}
int main() { mut int owner = 7; return edit(owner).code - 8; }
)gti",
           "propagation transfers exclusive external reference authority");
  rejected(mutableBorrowed + R"gti(
Status edit(mut int& owner) {
  Editing source{owner, 0};
  mut int& value = ?std::move(source);
  owner = 9;
  return Status(value);
}
int main() { mut int owner = 7; return edit(owner).code; }
)gti",
           "GTI-S2017", "borrow");

  const auto proof = analyze(borrowed + R"gti(
Status read(int& owner) {
  Reading source{owner, 0};
  int& value = ?std::move(source);
  return Status(value);
}
int main() { int owner = 7; return read(owner).code - 7; }
)gti");
  if (proof.canGenerateCode()) {
    for (int corruption = 0; corruption < 4; ++corruption) {
      auto malformed = proof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed)) {
        for (auto &block : function.body.blocks) {
          for (auto &instruction : block.instructions) {
            if (changed || instruction.kind != lang::MirInstructionKind::Call ||
                instruction.borrowOrigin != lang::BorrowOriginKind::Argument ||
                instruction.info.category != lang::ValueCategory::Place)
              continue;
            if (corruption == 0)
              instruction.borrowOrigin = lang::BorrowOriginKind::None;
            else if (corruption == 1)
              instruction.borrowArgument = 1;
            else if (corruption == 2)
              instruction.borrowAccess = lang::AccessMode::Mutable;
            else if (instruction.loan)
              function.body.loans[*instruction.loan - 1].source = 0;
            else
              continue;
            changed = true;
          }
        }
      }
      expect(changed, "borrowed propagation mutation must find its call proof");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "MIR must reject missing, substituted, upgraded or sourceless "
             "borrowed protocol result authority");
    }
  }

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
