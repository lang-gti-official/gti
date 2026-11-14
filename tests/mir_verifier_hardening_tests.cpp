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

// Issue #339: a projected place's recorded type is its root's type through
// every projection, for this-, binding-, temporary- and loan-rooted places.
void checkPlaceProjectionTypes() {
  const auto frontend =
      analyzeWithStandardLibrary("nested-field-permissions.gti",
                                 R"(
import std.utility;
struct Exclusive {
  mut int& value;
  Exclusive(mut int& source) : value(source) {}
};
struct Combined {
  int& input;
  mut Exclusive output;
  Combined(int& source, Exclusive target)
      : input(source), output(std::move(target)) {}
  int run() mut { output.value = input; return output.value; }
};
int main() {
  int a = 42; mut int b = 0;
  Exclusive item{b};
  mut Combined group{a, std::move(item)};
  return group.run() - 42;
}
)");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "#339 baseline verifies");
  if (!frontend.canGenerateCode())
    return;
  std::size_t dereferences = 0, fields = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (std::size_t index = 0; index < function.body.places.size(); ++index) {
      const auto &place = function.body.places[index];
      if (place.root != lang::MirPlaceRootKind::This ||
          place.projections.size() != 3 ||
          place.projections[0].kind != lang::MirProjectionKind::Field ||
          place.projections[1].kind != lang::MirProjectionKind::Field ||
          place.projections[2].kind != lang::MirProjectionKind::Dereference)
        continue;
      const auto forged = [&](auto mutate) {
        auto malformed = frontend.mir;
        auto &body =
            writable(*malformed.findFunctionInstance(function.id)).body;
        mutate(writable(body.places[index]));
        return lang::verifyMirProgram(malformed).valid();
      };
      ++dereferences;
      expect(!forged([](lang::MirPlace &forgery) {
        forgery.projections.pop_back();
      }),
             "#339 a place that drops its trailing Dereference is rejected");
      ++fields;
      expect(!forged([](lang::MirPlace &forgery) {
        forgery.projections[1].field = 999999;
      }),
             "#339 a field its receiver does not declare is rejected");
    }
  expect(dereferences != 0 && fields != 0,
         "#339 fixture has this.<field>.<field>.* places");
}

// Issue #340: a stage-publishing field initializer cannot be completed by an
// untagged Initialize, including lexical fields whose stage is optional.
void checkConstructorFieldStages() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/issue_144_class_array_field.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "#340 baseline verifies");
  if (!frontend.canGenerateCode())
    return;
  std::size_t stages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.constructorInitializer == 0 ||
            instruction.info.type.kind != lang::SemanticType::Array)
          continue;
        ++stages;
        auto malformed = frontend.mir;
        const auto *changed = malformed.findConstructorInstance(constructor.id);
        for (const auto &candidateBlock : changed->body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.id)
              writable(candidate).constructorInitializer = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "#340 an untagged field array Initialize is rejected (" +
                   std::to_string(instruction.id) + ")");
      }
  expect(stages == 6, "#340 fixture stages all six class-array fields");
}

// Issue #341: a reference selected from a local payload carrier sources the
// carrier's field-less Stored loan place, which is provenance only. Reading
// the selected binding through that place instead must be rejected.
void checkProvenanceOnlyCarrierOwners() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrowed_enum_local_selection.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "#341 baseline verifies");
  if (!frontend.canGenerateCode())
    return;
  std::size_t forged = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &place : function.body.places) {
      const auto *stored = place.root == lang::MirPlaceRootKind::Loan
                               ? function.body.findLoan(place.loan)
                               : nullptr;
      const auto selection =
          std::find_if(function.body.loans.begin(), function.body.loans.end(),
                       [&](const lang::MirLoan &loan) {
                         return loan.source == place.id &&
                                loan.referenceSelection &&
                                loan.carriers.size() == 1;
                       });
      if (stored == nullptr || stored->kind != lang::MirLoanKind::Stored ||
          stored->storedField != 0 || selection == function.body.loans.end())
        continue;
      auto malformed = frontend.mir;
      auto &body = writable(*malformed.findFunctionInstance(function.id)).body;
      bool changed = false;
      for (auto &block : body.blocks)
        for (auto &instruction : block.instructions) {
          if (changed || instruction.kind != lang::MirInstructionKind::Load ||
              instruction.operands.size() != 1)
            continue;
          const auto *operand =
              function.body.findPlace(instruction.operands.front().place);
          if (operand != nullptr &&
              operand->root == lang::MirPlaceRootKind::Binding &&
              operand->binding == selection->carriers.front() &&
              operand->projections.size() == 1 &&
              operand->projections.front().kind ==
                  lang::MirProjectionKind::Dereference &&
              operand->type == place.type) {
            instruction.operands.front().place = place.id;
            changed = true;
          }
        }
      if (!changed)
        continue;
      ++forged;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "#341 reading through a provenance-only carrier owner place is "
             "rejected");
    }
  expect(forged >= 10, "#341 fixture exercises the carrier selections");
}

} // namespace

int main() {
  checkProvenanceOnlyCarrierOwners();
  checkConstructorFieldStages();
  checkPlaceProjectionTypes();
  checkIntegerLiteralRange();
  checkSpecialMemberPolicy();
  if (failures != 0)
    return 1;
  std::cout << "MIR verifier hardening tests passed\n";
}
