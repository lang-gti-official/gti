#include "compiler_test_fixture.h"

int main() {
  const auto result = lang::Frontend().analyze(
      "reference-pack-loans.gti", R"gti(
class Ticket {
public:
  Ticket() = default;
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
void read<T>(T& value) {}
void write<T>(mut T& value) {}
void observe<Args...>(Args&... args) { (read(args), ...); }
void update<Args...>(mut Args&... args) {
  (write(args), ...);
  observe(args...);
}
int main() { mut Ticket first{}; mut Ticket second{}; update(first, second); return 0; }
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "reference pack calls and folds retain verified per-element loans");
  if (!result.canGenerateCode())
    return 1;
  bool checkedEntry = false;
  bool checkedInput = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (!loan.entry || loan.access != lang::AccessMode::Mutable || !place ||
          place->projections.empty() ||
          place->projections.front().kind !=
              lang::MirProjectionKind::PackElement)
        continue;
      auto forged = result.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          forged.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].access = lang::AccessMode::ReadOnly;
      expect(!lang::verifyMirProgram(forged).valid(),
             "a mutable pack entry cannot be forged into read-only authority");
      checkedEntry = true;
      break;
    }
    for (const auto &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &input = block.instructions[index];
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputRole != lang::MirCallInputRole::Argument ||
            input.callInputKind != lang::HirCallInputKind::MutableBorrow)
          continue;
        auto forged = result.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        instance->body.blocks[block.id - 1].instructions[index].callInputKind =
            lang::HirCallInputKind::MoveValue;
        expect(
            !lang::verifyMirProgram(forged).valid(),
            "a reference-pack call cannot transfer ownership of its referent");
        checkedInput = true;
        break;
      }
      if (checkedInput)
        break;
    }
    if (checkedEntry && checkedInput)
      break;
  }
  expect(checkedEntry && checkedInput,
         "reference packs publish entry and call authority");
  return failures ? 1 : 0;
}
