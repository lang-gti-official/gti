#include "gti/frontend.h"
#include "gti/mir.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// Shared failure cleanup: identical cleanup suffixes of a function's failure
// edges are emitted once, as continuation records entered by ContinueFailure
// edges. These tests pin the structural scaling, the verified continuation
// contract, and the shapes that must stay unshared.

namespace {

bool passed = true;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    passed = false;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::filesystem::path repositoryRoot() {
  return std::filesystem::path(__FILE__).parent_path().parent_path();
}

lang::FrontendResult analyze(const std::string &name,
                             const std::string &source) {
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() / name,
                                  source,
                                  {repositoryRoot() / "stdlib/prelude.gti"}, {},
                                  {repositoryRoot() / "stdlib"});
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

const lang::MirBody *functionBody(const lang::FrontendResult &frontend,
                                  const std::string &name) {
  for (const lang::MirFunctionInstance &instance :
       frontend.mir.functionInstances()) {
    const lang::FunctionInfo *info =
        frontend.semantics.findFunction(instance.declaration);
    if (info != nullptr && info->qualifiedName == name) {
      return &instance.body;
    }
  }
  return nullptr;
}

struct CleanupShape {
  std::size_t blocks = 0;
  std::size_t producers = 0;
  std::size_t continuations = 0;
  std::size_t failureDrops = 0;
};

CleanupShape shapeOf(const lang::MirBody &body) {
  CleanupShape shape{.blocks = body.blocks.size()};
  for (const lang::MirFailureRecord &record : body.failureRecords) {
    (record.kind == lang::MirFailureRecordKind::Continuation
         ? shape.continuations
         : shape.producers) += 1;
  }
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      shape.failureDrops +=
          instruction.kind == lang::MirInstructionKind::Drop &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup;
    }
  }
  return shape;
}

bool rejects(const lang::MirBody &body, const std::string &fragment) {
  const lang::MirVerificationResult result = lang::verifyMirBody(body);
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(fragment) != std::string::npos;
                     });
}

// Owners built by a failure-capable constructor, with a failure-capable
// destructor, `count` of them live at once. `calls` adds a failing call
// after each owner, `returns` an early return after each one.
std::string ownersProgram(std::size_t count, bool calls, bool returns) {
  std::string source = R"(mut int32_t zero = 0;
class Owner {
  int32_t divisor;
public:
  Owner(int32_t divisor) : divisor(10 / divisor) {}
  ~Owner() { int32_t checked = 10 / this.divisor; }
};
void step(int32_t at) {
  if (at == zero) {
    int32_t failure = 1 / zero;
  }
}
int32_t run(int32_t divisor) {
)";
  for (std::size_t index = 0; index < count; ++index) {
    const std::string id = std::to_string(index + 1);
    source += "  Owner owner" + id + " = Owner(divisor);\n";
    if (calls) {
      source += "  step(" + id + ");\n";
    }
    if (returns) {
      source += "  if (divisor == " + id + "0) { return " + id + "; }\n";
    }
  }
  source += "  return 0;\n}\nint main() { return run(1); }\n";
  return source;
}

void testLinearCleanupScaling() {
  struct Variant {
    const char *name;
    bool calls;
    bool returns;
  };
  for (const Variant variant : {Variant{"constructions", false, false},
                                Variant{"constructions and calls", true, false},
                                Variant{"early returns", true, true}}) {
    std::vector<CleanupShape> shapes;
    for (const std::size_t count : {10, 20, 40, 80}) {
      const auto frontend =
          analyze("shared-cleanup-scaling.gti",
                  ownersProgram(count, variant.calls, variant.returns));
      expect(frontend.canGenerateCode(),
             std::string(variant.name) + ": scaling fixture lowers");
      if (!frontend.canGenerateCode()) {
        printDiagnostics(frontend);
        return;
      }
      const lang::MirBody *body = functionBody(frontend, "run");
      expect(body != nullptr, "scaling fixture has its run body");
      if (body == nullptr)
        return;
      const CleanupShape shape = shapeOf(*body);
      shapes.push_back(shape);
      // Every failure entry reaches one shared chain of reverse drops, so
      // each owner is destroyed by exactly one failure-cleanup Drop however
      // many failure sites keep it live.
      expect(shape.failureDrops <= count + 1,
             std::string(variant.name) + " with " + std::to_string(count) +
                 " owners emits " + std::to_string(shape.failureDrops) +
                 " failure drops instead of one per owner");
      expect(shape.continuations >= count - 1,
             std::string(variant.name) + ": owners share cleanup suffixes");
    }
    // Without returns the whole body grows linearly in its owners. Early
    // returns add each return's own normal-path drops, which are not
    // failure cleanup and are bounded separately above.
    if (!variant.returns) {
      expect(shapes[3].blocks * 10 <= shapes[2].blocks * 22,
             std::string(variant.name) + ": blocks grow linearly (" +
                 std::to_string(shapes[2].blocks) + " for 40 owners, " +
                 std::to_string(shapes[3].blocks) + " for 80)");
    }
  }
}

const char *contractProgram = R"(mut int32_t zero = 0;
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  ~Guard() { int32_t checked = 100 / this.divisor; }
  int32_t get() { return this.divisor; }
};
class Passive {
public:
  Passive() {}
  ~Passive() {}
};
int32_t checked(int32_t value, int32_t divisor) { return value / divisor; }
int32_t run(int32_t divisor) {
  Guard a = Guard(1);
  Guard& view = a;
  int32_t x = checked(1, divisor);
  Passive passive = Passive();
  Guard b = Guard(2);
  int32_t y = checked(2, divisor);
  Guard c = Guard(3);
  int32_t z = checked(3, divisor);
  return x + y + z + view.get();
}
int main() { return run(1) - 7; }
)";

// The first block of `body` matching `predicate`, or zero.
lang::MirBlockId
findBlock(const lang::MirBody &body,
          const std::function<bool(const lang::MirBlock &)> &predicate) {
  for (const lang::MirBlock &block : body.blocks) {
    if (predicate(block))
      return block.id;
  }
  return 0;
}

lang::MirFailureRecordKind kindOf(const lang::MirBody &body,
                                  lang::MirFailureRecordId record) {
  const lang::MirFailureRecord *found = body.findFailureRecord(record);
  return found == nullptr ? lang::MirFailureRecordKind::Producer : found->kind;
}

void testSharedContinuationContract() {
  const auto frontend = analyze("shared-cleanup-contract.gti", contractProgram);
  expect(frontend.canGenerateCode(), "the contract fixture lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const lang::MirBody *found = functionBody(frontend, "run");
  if (found == nullptr) {
    expect(false, "the contract fixture has its run body");
    return;
  }
  const lang::MirBody &body = *found;
  expect(lang::verifyMirBody(body).valid(),
         "the shared cleanup of the contract fixture verifies");
  const CleanupShape shape = shapeOf(body);
  expect(shape.continuations > 0, "the contract fixture shares cleanup");

  const lang::MirBlockId edge = findBlock(body, [](const auto &block) {
    return block.terminator.kind == lang::MirTerminatorKind::ContinueFailure;
  });
  expect(edge != 0, "the contract fixture enters a continuation");
  if (edge == 0)
    return;
  const lang::MirFailureRecordId continuation =
      body.findBlock(edge)->terminator.failureRecord;
  const lang::MirFailureRecord &record = *body.findFailureRecord(continuation);

  {
    // A continuation edge must name a continuation, not a producer.
    auto forged = body;
    const lang::MirFailureRecordId producer =
        std::find_if(body.failureRecords.begin(), body.failureRecords.end(),
                     [](const auto &candidate) {
                       return candidate.kind ==
                              lang::MirFailureRecordKind::Producer;
                     })
            ->id;
    forged.blocks[edge - 1].terminator.failureRecord = producer;
    expect(rejects(forged, "failure continuation does not enter"),
           "a continuation edge naming a producer record is rejected");
  }
  {
    // An ordinary jump into a continuation would change the active record
    // without an explicit continuation edge.
    auto forged = body;
    forged.blocks[edge - 1].terminator.kind = lang::MirTerminatorKind::Goto;
    forged.blocks[edge - 1].terminator.failureRecord = 0;
    expect(rejects(forged, "active failure state"),
           "a Goto into a shared continuation is rejected");
  }
  {
    // A secondary cleanup failure terminates; it never continues cleanup.
    auto forged = body;
    const lang::MirBlockId secondary = findBlock(body, [](const auto &block) {
      return block.terminator.kind ==
             lang::MirTerminatorKind::TerminateCleanupFailure;
    });
    expect(secondary != 0, "the contract fixture has a fallible cleanup drop");
    if (secondary != 0) {
      forged.blocks[secondary - 1].terminator = {
          .kind = lang::MirTerminatorKind::ContinueFailure,
          .failureRecord = continuation,
          .target = record.parameterBlock};
      expect(!lang::verifyMirBody(forged).valid(),
             "a secondary failure cannot continue into shared cleanup");
    }
  }
  {
    // A continuation is a shared suffix, never an Invoke's own record.
    auto forged = body;
    forged.failureRecords[continuation - 1].kind =
        lang::MirFailureRecordKind::Producer;
    expect(!lang::verifyMirBody(forged).valid(),
           "a continuation forged as a producer record is rejected");
  }
  {
    // A shared drop runs once per path; dropping its obligation twice on a
    // path, or not at all, is rejected by the joined lifecycle state.
    const lang::MirBlockId shared = findBlock(body, [&](const auto &block) {
      return kindOf(body, block.activeFailure) ==
                 lang::MirFailureRecordKind::Continuation &&
             std::any_of(block.instructions.begin(), block.instructions.end(),
                         [](const auto &instruction) {
                           return instruction.kind ==
                                  lang::MirInstructionKind::Drop;
                         });
    });
    expect(shared != 0, "a continuation drops a shared owner");
    if (shared != 0) {
      const auto drop = std::find_if(
          body.findBlock(shared)->instructions.begin(),
          body.findBlock(shared)->instructions.end(),
          [](const auto &instruction) {
            return instruction.kind == lang::MirInstructionKind::Drop;
          });
      auto duplicated = body;
      lang::MirInstruction copy = *drop;
      copy.id = 1000000;
      auto &instructions = duplicated.blocks[shared - 1].instructions;
      instructions.insert(instructions.begin(), copy);
      lang::rebuildMirReachability(duplicated);
      (void)lang::rebuildMirValueUses(duplicated);
      expect(!lang::verifyMirBody(duplicated).valid(),
             "a duplicated shared drop is rejected");
    }
    // Without a passive owner's shared drop its obligation outlives the
    // cleanup on every path through that continuation.
    const lang::MirBlockId passive = findBlock(body, [&](const auto &block) {
      return kindOf(body, block.activeFailure) ==
                 lang::MirFailureRecordKind::Continuation &&
             std::any_of(block.instructions.begin(), block.instructions.end(),
                         [](const auto &instruction) {
                           return instruction.kind ==
                                      lang::MirInstructionKind::Drop &&
                                  instruction.definedFailure.propagation ==
                                      lang::FailurePropagationKind::None;
                         });
    });
    expect(passive != 0, "a continuation drops a passive shared owner");
    if (passive != 0) {
      auto missing = body;
      auto &kept = missing.blocks[passive - 1].instructions;
      kept.erase(std::find_if(kept.begin(), kept.end(), [](const auto &item) {
        return item.kind == lang::MirInstructionKind::Drop &&
               item.definedFailure.propagation ==
                   lang::FailurePropagationKind::None;
      }));
      expect(!lang::verifyMirBody(missing).valid(),
             "a missing shared drop is rejected");
    }
  }
  {
    // Ending a loan in a producer's private cleanup and again in the
    // shared suffix it enters retires the loan early on that path.
    const lang::MirBlockId sharedEnd = findBlock(body, [&](const auto &block) {
      return kindOf(body, block.activeFailure) ==
                 lang::MirFailureRecordKind::Continuation &&
             std::any_of(block.instructions.begin(), block.instructions.end(),
                         [](const auto &instruction) {
                           return instruction.kind ==
                                  lang::MirInstructionKind::EndBorrow;
                         });
    });
    expect(sharedEnd != 0, "a continuation ends a borrow it shares");
    if (sharedEnd != 0) {
      const lang::MirInstruction &end = *std::find_if(
          body.findBlock(sharedEnd)->instructions.begin(),
          body.findBlock(sharedEnd)->instructions.end(),
          [](const auto &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow;
          });
      auto early = body;
      lang::MirInstruction copy = end;
      copy.id = 1000001;
      early.blocks[edge - 1].instructions.push_back(copy);
      expect(!lang::verifyMirBody(early).valid(),
             "a loan retired before its shared cleanup endpoint is rejected");
    }
  }
  {
    // Continuations form an acyclic graph ending in one propagation.
    const lang::MirBlockId endpoint = findBlock(body, [&](const auto &block) {
      return block.terminator.kind ==
                 lang::MirTerminatorKind::PropagateFailure &&
             kindOf(body, block.activeFailure) ==
                 lang::MirFailureRecordKind::Continuation;
    });
    expect(endpoint != 0, "a shared suffix ends in its own propagation");
    // The newest continuation leads, through the others, to the one ending
    // at `endpoint`; looping that endpoint back to it closes a cycle.
    const lang::MirFailureRecordId own =
        endpoint == 0 ? 0 : body.findBlock(endpoint)->activeFailure;
    const auto newest = std::find_if(
        body.failureRecords.rbegin(), body.failureRecords.rend(),
        [&](const auto &candidate) {
          return candidate.kind == lang::MirFailureRecordKind::Continuation &&
                 candidate.id != own;
        });
    expect(newest != body.failureRecords.rend(),
           "the contract fixture chains several continuations");
    if (endpoint != 0 && newest != body.failureRecords.rend()) {
      auto cyclic = body;
      cyclic.blocks[endpoint - 1].terminator = {
          .kind = lang::MirTerminatorKind::ContinueFailure,
          .failureRecord = newest->id,
          .target = newest->parameterBlock};
      expect(rejects(cyclic, "form a cycle"),
             "continuations that re-enter one another are rejected");
    }
  }
  {
    // A cleanup-failure secondary keeps the continuation it runs under as
    // its primary.
    const lang::MirBlockId secondary = findBlock(body, [&](const auto &block) {
      return block.terminator.kind ==
                 lang::MirTerminatorKind::TerminateCleanupFailure &&
             kindOf(body, block.activeFailure) ==
                 lang::MirFailureRecordKind::Continuation;
    });
    expect(secondary != 0,
           "a shared fallible drop routes its own cleanup secondary");
    if (secondary != 0) {
      auto swapped = body;
      const lang::MirFailureRecordId other =
          std::find_if(body.failureRecords.begin(), body.failureRecords.end(),
                       [&](const auto &candidate) {
                         return candidate.kind ==
                                    lang::MirFailureRecordKind::Continuation &&
                                candidate.id !=
                                    body.findBlock(secondary)->activeFailure;
                       })
              ->id;
      swapped.blocks[secondary - 1].activeFailure = other;
      expect(!lang::verifyMirBody(swapped).valid(),
             "a secondary under the wrong primary record is rejected");
    }
  }
}

void testSingleFailureEdgeStaysUnshared() {
  const auto frontend = analyze("shared-cleanup-single.gti", R"(
class Guard {
public:
  Guard() {}
  ~Guard() {}
};
int32_t checked(int32_t value, int32_t divisor) { return value / divisor; }
int32_t run(int32_t divisor) {
  Guard a = Guard();
  return checked(1, divisor);
}
int main() { return run(1) - 1; }
)");
  expect(frontend.canGenerateCode(), "the single-edge fixture lowers");
  const lang::MirBody *body =
      frontend.canGenerateCode() ? functionBody(frontend, "run") : nullptr;
  if (body == nullptr)
    return;
  const CleanupShape shape = shapeOf(*body);
  expect(shape.continuations == 0 && shape.producers >= 1,
         "a lone failure edge keeps its own unshared cleanup chain");
}

void testMovedShellDropStaysPrivate() {
  // `kept` is moved before later failures; its failure-path drop retires a
  // moved-from shell without its destructor. That fact is proved from the
  // path back to the producer, so the drop may never sit in a shared tail.
  const auto frontend = analyze("shared-cleanup-moved.gti", R"(
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  ~Guard() { int32_t checked = 100 / this.divisor; }
};
int32_t checked(int32_t value, int32_t divisor) { return value / divisor; }
void sink(Guard value) {}
int32_t run(int32_t divisor) {
  Guard first = Guard(1);
  Guard kept = Guard(2);
  int32_t x = checked(1, divisor);
  sink(std::move(kept));
  int32_t y = checked(2, divisor);
  Guard last = Guard(3);
  int32_t z = checked(3, divisor);
  return x + y + z;
}
int main() { return run(1) - 6; }
)");
  expect(frontend.canGenerateCode(), "the moved-shell fixture lowers");
  const lang::MirBody *body =
      frontend.canGenerateCode() ? functionBody(frontend, "run") : nullptr;
  if (body == nullptr)
    return;
  for (const lang::MirBlock &block : body->blocks) {
    if (kindOf(*body, block.activeFailure) !=
        lang::MirFailureRecordKind::Continuation)
      continue;
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Drop) {
        expect(instruction.definedFailure.propagation ==
                   lang::FailurePropagationKind::Destructor,
               "a shared drop never relies on a moved-shell proof");
      }
    }
  }
  expect(lang::verifyMirBody(*body).valid(),
         "the moved-shell fixture verifies with private shell drops");
}

void testFixtureShares() {
  for (const char *name :
       {"shared_failure_cleanup.gti", "stdlib_surface_json.gti"}) {
    const std::filesystem::path path =
        repositoryRoot() / "tests/fixtures" / name;
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        path, std::nullopt, {repositoryRoot() / "stdlib/prelude.gti"}, {},
        {repositoryRoot() / "stdlib"});
    expect(frontend.canGenerateCode(), std::string(name) + " lowers");
    std::size_t continuations = 0;
    for (const lang::MirFunctionInstance &instance :
         frontend.mir.functionInstances()) {
      continuations += shapeOf(instance.body).continuations;
    }
    expect(continuations > 0, std::string(name) + " shares failure cleanup");
  }
}

} // namespace

int main() {
  testLinearCleanupScaling();
  testSharedContinuationContract();
  testSingleFailureEdgeStaysUnshared();
  testMovedShellDropStaysPrivate();
  testFixtureShares();
  if (!passed) {
    return 1;
  }
  std::cout << "shared failure cleanup tests passed\n";
  return 0;
}
