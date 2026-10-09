#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/closed_world.h"
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

std::size_t countProofs(const lang::OptimizedProgram &optimized,
                        lang::MirTransformProofKind proof) {
  return static_cast<std::size_t>(std::count_if(
      optimized.report.transformations.begin(),
      optimized.report.transformations.end(), [&](const auto &record) {
        return record.kind == lang::MirTransformKind::DevirtualizeCall &&
               record.proof == proof;
      }));
}

// A reference parameter whose referent class is exact on every visible call
// of a closed function admits static dispatch inside that function.
void testParameterReceiver(const std::string &name, const std::string &body,
                           std::size_t expected) {
  using namespace lang;
  const auto frontend = Frontend().analyze(name + ".gti", prefix + body);
  expect(frontend.canGenerateCode(), name + " lowers");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  for (auto level :
       {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::O3}) {
    auto optimized = OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), name + " verifies");
    for (const auto &error : optimized.report.outputVerification.errors)
      std::cerr << error.message << '\n';
    const std::size_t count =
        countProofs(optimized, MirTransformProofKind::ExactParameterReceiver);
    expect(count == (level == OptimizationLevel::O1 ? 0 : expected),
           name + " proves every exact parameter receiver at O2/O3 only");
    if (count == 0)
      continue;
    auto forged = optimized.report.transformations;
    for (auto &record : forged)
      if (record.proof == MirTransformProofKind::ExactParameterReceiver)
        record.requiredAnalyses.closedWorld = false;
    expect(!verifyMirOptimizationCoherence(frontend.mir, optimized.mir, forged)
                .valid(),
           name + " rejects a record that omits its closed-world proof input");
    forged = optimized.report.transformations;
    std::erase_if(forged, [](const auto &record) {
      return record.kind == MirTransformKind::DevirtualizeCall;
    });
    expect(!verifyMirOptimizationCoherence(frontend.mir, optimized.mir, forged)
                .valid(),
           name + " rejects missing parameter receiver evidence");
  }
}

// The analysis itself fails closed when a visible call stops being a plain
// direct call, passes an inexact object, or the callee may rebind its
// reference.
void testClosedWorldFacts() {
  using namespace lang;
  const auto frontend = Frontend().analyze("closed-world.gti", prefix + R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Increment concrete{}; return apply(concrete, 40) - 41; }
)");
  expect(frontend.canGenerateCode(), "closed-world fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const auto *apply = [&]() -> const MirFunctionInstance * {
    for (const auto &function : frontend.mir.functionInstances())
      if (function.parameterTypes.size() == 2 &&
          function.parameterTypes[0].kind == SemanticType::Reference &&
          !function.owner)
        return &function;
    return nullptr;
  }();
  expect(apply != nullptr, "closed-world fixture has the forwarding function");
  if (apply == nullptr)
    return;
  const auto facts = analyzeMirClosedWorldFacts(frontend.mir);
  const SemanticType *exact = facts.exactParameterClass(apply->id, 0);
  expect(facts.closed(apply->id) && exact != nullptr &&
             exact->kind == SemanticType::Class,
         "every visible call passes one complete local class object");
  expect(facts.exactParameterClass(apply->id, 1) == nullptr,
         "scalar parameters carry no class fact");
  for (const auto &function : frontend.mir.functionInstances())
    if (function.entryKind != ProgramEntryKind::None ||
        function.virtualMethod || function.overrideMethod)
      expect(!facts.closed(function.id),
             "entry and virtual functions are invoked outside visible calls");

  const auto mutateCall = [&](const auto &mutation) {
    auto changed = frontend.mir;
    for (const auto address : enumerateMirBodyAddresses(changed)) {
      auto *body = findMirBody(changed, address);
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.functionTarget == apply->id)
            mutation(*body, instruction);
    }
    return analyzeMirClosedWorldFacts(changed);
  };
  const auto virtualReference =
      mutateCall([](MirBody &, MirInstruction &call) {
        call.dispatch = CallDispatch::Virtual;
      });
  expect(!virtualReference.closed(apply->id) &&
             !virtualReference.exactParameterClass(apply->id, 0),
         "a reference other than a plain direct call opens the function");
  const auto copied = mutateCall([](MirBody &body, MirInstruction &call) {
    for (auto &block : body.blocks)
      for (auto &input : block.instructions)
        if (input.kind == MirInstructionKind::CallInput &&
            input.callSite == call.callSite && input.callInputIndex == 0 &&
            input.callInputRole == MirCallInputRole::Argument)
          input.operands.front().kind = MirOperandKind::Copy;
  });
  expect(copied.closed(apply->id) && !copied.exactParameterClass(apply->id, 0),
         "an argument that is not a borrowed exact object is unknown");

  auto rebound = frontend.mir;
  auto *body =
      findMirBody(rebound, {.kind = MirBodyKind::Function, .owner = apply->id});
  for (auto &place : body->places)
    if (place.binding == apply->parameterBindings[0] &&
        place.projections.empty())
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::CallInput &&
              instruction.callInputRole == MirCallInputRole::Receiver)
            instruction.destination = place.id;
  expect(!analyzeMirClosedWorldFacts(rebound).exactParameterClass(apply->id, 0),
         "a body that may replace its reference parameter loses the fact");
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
  testClosedWorldFacts();
  testParameterReceiver("parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Increment concrete{}; return apply(concrete, 40) - 41; }
)", 1);
  testParameterReceiver("forwarded-parameter", R"(
int inner(Operation& selected, int value) { return selected.run(value); }
int outer(Operation& selected, int value) { return inner(selected, value) + selected.run(0); }
int main() { Increment first{}; Increment second{}; return outer(first, 40) + inner(second, 1) - 44; }
)", 2);
  testParameterReceiver("recursive-parameter", R"(
int repeat(Operation& selected, int value, int count) {
  if (count > 0) { return repeat(selected, selected.run(value), count - 1); }
  return value;
}
int main() { Increment concrete{}; return repeat(concrete, 37, 3) - 40; }
)", 1);
  testParameterReceiver("observable-cleanup-parameter", R"(
extern "C" { void record_drop(); }
class Resource : public Operation {
public:
  Resource() {}
  ~Resource() { record_drop(); }
  int run(int value) override { return value + 1; }
};
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Resource concrete{}; return apply(concrete, 40) - 41; }
)", 1);
  testParameterReceiver("member-and-position", R"(
class Runner {
public:
  Runner() {}
  int both(int value, Operation& first, Operation& second) { return second.run(first.run(value)); }
};
int pair(Operation& first, int value, Operation& second) { return second.run(first.run(value)); }
int main() { Increment up{}; Decrement down{}; Runner runner{};
  return pair(up, 3, down) + runner.both(3, down, up) - 6; }
)", 4);
  testConservative("mixed-parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Increment up{}; Decrement down{}; return apply(up, 40) + apply(down, 2) - 42; }
)");
  testConservative("reference-origin-parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { Increment concrete{}; Operation& view = concrete; return apply(view, 40) - 41; }
)");
  testConservative("function-item-parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int call<Function>(Function function) { Increment concrete{}; return function(concrete, 40); }
int main() { Increment concrete{}; return apply(concrete, 40) + call(apply) - 82; }
)");
  testConservative("uncalled-parameter", R"(
int apply(Operation& selected, int value) { return selected.run(value); }
int main() { return 0; }
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
