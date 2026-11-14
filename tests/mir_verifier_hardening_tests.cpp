// MIR verification is the authority on what reaches a backend. Each check
// forges one verified program the way its issue describes and requires
// verification to reject it, so no backend admits the malformed MIR.
#include "mir_test_fixture.h"

#include <functional>

namespace {

template <typename T> T &writable(const T &value) {
  return const_cast<T &>(value);
}

// Each instruction of every function, constructor and destructor body.
void forEachInstruction(
    const lang::MirProgram &program,
    const std::function<void(const lang::MirInstruction &)> &visit) {
  const auto body = [&](const lang::MirBody &instructions) {
    for (const auto &block : instructions.blocks)
      for (const auto &instruction : block.instructions)
        visit(instruction);
  };
  for (const auto &function : program.functionInstances())
    body(function.body);
  for (const auto &constructor : program.constructorInstances())
    body(constructor.body);
}

// Issue #343: an integer literal must lie in its type's domain. Only the
// lexical magnitude of a signed minimum may exceed it, and only as the
// operand of a negation.
void checkIntegerLiteralRange() {
  const auto frontend = analyzeWithStandardLibrary(
      "literal-range.gti",
      "int32_t main() { int64_t v = 7; int8_t small = 9; int8_t low = -128;\n"
      "  int64_t grouped = -((9223372036854775808));\n"
      "  if (grouped != -9223372036854775807 - 1) { return 6; }\n"
      "  if (v < 0) { return 3; } if (small < 0) { return 4; }\n"
      "  return low == -128 ? 0 : 5; }\n");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "#343 baseline with a signed minimum literal verifies");
  if (!frontend.canGenerateCode())
    return;
  const auto forged = [&](std::uint64_t from, std::uint64_t to) {
    auto malformed = frontend.mir;
    bool changed = false;
    forEachInstruction(malformed, [&](const lang::MirInstruction &instruction) {
      if (changed || instruction.operation != lang::MirOperation::Literal ||
          !instruction.literal)
        return;
      const auto *value = std::get_if<std::uint64_t>(&*instruction.literal);
      if (value == nullptr || *value != from)
        return;
      writable(instruction).literal = lang::Literal{to};
      changed = true;
    });
    expect(changed, "#343 forgery found its literal");
    return lang::verifyMirProgram(malformed).valid();
  };
  expect(!forged(7, 9223372036854775808ull),
         "#343 an Int64 literal of 2^63 that is not negated is rejected");
  expect(!forged(9, 1000), "#343 an Int8 literal of 1000 is rejected");
  expect(!forged(9, 128),
         "#343 the signed-minimum magnitude outside a negation is rejected");
}

// Issue #342: class traits and special-member status agree in both
// directions, and a deleted destructor belongs only to a lifecycle-free class.
void checkSpecialMemberPolicy() {
  const auto frontend = analyzeWithStandardLibrary("special-members.gti", R"(
struct Unique {
  int value;
  Unique(int value) : value(value) {}
  Unique(Unique& other) = delete;
  Unique(Unique&& other) = default;
};
struct Plain {
  int value;
  Plain(int value) : value(value) {}
};
int main() { Unique u{1}; Plain p{2}; return u.value + p.value - 3; }
)");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "#342 baseline verifies");
  if (!frontend.canGenerateCode())
    return;
  std::size_t unique = 0, plain = 0;
  for (std::size_t index = 0; index < frontend.mir.classInstances().size();
       ++index) {
    const auto &instance = frontend.mir.classInstances()[index];
    if (instance.copyConstructor == lang::SpecialMemberStatus::Deleted &&
        instance.moveConstructor != lang::SpecialMemberStatus::Deleted &&
        !instance.traits.copyable && instance.traits.movable) {
      ++unique;
      auto malformed = frontend.mir;
      writable(malformed.classInstances()[index]).traits.copyable = true;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "#342 a copyable trait with a deleted copy constructor is "
             "rejected");
    }
    if (instance.destructorStatus == lang::SpecialMemberStatus::Generated &&
        instance.copyConstructor == lang::SpecialMemberStatus::Generated &&
        instance.traits.copyable) {
      ++plain;
      auto malformed = frontend.mir;
      writable(malformed.classInstances()[index]).destructorStatus =
          lang::SpecialMemberStatus::Deleted;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "#342 a deleted destructor on a live class is rejected");
    }
  }
  expect(unique != 0 && plain != 0, "#342 fixture has both class shapes");
}

} // namespace

int main() {
  checkIntegerLiteralRange();
  checkSpecialMemberPolicy();
  if (failures != 0)
    return 1;
  std::cout << "MIR verifier hardening tests passed\n";
}
