#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

int main() {
  auto frontend =
      lang::Frontend().analyze("/tmp/gti-full-expression-loop.gti", R"(
enum class item_error { unavailable };
class Item {
public:
  Item() {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() {}
};
expected<Item, item_error> produce(int index) {
  if (index == 2) { return unexpected(item_error::unavailable); }
  return Item();
}
void consume(Item value) {}
class Record {
  int value;
public:
  Record(int value) : value(value) {}
};
expected<Record, item_error> checked_record(int divisor) {
  return Record(12 / divisor);
}
expected<Record, item_error> relay_record(int divisor) {
  return checked_record(divisor);
}
expected<void, item_error> collect() {
  for (mut int outer = 0; outer < 2; outer++) {
    for (mut int inner = 0; inner < 4; inner++) {
      consume(?produce(inner));
    }
  }
  return;
}
int main() { auto result = collect(); return result.has_value() ? 0 : 1; }
)");
  if (!frontend.mirValid || !lang::verifyMirProgram(frontend.mir).valid()) {
    std::cerr
        << "nested loop argument propagation must have valid boundaries\n";
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return 1;
  }

  auto duplicate = frontend.mir;
  bool mutated = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           duplicate.functionInstances())) {
    auto &body = function.body;
    lang::MirInstructionId maximum = 0;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        maximum = std::max(maximum, instruction.id);
    for (auto &block : body.blocks) {
      for (size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &marker = block.instructions[index];
        if (marker.fullExpressionEnd == 0 || marker.earlyExitFullExpressionEnd)
          continue;
        const auto count = std::count_if(
            body.blocks.begin(), body.blocks.end(), [&](const auto &candidate) {
              return std::any_of(
                  candidate.instructions.begin(), candidate.instructions.end(),
                  [&](const auto &instruction) {
                    return instruction.fullExpressionEnd ==
                               marker.fullExpressionEnd &&
                           instruction.earlyExitFullExpressionEnd;
                  });
            });
        if (count == 0)
          continue;
        auto repeated = marker;
        repeated.id = maximum + 1;
        // Preserve the one-primary invariant and make an alternate boundary
        // reachable in the same iteration as that primary boundary.
        repeated.earlyExitFullExpressionEnd = true;
        block.instructions.insert(block.instructions.begin() + index, repeated);
        (void)lang::rebuildMirValueUses(body);
        mutated = true;
        break;
      }
      if (mutated)
        break;
    }
    if (mutated)
      break;
  }
  if (!mutated) {
    std::cerr
        << "expected a boundary with both normal and propagated-error paths\n";
    return 1;
  }
  const auto verification = lang::verifyMirProgram(duplicate);
  const bool rejectedDuplicate = std::any_of(
      verification.errors.begin(), verification.errors.end(),
      [](const auto &error) {
        return error.message.find("full-expression boundary markers are not on "
                                  "mutually exclusive") != std::string::npos;
      });
  if (verification.valid() || !rejectedDuplicate) {
    std::cerr << "same-iteration duplicate boundaries must still be rejected\n";
    for (const auto &error : verification.errors)
      std::cerr << error.message << '\n';
    return 1;
  }

  auto missingInvoke = frontend.mir;
  bool removedInvoke = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingInvoke.functionInstances())) {
    for (auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke ||
          block.instructions.empty())
        continue;
      const auto &call = block.instructions.back();
      if (call.kind != lang::MirInstructionKind::Call ||
          call.info.type.kind != lang::SemanticType::Expected ||
          call.info.type.arguments.front().kind != lang::SemanticType::Class ||
          call.successResultDrop || call.successResultDestination)
        continue;
      const auto target = block.terminator.target;
      block.terminator = {};
      block.terminator.kind = lang::MirTerminatorKind::Goto;
      block.terminator.target = target;
      lang::rebuildMirReachability(function.body);
      (void)lang::rebuildMirValueUses(function.body);
      removedInvoke = true;
      break;
    }
    if (removedInvoke)
      break;
  }
  if (!removedInvoke || lang::verifyMirProgram(missingInvoke).valid()) {
    std::cerr << "cleanup-free expected calls must retain their failure edge\n";
    return 1;
  }

  // An explicit initializer without source arguments still evaluates the
  // base constructor's omitted default arguments. They form one full
  // expression, so a nested owning default argument is a prepared input.
  const auto baseDefaults =
      lang::Frontend().analyze("/tmp/gti-explicit-base-default.gti", R"(
class Token {
  int value;
public:
  Token(int value) : value(value) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  ~Token() {}
  int get() { return this.value; }
};
int take(Token token = Token(3)) { return token.get(); }
class Base {
  int value;
public:
  Base(int value = take()) : value(value) {}
};
class Explicit : public Base {
public:
  Explicit() : Base() {}
};
int main() { Explicit first{}; return 0; }
)");
  bool preparedDefault = false;
  for (const auto &constructor : baseDefaults.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        preparedDefault =
            preparedDefault ||
            (instruction.kind == lang::MirInstructionKind::CallInput &&
             instruction.defaultArgument &&
             instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
             instruction.preparedParameterDrop.has_value());
  if (!baseDefaults.mir.valid() ||
      !lang::verifyMirProgram(baseDefaults.mir).valid() || !preparedDefault) {
    std::cerr << "an argumentless explicit base initializer must prepare its "
                 "owning default arguments\n";
    return 1;
  }
  return 0;
}
