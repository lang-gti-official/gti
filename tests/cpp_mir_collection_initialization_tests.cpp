#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

// Frozen C++ backend: C++ MIR body emitter admission of a collection
// protocol's multi-block moved-from return cleanup tail. The backend-neutral
// contract lives in collection_initialization_tests.

namespace {
int failures = 0;
lang::FrontendResult analyze(const std::string &path,
                             const std::string &source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(path, source, {library / "prelude.gti"}, {},
                                  {library});
}
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
void cppReturnCleanupTail() {
  const auto result = analyze("collection-return.gti", R"(
class Item {
public:
  int value;
  Item(int n) : value(n) {}
  ~Item() { int check = 1 / this.value; }
};
void consume(Item item) {}
class Bag {
  Item held;
public:
  Bag() : held(Item(9)) {}
  static Bag collection_init(uint64_t count) { return Bag(); }
  static Bag collection_insert(Bag bag, Item item) {
    // The unmoved witness's fallible cleanup splits the return tail; the
    // moved-from parameter shells retire after it without destructors.
    Item witness{3};
    consume(std::move(item));
    return std::move(bag);
  }
};
int main() { Bag values = {Item(1), Item(2)}; return 0; }
)");
  expect(result.canGenerateCode(), "owning protocol return cleanup verifies");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto lowered = gti_test::lowerProgram(result, result.mir, result.mir);
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  expect(lang::CppMirBodyEmitter(result.mir, map).analyzeProgram().ready(),
         "moved-from return cleanup tail has a representation");
  bool mutated = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
        continue;
      const auto *tail = function.body.findBlock(block.terminator.target);
      if (tail == nullptr)
        continue;
      for (const auto &move : block.instructions) {
        if (move.kind != lang::MirInstructionKind::Move ||
            move.operands.size() != 1 ||
            move.info.type != function.body.returnType)
          continue;
        for (const auto &drop : tail->instructions) {
          if (drop.kind != lang::MirInstructionKind::Drop ||
              drop.destination != move.operands.front().place)
            continue;
          auto forged = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          for (auto &candidate : changed->body.blocks) {
            if (candidate.id != tail->id)
              continue;
            for (auto &instruction : candidate.instructions) {
              if (instruction.id == drop.id)
                instruction.kind = lang::MirInstructionKind::Initialize;
            }
          }
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "return tail must reject a source reinitialization");
          mutated = true;
        }
      }
    }
  }
  expect(mutated, "regression exercises a multi-block moved-from cleanup tail");
}

} // namespace

int main() {
  cppReturnCleanupTail();
  return failures == 0 ? 0 : 1;
}
