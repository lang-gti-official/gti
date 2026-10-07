#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/devirtualization.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
const std::string prefix = R"(
interface Operation { int run(int value); };
class Increment : public Operation {
public:
  Increment() {}
  int run(int value) override { return value + this.bias; }
private:
  int bias = 1;
};
class Decrement : public Operation {
public:
  Decrement() {}
  int run(int value) override { return value - 1; }
};
)";

void testLocalReceiver() {
  using namespace lang;
  const auto frontend = Frontend().analyze("exact-receiver.gti", prefix + R"(
int apply(int value) {
  Increment concrete{};
  Operation& selected = concrete;
  return selected.run(value);
}
int main() { return apply(40) - 41; }
)");
  expect(frontend.canGenerateCode(), "exact local interface fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  for (const auto address : enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = findMirBody(frontend.mir, address);
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        const auto proof =
            proveMirExactReceiver(frontend.mir, address, instruction.id);
        if (!proof)
          continue;
        auto renumbered = frontend.mir;
        auto *renumberedBody = findMirBody(renumbered, address);
        MirInstructionId nextId = 1;
        for (const auto &b : renumberedBody->blocks)
          for (const auto &i : b.instructions)
            nextId = std::max(nextId, i.id + 1);
        bool sameBlockInitialization = false;
        for (auto &b : renumberedBody->blocks) {
          for (auto &i : b.instructions) {
            if (i.kind != MirInstructionKind::Initialize ||
                i.destination != proof->concretePlace)
              continue;
            sameBlockInitialization = b.id == proof->block;
            const auto oldId = i.id;
            i.id = nextId;
            for (auto &value : renumberedBody->values)
              if (value.definition == oldId)
                value.definition = nextId;
          }
        }
        expect(sameBlockInitialization,
               "ordering fixture initializes in the virtual call block");
        expect(rebuildMirValueUses(*renumberedBody) &&
                   verifyMirProgram(renumbered).valid(),
               "stable initialization identity may exceed later call identity");
        expect(proveMirExactReceiver(renumbered, address, instruction.id)
                   .has_value(),
               "receiver initialization proof uses order, not stable IDs");
        auto unavailable = frontend.mir;
        auto *unavailableBody = findMirBody(unavailable, address);
        for (auto &b : unavailableBody->blocks)
          for (auto &i : b.instructions)
            if (i.kind == MirInstructionKind::Initialize &&
                i.destination == proof->concretePlace)
              i.tombstone = true;
        expect(!proveMirExactReceiver(unavailable, address, instruction.id),
               "missing receiver initialization blocks proof");
        auto late = frontend.mir;
        auto *lateBody = findMirBody(late, address);
        auto *lateBlock = lateBody->findBlock(proof->block);
        const auto initialization =
            std::find_if(lateBlock->instructions.begin(),
                         lateBlock->instructions.end(), [&](const auto &i) {
                           return i.kind == MirInstructionKind::Initialize &&
                                  i.destination == proof->concretePlace;
                         });
        if (initialization != lateBlock->instructions.end()) {
          std::rotate(initialization, std::next(initialization),
                      lateBlock->instructions.end());
          expect(
              !proveMirExactReceiver(late, address, instruction.id),
              "initialization after the call blocks proof despite earlier ID");
        }
        auto malformed = frontend.mir;
        auto *changed = findMirBody(malformed, address);
        changed->blocks.front().instructions.front().rawMemoryAccess = true;
        expect(!proveMirExactReceiver(malformed, address, instruction.id),
               "raw-memory boundary blocks receiver proof");
        malformed = frontend.mir;
        changed = findMirBody(malformed, address);
        changed->loans.front().escapes = true;
        expect(!proveMirExactReceiver(malformed, address, instruction.id),
               "escaped receiver loan blocks proof");
        malformed = frontend.mir;
        changed = findMirBody(malformed, address);
        changed->blocks.front().instructions.front().synchronization.kind =
            SynchronizationOperationKind::AtomicLoad;
        expect(!proveMirExactReceiver(malformed, address, instruction.id),
               "synchronization boundary blocks proof");
        auto independent = frontend.mir;
        MirEditor editor(independent, "exact-receiver-test");
        editor.queueExactReceiver({address, instruction.id});
        editor.queueExactReceiver({address, instruction.id});
        expect(!editor.apply().valid() && independent == frontend.mir,
               "duplicate exact receiver claims fail atomically");
      }
    }
  }
  for (auto level :
       {OptimizationLevel::O0, OptimizationLevel::O2, OptimizationLevel::O3}) {
    auto optimized = OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "optimized exact receiver verifies");
    for (const auto &error : optimized.report.outputVerification.errors)
      std::cerr << error.message << '\n';
    const auto count = std::count_if(
        optimized.report.transformations.begin(),
        optimized.report.transformations.end(), [](const auto &record) {
          return record.kind == MirTransformKind::DevirtualizeCall;
        });
    expect(count == (level == OptimizationLevel::O0 ? 0 : 1),
           "exactly one local receiver proof at O2/O3");
    expect(verifyMirOptimizationCoherence(frontend.mir, optimized.mir,
                                          optimized.report.transformations)
               .valid(),
           "canonical source independently replays exact dispatch");
    if (count) {
      auto forged = optimized.report.transformations;
      auto record =
          std::find_if(forged.begin(), forged.end(), [](const auto &r) {
            return r.kind == MirTransformKind::DevirtualizeCall;
          });
      record->relatedPlaces[0] = 0;
      expect(
          !verifyMirOptimizationCoherence(frontend.mir, optimized.mir, forged)
               .valid(),
          "forged receiver place rejected");
      forged = optimized.report.transformations;
      std::erase_if(forged, [](const auto &r) {
        return r.kind == MirTransformKind::DevirtualizeCall;
      });
      expect(
          !verifyMirOptimizationCoherence(frontend.mir, optimized.mir, forged)
               .valid(),
          "missing receiver evidence rejected");
    }
  }
}

void testConservative(const std::string &name, const std::string &body) {
  using namespace lang;
  const auto frontend = Frontend().analyze(name + ".gti", prefix + body);
  expect(frontend.canGenerateCode(), name + " lowers");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  auto optimized = OptimizationPipeline().run({.hir = frontend.hir,
                                               .mir = frontend.mir,
                                               .level = OptimizationLevel::O3});
  expect(optimized.valid(), name + " verifies");
  expect(std::none_of(optimized.report.transformations.begin(),
                      optimized.report.transformations.end(),
                      [](const auto &r) {
                        return r.kind == MirTransformKind::DevirtualizeCall;
                      }),
         name + " retains dynamic dispatch");
}
} // namespace

int main() {
  testLocalReceiver();
  testConservative("parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Increment concrete{}; return apply(concrete, 40) - 41; }
)");
  testConservative("mutable", R"(
int apply(int value) { mut Increment concrete{}; Operation& selected = concrete; return selected.run(value); }
int main() { return apply(40) - 41; }
)");
  testConservative("other-loan", R"(
int apply(int value) { Increment concrete{}; Operation& selected = concrete;
  Operation& other = concrete; int first = other.run(value); return selected.run(first); }
int main() { return apply(40) - 42; }
)");
  testConservative("native-target", R"(
extern "C" { int opaque(int value); }
class Native : public Operation {
public:
  Native() {}
  int run(int value) override { return opaque(value); }
};
int main() { Native concrete{}; Operation& selected = concrete; return selected.run(40); }
)");
  testConservative("multiple-interface-bases", R"(
interface Tagged { int tag(); };
class TaggedIncrement : public Operation, public Tagged {
public:
  TaggedIncrement() {}
  int run(int value) override { return value + 1; }
  int tag() override { return 1; }
};
int main() { TaggedIncrement concrete{}; Operation& selected = concrete; return selected.run(40) - 41; }
)");
  testConservative("borrowed-argument", R"(
interface Borrowing { int run(int& value); };
class BorrowingIncrement : public Borrowing {
public:
  BorrowingIncrement() {}
  int run(int& value) override { return value + 1; }
};
int main() { BorrowingIncrement concrete{}; Borrowing& selected = concrete;
  int value = 40; return selected.run(value) - 41; }
)");
  testConservative("recursive-target", R"(
class Recursive : public Operation {
public:
  Recursive() {}
  int run(int value) override { if (value > 0) { return this.run(value - 1); } return 0; }
};
int main() { Recursive concrete{}; Operation& selected = concrete; return selected.run(40); }
)");
  testConservative("observable-cleanup", R"(
extern "C" { void record_drop(); }
class Resource : public Operation {
public:
  Resource() {}
  ~Resource() { record_drop(); }
  int run(int value) override { return value + 1; }
};
int main() { Resource concrete{}; Operation& selected = concrete; return selected.run(40); }
)");
  return failures ? 1 : 0;
}
