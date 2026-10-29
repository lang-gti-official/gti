#include "compiler_test_fixture.h"

namespace {
bool hasMirIssue(const lang::MirVerificationResult &result,
                 std::string_view fragment) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const auto &error) {
                       return error.message.find(fragment) != std::string::npos;
                     });
}

void checkTemporaryOwnerBorrow() {
  const auto passive = lang::Frontend().analyze("passive-owner-borrow.gti", R"(
class Owner { int value = 3; public: int& get() { return this.value; }  public: Owner() = default; };
Owner produce() { return Owner(); }
int read(int& value) { return value; }
int main() { return read(produce().get()) + read(Owner().get()) - 6; }
)");
  expect(passive.canGenerateCode() &&
             lang::verifyMirProgram(passive.mir).valid(),
         "returned and constructed passive owners supply exact temporary "
         "receiver borrows");
  if (!passive.canGenerateCode()) {
    for (const auto &diagnostic : passive.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  if (passive.canGenerateCode()) {
    auto malformed = passive.mir;
    expect(malformed.classInstances().size() == 1,
           "passive fixture isolates exactly one generated owner");
    if (malformed.classInstances().size() == 1) {
      auto &owner = const_cast<lang::MirClassInstance &>(
          malformed.classInstances().front());
      expect(owner.generatedDefaultRequiresSchedule &&
                 std::any_of(malformed.constructorInstances().begin(),
                             malformed.constructorInstances().end(),
                             [&](const auto &constructor) {
                               return constructor.owner == owner.id &&
                                      constructor.generatedDefault;
                             }),
             "the field-default owner retains its selected generated schedule");
      owner.defaultConstructor = lang::SpecialMemberStatus::Deleted;
      expect(hasMirIssue(lang::verifyMirProgram(malformed),
                         "generated constructor lost its exact default "
                         "initialization schedule"),
             "selected temporary construction must retain an available "
             "generated default schedule");
    }
  }

  const auto targetless = lang::Frontend().analyze("targetless-owner.gti", R"(
class Owner { public: int get() { return 3; }  public: Owner() = default; };
Owner produce() { return Owner(); }
int main() { return produce().get() + Owner().get() - 6; }
)");
  expect(targetless.canGenerateCode() &&
             lang::verifyMirProgram(targetless.mir).valid(),
         "an empty generated owner preserves targetless temporary receivers");
  if (targetless.canGenerateCode() &&
      targetless.mir.classInstances().size() == 1) {
    auto malformed = targetless.mir;
    auto &owner = const_cast<lang::MirClassInstance &>(
        malformed.classInstances().front());
    expect(!owner.generatedDefaultRequiresSchedule &&
               std::none_of(malformed.constructorInstances().begin(),
                            malformed.constructorInstances().end(),
                            [&](const auto &constructor) {
                              return constructor.owner == owner.id;
                            }),
           "the targetless control cannot accidentally gain a selected body");
    owner.defaultConstructor = lang::SpecialMemberStatus::Deleted;
    expect(hasMirIssue(lang::verifyMirProgram(malformed),
                       "temporary receiver input does not match"),
           "targetless temporary construction must name an available "
           "generated default");
  } else {
    expect(false, "targetless receiver mutation needs exactly one owner");
  }

  const auto owned =
      analyzeTrustedPreludeFixture("temporary-string-view.gti", R"(
import std.string;
std::string produce() { return std::string("hello"); }
int read(std::string_view value, int divisor) { return int(value.size()) / divisor; }
int later(int divisor) { return 1 / divisor; }
int main() {
  int first = read(produce().view(), later(1));
  return first + read(std::string("hello").view(), 1) - 10;
}
)");
  expect(owned.canGenerateCode() && lang::verifyMirProgram(owned.mir).valid(),
         "temporary string views retain exact receiver identity and failure "
         "cleanup");
  if (!owned.canGenerateCode()) {
    for (const auto &diagnostic : owned.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t receiverStages = 0;
  std::size_t cleanupMutations = 0;
  for (const auto &function : owned.mir.functionInstances()) {
    const auto *hir = owned.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source || hir->source->name().lexeme != "main")
      continue;
    expect(
        function.body.loans.size() == 2 &&
            function.body.loans.front().source !=
                function.body.loans.back().source,
        "two independent temporary views retain distinct exact owner places");
    if (function.body.loans.size() == 2) {
      auto malformed = owned.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      changed->body.loans.back().source = changed->body.loans.front().source;
      expect(hasMirIssue(lang::verifyMirProgram(malformed),
                         "call-result loan does not preserve the selected"),
             "same-typed temporary owners cannot substitute their borrow "
             "identities");
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Receiver &&
            instruction.callInputKind ==
                lang::HirCallInputKind::ReadTemporaryBorrow) {
          ++receiverStages;
          auto malformed = owned.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == instruction.id)
                candidate.operands.front().kind = lang::MirOperandKind::Value;
          expect(
              !lang::verifyMirProgram(malformed).valid(),
              "temporary borrowed result cannot lose its exact receiver place");
        }
        if (block.activeFailure == 0 ||
            instruction.kind != lang::MirInstructionKind::EndBorrow ||
            !instruction.loan)
          continue;
        const auto *loan = function.body.findLoan(*instruction.loan);
        const auto *source =
            loan ? function.body.findPlace(loan->source) : nullptr;
        if (!loan ||
            (loan->kind != lang::MirLoanKind::CallResult &&
             loan->kind != lang::MirLoanKind::Stored) ||
            !source || source->root != lang::MirPlaceRootKind::Value)
          continue;
        auto malformed = owned.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidateBlock : changed->body.blocks)
          if (candidateBlock.id == block.id)
            std::erase_if(candidateBlock.instructions,
                          [&](const auto &candidate) {
                            return candidate.id == instruction.id;
                          });
        expect(hasMirIssue(lang::verifyMirProgram(malformed),
                           "conflicts with active read-only loan"),
               "failure cleanup cannot destroy a temporary with its view loan "
               "still active");
        ++cleanupMutations;
      }
    }
  }
  expect(receiverStages == 2 && cleanupMutations >= 2,
         "regression exercises both exact receiver staging and later-call "
         "failure retirement");

  for (const std::string source :
       {"import std.string;\nstd::string produce() { return "
        "std::string(\"hello\"); }\nint main() { auto view = produce().view(); "
        "return int(view.size()); }",
        "class Owner { int value = 3; public: int& get() { return this.value; "
        "}  public: Owner() = default; }; int& escape() { return "
        "Owner().get(); } int main() { return 0; }",
        "class Owner { mut int value = 3; public: mut int& get() mut { return "
        "this.value; }  public: Owner() = default; }; int main() { [[discard]] "
        "Owner().get(); return 0; "
        "}"}) {
    const auto invalid =
        analyzeTrustedPreludeFixture("escaping-temporary-borrow.gti", source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "escaping or mutable temporary-owner views remain source errors");
  }
}
// A fresh payload-enum value, with or without cleanup, is borrowed for one
// call exactly as a fresh class value is. Scalar-backed enums and mutable
// references to temporaries stay source errors.
void checkPayloadEnumTemporaryArguments() {
  const std::string declarations = R"(
class Tracked {
  int id;
public:
  Tracked(int value) : id(value) {}
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() {}
  int read() { return this.id; }
};
enum class state { empty, engaged(int value) };
enum class owned { empty, held(Tracked item) };
state make(int v) {
  if (v < 0) {
    return state::empty;
  }
  return state::engaged(v);
}
owned hold(int v) { return owned::held(Tracked(v)); }
bool engaged(state& s) {
  switch (s) {
  case state::engaged:
    return true;
  case state::empty:
    return false;
  }
}
bool held(owned& o) {
  switch (o) {
  case owned::held:
    return true;
  case owned::empty:
    return false;
  }
}
)";
  const auto valid =
      lang::Frontend().analyze("payload-enum-temporary.gti", declarations + R"(
int main() {
  bool constructed = engaged(state::engaged(1));
  bool unit = engaged(state::empty);
  bool returned = engaged(make(2));
  bool owning = held(hold(3));
  bool unitOwning = held(owned::empty);
  return constructed && !unit && returned && owning && !unitOwning ? 0 : 1;
}
)");
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "fresh payload-enum values are borrowed for one call");
  if (!valid.canGenerateCode()) {
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t enumStages = 0;
  for (const auto &function : valid.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Argument ||
            instruction.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            instruction.info.type.arguments.size() != 1 ||
            instruction.info.type.arguments.front().kind !=
                lang::SemanticType::Enum)
          continue;
        ++enumStages;
        auto malformed = valid.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidateBlock : changed->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.id)
              candidate.loan.reset();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a borrowed payload-enum temporary keeps its call-scoped loan");
      }
    }
  }
  expect(enumStages == 5,
         "each fresh payload-enum argument stages one temporary borrow");

  for (const std::string source :
       {"enum class code { ok, failed }; code pick() { return code::ok; } "
        "bool good(code& c) { return c == code::ok; } int main() { return "
        "good(pick()) ? 0 : 1; }",
        "enum class state { empty, engaged(int value) }; void clear(mut state& "
        "s) { s = state::empty; } int main() { clear(state::engaged(1)); "
        "return 0; }"}) {
    const auto invalid =
        lang::Frontend().analyze("payload-enum-temporary-invalid.gti", source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "scalar-backed enum temporaries and mutable temporary borrows "
           "remain source errors");
  }
}
} // namespace

int main() {
  checkTemporaryOwnerBorrow();
  checkPayloadEnumTemporaryArguments();
  return failures == 0 ? 0 : 1;
}
