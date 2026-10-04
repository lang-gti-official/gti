#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testContracts() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result =
      lang::Frontend().analyze("panic-contracts.gti", R"(
#include <std/panic>
[[noreturn, track_caller]] void fail(std::string_view reason) {
  std::panic(reason);
}
[[track_caller]] void checked(bool okay) {
  if (!okay) { fail("invalid state"); }
}
int main() { checked(true); return 0; }
)",
                               {library / "prelude.gti"}, {}, {library});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  check(result.canGenerateCode(), "terminal/caller fixture lowers completely");
  if (!result.canGenerateCode())
    return;

  unsigned calls = 0;
  unsigned forwarded = 0;
  unsigned terminals = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.tracksCaller)
          continue;
        ++calls;
        forwarded += instruction.forwardsCallerLocation;
        terminals += instruction.neverReturns;
        check(instruction.callerLocation.has_value() &&
                  result.mir.failureMetadata().callerSiteFor(
                      *instruction.callerLocation) ==
                      instruction.callerLocationSite,
              "tracked call carries its independently bound source anchor");
        for (unsigned mutation = 0; mutation != 6; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &call = *std::find_if(
              changed->body.blocks[block.id - 1].instructions.begin(),
              changed->body.blocks[block.id - 1].instructions.end(),
              [&](const auto &item) { return item.id == instruction.id; });
          switch (mutation) {
          case 0:
            call.tracksCaller = false;
            break;
          case 1:
            call.callerLocation.reset();
            break;
          case 2:
            call.callerLocationSite = 0;
            break;
          case 3:
            ++call.callerLocation->start;
            break;
          case 4:
            call.forwardsCallerLocation = !call.forwardsCallerLocation;
            break;
          case 5:
            call.neverReturns = !call.neverReturns;
            break;
          }
          check(!lang::verifyMirProgram(malformed).valid(),
                "MIR rejects erased, forged or mis-forwarded caller authority");
        }
        if (!instruction.neverReturns)
          continue;
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidate : changed->body.blocks) {
          if (candidate.terminator.kind == lang::MirTerminatorKind::Unreachable)
            candidate.terminator = {.kind = lang::MirTerminatorKind::Return};
        }
        const auto verification = lang::verifyMirProgram(malformed);
        check(!verification.valid() &&
                  std::any_of(verification.errors.begin(),
                              verification.errors.end(),
                              [](const auto &error) {
                                return error.message.find("terminal") !=
                                       std::string::npos;
                              }),
              "terminal calls cannot acquire an ordinary return continuation");
      }
    }
  }
  check(calls >= 4 && forwarded + 1 == calls && terminals >= 3,
        "ordinary caller anchors once and tracked wrappers forward it");

  unsigned hirCalls = 0;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (!value.tracksCaller)
        continue;
      ++hirCalls;
      for (unsigned mutation = 0; mutation != 3; ++mutation) {
        auto malformed = result.hir;
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &call = *std::find_if(
            changed->body.values.begin(), changed->body.values.end(),
            [&](const auto &item) { return item.id == value.id; });
        if (mutation == 0)
          call.tracksCaller = false;
        if (mutation == 1)
          call.neverReturns = !call.neverReturns;
        if (mutation == 2)
          ++call.callerLocation->line;
        check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
              "HIR independently retains declaration and source contracts");
      }
    }
  }
  check(hirCalls == calls, "HIR/MIR preserve the same tracked-call inventory");
  const auto lowered = gti_test::lowerProgram(result, result.mir, result.mir);
  const auto printed = lang::LoweredProgramPrinter().print(lowered);
  check(printed.find(";never-returns=1;tracks-caller=1") != std::string::npos,
        "deterministic lowered serialization includes callable contracts");
  unsigned loweredContracts = 0;
  for (const auto &declaration : lowered.declarations()) {
    const auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (function == nullptr || !function->tracksCaller)
      continue;
    ++loweredContracts;
    for (unsigned mutation = 0; mutation != 2; ++mutation) {
      auto malformed = lowered;
      auto *changed = const_cast<lang::LoweredFunctionDeclaration *>(
          malformed.findFunctionDeclaration(function->id));
      if (mutation == 0)
        changed->tracksCaller = false;
      if (mutation == 1)
        changed->neverReturns = !changed->neverReturns;
      check(!lang::verifyLoweredProgram(malformed).empty(),
            "sealed backend input rejects mismatched declaration contracts");
    }
  }
  check(loweredContracts >= 4,
        "lowered declarations retain private and source tracked contracts");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto artifact =
          gti_test::emitCpp(result, standard, lang::TargetInfo::host(), level);
      check(
          !artifact.contents.empty(),
          "verified panic contracts survive optimization and backend lowering");
    }
  }
}
void testAbandonedCallInputs() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result =
      lang::Frontend().analyze("terminal-arguments.gti", R"(
#include <std/panic>
void observe(int value) {}
struct Guard {
  int value;
  Guard(int input) : value(input) {}
  ~Guard() { observe(value); }
};
[[noreturn]] int terminal_argument() { std::panic("argument failed"); }
int later() { observe(999); return 3; }
void consume(Guard first, int second, int third) { observe(888); }
void interrupted() { consume(Guard(85), terminal_argument(), later()); }
void branched(bool condition) {
  consume(Guard(86), condition ? terminal_argument() : terminal_argument(), later());
}
void moved() {
  Guard owner{88};
  consume(std::move(owner), terminal_argument(), later());
}
struct Plain { int value = 1; };
void consume_copy(Plain first, int second) {}
void copied() { Plain value{}; consume_copy(value, terminal_argument()); }
void consume_borrow(int& first, int second) {}
void borrowed() { int value = 1; consume_borrow(value, terminal_argument()); }
void continuing() { consume(Guard(87), 2, 3); std::panic("later statement"); }
int main() {
  interrupted(); branched(false); moved(); copied(); borrowed(); continuing();
  return 0;
}
)",
                               {library / "prelude.gti"}, {}, {library});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  check(result.canGenerateCode(),
        "terminal later arguments retain valid prepared-owner MIR");
  if (!result.canGenerateCode())
    return;
  unsigned abandoned = 0;
  unsigned continuing = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.preparedParameterDrop || !stage.result)
          continue;
        if (lang::mirCallInputAbandonedByTerminal(function.body, stage)) {
          ++abandoned;
          auto mutated = function.body;
          bool removedWitness = false;
          for (auto &candidate : mutated.blocks)
            for (auto &instruction : candidate.instructions)
              if (instruction.neverReturns && !removedWitness) {
                instruction.neverReturns = false;
                removedWitness = true;
              }
          const auto *changed = mutated.findBlock(block.id);
          const auto found = std::find_if(changed->instructions.begin(),
                                          changed->instructions.end(),
                                          [&](const auto &instruction) {
                                            return instruction.id == stage.id;
                                          });
          check(!lang::mirCallInputAbandonedByTerminal(mutated, *found),
                "every branch needs a terminal witness, not bare unreachable");
          auto lostCleanup = result.mir;
          auto *changedFunction = const_cast<lang::MirFunctionInstance *>(
              lostCleanup.findFunctionInstance(function.id));
          bool removedCleanup = false;
          for (auto &candidate : changedFunction->body.blocks) {
            std::erase_if(candidate.instructions, [&](const auto &instruction) {
              const bool exactDrop =
                  instruction.kind == lang::MirInstructionKind::Drop &&
                  std::any_of(
                      instruction.lifecycle.begin(),
                      instruction.lifecycle.end(), [&](const auto &event) {
                        return event.source == *stage.preparedParameterDrop &&
                               event.failureCleanup;
                      });
              removedCleanup = removedCleanup || exactDrop;
              return exactDrop;
            });
          }
          check(removedCleanup &&
                    lang::rebuildMirValueUses(changedFunction->body) &&
                    !lang::verifyMirProgram(lostCleanup).valid(),
                "terminal abandonment never permits omitted prepared cleanup");
          continue;
        }
        const auto &uses = function.body.usesOf(*stage.result);
        if (uses.size() != 1)
          continue;
        ++continuing;
        auto malformed = result.mir;
        auto *changedFunction = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changedBody = changedFunction->body;
        for (auto &candidate : changedBody.blocks) {
          std::erase_if(candidate.instructions, [&](const auto &instruction) {
            return instruction.id == uses.front().instruction;
          });
        }
        check(lang::rebuildMirValueUses(changedBody),
              "mutation rebuilds the independent executable-use index");
        const auto *changedBlock = changedBody.findBlock(block.id);
        const auto found = std::find_if(changedBlock->instructions.begin(),
                                        changedBlock->instructions.end(),
                                        [&](const auto &instruction) {
                                          return instruction.id == stage.id;
                                        });
        check(found != changedBlock->instructions.end() &&
                  !lang::mirCallInputAbandonedByTerminal(changedBody, *found),
              "a later statement's panic cannot excuse a dropped normal call");
        check(
            !lang::verifyMirProgram(malformed).valid(),
            "verification rejects a lost use from a normally continuing call");
      }
    }
  }
  check(abandoned == 4 && continuing >= 1,
        "fresh, moved, copied and branching arguments have exact abandoned "
        "stages");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      try {
        const auto artifact = gti_test::emitCpp(
            result, standard, lang::TargetInfo::host(), level);
        check(
            !artifact.contents.empty(),
            "abandoned prepared arguments have a verified C++ representation");
      } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        check(false, "abandoned prepared arguments have a C++ representation");
      }
    }
  }
}
void testTerminalConstructorPublication() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result =
      lang::Frontend().analyze("terminal-constructor.gti", R"(
#include <std/panic>
struct Guard {
  int value;
  Guard(int input) : value(input) {}
  ~Guard() { if (value == 2) { std::panic("secondary"); } }
};
struct Partial {
  Guard field;
  Partial() : field(Guard(1)) {
    Guard local{2};
    std::panic("primary");
  }
};
int main() { Partial value{}; return 0; }
)",
                               {library / "prelude.gti"}, {}, {library});
  check(result.canGenerateCode(),
        "terminal constructors retain initialized-field rollback authority");
  if (!result.canGenerateCode())
    return;
  bool exercised = false;
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto field = std::find_if(
        constructor.body.dropObligations.begin(),
        constructor.body.dropObligations.end(), [](const auto &drop) {
          return drop.kind == lang::MirDropObligationKind::ConstructionRollback;
        });
    if (field == constructor.body.dropObligations.end())
      continue;
    for (const auto &block : constructor.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Unreachable)
        continue;
      exercised = true;
      for (bool falsifyReachability : {false, true}) {
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        auto &continuation = changed->body.blocks[block.id - 1];
        continuation.terminator = {.kind = lang::MirTerminatorKind::Return};
        if (falsifyReachability)
          continuation.reachable = false;
        check(!lang::verifyMirProgram(malformed).valid(),
              "normal completion without field publication is rejected, "
              "including a forged unreachable flag");
      }
    }
  }
  check(exercised, "terminal constructor publication mutation is exercised");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      try {
        const auto artifact = gti_test::emitCpp(
            result, standard, lang::TargetInfo::host(), level);
        check(!artifact.contents.empty(),
              "a terminal constructor needs no invented field publication");
      } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        check(false, "terminal constructor has a verified C++ representation");
      }
    }
  }
}
} // namespace

int main() {
  testContracts();
  testAbandonedCallInputs();
  testTerminalConstructorPublication();
  return failures == 0 ? 0 : 1;
}
