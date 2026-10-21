#include "gti/frontend.h"
#include "gti/mir.h"
#include "gti/mir_editor.h"
#include "gti/mir_lowering.h"
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

// Shared normal-exit cleanup: the exits of a function or lambda body share
// the drops and loan ends they have in common, as ladders of blocks each
// registered under the exit state it continues from. These tests pin the
// scaling, the exit kinds that share, the ones that keep their own cleanup,
// and the verifier's rejection of a corrupted shared exit.

std::size_t returnTerminators(const lang::MirBody &body) {
  return static_cast<std::size_t>(std::count_if(
      body.blocks.begin(), body.blocks.end(), [](const lang::MirBlock &block) {
        return block.terminator.kind == lang::MirTerminatorKind::Return;
      }));
}

std::size_t normalDrops(const lang::MirBody &body) {
  std::size_t drops = 0;
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      drops += instruction.kind == lang::MirInstructionKind::Drop &&
               instruction.lifecycle.size() == 1 &&
               !instruction.lifecycle.front().failureCleanup;
    }
  }
  return drops;
}

// Predecessor counts indexed by block id.
std::vector<std::size_t> predecessorCounts(const lang::MirBody &body) {
  std::vector<std::size_t> counts(body.blocks.size() + 1, 0);
  for (const lang::MirBlock &block : body.blocks) {
    const lang::MirTerminator &terminator = block.terminator;
    for (const lang::MirBlockId target :
         {terminator.target, terminator.elseTarget}) {
      if (target != 0 && target < counts.size())
        ++counts[target];
    }
    for (const lang::MirSwitchTarget &target : terminator.switchTargets) {
      if (target.target != 0 && target.target < counts.size())
        ++counts[target.target];
    }
  }
  return counts;
}

bool isTemporaryPlace(const lang::MirBody &body, lang::MirPlaceId id) {
  const lang::MirPlace *place = body.findPlace(id);
  return place != nullptr && place->root == lang::MirPlaceRootKind::Temporary;
}

// The instruction defining `value`, or null.
const lang::MirInstruction *definitionOf(const lang::MirBody &body,
                                         lang::MirValueId value) {
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.result == value)
        return &instruction;
    }
  }
  return nullptr;
}

// Whether a Return returns a value loaded from a Temporary carrier, either
// directly or wrapped as Unexpected: the tail of a shared exit ladder.
bool returnsCarrier(const lang::MirBody &body, const lang::MirBlock &block,
                    bool unexpected) {
  if (block.terminator.kind != lang::MirTerminatorKind::Return ||
      !block.terminator.value ||
      block.terminator.value->kind != lang::MirOperandKind::Value)
    return false;
  const lang::MirInstruction *definition =
      definitionOf(body, block.terminator.value->value);
  if (definition == nullptr)
    return false;
  if (unexpected) {
    if (definition->kind != lang::MirInstructionKind::Compute ||
        definition->operation != lang::MirOperation::Unexpected ||
        definition->operands.size() != 1 ||
        definition->operands.front().kind != lang::MirOperandKind::Value)
      return false;
    definition = definitionOf(body, definition->operands.front().value);
    if (definition == nullptr)
      return false;
  }
  return definition->kind == lang::MirInstructionKind::Load &&
         definition->operands.size() == 1 &&
         definition->operands.front().kind == lang::MirOperandKind::Copy &&
         isTemporaryPlace(body, definition->operands.front().place);
}

std::string firstError(const lang::MirBody &body) {
  const lang::MirVerificationResult result = lang::verifyMirBody(body);
  return result.errors.empty() ? std::string("(valid)")
                               : result.errors.front().message;
}

lang::FrontendResult analyzeUnsharedExits(const std::string &name,
                                          const std::string &source) {
  lang::MirLowerer::setShareExitCleanup(false);
  lang::FrontendResult frontend = analyze(name, source);
  lang::MirLowerer::setShareExitCleanup(true);
  return frontend;
}

void expectNoExitSharingFallback(const std::string &context) {
  const lang::MirCleanupSharingStatistics &statistics =
      lang::MirLowerer::cleanupSharingStatistics();
  expect(statistics.exitSharingBodyFallbacks == 0,
         context + ": no body fell back to unshared exits (" +
             statistics.lastExitSharingBodyFallback + ")");
  expect(statistics.exitSharingProgramFallbacks == 0,
         context + ": the program never fell back to unshared exits");
}

void testSharedExitScaling() {
  struct Shape {
    std::size_t blocks = 0;
    std::size_t drops = 0;
    std::size_t returns = 0;
    std::size_t records = 0;
  };
  const auto shapeFor = [](const lang::FrontendResult &frontend) {
    const lang::MirBody *body = functionBody(frontend, "run");
    return body == nullptr ? Shape{}
                           : Shape{.blocks = body->blocks.size(),
                                   .drops = normalDrops(*body),
                                   .returns = returnTerminators(*body),
                                   .records = body->failureRecords.size()};
  };
  std::vector<Shape> shared;
  for (const std::size_t count : {10, 20, 40, 80}) {
    const std::string source = ownersProgram(count, true, true);
    lang::MirLowerer::resetCleanupSharingStatistics();
    const auto frontend = analyze("shared-exit-scaling.gti", source);
    expect(frontend.canGenerateCode(), "the exit scaling fixture lowers");
    if (!frontend.canGenerateCode()) {
      printDiagnostics(frontend);
      return;
    }
    expectNoExitSharingFallback("exit scaling");
    expect(lang::MirLowerer::cleanupSharingStatistics().exitSharingBodies > 0,
           "the exit scaling fixture shares exits");
    const Shape after = shapeFor(frontend);
    const Shape before =
        shapeFor(analyzeUnsharedExits("shared-exit-scaling.gti", source));
    shared.push_back(after);
    std::cout << "shared exits, " << count << " owners: blocks "
              << before.blocks << " -> " << after.blocks << ", normal drops "
              << before.drops << " -> " << after.drops << ", returns "
              << before.returns << " -> " << after.returns
              << ", failure records " << before.records << " -> "
              << after.records << '\n';
    // Each owner is dropped by one rung, entered by every exit that still
    // holds it, and every exit ends in the one shared return.
    expect(after.drops == count,
           std::to_string(count) + " owners: one normal drop per owner, not " +
               std::to_string(after.drops));
    expect(after.returns == 1,
           std::to_string(count) + " owners: the exits share one return");
    expect(before.drops == count * (count + 3) / 2,
           std::to_string(count) +
               " owners: unshared exits drop exits times owners");
  }
  if (shared.size() == 4) {
    expect(shared[3].blocks * 10 <= shared[2].blocks * 22,
           "blocks grow linearly with shared exits (" +
               std::to_string(shared[2].blocks) + " for 40 owners, " +
               std::to_string(shared[3].blocks) + " for 80)");
    expect(shared[3].records * 10 <= shared[2].records * 22,
           "failure records grow linearly with shared exits");
  }
}

void testSharedExitKinds() {
  // The runtime fixture: plain returns, `?` propagations, `unexpected(...)`
  // returns, void returns and a void body's end, across a nested scope.
  const std::filesystem::path path =
      repositoryRoot() / "tests/fixtures/shared_exit_cleanup.gti";
  lang::MirLowerer::resetCleanupSharingStatistics();
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      path, std::nullopt, {repositoryRoot() / "stdlib/prelude.gti"}, {},
      {repositoryRoot() / "stdlib"});
  expect(frontend.canGenerateCode(), "shared_exit_cleanup.gti lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  expectNoExitSharingFallback("shared_exit_cleanup.gti");
  // The same fixture with every exit destroying its own owners.
  lang::MirLowerer::setShareExitCleanup(false);
  const lang::FrontendResult unshared = lang::Frontend().analyze(
      path, std::nullopt, {repositoryRoot() / "stdlib/prelude.gti"}, {},
      {repositoryRoot() / "stdlib"});
  lang::MirLowerer::setShareExitCleanup(true);
  expect(unshared.canGenerateCode(), "shared_exit_cleanup.gti lowers unshared");
  struct Expected {
    const char *name;
    std::size_t exits;
    std::size_t returns;
    std::size_t drops;
    // RT-12 (docs/plans/optimization.md): exits that construct the source
    // result class do not yet share their ladders.
    bool recoveryPending = false;
  };
  // `run`: nine exits over owners a, b, c, d become a success ladder and an
  // error ladder of four rungs each, beside the inner block's own drop of c.
  // `drain`: four exits become one ladder. `probe`: three exits become one
  // rung per value kind.
  for (const Expected &expected :
       {Expected{"run", 9, 2, 9, true}, Expected{"drain", 4, 1, 3},
        Expected{"probe", 3, 2, 2, true}}) {
    const lang::MirBody *body = functionBody(frontend, expected.name);
    const lang::MirBody *separate = unshared.canGenerateCode()
                                        ? functionBody(unshared, expected.name)
                                        : nullptr;
    expect(body != nullptr && separate != nullptr,
           std::string(expected.name) + " has a body in both lowerings");
    if (body == nullptr || separate == nullptr)
      continue;
    expect(lang::verifyMirBody(*body).valid(),
           std::string(expected.name) + " verifies: " + firstError(*body));
    if (expected.recoveryPending) {
      expect(returnTerminators(*separate) == expected.exits,
             std::string(expected.name) +
                 " keeps one return per exit unshared");
      std::cout << expected.name << ": " << expected.exits
                << " exits, sharing pending RT-12 (normal drops "
                << normalDrops(*separate) << " -> " << normalDrops(*body)
                << ", returns " << returnTerminators(*separate) << " -> "
                << returnTerminators(*body) << ")\n";
      continue;
    }
    expect(returnTerminators(*body) == expected.returns,
           std::string(expected.name) + " has " +
               std::to_string(returnTerminators(*body)) +
               " returns instead of " + std::to_string(expected.returns));
    expect(normalDrops(*body) == expected.drops,
           std::string(expected.name) + " has " +
               std::to_string(normalDrops(*body)) +
               " normal drops instead of " + std::to_string(expected.drops));
    expect(returnTerminators(*separate) == expected.exits,
           std::string(expected.name) + " keeps one return per exit unshared");
    expect(normalDrops(*separate) > normalDrops(*body),
           std::string(expected.name) + " drops less when exits are shared");
    std::cout << expected.name << ": " << expected.exits
              << " exits, normal drops " << normalDrops(*separate) << " -> "
              << normalDrops(*body) << ", returns "
              << returnTerminators(*separate) << " -> "
              << returnTerminators(*body) << '\n';
  }
  // RT-12 (docs/plans/optimization.md): `run` carries its success and error
  // values to shared ladders, with rungs entered by several exits, only once
  // exits that construct the source result class share again. Report the
  // current shape instead of requiring it.
  const lang::MirBody *run = functionBody(frontend, "run");
  if (run != nullptr) {
    bool successTail = false;
    bool errorTail = false;
    for (const lang::MirBlock &block : run->blocks) {
      successTail = successTail || returnsCarrier(*run, block, false);
      errorTail = errorTail || returnsCarrier(*run, block, true);
    }
    const std::vector<std::size_t> counts = predecessorCounts(*run);
    std::size_t joinedRungs = 0;
    for (const lang::MirBlock &block : run->blocks) {
      const bool drops = std::any_of(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Drop;
          });
      joinedRungs += drops && counts[block.id] >= 2;
    }
    std::cout << "run: carried success tail " << successTail
              << ", carried error tail " << errorTail << ", joined rungs "
              << joinedRungs << " (pending RT-12)\n";
  }
}

void testExitsThatStayPrivate() {
  // Owning and borrowed results keep their own cleanup, and so does a body
  // with a single exit.
  lang::MirLowerer::resetCleanupSharingStatistics();
  const auto frontend = analyze("shared-exit-private.gti", R"(
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  ~Guard() { int32_t checked = 100 / this.divisor; }
  int32_t get() { return this.divisor; }
};
Guard owning(int32_t p) {
  Guard a = Guard(1);
  Guard r = Guard(2);
  if (p == 1) { return r; }
  Guard b = Guard(3);
  return r;
}
Guard& borrowed(Guard& value, int32_t p) {
  Guard a = Guard(1);
  if (p == 1) { return value; }
  Guard b = Guard(2);
  return value;
}
int32_t single(int32_t p) {
  Guard a = Guard(1);
  Guard b = Guard(2);
  return p + a.get() + b.get();
}
int main() {
  Guard g = Guard(5);
  return owning(1).get() + borrowed(g, 1).get() + single(1) - 11;
}
)");
  expect(frontend.canGenerateCode(), "the private-exit fixture lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  expectNoExitSharingFallback("private exits");
  expect(lang::MirLowerer::cleanupSharingStatistics().exitSharingBodies == 0,
         "owning, borrowed and single exits share no cleanup");
  struct Expected {
    const char *name;
    std::size_t returns;
    std::size_t drops;
  };
  for (const Expected &expected :
       {Expected{"owning", 2, 5}, Expected{"borrowed", 2, 3},
        Expected{"single", 1, 2}}) {
    const lang::MirBody *body = functionBody(frontend, expected.name);
    expect(body != nullptr, std::string(expected.name) + " has a body");
    if (body == nullptr)
      continue;
    expect(returnTerminators(*body) == expected.returns &&
               normalDrops(*body) == expected.drops,
           std::string(expected.name) + " keeps its own exits (" +
               std::to_string(returnTerminators(*body)) + " returns, " +
               std::to_string(normalDrops(*body)) + " drops)");
    for (const lang::MirBlock &block : body->blocks) {
      expect(!returnsCarrier(*body, block, false) &&
                 !returnsCarrier(*body, block, true),
             std::string(expected.name) + " returns no carried value");
    }
  }
}

void testSharedExitVerifierContract() {
  // Three owners with an early return after each: exit k drops owners k..1
  // through rungs R_k -> C_k -> R_{k-1}, where C_k is the continuation of
  // the rung's fallible drop and the deepest rung's continuation returns.
  const auto frontend =
      analyze("shared-exit-contract.gti", ownersProgram(3, true, true));
  expect(frontend.canGenerateCode(), "the exit contract fixture lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const lang::MirBody *found = functionBody(frontend, "run");
  if (found == nullptr) {
    expect(false, "the exit contract fixture has its run body");
    return;
  }
  const lang::MirBody &body = *found;
  expect(lang::verifyMirBody(body).valid(),
         "the exit contract fixture verifies: " + firstError(body));
  const std::vector<std::size_t> counts = predecessorCounts(body);

  // An exit block stores the carrier and jumps to its first rung.
  const auto isExitBlock = [&](const lang::MirBlock &block) {
    return block.terminator.kind == lang::MirTerminatorKind::Goto &&
           std::any_of(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Initialize &&
                                instruction.destination &&
                                isTemporaryPlace(body,
                                                 *instruction.destination);
                       });
  };
  // The rung an exit block enters, its continuation, and the next rung.
  struct Ladder {
    lang::MirBlockId exit = 0;
    lang::MirBlockId rung = 0;
    lang::MirBlockId continuation = 0;
    lang::MirBlockId next = 0;
  };
  std::vector<Ladder> ladders;
  for (const lang::MirBlock &block : body.blocks) {
    if (!isExitBlock(block))
      continue;
    Ladder ladder{.exit = block.id, .rung = block.terminator.target};
    const lang::MirBlock *rung = body.findBlock(ladder.rung);
    if (rung != nullptr &&
        rung->terminator.kind == lang::MirTerminatorKind::Invoke) {
      ladder.continuation = rung->terminator.target;
      const lang::MirBlock *continuation = body.findBlock(ladder.continuation);
      if (continuation != nullptr &&
          continuation->terminator.kind == lang::MirTerminatorKind::Goto) {
        ladder.next = continuation->terminator.target;
      }
    }
    ladders.push_back(ladder);
  }
  expect(ladders.size() == 4, "four exits enter the ladder");
  const auto rungDepth = [&](lang::MirBlockId rung) {
    std::size_t depth = 0;
    lang::MirBlockId cursor = rung;
    while (cursor != 0 && depth < body.blocks.size()) {
      ++depth;
      const lang::MirBlock *block = body.findBlock(cursor);
      const lang::MirBlock *continuation =
          block != nullptr &&
                  block->terminator.kind == lang::MirTerminatorKind::Invoke
              ? body.findBlock(block->terminator.target)
              : nullptr;
      cursor = continuation != nullptr && continuation->terminator.kind ==
                                              lang::MirTerminatorKind::Goto
                   ? continuation->terminator.target
                   : 0;
    }
    return depth;
  };
  const auto ladderOfDepth = [&](std::size_t depth) {
    for (const Ladder &ladder : ladders) {
      if (rungDepth(ladder.rung) == depth)
        return ladder;
    }
    return Ladder{};
  };
  const Ladder deepest = ladderOfDepth(1);
  const Ladder second = ladderOfDepth(2);
  const Ladder third = ladderOfDepth(3);
  expect(deepest.exit != 0 && second.exit != 0 && third.exit != 0,
         "the ladder has rungs one, two and three owners deep");
  expect(counts[deepest.rung] >= 2 && counts[second.rung] >= 2,
         "the shared rungs are entered from several exits");
  if (deepest.exit == 0 || second.exit == 0 || third.exit == 0)
    return;
  {
    // An exit skipping a rung reaches the shared return with an owner the
    // other exits already dropped still live: a drop missing on one
    // entering path.
    auto skipped = body;
    skipped.blocks[second.exit - 1].terminator.target = second.next;
    lang::rebuildMirReachability(skipped);
    expect(!lang::verifyMirBody(skipped).valid(),
           "an exit missing a shared drop on its path is rejected");
  }
  {
    // An exit entering a deeper rung drops an owner never initialized on
    // its path.
    auto deeper = body;
    deeper.blocks[deepest.exit - 1].terminator.target = second.rung;
    lang::rebuildMirReachability(deeper);
    expect(rejects(deeper, "drop conditionality disagrees with path state"),
           "an exit entering a rung that drops an owner it never had is "
           "rejected");
  }
  {
    // The shared return reads the carrier every exit must have written.
    auto uncarried = body;
    auto &instructions = uncarried.blocks[third.exit - 1].instructions;
    const auto initialize = std::find_if(
        instructions.begin(), instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Initialize;
        });
    expect(initialize != instructions.end(), "an exit writes the carrier");
    if (initialize != instructions.end()) {
      instructions.erase(initialize);
      (void)lang::rebuildMirValueUses(uncarried);
      expect(rejects(uncarried, "definitely initialized"),
             "a carrier left unwritten on one exit is rejected");
    }
  }
}

void testSharedLoanEndsVerifierContract() {
  // A borrow held across the exits, live until the body's last return, ends
  // in a shared rung before the drops.
  const auto frontend = analyze("shared-exit-loans.gti", R"(
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  ~Guard() { int32_t checked = 100 / this.divisor; }
  int32_t get() { return this.divisor; }
};
int32_t run(int32_t p) {
  Guard a = Guard(1);
  Guard& view = a;
  if (p == 1) { return 1; }
  Guard b = Guard(2);
  if (p == 2) { return 2; }
  return view.get();
}
int main() { return run(1) - 1; }
)");
  expect(frontend.canGenerateCode(), "the shared loan fixture lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const lang::MirBody *found = functionBody(frontend, "run");
  if (found == nullptr) {
    expect(false, "the shared loan fixture has its run body");
    return;
  }
  const lang::MirBody &body = *found;
  expect(lang::verifyMirBody(body).valid(),
         "the shared loan fixture verifies: " + firstError(body));
  expect(returnTerminators(body) == 1, "the loan fixture shares one return");
  const std::vector<std::size_t> counts = predecessorCounts(body);
  // A rung that only ends the borrow and is entered by several exits: the
  // exits after `b` share it.
  const lang::MirBlockId loanRung = findBlock(body, [&](const auto &block) {
    return counts[block.id] >= 2 && block.instructions.size() == 1 &&
           block.instructions.front().kind ==
               lang::MirInstructionKind::EndBorrow &&
           block.terminator.kind == lang::MirTerminatorKind::Goto;
  });
  expect(loanRung != 0, "the exits share the rung ending the borrow");
  if (loanRung == 0)
    return;
  {
    // Without that end the borrow is live on one path into the shared drop
    // rung and ended on the other: unequal loan state at a join.
    auto unequal = body;
    unequal.blocks[loanRung - 1].instructions.clear();
    (void)lang::rebuildMirValueUses(unequal);
    expect(!lang::verifyMirBody(unequal).valid(),
           "unequal loan state entering a shared rung is rejected");
  }
}

void testFixtureSharesExits() {
  // Long bodies with many `?` exits are the motivating case.
  for (const char *name :
       {"filesystem_operations.gti", "shared_exit_cleanup.gti"}) {
    const std::filesystem::path path =
        repositoryRoot() / "tests/fixtures" / name;
    lang::MirLowerer::resetCleanupSharingStatistics();
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        path, std::nullopt, {repositoryRoot() / "stdlib/prelude.gti"}, {},
        {repositoryRoot() / "stdlib"});
    expect(frontend.canGenerateCode(), std::string(name) + " lowers");
    if (!frontend.canGenerateCode()) {
      printDiagnostics(frontend);
      continue;
    }
    expectNoExitSharingFallback(name);
    expect(lang::MirLowerer::cleanupSharingStatistics().exitSharingBodies > 0,
           std::string(name) + " shares normal-exit cleanup");
  }
}

void testProgramConstantExitsStayPrivate() {
  // A substituted program constant has exactly one MIR materialization, so
  // an exit returning one keeps its own cleanup instead of a shared tail's
  // reload. The body's other exits still share, and nothing falls back.
  lang::MirLowerer::resetCleanupSharingStatistics();
  const auto frontend = analyze("shared-exit-constant.gti", R"(
constexpr int32_t limit = 7;
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  ~Guard() { int32_t checked = 100 / this.divisor; }
};
int32_t clamp(int32_t p) {
  Guard a = Guard(1);
  if (p > limit) { return limit; }
  if (p < 0) { return 0; }
  Guard b = Guard(2);
  if (p == limit) { return limit; }
  return p;
}
int main() { return clamp(3) - 3; }
)");
  expect(frontend.canGenerateCode(),
         "the program-constant exit fixture lowers");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  expectNoExitSharingFallback("program-constant exits");
  expect(lang::MirLowerer::cleanupSharingStatistics().exitSharingBodies > 0,
         "clamp's other exits share cleanup");
  const lang::MirBody *body = functionBody(frontend, "clamp");
  expect(body != nullptr && returnTerminators(*body) == 3,
         "clamp keeps a return per constant exit and one shared return");
}

} // namespace

int main() {
  testLinearCleanupScaling();
  testSharedContinuationContract();
  testSingleFailureEdgeStaysUnshared();
  testMovedShellDropStaysPrivate();
  testFixtureShares();
  testSharedExitScaling();
  testSharedExitKinds();
  testExitsThatStayPrivate();
  testSharedExitVerifierContract();
  testSharedLoanEndsVerifierContract();
  testFixtureSharesExits();
  testProgramConstantExitsStayPrivate();
  if (!passed) {
    return 1;
  }
  std::cout << "shared failure cleanup tests passed\n";
  return 0;
}
