#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("issues_30_33.gti", source,
                                  {library / "prelude.gti"}, {}, {library});
}
const std::string errorType = R"(
struct Error {
  int id;
  Error(int value) : id(value) {}
  Error(Error& other) = delete;
};
expected<int, Error> failure() { return unexpected(Error(1)); }
)";

void errors() {
  for (const auto &body :
       {"expected<int, Error> test() { Error error{1}; return "
        "unexpected(error); }",
        "expected<int, Error> test() { auto result = failure(); return "
        "unexpected(result.error()); }",
        "int main() { Error error{1}; expected<int, Error> result = "
        "unexpected(error); return 0; }",
        "void take(expected<int, Error> result) {} int main() { Error "
        "error{1}; take(unexpected(error)); return 0; }"}) {
    const std::string source = errorType + body;
    const auto result = analyze(source);
    check(result.syntaxValid && !result.semanticValid &&
              !result.canGenerateCode(),
          "move-only error copying stops before MIR/backend generation");
    // unexpected(e) is an ordinary source function: copying a move-only
    // payload into its by-value parameter is the general deleted-copy error.
    const auto diagnostic = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &d) {
          return d.code == "GTI-S2018" &&
                 d.message.find("would call the deleted copy "
                                "constructor of 'Error'") != std::string::npos;
        });
    check(diagnostic != result.diagnostics.end(),
          "error transport explains unavailable copy");
    if (diagnostic != result.diagnostics.end()) {
      const auto expression =
          source.rfind("unexpected(") + std::string("unexpected(").size();
      check(diagnostic->primary.start >= expression &&
                diagnostic->primary.end <= source.find(';', expression),
            "ownership diagnostic identifies the error payload expression");
      check(!diagnostic->hints.empty() && diagnostic->fixes.empty(),
            "ownership advice does not offer an intent-changing automatic fix");
    }
  }
  const auto valid = analyze(errorType + R"(
expected<int, Error> moved() { Error error{2}; return unexpected(std::move(error)); }
expected<int, Error> relay() { auto result = failure(); return unexpected(std::move(result).error()); }
expected<int, int> copied() { int error = 2; return unexpected(error); }
int main() { auto first = moved(); auto second = relay(); auto third = copied(); return 0; }
)");
  check(valid.canGenerateCode(),
        "fresh, moved and copyable error transport remains accepted");
  if (valid.canGenerateCode()) {
    check(!gti_test::emitCppText(valid).empty(),
          "owning errors reach verified backend");
    for (bool loseOwnership : {false, true}) {
      auto malformed = valid.mir;
      bool changed = false;
      for (const auto &function : valid.mir.functionInstances()) {
        auto *target = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &block : target->body.blocks) {
          for (auto &instruction : block.instructions) {
            if (changed)
              continue;
            if (loseOwnership &&
                instruction.kind == lang::MirInstructionKind::Move &&
                instruction.info.type.kind == lang::SemanticType::Class &&
                instruction.ownership) {
              instruction.ownership.reset();
              changed = true;
            } else if (!loseOwnership &&
                       instruction.kind == lang::MirInstructionKind::Call &&
                       instruction.functionTarget &&
                       instruction.operands.size() == 1 && [&] {
                         // The prelude's source unexpected(e) factory.
                         const auto *callee = valid.hir.findFunctionInstance(
                             *instruction.functionTarget);
                         return callee != nullptr &&
                                callee->source != nullptr &&
                                callee->source->name().lexeme == "unexpected";
                       }()) {
              instruction.operands.front().type = lang::SemanticType::Int32;
              changed = true;
            }
          }
        }
      }
      check(changed,
            "error transport corruption fixture reaches its exact MIR proof");
      bool rejected = false;
      try {
        (void)gti_test::emitCpp(valid, malformed, malformed);
      } catch (const std::exception &) {
        rejected = true;
      }
      check(rejected, "error transport rejects lost payload identity or source "
                      "move ownership");
    }
  }
  const auto used = analyze(errorType + R"(
int main() { Error error{2}; expected<int, Error> result = unexpected(std::move(error)); return error.id; }
)");
  check(!used.semanticValid &&
            std::any_of(used.diagnostics.begin(), used.diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-S2018"; }),
        "error wrapping preserves use-after-move rejection");
}

void schedules() {
  const auto result = analyze(R"(
struct Counter { mut uint16_t value = 1;
  void implicit_update() mut { value += uint16_t(2); }
  void explicit_update() mut { this.value += uint16_t(2); }
public:
  Counter() = default;
};
int main() { mut uint8_t value = 1; uint8_t old = value++; --value;
  mut Counter counter{}; counter.implicit_update(); counter.explicit_update(); return int(old); }
)");
  check(result.canGenerateCode(),
        "narrow and implicit member updates have verified MIR");
  if (!result.canGenerateCode())
    return;
  unsigned arithmetic = 0, conversion = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        check(
            instruction.kind != lang::MirInstructionKind::Modify,
            "narrow modifiers are primitive MIR schedules, not closed Modify");
        if (instruction.kind != lang::MirInstructionKind::Compute ||
            instruction.localFailureSites.empty() ||
            instruction.info.updateArithmeticType ==
                lang::SemanticType::Unknown)
          continue;
        if (instruction.operation == lang::MirOperation::Convert)
          ++conversion;
        else if (instruction.operation == lang::MirOperation::Add ||
                 instruction.operation == lang::MirOperation::Subtract) {
          ++arithmetic;
          check(instruction.info.type == lang::SemanticType::Int32,
                "arithmetic uses semantic promoted domain");
        } else
          continue;
        check(instruction.definedFailure.localOrigins.size() == 1,
              "arithmetic and narrowing have independently authored origins");
        if (instruction.definedFailure.localOrigins.size() == 1) {
          const auto &origin = instruction.definedFailure.localOrigins.front();
          const auto code =
              instruction.operation == lang::MirOperation::Convert
                  ? lang::DefinedFailureCode::NumericConversionOutOfRange
                  : lang::DefinedFailureCode::IntegerOverflow;
          check(origin.outcomes.size() == 1 &&
                    origin.outcomes.front().code == code &&
                    origin.end > origin.start &&
                    instruction.localFailureSites.size() == 1,
                "each arithmetic/narrowing stage retains its exact source and "
                "failure outcome");
        }
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto *changedBlock =
            const_cast<lang::MirBlock *>(changed->body.findBlock(block.id));
        auto found =
            std::find_if(changedBlock->instructions.begin(),
                         changedBlock->instructions.end(),
                         [&](const auto &i) { return i.id == instruction.id; });
        found->definedFailure = {};
        found->localFailureSites.clear();
        check(!lang::verifyMirProgram(malformed).valid(),
              "lost arithmetic/conversion contract is rejected");
        bool rejected = false;
        try {
          (void)gti_test::emitCpp(result, malformed, malformed);
        } catch (const std::exception &) {
          rejected = true;
        }
        check(rejected,
              "backend input cannot bypass malformed failure schedules");
      }
    }
  }
  check(arithmetic == 4 && conversion == 4,
        "every narrow update has arithmetic and narrowing stages (arithmetic=" +
            std::to_string(arithmetic) +
            ", conversion=" + std::to_string(conversion) + ")");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    check(!gti_test::emitCppText(result, standard).empty(),
          "qualified/unqualified updates emit both native standards");
}

void borrows() {
  const std::string declarations =
      "struct Grid { mut int cells[2] = {};  public: Grid() = default; }; "
      "struct Frame { mut Grid grid{};  public: Frame() = default; };\n";
  const auto valid = analyze(declarations + R"(
int read(Frame& frame, uint64_t index) { return frame.grid.cells[index]; }
void write(mut Frame& frame, uint64_t index) { frame.grid.cells[index] = 3; }
int main() { mut Frame frame{}; write(frame, uint64_t(1)); return read(frame, uint64_t(1)); }
)");
  check(valid.canGenerateCode(),
        "borrowed nested arrays preserve reference projection");
  if (valid.canGenerateCode())
    check(!gti_test::emitCppText(valid).empty(),
          "borrowed nested array reads/writes emit");
  for (const auto &body :
       {"void bad(Frame& frame) { frame.grid.cells[0] = 1; }",
        "int& bad() { Frame frame{}; return frame.grid.cells[0]; }",
        "int main() { mut Frame frame{}; int& held = frame.grid.cells[0]; "
        "frame.grid.cells[0] = 1; return held; }"}) {
    const auto invalid = analyze(declarations + body);
    check(
        invalid.syntaxValid && !invalid.semanticValid,
        "borrowed array mutability, escape and live-loan rules are unchanged");
  }
}
} // namespace
int main() {
  errors();
  schedules();
  borrows();
  return failures == 0 ? 0 : 1;
}
