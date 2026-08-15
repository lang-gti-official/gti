#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

constexpr std::string_view marker =
    "// GTI verified-MIR body: owned-lifecycle-call-v1";
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

const lang::HirFunctionInstance *
findHirFunction(const lang::HirProgram &program, std::string_view name) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [name](const auto &function) {
        return function.source != nullptr &&
               function.source->name().lexeme == name;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

const lang::HirClassInstance *findHirClass(const lang::HirProgram &program,
                                           std::string_view name) {
  const auto found = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [name](const auto &instance) {
        return instance.source != nullptr &&
               instance.source->name().lexeme == name;
      });
  return found == program.classInstances().end() ? nullptr : &*found;
}

lang::MirFunctionInstance *findFunction(const lang::HirProgram &hir,
                                        lang::MirProgram &mir,
                                        std::string_view name) {
  const auto *source = findHirFunction(hir, name);
  if (source == nullptr) {
    return nullptr;
  }
  auto &values = const_cast<std::vector<lang::MirFunctionInstance> &>(
      mir.functionInstances());
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [&](auto &item) { return item.id == source->id; });
  return found == values.end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *findFunction(const lang::HirProgram &hir,
                                              const lang::MirProgram &mir,
                                              std::string_view name) {
  return findFunction(hir, const_cast<lang::MirProgram &>(mir), name);
}

lang::MirConstructorInstance *findConstructor(const lang::HirProgram &hir,
                                              lang::MirProgram &mir,
                                              std::string_view ownerName) {
  const auto *owner = findHirClass(hir, ownerName);
  if (owner == nullptr) {
    return nullptr;
  }
  auto &values = const_cast<std::vector<lang::MirConstructorInstance> &>(
      mir.constructorInstances());
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [&](auto &item) { return item.owner == owner->id; });
  return found == values.end() ? nullptr : &*found;
}

const lang::MirConstructorInstance *
findConstructor(const lang::HirProgram &hir, const lang::MirProgram &mir,
                std::string_view ownerName) {
  return findConstructor(hir, const_cast<lang::MirProgram &>(mir), ownerName);
}

lang::MirDestructorInstance *findDestructor(const lang::HirProgram &hir,
                                            lang::MirProgram &mir,
                                            std::string_view ownerName) {
  const auto *owner = findHirClass(hir, ownerName);
  if (owner == nullptr || !owner->destructor) {
    return nullptr;
  }
  auto &values = const_cast<std::vector<lang::MirDestructorInstance> &>(
      mir.destructorInstances());
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [&](auto &item) { return item.id == *owner->destructor; });
  return found == values.end() ? nullptr : &*found;
}

const lang::MirDestructorInstance *findDestructor(const lang::HirProgram &hir,
                                                  const lang::MirProgram &mir,
                                                  std::string_view ownerName) {
  return findDestructor(hir, const_cast<lang::MirProgram &>(mir), ownerName);
}

template <typename Body>
auto instructionsOf(Body &body, lang::MirInstructionKind kind) {
  using Instruction =
      std::conditional_t<std::is_const_v<std::remove_reference_t<Body>>,
                         const lang::MirInstruction, lang::MirInstruction>;
  std::vector<Instruction *> result;
  for (auto &block : body.blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.kind == kind) {
        result.push_back(&instruction);
      }
    }
  }
  return result;
}

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility) {
  return lang::CppBackend().generate({.program = frontend.program,
                                      .semantics = frontend.semantics,
                                      .hir = frontend.hir,
                                      .mir = mir,
                                      .sourceMir = &frontend.mir,
                                      .optimizations = compatibility});
}

bool rejected(const lang::FrontendResult &frontend, const lang::MirProgram &mir,
              const lang::OptimizationResult &compatibility) {
  try {
    (void)emit(frontend, mir, compatibility);
    return false;
  } catch (const std::logic_error &) {
    return true;
  }
}

void expectRejected(const lang::FrontendResult &frontend,
                    const lang::OptimizationResult &compatibility,
                    std::string_view description, const auto &mutation) {
  lang::MirProgram changed = frontend.mir;
  mutation(changed);
  expect(rejected(frontend, changed, compatibility), description);
}

void expectRejectedVerified(const lang::FrontendResult &frontend,
                            const lang::OptimizationResult &compatibility,
                            std::string_view description,
                            const auto &mutation) {
  lang::MirProgram changed = frontend.mir;
  mutation(changed);
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(changed);
  if (!verification.valid()) {
    for (const auto &error : verification.errors) {
      std::cerr << "unexpected generic rejection: " << error.message << '\n';
    }
  }
  expect(verification.valid(),
         std::string(description) +
             " should remain generically valid so source coherence owns the "
             "rejection");
  expect(rejected(frontend, changed, compatibility), description);
}

void testFamily(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), readFile(fixture),
      {fixture.parent_path().parent_path().parent_path() / "stdlib" /
       "prelude.gti"});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the owned lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(effects.functions.size() == frontend.mir.functionInstances().size() &&
             effects.constructors.size() ==
                 frontend.mir.constructorInstances().size() &&
             effects.destructors.size() ==
                 frontend.mir.destructorInstances().size(),
         "the effect closure should cover all executable instance kinds");

  const auto *consume = findFunction(frontend.hir, frontend.mir, "consume");
  const auto *run = findFunction(frontend.hir, frontend.mir, "run_scopes");
  const auto *constructor =
      findConstructor(frontend.hir, frontend.mir, "ScopeFlag");
  const auto *destructor =
      findDestructor(frontend.hir, frontend.mir, "ScopeFlag");
  expect(consume != nullptr && run != nullptr && constructor != nullptr &&
             destructor != nullptr && !consume->mayRaiseDefinedFailure &&
             !run->mayRaiseDefinedFailure &&
             !constructor->mayRaiseDefinedFailure &&
             !destructor->mayRaiseDefinedFailure,
         "the ScopeFlag graph should be proved failure-free from MIR");
  if (consume == nullptr || run == nullptr || constructor == nullptr ||
      destructor == nullptr) {
    return;
  }

  const auto stages =
      instructionsOf(constructor->body, lang::MirInstructionKind::Initialize);
  const lang::MirPlace *stagePlace =
      stages.size() == 1 && stages.front()->destination
          ? constructor->body.findPlace(*stages.front()->destination)
          : nullptr;
  expect(stages.size() == 1 && stages.front()->constructorInitializer == 1 &&
             stages.front()->operands.size() == 1 && stagePlace != nullptr &&
             stagePlace->root == lang::MirPlaceRootKind::This &&
             stagePlace->projections.size() == 1 &&
             stagePlace->projections.front().kind ==
                 lang::MirProjectionKind::Field,
         "the constructor should retain one exact this.field initializer "
         "stage");

  const auto constructs =
      instructionsOf(run->body, lang::MirInstructionKind::Construct);
  const auto moves = instructionsOf(run->body, lang::MirInstructionKind::Move);
  const auto inputs =
      instructionsOf(run->body, lang::MirInstructionKind::CallInput);
  const auto calls = instructionsOf(run->body, lang::MirInstructionKind::Call);
  const auto drops = instructionsOf(run->body, lang::MirInstructionKind::Drop);
  const auto initializes =
      instructionsOf(run->body, lang::MirInstructionKind::Initialize);
  const auto scalarInitialize =
      std::find_if(initializes.begin(), initializes.end(), [&](const auto *op) {
        const lang::MirPlace *destination =
            op->destination ? run->body.findPlace(*op->destination) : nullptr;
        return destination != nullptr &&
               destination->type == lang::SemanticType::Int32;
      });
  const auto parameterDrops =
      instructionsOf(consume->body, lang::MirInstructionKind::Drop);
  const auto movedInput =
      std::find_if(inputs.begin(), inputs.end(), [](auto *op) {
        return op->callInputKind == lang::HirCallInputKind::MoveValue;
      });
  expect(constructs.size() == 2 && moves.size() == 1 &&
             movedInput != inputs.end() &&
             (*movedInput)->preparedParameterDrop &&
             (*movedInput)->lifecycle.size() == 1 && calls.size() == 1 &&
             std::count_if(calls.front()->lifecycle.begin(),
                           calls.front()->lifecycle.end(),
                           [](const auto &event) {
                             return event.kind ==
                                    lang::MirLifecycleEventKind::TransferOut;
                           }) == 1 &&
             drops.size() >= 2 && parameterDrops.size() == 1 &&
             scalarInitialize != initializes.end(),
         "construction, MoveValue staging, transfer, and normal cleanup must "
         "all remain explicit");
  expect(std::all_of(constructs.begin(), constructs.end(),
                     [&](auto *op) {
                       return op->constructorTarget == constructor->id &&
                              op->definedFailure.propagation ==
                                  lang::FailurePropagationKind::None;
                     }),
         "proved constructors should use exact targets without failure edges");

  const std::string dump = lang::MirPrinter().print(frontend.mir);
  expect(dump.starts_with("mir-v28 ") &&
             dump.find("constructor-initializer=1") != std::string::npos,
         "MIR v24 should print the new initializer-stage authority");

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const std::string generated =
      emit(frontend, frontend.mir, compatibility).contents;
  std::size_t markers = 0;
  for (std::size_t found = generated.find(marker); found != std::string::npos;
       found = generated.find(marker, found + marker.size())) {
    ++markers;
  }
  expect(markers == 4,
         "two free functions, one constructor, and one destructor should be "
         "MIR-emitted");
  expect(generated.find("mir_lifetime_slot<") != std::string::npos &&
             generated.find("__gti_mir_p_") != std::string::npos &&
             generated.find("__gti_mir_v_") != std::string::npos &&
             generated.find(".construct(") != std::string::npos &&
             generated.find(".destroy();") != std::string::npos &&
             generated.find("std::move(__gti_mir_p_") != std::string::npos,
         "emission must consume MIR places, values, construction, moves, and "
         "explicit drops");
  if (scalarInitialize != initializes.end()) {
    expect(generated.find("__gti_mir_p_" +
                          std::to_string(*(*scalarInitialize)->destination) +
                          " = ") != std::string::npos,
           "a selected scalar local must use scalar MIR initialization rather "
           "than a class lifetime-slot move");
  }

  expectRejected(frontend, compatibility,
                 "constructor summary drift must fail closed",
                 [&](lang::MirProgram &changed) {
                   findConstructor(frontend.hir, changed, "ScopeFlag")
                       ->mayRaiseDefinedFailure = true;
                 });
  expectRejected(frontend, compatibility,
                 "destructor summary drift must fail closed",
                 [&](lang::MirProgram &changed) {
                   findDestructor(frontend.hir, changed, "ScopeFlag")
                       ->mayRaiseDefinedFailure = true;
                 });
  expectRejected(
      frontend, compatibility,
      "missing constructor-stage identity must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findConstructor(frontend.hir, changed, "ScopeFlag");
        instructionsOf(item->body, lang::MirInstructionKind::Initialize)
            .front()
            ->constructorInitializer = 0;
      });
  expectRejected(
      frontend, compatibility,
      "wrong constructor field identity must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findConstructor(frontend.hir, changed, "ScopeFlag");
        auto *stage =
            instructionsOf(item->body, lang::MirInstructionKind::Initialize)
                .front();
        item->body.places[*stage->destination - 1].projections.front().field +=
            1;
      });
  expectRejected(
      frontend, compatibility,
      "a duplicate constructor initializer stage must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findConstructor(frontend.hir, changed, "ScopeFlag");
        auto &instructions = item->body.blocks.front().instructions;
        const auto stage = std::find_if(
            instructions.begin(), instructions.end(),
            [](const auto &op) { return op.constructorInitializer == 1; });
        lang::MirInstruction duplicate = *stage;
        duplicate.id =
            std::max_element(instructions.begin(), instructions.end(),
                             [](const auto &left, const auto &right) {
                               return left.id < right.id;
                             })
                ->id +
            1;
        instructions.insert(std::next(stage), std::move(duplicate));
        (void)lang::rebuildMirValueUses(item->body);
      });
  expectRejected(
      frontend, compatibility,
      "destructor operation retargeting must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findDestructor(frontend.hir, changed, "ScopeFlag");
        const auto computes =
            instructionsOf(item->body, lang::MirInstructionKind::Compute);
        const auto operation =
            std::find_if(computes.begin(), computes.end(), [](const auto *op) {
              return op->operation == lang::MirOperation::BitwiseOr;
            });
        (*operation)->operation = lang::MirOperation::BitwiseXor;
      });
  expectRejected(
      frontend, compatibility,
      "destructor literal retargeting must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findDestructor(frontend.hir, changed, "ScopeFlag");
        const auto computes =
            instructionsOf(item->body, lang::MirInstructionKind::Compute);
        const auto literal =
            std::find_if(computes.begin(), computes.end(), [](const auto *op) {
              return op->operation == lang::MirOperation::Literal;
            });
        (*literal)->literal = lang::Literal{std::uint64_t{1}};
      });
  expectRejected(
      frontend, compatibility, "destructor global retargeting must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findDestructor(frontend.hir, changed, "ScopeFlag");
        auto first =
            std::find_if(item->body.places.begin(), item->body.places.end(),
                         [](const auto &place) {
                           return place.root == lang::MirPlaceRootKind::Symbol;
                         });
        const auto other = std::find_if(
            std::next(first), item->body.places.end(), [&](const auto &place) {
              return place.root == lang::MirPlaceRootKind::Symbol &&
                     place.symbol != first->symbol;
            });
        first->symbol = other->symbol;
      });
  expectRejected(
      frontend, compatibility, "destructor field retargeting must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findDestructor(frontend.hir, changed, "ScopeFlag");
        auto field =
            std::find_if(item->body.places.begin(), item->body.places.end(),
                         [](const auto &place) {
                           return place.root == lang::MirPlaceRootKind::This &&
                                  !place.projections.empty();
                         });
        field->projections.front().field += 1;
      });
  expectRejected(
      frontend, compatibility,
      "destructor true and false CFG edges must not be swapped",
      [&](lang::MirProgram &changed) {
        auto *item = findDestructor(frontend.hir, changed, "ScopeFlag");
        auto branch = std::find_if(
            item->body.blocks.begin(), item->body.blocks.end(),
            [](const auto &block) {
              return block.terminator.kind == lang::MirTerminatorKind::Branch;
            });
        std::swap(branch->terminator.target, branch->terminator.elseTarget);
      });
  expectRejectedVerified(
      frontend, compatibility,
      "selected function operation retargeting must fail source coherence",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        const auto computes =
            instructionsOf(item->body, lang::MirInstructionKind::Compute);
        const auto comparison =
            std::find_if(computes.begin(), computes.end(), [](const auto *op) {
              return op->operation == lang::MirOperation::NotEqual;
            });
        (*comparison)->operation = lang::MirOperation::Equal;
      });
  expectRejectedVerified(
      frontend, compatibility,
      "selected function branch successors must remain source coherent",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        const auto branch = std::find_if(
            item->body.blocks.begin(), item->body.blocks.end(),
            [](const auto &block) {
              return block.terminator.kind == lang::MirTerminatorKind::Branch;
            });
        std::swap(branch->terminator.target, branch->terminator.elseTarget);
      });
  expectRejectedVerified(
      frontend, compatibility,
      "selected function global places must remain tied to their HIR symbol",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        auto first =
            std::find_if(item->body.places.begin(), item->body.places.end(),
                         [](const auto &place) {
                           return place.root == lang::MirPlaceRootKind::Symbol;
                         });
        const auto other = std::find_if(
            std::next(first), item->body.places.end(), [&](const auto &place) {
              return place.root == lang::MirPlaceRootKind::Symbol &&
                     place.symbol != first->symbol;
            });
        first->symbol = other->symbol;
      });
  expectRejectedVerified(
      frontend, compatibility,
      "selected function Return values must remain tied to the HIR root",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        auto finalReturn = std::find_if(
            item->body.blocks.rbegin(), item->body.blocks.rend(),
            [](const auto &block) {
              return block.terminator.kind == lang::MirTerminatorKind::Return &&
                     block.terminator.value.has_value();
            });
        finalReturn->terminator.value =
            lang::MirOperand{.kind = lang::MirOperandKind::Value,
                             .value = item->body.values.front().id,
                             .type = lang::SemanticType::Int32};
        (void)lang::rebuildMirValueUses(item->body);
      });
  expectRejected(
      frontend, compatibility,
      "copy-forging a moved parameter stage must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        auto changedInputs =
            instructionsOf(item->body, lang::MirInstructionKind::CallInput);
        auto moved = std::find_if(
            changedInputs.begin(), changedInputs.end(), [](auto *op) {
              return op->callInputKind == lang::HirCallInputKind::MoveValue;
            });
        (*moved)->callInputKind = lang::HirCallInputKind::CopyValue;
      });
  expectRejected(frontend, compatibility,
                 "missing prepared-parameter transfer must be rejected",
                 [&](lang::MirProgram &changed) {
                   auto *item =
                       findFunction(frontend.hir, changed, "run_scopes");
                   instructionsOf(item->body, lang::MirInstructionKind::Call)
                       .front()
                       ->lifecycle.clear();
                 });
  expectRejected(frontend, compatibility,
                 "call-target drift inside the closed graph must be rejected",
                 [&](lang::MirProgram &changed) {
                   auto *item =
                       findFunction(frontend.hir, changed, "run_scopes");
                   instructionsOf(item->body, lang::MirInstructionKind::Call)
                       .front()
                       ->functionTarget = item->id;
                 });
  expectRejected(
      frontend, compatibility, "Drop-target drift must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        auto changedDrops =
            instructionsOf(item->body, lang::MirInstructionKind::Drop);
        changedDrops.front()->destination = changedDrops.back()->destination;
      });
  expectRejected(
      frontend, compatibility, "a removed reachable Drop must be rejected",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "consume");
        for (auto &block : item->body.blocks) {
          const auto drop =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [](const auto &op) {
                             return op.kind == lang::MirInstructionKind::Drop;
                           });
          if (drop != block.instructions.end()) {
            block.instructions.erase(drop);
            break;
          }
        }
        (void)lang::rebuildMirValueUses(item->body);
      });
  expectRejected(
      frontend, compatibility,
      "constructor retargeting outside the selected class must "
      "fail closed",
      [&](lang::MirProgram &changed) {
        auto *item = findFunction(frontend.hir, changed, "run_scopes");
        const auto *other =
            findConstructor(frontend.hir, changed, "CompatibilityDefaultField");
        instructionsOf(item->body, lang::MirInstructionKind::Construct)
            .front()
            ->constructorTarget = other->id;
      });
}

void testSameTypedConstructorSwapAndCheckedFallback(
    const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), readFile(fixture),
      {fixture.parent_path().parent_path().parent_path() / "stdlib" /
       "prelude.gti"});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the same-typed constructor and checked-cleanup fixture should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto *pair = findConstructor(frontend.hir, frontend.mir, "PairFlag");
  const auto *checked =
      findDestructor(frontend.hir, frontend.mir, "CheckedCleanup");
  expect(pair != nullptr && !pair->mayRaiseDefinedFailure &&
             checked != nullptr && checked->mayRaiseDefinedFailure,
         "the exact PairFlag lifecycle should be failure-free while checked "
         "cleanup remains outside the family");

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  std::optional<lang::BackendArtifact> baseline;
  try {
    baseline = emit(frontend, frontend.mir, compatibility);
  } catch (const std::logic_error &error) {
    std::cerr << "checked-cleanup fallback unexpectedly failed: "
              << error.what() << '\n';
  }
  expect(baseline.has_value(),
         "a valid failure-capable destructor must fall back instead of "
         "triggering the owned-lifecycle selector");
  if (baseline) {
    std::size_t markers = 0;
    for (std::size_t found = baseline->contents.find(marker);
         found != std::string::npos;
         found = baseline->contents.find(marker, found + marker.size())) {
      ++markers;
    }
    expect(markers == 4,
           "PairFlag's function, reverse scalar caller, constructor, and "
           "destructor should form one owned-lifecycle graph");
  }

  expectRejected(
      frontend, compatibility,
      "same-typed constructor arguments must not be swapped",
      [&](lang::MirProgram &changed) {
        auto *constructor = findConstructor(frontend.hir, changed, "PairFlag");
        auto stages = instructionsOf(constructor->body,
                                     lang::MirInstructionKind::Initialize);
        std::sort(stages.begin(), stages.end(),
                  [](const auto *left, const auto *right) {
                    return left->constructorInitializer <
                           right->constructorInitializer;
                  });
        std::swap(stages[0]->operands.front(), stages[1]->operands.front());
        (void)lang::rebuildMirValueUses(constructor->body);
      });
  expectRejected(frontend, compatibility,
                 "the reverse caller must not introduce a graph cycle",
                 [&](lang::MirProgram &changed) {
                   auto *wrapper =
                       findFunction(frontend.hir, changed, "pair_wrapper");
                   instructionsOf(wrapper->body, lang::MirInstructionKind::Call)
                       .front()
                       ->functionTarget = wrapper->id;
                 });
  expectRejectedVerified(
      frontend, compatibility,
      "same-typed constructor CallInput operands must not be duplicated",
      [&](lang::MirProgram &changed) {
        auto *function = findFunction(frontend.hir, changed, "use_pair");
        auto inputs =
            instructionsOf(function->body, lang::MirInstructionKind::CallInput);
        std::sort(inputs.begin(), inputs.end(),
                  [](const auto *left, const auto *right) {
                    return left->callInputIndex < right->callInputIndex;
                  });
        inputs[1]->operands.front() = inputs[0]->operands.front();
        (void)lang::rebuildMirValueUses(function->body);
      });
  expectRejectedVerified(
      frontend, compatibility,
      "a reverse caller cannot erase its HIR call edge from selected MIR",
      [&](lang::MirProgram &changed) {
        auto *wrapper = findFunction(frontend.hir, changed, "pair_wrapper");
        auto *call =
            instructionsOf(wrapper->body, lang::MirInstructionKind::Call)
                .front();
        call->kind = lang::MirInstructionKind::Compute;
        call->callSite = 0;
        call->parameterTypes.clear();
        call->functionTarget.reset();
        call->operation = lang::MirOperation::Literal;
        call->literal = lang::Literal{std::uint64_t{0}};
        call->literalProvenance = {.kind =
                                       lang::MirLiteralProvenanceKind::Source};
        call->operands.clear();
        (void)lang::rebuildMirValueUses(wrapper->body);
      });
}

void testUnsupportedCommaFallsBack(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), readFile(fixture),
      {fixture.parent_path().parent_path().parent_path() / "stdlib" /
       "prelude.gti"});
  expect(frontend.canGenerateCode(),
         "the unsupported comma near miss should remain a valid source "
         "program");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  std::optional<lang::BackendArtifact> generated;
  try {
    generated = emit(frontend, frontend.mir, compatibility);
  } catch (const std::logic_error &error) {
    std::cerr << "comma near-miss fallback unexpectedly failed: "
              << error.what() << '\n';
  }
  expect(generated.has_value(),
         "an unsupported comma operation must demote the source function "
         "instead of reaching the owned MIR emitter");
  if (generated) {
    expect(generated->contents.find(marker) == std::string::npos,
           "the comma near miss must atomically keep its function, "
           "constructor, and destructor on compatibility emission");
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gti_mir_backend_owned_lifecycle_tests <fixture>\n";
    return 2;
  }
  testFamily(argv[1]);
  testSameTypedConstructorSwapAndCheckedFallback(
      std::filesystem::path(argv[1]).parent_path() /
      "mir_backend_owned_lifecycle_ctor_swap.gti");
  testUnsupportedCommaFallsBack(
      std::filesystem::path(argv[1]).parent_path() /
      "mir_backend_owned_lifecycle_comma_near_miss.gti");
  if (failures != 0) {
    std::cerr << failures << " owned lifecycle backend test(s) failed\n";
    return 1;
  }
  return 0;
}
