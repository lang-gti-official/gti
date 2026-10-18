#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const std::string prefix = R"gti(
class Carrier {
  int& owner;
public:
  Carrier(int& value) : owner(value) {}
  int read() { return this.owner; }
};
enum Holder { none, item(Carrier value) };
enum Pair { empty, both(int& first, int& second) };
int inspect(Holder& held) {
  switch (held) {
    case Holder::none: return 0;
    case Holder::item(auto& value): return value.read();
  }
}
int inspect_pair(Pair& held) {
  switch (held) {
    case Pair::empty: return 0;
    case Pair::both(first, second): return first + second;
  }
}
[[borrowed_from(owner)]]
Holder clone(int& owner) {
  Carrier value = Carrier(owner);
  Holder original = Holder::item(value);
  Holder copied = original;
  return copied;
}
)gti";
  const auto positive =
      analyzeWithStandardLibrary("borrowed-copy.gti", prefix + R"gti(
int main() {
  mut int first = 17;
  mut int second = 29;
  {
    Carrier value = Carrier(first);
    Holder original = Holder::item(value);
    Holder copied = original;
    Holder again = (copied);
    if (inspect(again) != 17) return 1;
    {
      Holder again = again;
      if (inspect(again) != 17) return 5;
      {
        auto again = again;
        if (inspect(again) != 17) return 6;
      }
    }
    Pair pair = Pair::both(first, second);
    Pair pair_copy = pair;
    if (inspect_pair(pair_copy) != 46) return 2;
    Pair empty = Pair::empty;
    Pair empty_copy = empty;
    if (inspect_pair(empty_copy) != 0) return 3;
    Holder escaped_shell = clone(first);
    if (inspect(escaped_shell) != 17) return 4;
  }
  first = 31;
  second = 47;
  return 0;
}
)gti");
  expect(positive.canGenerateCode(), "shared class and multi-origin enum "
                                     "copies retain exact dependency groups");
  if (positive.canGenerateCode()) {
    expect(!gti_test::emitCppText(positive).empty(),
           "copied borrowed enums retain native representation");
    std::size_t aliases = 0;
    for (const auto &function : positive.mir.functionInstances())
      for (const auto &loan : function.body.loans) {
        if (loan.entry || loan.conditions.empty() || loan.carriers.size() < 2)
          continue;
        ++aliases;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = positive.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changed = body->loans[loan.id - 1];
          if (mutation == 0)
            changed.conditions.front().variant += 50;
          else if (mutation == 1)
            changed.access = lang::AccessMode::Mutable;
          else if (mutation == 2)
            changed.designatedFields.clear();
          else {
            if (changed.additionalDependencies.empty())
              continue;
            changed.additionalDependencies.clear();
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "copy aliases reject substituted tag/access/path or omitted "
                 "origin");
        }
      }
    expect(aliases >= 2, "single and multiple owners use shared alias groups");
  }
  const auto reject = [&](std::string source, const char *message) {
    const auto result = analyzeWithStandardLibrary("invalid-borrowed-copy.gti",
                                                   std::move(source));
    expect(result.syntaxValid && !result.semanticValid, message);
  };
  reject(prefix + R"gti(
int main() {
  mut int owner = 17;
  Carrier value = Carrier(owner);
  Holder held = Holder::item(value);
  Holder copied = held;
  owner = 23;
  return inspect(copied);
}
)gti",
         "owner cannot mutate before the last shared alias use");
  reject(prefix + R"gti(
Holder escape() {
  int owner = 17;
  Carrier value = Carrier(owner);
  Holder held = Holder::item(value);
  Holder copied = held;
  return copied;
}
int main() { return 0; }
)gti",
         "a shared copy cannot escape its local referent");
  reject(R"gti(
enum Exclusive { empty, item(mut int& value) };
int main() {
  mut int owner = 17;
  Exclusive held = Exclusive::item(owner);
  Exclusive copied = held;
  return 0;
}
)gti",
         "copy transport cannot duplicate exclusive authority");
  reject(prefix + R"gti(
int main() {
  mut int first = 17;
  mut int second = 29;
  Pair held = Pair::both(first, second);
  Pair copied = held;
  second = 7;
  return inspect_pair(copied);
}
)gti",
         "a shared copy protects every additional owner");
  reject(prefix + R"gti(
int main() {
  mut int owner = 17;
  Carrier value = Carrier(owner);
  Holder held = Holder::item(value);
  {
    Holder held = held;
    owner = 23;
    return inspect(held);
  }
}
)gti",
         "shadowed copies still protect the original external owner");
  return failures ? 1 : 0;
}
