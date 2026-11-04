#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string_view>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testIntrinsicDeclarationContract() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("gti-intrinsic-contract-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{temporary};
  std::filesystem::copy(library, temporary,
                        std::filesystem::copy_options::recursive);
  const auto prelude = temporary / "prelude.gti";
  std::ifstream stream(prelude);
  std::string text((std::istreambuf_iterator<char>(stream)), {});
  const std::string declaration = "  void require_key(bool found);";
  const auto position = text.find(declaration);
  check(position != std::string::npos,
        "trusted require_key declaration exists");
  if (position == std::string::npos)
    return;
  text.replace(position, declaration.size(),
               "  [[track_caller]] void require_key(bool found);");
  stream.close();
  {
    std::ofstream output(prelude);
    output << text;
  }
  const auto capabilities = temporary / "std/detail/memory_capabilities.gti";
  std::ifstream capabilityInput(capabilities);
  std::string capabilityText((std::istreambuf_iterator<char>(capabilityInput)),
                             {});
  capabilityInput.close();
  for (const std::string declaration :
       {"export uint64_t object_size<T>();",
        "export uint64_t object_alignment<T>();",
        "export void object_construct<T, Args...>(T* address, Args... "
        "args);"}) {
    const auto position = capabilityText.find(declaration);
    check(position != std::string::npos,
          "trusted memory intrinsic declaration exists");
    if (position != std::string::npos)
      capabilityText.insert(position + 7, "[[track_caller]] ");
  }
  {
    std::ofstream output(capabilities);
    output << capabilityText;
  }
  const auto analyze = [&](std::string_view name) {
    return lang::Frontend().analyze(
        std::string(name),
        "import std.map; int main() { mut std::map<int, int> values{}; "
        "int key = 3; values.insert(key, 7); return values.at(key); }",
        {prelude}, {}, {temporary});
  };
  const auto result = analyze("intrinsic-contract.gti");
  const auto foreign = analyze("foreign-intrinsic-contract.gti");
  check(result.canGenerateCode() && foreign.canGenerateCode(),
        "a selected trusted intrinsic retains explicit track_caller");
  if (!result.canGenerateCode() || !foreign.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::optional<lang::FunctionControlContract> foreignContract;
  for (const auto &function : foreign.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::RequireKey)
          foreignContract = instruction.intrinsicCallContract;
  check(foreignContract.has_value(), "foreign intrinsic witness exists");
  const auto lowered = gti_test::lowerProgram(result, result.mir, result.mir);
  check(lang::verifyLoweredProgram(lowered).empty(),
        "selected intrinsic declarations survive sealed inventory collection");
  const std::set<lang::IntrinsicKind> expectedIntrinsics{
      lang::IntrinsicKind::RequireKey, lang::IntrinsicKind::ObjectSize,
      lang::IntrinsicKind::ObjectAlignment,
      lang::IntrinsicKind::ObjectConstruct};
  std::set<lang::IntrinsicKind> mirIntrinsics, hirIntrinsics;
  unsigned mirCalls = 0, mirMutations = 0, hirCalls = 0, hirMutations = 0;
  const auto annotated = [](lang::IntrinsicKind intrinsic) {
    return intrinsic == lang::IntrinsicKind::RequireKey ||
           intrinsic == lang::IntrinsicKind::ObjectSize ||
           intrinsic == lang::IntrinsicKind::ObjectAlignment ||
           intrinsic == lang::IntrinsicKind::ObjectConstruct;
  };
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsicCallContract) {
          const auto *declaration = lowered.findFunctionDeclaration(
              instruction.intrinsicCallContract->declaration());
          check(declaration && declaration->controlContract ==
                                   instruction.intrinsicCallContract,
                "every selected intrinsic has its exact semantic declaration");
        }
        if (!annotated(instruction.intrinsic))
          continue;
        mirIntrinsics.insert(instruction.intrinsic);
        ++mirCalls;
        check(instruction.tracksCaller && instruction.intrinsicCallContract &&
                  instruction.intrinsicCallContract->declaration() != 0 &&
                  instruction.intrinsicCallContract->snapshot() ==
                      result.mir.semanticSnapshot(),
              "MIR preserves exact declaration and semantic snapshot identity");
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
          ++mirMutations;
          auto malformed = result.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &call = *std::find_if(
              owner->body.blocks[block.id - 1].instructions.begin(),
              owner->body.blocks[block.id - 1].instructions.end(),
              [&](const auto &candidate) {
                return candidate.id == instruction.id;
              });
          if (mutation == 0)
            call.intrinsicCallContract.reset();
          if (mutation == 1)
            call.tracksCaller = false;
          if (mutation == 2)
            call.intrinsic = lang::IntrinsicKind::RequireContainerArgument;
          if (mutation == 3)
            call.intrinsicCallContract = foreignContract;
          check(!lang::verifyMirProgram(malformed).valid(),
                "MIR rejects missing, mismatched and foreign intrinsic "
                "contracts");
        }
      }
    }
  }
  check(mirIntrinsics == expectedIntrinsics && mirCalls != 0 &&
            mirMutations == mirCalls * 4,
        "every annotated intrinsic family and all four MIR mutations are "
        "exercised");
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      if (!annotated(value.intrinsic))
        continue;
      hirIntrinsics.insert(value.intrinsic);
      ++hirCalls;
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        ++hirMutations;
        auto malformed = result.hir;
        auto *owner = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &call = *std::find_if(
            owner->body.values.begin(), owner->body.values.end(),
            [&](const auto &candidate) { return candidate.id == value.id; });
        if (mutation == 0)
          call.intrinsicCallContractProof.reset();
        if (mutation == 1)
          call.source = nullptr;
        if (mutation == 2)
          call.intrinsic = lang::IntrinsicKind::RequireContainerArgument;
        if (mutation == 3)
          call.tracksCaller = false;
        check(
            !lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "HIR rejects erased or transplanted selected intrinsic contracts");
      }
    }
  }
  check(hirIntrinsics == expectedIntrinsics && hirCalls == mirCalls &&
            hirMutations == hirCalls * 4,
        "every annotated intrinsic family and all four HIR mutations are "
        "exercised");
}
void testContracts() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto result =
      lang::Frontend().analyze("panic-contracts.gti", R"(
import std.panic;
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

  unsigned allCalls = 0;
  unsigned calls = 0;
  unsigned forwarded = 0;
  unsigned terminals = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.tracksCaller)
          continue;
        ++allCalls;
        // Count the tracked forwarding chain and this fixture's entry, not
        // unrelated ordinary anchors in additional standard-library helpers.
        // All calls below still receive the exact-location/mutation checks.
        if (function.tracksCaller ||
            function.entryKind != lang::ProgramEntryKind::None) {
          ++calls;
          forwarded += instruction.forwardsCallerLocation;
          terminals += instruction.neverReturns;
        } else {
          check(!instruction.forwardsCallerLocation,
                "ordinary library callers anchor rather than forward");
        }
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
      for (unsigned mutation = 0; mutation != 6; ++mutation) {
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
        if (mutation == 3)
          call.callerLocationProof.reset();
        if (mutation == 4)
          call.source = nullptr;
        if (mutation == 5) {
          const lang::HirValue *other = nullptr;
          for (const auto &candidate : result.hir.functionInstances())
            for (const auto &item : candidate.body.values)
              if (item.callerLocationProof && item.source != value.source)
                other = &item;
          check(other != nullptr, "another tracked source occurrence exists");
          if (other != nullptr)
            call.callerLocationProof = other->callerLocationProof;
        }
        check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
              "HIR independently retains declaration and source contracts");
      }
    }
  }
  check(hirCalls == allCalls,
        "HIR/MIR preserve the same tracked-call inventory");
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
import std.panic;
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
struct Plain { int value = 1;  public: Plain() = default; };
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
import std.panic;
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
  testIntrinsicDeclarationContract();
  testAbandonedCallInputs();
  testTerminalConstructorPublication();
  return failures == 0 ? 0 : 1;
}
