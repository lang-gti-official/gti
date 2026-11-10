#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"

namespace {
void checkNestedConstructorPublication() {
  const auto differingPermissions = lang::Frontend().analyze(
      "nested-field-permissions.gti", R"(
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
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(differingPermissions.canGenerateCode(),
         "nested publication uses the nested field's permission, not the "
         "outer carrier's first dependency");
  if (differingPermissions.canGenerateCode()) {
    const auto cpp = gti_test::emitCppText(differingPermissions);
    expect(!cpp.empty(),
           "backend resolves nested receiver reference projections");
    std::size_t constructorBorrows = 0;
    for (const auto &function : differingPermissions.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              !stage.result ||
              stage.callInputRole != lang::MirCallInputRole::Argument ||
              stage.callInputIndex != 0 || stage.operands.size() != 1 ||
              (stage.operands[0].kind != lang::MirOperandKind::BorrowRead &&
               stage.operands[0].kind != lang::MirOperandKind::BorrowWrite))
            continue;
          for (const auto &use : function.body.usesOf(*stage.result)) {
            const auto *consumerBlock = function.body.findBlock(use.block);
            if (!consumerBlock)
              continue;
            for (const auto &consumer : consumerBlock->instructions) {
              if (consumer.id != use.instruction ||
                  consumer.kind != lang::MirInstructionKind::Construct ||
                  consumer.operands.size() != 2)
                continue;
              ++constructorBorrows;
              expect(cpp.find(".construct(__gti_mir_v_" +
                              std::to_string(*stage.result) + ",") ==
                         std::string::npos,
                     "constructor publishes exact borrowed referent, not an "
                     "erased SSA stage");
            }
          }
        }
    expect(constructorBorrows == 1, "mixed constructor regression checks its "
                                    "borrowed argument publication");
    bool inspected = false;
    for (const auto &function : differingPermissions.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        if (place.root != lang::MirPlaceRootKind::This ||
            place.projections.size() != 3 ||
            place.projections[0].kind != lang::MirProjectionKind::Field ||
            place.projections[1].kind != lang::MirProjectionKind::Field ||
            place.projections[2].kind != lang::MirProjectionKind::Dereference)
          continue;
        inspected = true;
        auto changed = differingPermissions.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.places[place.id - 1].projections.pop_back();
        bool rejected = false;
        try {
          static_cast<void>(
              gti_test::emitCpp(differingPermissions, changed, changed));
        } catch (const std::logic_error &) {
          rejected = true;
        }
        expect(rejected,
               "backend cannot erase a nested reference dereference step");
      }
    }
    expect(inspected, "nested receiver regression exercises its exact path");
  }
}
} // namespace

int main() {
  checkNestedConstructorPublication();
  return failures == 0 ? 0 : 1;
}
