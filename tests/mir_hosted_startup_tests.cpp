#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool passed = true;

void expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  passed = false;
  std::cerr << "FAIL: " << message << '\n';
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void printVerification(const lang::MirVerificationResult &verification) {
  for (const lang::MirVerificationError &error : verification.errors) {
    std::cerr << "  body=" << static_cast<int>(error.bodyKind)
              << " owner=" << error.owner << " block=" << error.block
              << " instruction=" << error.instruction << ": " << error.message
              << '\n';
  }
}

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

lang::FrontendResult analyzeOwned() {
  return lang::Frontend().analyze("/tmp/gti-mir-hosted-startup/owned.gti", R"(
#include <std/string>
#include <std/vector>
using arguments_type = std::vector<std::string>;

mut int32_t startup_value = 1 + 2;

int main(mut int argc, mut arguments_type argv) {
  return argc + startup_value;
}
)",
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

lang::MirHostedStartupPlan &hostedPlan(lang::MirProgram &program) {
  return *const_cast<std::optional<lang::MirHostedStartupPlan> &>(
      program.hostedStartupPlan());
}

lang::MirBody *hostedBody(lang::MirProgram &program) {
  const auto &plan = program.hostedStartupPlan();
  return !plan ? nullptr
               : lang::findMirBody(program,
                                   {.kind = lang::MirBodyKind::HostedStartup,
                                    .owner = plan->entry});
}

lang::MirHostedStartupOperation *
operation(lang::MirProgram &program, lang::MirHostedStartupOperationKind kind) {
  auto &operations = hostedPlan(program).operations;
  const auto found =
      std::find_if(operations.begin(), operations.end(),
                   [kind](const lang::MirHostedStartupOperation &candidate) {
                     return candidate.kind == kind;
                   });
  return found == operations.end() ? nullptr : &*found;
}

lang::MirInstruction *instruction(lang::MirProgram &program,
                                  lang::MirHostedStartupOperationKind kind) {
  lang::MirBody *body = hostedBody(program);
  lang::MirHostedStartupOperation *row = operation(program, kind);
  if (body == nullptr || row == nullptr || row->instruction == 0) {
    return nullptr;
  }
  lang::MirBlock *block = row->block == 0 || row->block > body->blocks.size()
                              ? nullptr
                              : &body->blocks[row->block - 1];
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const lang::MirInstruction &candidate) {
                     return candidate.id == row->instruction;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

bool hasError(const lang::MirVerificationResult &verification,
              std::string_view text) {
  return std::any_of(verification.errors.begin(), verification.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(text) != std::string::npos;
                     });
}

void expectRejected(const lang::MirProgram &program, std::string_view text,
                    std::string_view message) {
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(program);
  if (verification.valid() || !hasError(verification, text)) {
    printVerification(verification);
    expect(false, message);
  }
}

void expectHostedBodyValid(const lang::MirProgram &program,
                           std::string_view message) {
  const lang::MirBody *body = program.hostedStartup();
  const auto &plan = program.hostedStartupPlan();
  const lang::MirVerificationResult verification =
      body == nullptr || !plan
          ? lang::MirVerificationResult{{lang::MirVerificationError{
                .message = "missing hosted body"}}}
          : lang::verifyMirBody(*body, plan->entry);
  if (!verification.valid()) {
    printVerification(verification);
    expect(false, message);
  }
}

std::size_t countOperation(const lang::MirHostedStartupPlan &plan,
                           lang::MirHostedStartupOperationKind kind) {
  return static_cast<std::size_t>(
      std::count_if(plan.operations.begin(), plan.operations.end(),
                    [kind](const lang::MirHostedStartupOperation &operation) {
                      return operation.kind == kind;
                    }));
}

void testPresenceAndNoArgumentSchedules() {
  const lang::FrontendResult noEntry =
      lang::Frontend().analyze("no-entry.gti", "int helper() { return 1; }\n");
  expect(noEntry.canGenerateCode() &&
             lang::verifyMirProgram(noEntry.mir).valid() &&
             !noEntry.mir.hostedStartupPlan() &&
             noEntry.mir.hostedStartup() == nullptr,
         "a program without an entry should have no hosted-startup MIR");

  const lang::FrontendResult noArguments =
      lang::Frontend().analyze("noarg.gti", "int main() { return 0; }\n");
  if (!noArguments.canGenerateCode()) {
    printDiagnostics(noArguments);
  }
  const auto &noargPlan = noArguments.mir.hostedStartupPlan();
  expect(
      noArguments.canGenerateCode() &&
          lang::verifyMirProgram(noArguments.mir).valid() && noargPlan &&
          noargPlan->kind == lang::ProgramEntryKind::NoArguments &&
          noArguments.mir.hostedStartup() != nullptr &&
          noArguments.mir.hostedStartup()->blocks.size() == 1 &&
          countOperation(
              *noargPlan,
              lang::MirHostedStartupOperationKind::CallProgramInitialization) ==
              0 &&
          countOperation(*noargPlan,
                         lang::MirHostedStartupOperationKind::CallEntry) == 1 &&
          countOperation(*noargPlan,
                         lang::MirHostedStartupOperationKind::ReturnEntry) == 1,
      "an in-memory no-argument entry should have one exact generated call "
      "and return schedule");

  const lang::FrontendResult dataOnly = lang::Frontend().analyze(
      "data-only.gti",
      "constexpr int32_t seed = 1; int main() { return seed; }\n");
  const auto &dataPlan = dataOnly.mir.hostedStartupPlan();
  expect(
      dataOnly.canGenerateCode() && dataPlan &&
          countOperation(
              *dataPlan,
              lang::MirHostedStartupOperationKind::CallProgramInitialization) ==
              0,
      "a DataOnly Module plan must not execute abstract data stages from "
      "HostedStartup");

  const lang::FrontendResult dynamic = lang::Frontend().analyze(
      "dynamic.gti", "mut int32_t base = 1; mut int32_t value = base; "
                     "int main() { return value; }\n");
  const auto &dynamicPlan = dynamic.mir.hostedStartupPlan();
  expect(
      dynamic.canGenerateCode() && dynamicPlan &&
          countOperation(
              *dynamicPlan,
              lang::MirHostedStartupOperationKind::CallProgramInitialization) ==
              1,
      "a dynamic Module plan must have one exact CallBody startup stage");
  const std::vector<lang::MirBodyAddress> addresses =
      lang::enumerateMirBodyAddresses(dynamic.mir);
  expect(!addresses.empty() &&
             addresses.back() ==
                 lang::MirBodyAddress{.kind = lang::MirBodyKind::HostedStartup,
                                      .owner =
                                          dynamicPlan ? dynamicPlan->entry : 0},
         "HostedStartup/<entry> should be the last deterministic MIR body");
  expect(lang::verifyMirOptimizationCoherence(dynamic.mir, dynamic.mir).valid(),
         "identity optimization should retain hosted-startup authority");
}

void testOwnedScheduleAndPrinter() {
  const lang::FrontendResult owned = analyzeOwned();
  if (!owned.canGenerateCode()) {
    printDiagnostics(owned);
  }
  const auto &plan = owned.mir.hostedStartupPlan();
  const lang::MirBody *body = owned.mir.hostedStartup();
  expect(owned.canGenerateCode() && plan && body != nullptr &&
             lang::verifyMirProgram(owned.mir).valid(),
         "owned arguments should lower to verified structural HostedStartup "
         "authority");
  if (!plan || body == nullptr) {
    return;
  }
  expect(!body->blocks.empty() && body->entry == 1 &&
             std::all_of(body->blocks.begin(), body->blocks.end(),
                         [&](const lang::MirBlock &block) {
                           return block.id >= 1 &&
                                  block.id <= body->blocks.size() &&
                                  block.reachable;
                         }),
         "owned startup should retain dense reachable blocks without dangling "
         "block-reference corruption");
  const auto findStage = [&](lang::MirHostedStartupOperationKind kind)
      -> const lang::MirHostedStartupOperation * {
    for (const lang::MirHostedStartupOperation &row : plan->operations) {
      if (row.kind == kind) {
        return &row;
      }
    }
    return nullptr;
  };
  const lang::MirHostedStartupOperation *validate =
      findStage(lang::MirHostedStartupOperationKind::ValidateArgumentCount);
  const lang::MirHostedStartupOperation *convert =
      findStage(lang::MirHostedStartupOperationKind::ConvertArgumentCount);
  const std::size_t detectStages = static_cast<std::size_t>(
      std::count_if(plan->operations.begin(), plan->operations.end(),
                    [](const lang::MirHostedStartupOperation &row) {
                      return row.failureBehavior ==
                             lang::MirHostedStartupFailureBehavior::Detect;
                    }));
  expect(validate != nullptr && convert != nullptr && detectStages == 2 &&
             validate->id < convert->id &&
             validate->failureBehavior ==
                 lang::MirHostedStartupFailureBehavior::Detect &&
             convert->failureBehavior ==
                 lang::MirHostedStartupFailureBehavior::Detect,
         "the two hosted-local detectors should be the only ordered Detect "
         "stages");
  const lang::MirInstruction *validateInstruction =
      validate == nullptr ? nullptr : [&]() -> const lang::MirInstruction * {
    const lang::MirBlock *block = body->findBlock(validate->block);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &candidate) {
                       return candidate.id == validate->instruction;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  }();
  const lang::MirInstruction *convertInstruction =
      convert == nullptr ? nullptr : [&]() -> const lang::MirInstruction * {
    const lang::MirBlock *block = body->findBlock(convert->block);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &candidate) {
                       return candidate.id == convert->instruction;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  }();
  const lang::FailureSiteId validateSite =
      validateInstruction == nullptr ||
              validateInstruction->localFailureSites.empty()
          ? 0
          : validateInstruction->localFailureSites.front();
  expect(validateInstruction != nullptr && convertInstruction != nullptr &&
             validateInstruction->definedFailure.localOrigins.size() == 1 &&
             convertInstruction->definedFailure.localOrigins.size() == 1 &&
             validateSite != 0 &&
             convertInstruction->localFailureSites ==
                 std::vector<lang::FailureSiteId>{validateSite} &&
             owned.failureMetadata.findSite(validateSite) != nullptr &&
             owned.failureMetadata.findSite(validateSite)->outcomes.size() == 2,
         "owned startup should retain the exact two-origin shared failure "
         "site");

  const auto vector = std::find_if(
      plan->operations.begin(), plan->operations.end(),
      [](const lang::MirHostedStartupOperation &row) {
        return row.kind ==
               lang::MirHostedStartupOperationKind::ConstructArgumentVector;
      });
  const auto string = std::find_if(
      plan->operations.begin(), plan->operations.end(),
      [](const lang::MirHostedStartupOperation &row) {
        return row.kind ==
               lang::MirHostedStartupOperationKind::ConstructArgumentString;
      });
  const auto stringMove = std::find_if(
      plan->operations.begin(), plan->operations.end(),
      [](const lang::MirHostedStartupOperation &row) {
        return row.kind ==
               lang::MirHostedStartupOperationKind::PrepareAppendArgumentMove;
      });
  const auto vectorMove = std::find_if(
      plan->operations.begin(), plan->operations.end(),
      [](const lang::MirHostedStartupOperation &row) {
        return row.kind ==
               lang::MirHostedStartupOperationKind::PrepareEntryArgumentsMove;
      });
  expect(vector != plan->operations.end() && string != plan->operations.end() &&
             stringMove != plan->operations.end() &&
             vectorMove != plan->operations.end() &&
             vector->dropObligation != 0 && string->dropObligation != 0 &&
             stringMove->dropObligation != 0 && vectorMove->dropObligation != 0,
         "generated vector/string construction and move stages should close "
         "over their exact drop obligations");

  const auto countTerminators = [&](lang::MirTerminatorKind kind) {
    return static_cast<std::size_t>(
        std::count_if(body->blocks.begin(), body->blocks.end(),
                      [&](const lang::MirBlock &block) {
                        return block.terminator.kind == kind;
                      }));
  };
  expect(
      lang::supportsMirFailureControlFlow(lang::MirBodyKind::HostedStartup) &&
          !body->failureRecords.empty() &&
          countTerminators(lang::MirTerminatorKind::Invoke) ==
              body->failureRecords.size() &&
          countTerminators(lang::MirTerminatorKind::ContainFailure) ==
              body->failureRecords.size(),
      "every failure-capable startup stage should route one contained "
      "Stage-E failure record");

  const auto readView = std::find_if(
      plan->operations.begin(), plan->operations.end(),
      [](const lang::MirHostedStartupOperation &row) {
        return row.kind ==
               lang::MirHostedStartupOperationKind::ReadArgumentView;
      });
  const lang::MirInstruction *readInstruction =
      readView == plan->operations.end()
          ? nullptr
          : [&]() -> const lang::MirInstruction * {
    const lang::MirBlock *block = body->findBlock(readView->block);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &candidate) {
                       return candidate.id == readView->instruction;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  }();
  const lang::MirEffectTraits readEffects =
      readInstruction == nullptr ? lang::MirEffectTraits{}
                                 : lang::effects(*readInstruction);
  expect(readInstruction != nullptr &&
             readInstruction->operation == lang::MirOperation::None &&
             readEffects.readsUnknownMemory && readEffects.invokesRuntime &&
             !readEffects.speculatable && !readEffects.removableWhenUnused &&
             !readEffects.reorderable,
         "sealed native argv access should retain conservative observable "
         "effects");

  const std::string dump = lang::MirPrinter().print(owned.mir);
  expect(dump.starts_with("mir-v25 valid=") &&
             dump.find("hosted-startup kind=") != std::string::npos &&
             dump.find("hosted-operation @") != std::string::npos &&
             dump.find("hosted-startup-body") != std::string::npos &&
             dump.find("generated-value=%") != std::string::npos &&
             dump.find("body-target=0:0") != std::string::npos,
         "MIR v24 should serialize the complete hosted plan, body, and "
         "generated entity provenance");
}

void testProvenanceEntityAndCfgMutations() {
  const lang::FrontendResult owned = analyzeOwned();
  expect(owned.canGenerateCode(),
         "owned mutation fixture should compile before corruption");
  if (!owned.canGenerateCode()) {
    return;
  }

  lang::MirProgram sourceTagged = owned.mir;
  lang::MirBody *module = lang::findMirBody(
      sourceTagged, {.kind = lang::MirBodyKind::Module, .owner = 0});
  if (module != nullptr && !module->blocks.empty() &&
      !module->blocks.front().instructions.empty()) {
    module->blocks.front().instructions.front().hostedStartupOperation = 1;
    expect(!lang::verifyMirBody(*module, 0).valid(),
           "source bodies should reject generated hosted provenance");
  }

  lang::MirProgram sourceProvenance = owned.mir;
  lang::MirInstruction *read = instruction(
      sourceProvenance, lang::MirHostedStartupOperationKind::ReadArgumentView);
  if (read != nullptr) {
    read->hirValue = 1;
  }
  expectRejected(sourceProvenance, "generated shape or provenance",
                 "HostedStartup should reject forged source HIR provenance");

  lang::MirProgram clearedPlace = owned.mir;
  lang::MirBody *placeBody = hostedBody(clearedPlace);
  if (placeBody != nullptr && !placeBody->places.empty()) {
    placeBody->places.front().hostedStartupOperation = 0;
  }
  expectRejected(clearedPlace, "generated provenance",
                 "HostedStartup should reject a cleared generated place tag");

  lang::MirProgram orphanPlace = owned.mir;
  lang::MirBody *orphanBody = hostedBody(orphanPlace);
  if (orphanBody != nullptr) {
    lang::MirPlace extra;
    extra.id = orphanBody->places.size() + 1;
    extra.hostedStartupOperation = 1;
    extra.root = lang::MirPlaceRootKind::Temporary;
    extra.temporary = 99;
    extra.type = lang::SemanticType::Int32;
    extra.access = lang::AccessMode::Mutable;
    extra.traits = lang::semanticTraits(extra.type);
    orphanBody->places.push_back(std::move(extra));
  }
  expectHostedBodyValid(orphanPlace,
                        "orphan-place mutation should remain body-valid");
  expectRejected(orphanPlace, "orphaned or multiply owned",
                 "the plan should reject an unowned generated place");

  lang::MirProgram rowKind = owned.mir;
  hostedPlan(rowKind).operations.front().kind =
      lang::MirHostedStartupOperationKind::ReadArgumentView;
  expectHostedBodyValid(rowKind,
                        "operation-kind drift should remain body-valid");
  expectRejected(rowKind, "dense operation sequence",
                 "the plan should reject operation-kind drift");

  lang::MirProgram staleGoto = owned.mir;
  lang::MirBody *gotoBody = hostedBody(staleGoto);
  lang::MirHostedStartupOperation *enter = operation(
      staleGoto, lang::MirHostedStartupOperationKind::EnterArgumentLoop);
  if (gotoBody != nullptr && enter != nullptr) {
    gotoBody->blocks[enter->block - 1].terminator.elseTarget = 4;
  }
  expectHostedBodyValid(staleGoto,
                        "stale Goto payload should remain structurally valid");
  expectRejected(staleGoto, "terminator is not its exact canonical stage",
                 "the exact plan should reject stale Goto union payload");

  lang::MirProgram swappedBranch = owned.mir;
  lang::MirBody *branchBody = hostedBody(swappedBranch);
  lang::MirHostedStartupOperation *branch = operation(
      swappedBranch, lang::MirHostedStartupOperationKind::BranchArgumentLoop);
  if (branchBody != nullptr && branch != nullptr) {
    std::swap(branchBody->blocks[branch->block - 1].terminator.target,
              branchBody->blocks[branch->block - 1].terminator.elseTarget);
    lang::rebuildMirReachability(*branchBody);
  }
  expectHostedBodyValid(swappedBranch,
                        "swapped reachable branch should remain body-valid");
  expectRejected(swappedBranch, "terminator is not its exact canonical stage",
                 "the exact schedule should reject swapped loop edges");

  lang::MirProgram wrongEntry = owned.mir;
  lang::MirBody *entryBody = hostedBody(wrongEntry);
  if (entryBody != nullptr) {
    entryBody->entry = 2;
    lang::rebuildMirReachability(*entryBody);
  }
  expectRejected(wrongEntry, "reachability inventory",
                 "the hosted body should reject an orphaned generated entry "
                 "prefix");

  lang::MirProgram missingPlan = owned.mir;
  const_cast<std::optional<lang::MirHostedStartupPlan> &>(
      missingPlan.hostedStartupPlan())
      .reset();
  expectRejected(missingPlan, "missing hosted-startup MIR authority",
                 "the body inventory should reject a missing hosted plan");
}

void testFailureTargetAndOwnershipMutations() {
  const lang::FrontendResult owned = analyzeOwned();
  const lang::FrontendResult noarg =
      lang::Frontend().analyze("summary.gti", "int main() { return 0; }\n");
  expect(owned.canGenerateCode() && noarg.canGenerateCode(),
         "failure/target mutation fixtures should compile before corruption");
  if (!owned.canGenerateCode() || !noarg.canGenerateCode()) {
    return;
  }

  lang::MirProgram badAnchor = owned.mir;
  hostedPlan(badAnchor).sourceAnchor.end =
      hostedPlan(badAnchor).sourceAnchor.start;
  expectRejected(badAnchor, "source anchor",
                 "HostedStartup should reject an empty source anchor");

  lang::MirProgram hostedArguments = owned.mir;
  lang::MirInstruction *validate =
      instruction(hostedArguments,
                  lang::MirHostedStartupOperationKind::ValidateArgumentCount);
  if (validate != nullptr && !validate->definedFailure.localOrigins.empty()) {
    validate->definedFailure.localOrigins.front().outcomes = {
        {.code = lang::DefinedFailureCode::AllocationFailure,
         .detail = lang::DefinedFailureDetail::HostedArguments},
        {.code = lang::DefinedFailureCode::HostedRuntimeContractFailure,
         .detail = lang::DefinedFailureDetail::NegativeArgumentCount}};
  }
  expectHostedBodyValid(
      hostedArguments,
      "reserved hosted_arguments mutation should remain body-valid");
  expectRejected(hostedArguments, "reserved hosted_arguments",
                 "the reserved hosted_arguments detail must remain unproduced");

  lang::MirProgram wrongSite = owned.mir;
  lang::MirInstruction *convert = instruction(
      wrongSite, lang::MirHostedStartupOperationKind::ConvertArgumentCount);
  if (convert != nullptr && !convert->localFailureSites.empty()) {
    const lang::FailureSiteId original = convert->localFailureSites.front();
    const auto replacement =
        std::find_if(wrongSite.failureMetadata().sites().begin(),
                     wrongSite.failureMetadata().sites().end(),
                     [&](const lang::FailureSiteDescriptor &site) {
                       return site.id != original;
                     });
    if (replacement != wrongSite.failureMetadata().sites().end()) {
      convert->localFailureSites.front() = replacement->id;
    }
  }
  expectHostedBodyValid(wrongSite,
                        "wrong hosted site should remain body-valid");
  expectRejected(wrongSite, "canonical stage",
                 "the two detector operations should retain one exact site");

  lang::MirProgram retargetAppend = owned.mir;
  lang::MirHostedStartupOperation *appendRow = operation(
      retargetAppend, lang::MirHostedStartupOperationKind::CallAppend);
  lang::MirInstruction *appendCall = instruction(
      retargetAppend, lang::MirHostedStartupOperationKind::CallAppend);
  if (appendRow != nullptr && appendCall != nullptr) {
    hostedPlan(retargetAppend).appendFunction =
        hostedPlan(retargetAppend).entry;
    appendCall->functionTarget = hostedPlan(retargetAppend).entry;
  }
  expectHostedBodyValid(retargetAppend,
                        "coordinated append retarget should remain body-valid");
  expectRejected(retargetAppend, "targets, signatures",
                 "the plan should reject a coordinated same-program retarget");

  lang::MirProgram callableHeader = owned.mir;
  auto &callableFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          callableHeader.functionInstances());
  lang::MirFunctionInstance &callableEntry =
      callableFunctions[hostedPlan(callableHeader).entry - 1];
  callableEntry.callableParameters.push_back(
      {.parameterIndex = 0,
       .callableType = lang::SemanticType::Int32,
       .access = lang::AccessMode::ReadOnly,
       .boundary = lang::CallableBoundary::Confined});
  expectRejected(callableHeader, "entry is invalid",
                 "generated entry calls should reject callable-header drift");

  lang::MirProgram virtualHeader = owned.mir;
  auto &virtualFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      virtualHeader.functionInstances());
  lang::MirFunctionInstance &virtualAppend =
      virtualFunctions[hostedPlan(virtualHeader).appendFunction - 1];
  virtualAppend.virtualRoots.push_back(virtualAppend.declaration);
  expectRejected(virtualHeader, "targets, signatures",
                 "generated append calls should reject virtual-root drift");

  lang::MirProgram copiedString = owned.mir;
  lang::MirInstruction *stringMove = instruction(
      copiedString,
      lang::MirHostedStartupOperationKind::PrepareAppendArgumentMove);
  if (stringMove != nullptr) {
    stringMove->callInputKind = lang::HirCallInputKind::CopyValue;
  }
  expectHostedBodyValid(copiedString,
                        "copy-label mutation should remain body-valid");
  expectRejected(copiedString, "canonical stage",
                 "the string should move exactly once into append");

  lang::MirProgram missingTransfer = owned.mir;
  lang::MirInstruction *entryCall = instruction(
      missingTransfer, lang::MirHostedStartupOperationKind::CallEntry);
  if (entryCall != nullptr) {
    entryCall->lifecycle.clear();
  }
  // The staged vector is a caller-owned prepared parameter, so normal-exit
  // lifecycle verification now rejects the missing transfer as a live
  // obligation before the hosted stage check reports the same drift.
  expectRejected(missingTransfer, "active drop obligation",
                 "a missing entry transfer should leave a live obligation at "
                 "body exit");
  expectRejected(missingTransfer, "canonical stage",
                 "entry should transfer the generated vector exactly once");

  lang::MirProgram bodyCallLocalOrigin = owned.mir;
  lang::MirInstruction *bodyCall = instruction(
      bodyCallLocalOrigin,
      lang::MirHostedStartupOperationKind::CallProgramInitialization);
  if (bodyCall != nullptr) {
    const lang::MirInstruction *ownedValidate =
        instruction(bodyCallLocalOrigin,
                    lang::MirHostedStartupOperationKind::ValidateArgumentCount);
    if (ownedValidate != nullptr &&
        !ownedValidate->definedFailure.localOrigins.empty() &&
        !ownedValidate->localFailureSites.empty()) {
      bodyCall->definedFailure.localOrigins =
          ownedValidate->definedFailure.localOrigins;
      bodyCall->definedFailure.propagation = lang::FailurePropagationKind::None;
      bodyCall->localFailureSites = ownedValidate->localFailureSites;
    }
  }
  expectRejected(bodyCallLocalOrigin, "generated shape",
                 "CallBody must propagate without re-siting Module failures");

  lang::MirProgram summaryDrift = noarg.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      summaryDrift.functionInstances());
  lang::MirFunctionInstance *entry =
      functions.empty() ? nullptr
                        : &functions[hostedPlan(summaryDrift).entry - 1];
  lang::MirHostedStartupOperation *callRow =
      operation(summaryDrift, lang::MirHostedStartupOperationKind::CallEntry);
  lang::MirInstruction *call =
      instruction(summaryDrift, lang::MirHostedStartupOperationKind::CallEntry);
  if (entry != nullptr && callRow != nullptr && call != nullptr) {
    entry->mayRaiseDefinedFailure = true;
    callRow->failureBehavior = lang::MirHostedStartupFailureBehavior::Propagate;
    call->definedFailure.propagation = lang::FailurePropagationKind::DirectCall;
  }
  expectHostedBodyValid(summaryDrift,
                        "coordinated summary drift should remain body-valid");
  expectRejected(summaryDrift, "not exactly MIR-derived",
                 "hosted target summaries should equal fresh MIR effects");

  lang::MirProgram definitionDrift = noarg.mir;
  auto &definitionFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          definitionDrift.functionInstances());
  lang::MirFunctionInstance &definitionEntry =
      definitionFunctions[hostedPlan(definitionDrift).entry - 1];
  definitionEntry.definitionKind = lang::MirDefinitionKind::Declaration;
  lang::MirBody declarationBody;
  declarationBody.kind = lang::MirBodyKind::Function;
  declarationBody.placeDomain = definitionEntry.body.placeDomain;
  declarationBody.entry = 1;
  declarationBody.returnType = lang::SemanticType::Int32;
  declarationBody.blocks = {
      {.id = 1,
       .terminator = {.kind = lang::MirTerminatorKind::Unreachable},
       .reachable = true}};
  definitionEntry.body = std::move(declarationBody);
  expect(lang::verifyMirBody(definitionEntry.body, definitionEntry.id).valid(),
         "coordinated entry definition drift should use a body-valid shell");
  expectRejected(definitionDrift, "entry is invalid",
                 "HostedStartup should require an exact source-defined entry");
}

} // namespace

int main() {
  testPresenceAndNoArgumentSchedules();
  testOwnedScheduleAndPrinter();
  testProvenanceEntityAndCfgMutations();
  testFailureTargetAndOwnershipMutations();
  if (!passed) {
    return 1;
  }
  std::cout << "All hosted-startup MIR tests passed\n";
  return 0;
}
