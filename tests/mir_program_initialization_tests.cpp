#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

lang::FrontendResult analyzeFixture() {
  return lang::Frontend().analyze(
      "/tmp/gti-mir-program-initialization/main.gti", R"(
constexpr int32_t seed = 1;
mut int32_t zeroed;
mut int32_t dynamic = seed;
constexpr bool enabled = true;
mut double left = 1.0;
mut double selected = enabled ? left : 2.0d;
mut int32_t folded = ((42));
mut double after = left;

class Registry {
public:
  int32_t member = 1;
  static constexpr int32_t fixed = 2;
  static mut int32_t value = dynamic;
  static mut int32_t deferred = Registry::later;
  static constexpr int32_t later = 3;
};

class Plain {
public:
  int32_t member = 1;
};

mut int32_t checked = dynamic + 1;

int main() {
  return checked + Registry::value + Registry::deferred + zeroed;
}
)");
}

lang::MirBody *module(lang::MirProgram &program) {
  return lang::findMirBody(program,
                           {.kind = lang::MirBodyKind::Module, .owner = 0});
}

lang::MirProgramInitializationPlan &plan(lang::MirProgram &program) {
  return const_cast<lang::MirProgramInitializationPlan &>(
      program.programInitializationPlan());
}

std::vector<lang::MirClassInstance> &classes(lang::MirProgram &program) {
  return const_cast<std::vector<lang::MirClassInstance> &>(
      program.classInstances());
}

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (lang::MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const lang::MirInstruction &candidate) {
                       return candidate.id == id;
                     });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

lang::MirBlock *instructionBlock(lang::MirBody &body,
                                 lang::MirInstructionId id) {
  const auto found = std::find_if(
      body.blocks.begin(), body.blocks.end(), [&](const lang::MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == id;
                           });
      });
  return found == body.blocks.end() ? nullptr : &*found;
}

std::optional<lang::SymbolId> symbolFor(const lang::FrontendResult &frontend,
                                        std::string_view name) {
  for (const lang::ProgramInitializationStep &step :
       frontend.semantics.programInitializationPlan().steps) {
    if (step.declaration != nullptr &&
        step.declaration->name().lexeme == name) {
      return step.symbol;
    }
  }
  return std::nullopt;
}

std::optional<lang::HirClassInstanceId>
classFor(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr && instance.source->name().lexeme == name) {
      return instance.id;
    }
  }
  return std::nullopt;
}

void expectRejected(const lang::MirProgram &source, std::string_view message,
                    const std::function<void(lang::MirProgram &)> &mutate,
                    std::string_view expectedDiagnostic = {},
                    bool moduleBodyMustVerify = false) {
  lang::MirProgram forged = source;
  mutate(forged);
  if (moduleBodyMustVerify) {
    expect(lang::verifyMirBody(forged.module(), 0).valid(),
           std::string(message) +
               " (mutation should remain generic-Module-valid)");
  }
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(forged);
  if (verification.valid()) {
    printVerification(verification);
  }
  expect(!verification.valid(), message);
  if (!expectedDiagnostic.empty()) {
    expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                       [&](const lang::MirVerificationError &error) {
                         return error.message.find(expectedDiagnostic) !=
                                std::string::npos;
                       }),
           std::string(message) + " (missing exact Stage C diagnostic)");
  }
}

bool activePlaceOperand(lang::MirOperandKind kind) {
  switch (kind) {
  case lang::MirOperandKind::Address:
  case lang::MirOperandKind::Copy:
  case lang::MirOperandKind::Move:
  case lang::MirOperandKind::BorrowRead:
  case lang::MirOperandKind::BorrowWrite:
    return true;
  case lang::MirOperandKind::Value:
  case lang::MirOperandKind::Constant:
  case lang::MirOperandKind::Loan:
    return false;
  }
  return false;
}

bool redirectEdge(lang::MirTerminator &terminator, lang::MirBlockId from,
                  lang::MirBlockId to) {
  if (terminator.target == from) {
    terminator.target = to;
    return true;
  }
  if (terminator.elseTarget == from) {
    terminator.elseTarget = to;
    return true;
  }
  for (lang::MirSwitchTarget &target : terminator.switchTargets) {
    if (target.target == from) {
      target.target = to;
      return true;
    }
  }
  return false;
}

void testExactLoweringAndPrinting(const lang::FrontendResult &frontend) {
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(frontend.mir);
  if (!frontend.mirValid || !verification.valid()) {
    printDiagnostics(frontend);
    printVerification(verification);
  }
  const lang::MirProgramInitializationPlan &initialization =
      frontend.mir.programInitializationPlan();
  const std::vector<lang::ProgramInitializationStepId> dense = [&] {
    std::vector<lang::ProgramInitializationStepId> result;
    for (std::size_t index = 0; index < initialization.steps.size(); ++index) {
      result.push_back(index + 1);
    }
    return result;
  }();
  const bool exactSteps =
      !initialization.steps.empty() && initialization.units.size() == 1 &&
      initialization.units.front().sourceUnit != 0 &&
      initialization.units.front().steps == dense &&
      std::all_of(initialization.steps.begin(), initialization.steps.end(),
                  [&](const lang::MirProgramInitializationStep &step) {
                    return step.id != 0 &&
                           step.sourceUnit ==
                               initialization.units.front().sourceUnit &&
                           step.storagePlace != 0 && step.entryBlock != 0 &&
                           step.storageInitialization != 0;
                  });
  const bool sawImplicit = std::any_of(
      initialization.steps.begin(), initialization.steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.dataInitialization ==
                   lang::MirProgramDataInitializationKind::ImplicitZero &&
               !step.dataConstant;
      });
  const bool sawConstant = std::any_of(
      initialization.steps.begin(), initialization.steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.dataInitialization ==
                   lang::MirProgramDataInitializationKind::Constant &&
               step.dataConstant.has_value();
      });
  const bool sawDynamic = std::any_of(
      initialization.steps.begin(), initialization.steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.role == lang::ProgramInitializationStepRole::Initializer &&
               step.statement != 0 && step.initializer != 0 &&
               step.fullExpression != 0;
      });
  const bool sawStatic = std::any_of(
      initialization.steps.begin(), initialization.steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.storageKind == lang::ProgramStorageKind::StaticField &&
               step.ownerClass != 0;
      });
  bool exactTags = true;
  for (const lang::MirBlock &block : frontend.mir.module().blocks) {
    exactTags = exactTags && block.programInitializationStep != 0 &&
                block.programInitializationStep <= initialization.steps.size();
  }
  bool checkedFailure = false;
  bool substitution = false;
  for (const lang::MirBlock &block : frontend.mir.module().blocks) {
    for (const lang::MirInstruction &item : block.instructions) {
      checkedFailure = checkedFailure || !item.definedFailure.empty();
      substitution = substitution || item.programConstantSubstitution;
    }
  }
  const std::string dump = lang::MirPrinter().print(frontend.mir);
  expect(frontend.sourceValid && frontend.syntaxValid &&
             frontend.semanticValid && frontend.hirValid &&
             frontend.failureMetadataValid && frontend.mirValid &&
             verification.valid() && exactSteps && sawImplicit && sawConstant &&
             sawDynamic && sawStatic && exactTags && checkedFailure &&
             substitution && dump.starts_with("mir-v32 valid=") &&
             dump.find("program-initialization units=1") != std::string::npos &&
             dump.find("data-kind=implicit-zero") != std::string::npos &&
             dump.find("program-initialization-step=") != std::string::npos,
         "MIR should retain and print one exact deterministic Module/0 "
         "initialization plan, including checked and substituted dynamics");
}

void testSourceLessSyntheticLowering(const lang::FrontendResult &frontend) {
  lang::HirProgram synthetic = frontend.hir;
  auto &initialization = const_cast<lang::HirProgramInitializationPlan &>(
      synthetic.programInitializationPlan());
  initialization.unitOrder.clear();
  for (lang::HirProgramInitializationStep &step : initialization.steps) {
    step.sourceUnit = 0;
  }
  const lang::MirLoweringResult lowered =
      lang::MirLowerer().lower(synthetic, frontend.failureMetadata);
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(lowered.program);
  expect(
      lowered.valid() && verification.valid() &&
          lowered.program.programInitializationPlan().units.empty() &&
          std::all_of(lowered.program.programInitializationPlan().steps.begin(),
                      lowered.program.programInitializationPlan().steps.end(),
                      [](const lang::MirProgramInitializationStep &step) {
                        return step.sourceUnit == 0;
                      }),
      "source-less synthetic HIR should lower to the canonical zero-unit "
      "MIR initialization inventory");
}

void testPlanAndStorageMutations(const lang::FrontendResult &frontend) {
  const lang::MirProgram &source = frontend.mir;
  expectRejected(source, "a zero real source-unit row must be rejected",
                 [](lang::MirProgram &forged) {
                   plan(forged).units.front().sourceUnit = 0;
                 });
  expectRejected(source, "an incomplete unit/step inventory must be rejected",
                 [](lang::MirProgram &forged) {
                   plan(forged).units.front().steps.pop_back();
                 });
  expectRejected(source, "a duplicate real source-unit row must be rejected",
                 [](lang::MirProgram &forged) {
                   plan(forged).units.push_back(plan(forged).units.front());
                 });
  lang::MirProgram withEmptyUnit = source;
  {
    auto &units = plan(withEmptyUnit).units;
    const lang::SourceUnitId emptySource =
        std::max_element(units.begin(), units.end(),
                         [](const auto &left, const auto &right) {
                           return left.sourceUnit < right.sourceUnit;
                         })
            ->sourceUnit +
        1;
    units.push_back({.sourceUnit = emptySource});
  }
  expect(lang::verifyMirProgram(withEmptyUnit).valid(),
         "an exact real source-unit row may contribute no program storage");
  lang::MirProgram missingEmptyUnit = withEmptyUnit;
  plan(missingEmptyUnit).units.pop_back();
  expect(
      lang::verifyMirProgram(missingEmptyUnit).valid() &&
          !lang::verifyMirOptimizationCoherence(withEmptyUnit, missingEmptyUnit)
               .valid(),
      "optimizer coherence must reject a removed empty source-unit row");
  lang::MirProgram reorderedEmptyUnit = withEmptyUnit;
  std::swap(plan(reorderedEmptyUnit).units.front(),
            plan(reorderedEmptyUnit).units.back());
  expect(lang::verifyMirProgram(reorderedEmptyUnit).valid() &&
             !lang::verifyMirOptimizationCoherence(withEmptyUnit,
                                                   reorderedEmptyUnit)
                  .valid(),
         "optimizer coherence must reject a reordered empty source-unit row");
  expectRejected(source, "a step source-unit drift must be rejected",
                 [](lang::MirProgram &forged) {
                   ++plan(forged).steps.front().sourceUnit;
                 });
  lang::MirProgram twoUnits = source;
  {
    auto &initialization = plan(twoUnits);
    const std::size_t split = initialization.steps.size() / 2;
    const lang::SourceUnitId secondSource =
        initialization.units.front().sourceUnit + 1;
    lang::MirProgramInitializationUnit second{.sourceUnit = secondSource};
    second.steps.assign(initialization.units.front().steps.begin() + split,
                        initialization.units.front().steps.end());
    initialization.units.front().steps.resize(split);
    initialization.units.push_back(std::move(second));
    for (std::size_t index = split; index < initialization.steps.size();
         ++index) {
      initialization.steps[index].sourceUnit = secondSource;
    }
  }
  expect(lang::verifyMirProgram(twoUnits).valid(),
         "a pointer-free synthetic two-unit inventory should verify before "
         "the reorder mutation");
  expectRejected(twoUnits, "source-unit order cannot be reordered",
                 [](lang::MirProgram &forged) {
                   std::swap(plan(forged).units[0], plan(forged).units[1]);
                 });
  expectRejected(
      source, "a non-dense step identity must be rejected",
      [](lang::MirProgram &forged) { plan(forged).steps.front().id = 2; });
  expectRejected(
      source, "erasing an explicit constant into implicit-zero must fail",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto found =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.dataInitialization ==
                     lang::MirProgramDataInitializationKind::Constant;
            });
        found->dataConstant.reset();
      });
  expectRejected(
      source, "forging data on an implicit-zero step must fail",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto found =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.dataInitialization ==
                     lang::MirProgramDataInitializationKind::ImplicitZero;
            });
        found->dataInitialization =
            lang::MirProgramDataInitializationKind::Constant;
      });
  expectRejected(source, "a block cannot lose its exact dense step tag",
                 [](lang::MirProgram &forged) {
                   module(forged)->blocks.front().programInitializationStep = 0;
                 });
  expectRejected(source, "a step cannot claim another storage place",
                 [](lang::MirProgram &forged) {
                   auto &steps = plan(forged).steps;
                   steps.front().storagePlace = steps.back().storagePlace;
                 });
  expectRejected(
      source, "a static step cannot lose its concrete owner",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto found =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.storageKind == lang::ProgramStorageKind::StaticField;
            });
        found->ownerClass = 0;
      });
  expectRejected(
      source, "a static step cannot claim namespace storage",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto found =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.storageKind == lang::ProgramStorageKind::StaticField;
            });
        found->storageKind = lang::ProgramStorageKind::NamespaceGlobal;
      });
  expectRejected(
      source, "a step symbol must match its exact storage place",
      [](lang::MirProgram &forged) { ++plan(forged).steps.front().symbol; });
  expectRejected(
      source, "a step binding must match its exact storage place",
      [](lang::MirProgram &forged) { ++plan(forged).steps.front().binding; });
  expectRejected(source, "a step entry block must be exact",
                 [](lang::MirProgram &forged) {
                   plan(forged).steps.front().entryBlock =
                       plan(forged).steps[1].entryBlock;
                 });
  expectRejected(source, "a step Initialize identity must be exact",
                 [](lang::MirProgram &forged) {
                   ++plan(forged).steps.front().storageInitialization;
                 });
  expectRejected(source, "program storage cannot start available",
                 [](lang::MirProgram &forged) {
                   module(forged)
                       ->places[plan(forged).steps.front().storagePlace - 1]
                       .initiallyAvailable = true;
                 });
  expectRejected(source, "program storage cannot change its root kind",
                 [](lang::MirProgram &forged) {
                   module(forged)
                       ->places[plan(forged).steps.front().storagePlace - 1]
                       .root = lang::MirPlaceRootKind::Symbol;
                 });
  expectRejected(
      source, "program storage cannot change its ownership key",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        module(forged)->places[steps.front().storagePlace - 1].key->root =
            steps.back().symbol;
      });
  expectRejected(source, "program storage cannot change its MIR type",
                 [](lang::MirProgram &forged) {
                   module(forged)
                       ->places[plan(forged).steps.front().storagePlace - 1]
                       .type = lang::SemanticType::Bool;
                 });
  expectRejected(source, "Stage C storage cannot claim active cleanup",
                 [](lang::MirProgram &forged) {
                   plan(forged).steps.front().requiresActiveCleanup = true;
                 });
  expectRejected(source, "a DataOnly step cannot use the None data kind",
                 [](lang::MirProgram &forged) {
                   plan(forged).steps.front().dataInitialization =
                       lang::MirProgramDataInitializationKind::None;
                 });
  expectRejected(source, "a DataOnly step cannot use the Count data kind",
                 [](lang::MirProgram &forged) {
                   plan(forged).steps.front().dataInitialization =
                       lang::MirProgramDataInitializationKind::Count;
                 });
  expectRejected(
      source, "a data constant must match its storage type",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto found =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.dataInitialization ==
                         lang::MirProgramDataInitializationKind::Constant &&
                     step.dataConstant.has_value();
            });
        found->dataConstant = lang::ConstantValue{true};
      });
  expectRejected(
      source, "an unused undeclared Symbol-root place must be rejected",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        lang::MirPlace extra = body->places.front();
        extra.id = body->places.size() + 1;
        extra.root = lang::MirPlaceRootKind::Symbol;
        extra.binding = 0;
        extra.symbol = 999999;
        extra.key =
            lang::PlaceKey{.domain = body->placeDomain, .root = extra.symbol};
        body->places.push_back(std::move(extra));
      },
      "does not name one exact planned storage step", true);
  expectRejected(
      source, "a used undeclared Symbol-root place must be rejected",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        for (lang::MirBlock &block : body->blocks) {
          for (lang::MirInstruction &item : block.instructions) {
            const auto operand =
                std::find_if(item.operands.begin(), item.operands.end(),
                             [](const lang::MirOperand &candidate) {
                               return candidate.place != 0;
                             });
            if (operand == item.operands.end()) {
              continue;
            }
            lang::MirPlace extra = *body->findPlace(operand->place);
            extra.id = body->places.size() + 1;
            extra.root = lang::MirPlaceRootKind::Symbol;
            extra.binding = 0;
            extra.symbol = 999998;
            extra.capture = 0;
            extra.temporary = 0;
            extra.value = 0;
            extra.loan = 0;
            extra.key = lang::PlaceKey{.domain = body->placeDomain,
                                       .root = extra.symbol};
            body->places.push_back(std::move(extra));
            operand->place = body->places.back().id;
            return;
          }
        }
      },
      "does not name one exact planned storage step", true);
  expectRejected(
      source, "an unused Module temporary must have a real producer/use",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto existing = std::find_if(
            body->places.begin(), body->places.end(),
            [](const lang::MirPlace &place) {
              return place.root == lang::MirPlaceRootKind::Temporary;
            });
        lang::MirPlace extra =
            existing == body->places.end() ? body->places.front() : *existing;
        extra.id = body->places.size() + 1;
        extra.root = lang::MirPlaceRootKind::Temporary;
        extra.binding = 0;
        extra.symbol = 0;
        extra.capture = 0;
        extra.temporary = 999998;
        extra.value = 0;
        extra.loan = 0;
        extra.key.reset();
        extra.sourceValue = plan(forged).steps.back().initializer;
        body->places.push_back(std::move(extra));
      },
      "orphan program-initialization temporary", true);

  bool retargetedInitializer = false;
  expectRejected(
      source, "a dynamic Initialize cannot retarget a same-typed MIR value",
      [&](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        lang::MirBody *body = module(forged);
        std::vector<lang::MirInstruction *> initializers;
        for (const auto &step : steps) {
          if (step.role == lang::ProgramInitializationStepRole::Initializer) {
            initializers.push_back(
                instruction(*body, step.storageInitialization));
          }
        }
        for (std::size_t target = 1;
             target < initializers.size() && !retargetedInitializer; ++target) {
          for (std::size_t replacement = 0; replacement < target;
               ++replacement) {
            if (initializers[target] == nullptr ||
                initializers[replacement] == nullptr ||
                initializers[target]->operands.empty() ||
                initializers[replacement]->operands.empty() ||
                initializers[target]->operands.front().type !=
                    initializers[replacement]->operands.front().type) {
              continue;
            }
            initializers[target]->operands.front() =
                initializers[replacement]->operands.front();
            retargetedInitializer = true;
            (void)lang::rebuildMirValueUses(*body);
            break;
          }
        }
      },
      "dynamic initialization step lacks", true);
  expect(retargetedInitializer,
         "the fixture should expose two same-typed dynamic initializer "
         "operands for provenance retargeting");
  bool widenedDataSchedule = false;
  expectRejected(
      source, "a DataOnly step must retain its zero-operand schedule",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto found = std::find_if(
            plan(forged).steps.begin(), plan(forged).steps.end(),
            [](const auto &step) {
              return step.role == lang::ProgramInitializationStepRole::DataOnly;
            });
        if (found == plan(forged).steps.end()) {
          return;
        }
        lang::MirInstruction *initialize =
            instruction(*body, found->storageInitialization);
        const lang::MirPlace *storage = body->findPlace(found->storagePlace);
        if (initialize == nullptr || storage == nullptr) {
          return;
        }
        initialize->operands.push_back(
            {.kind = lang::MirOperandKind::Constant,
             .literal = lang::Literal{std::uint64_t{0}},
             .type = storage->type});
        widenedDataSchedule = true;
      },
      "data-only initialization step is not its exact", true);
  expect(widenedDataSchedule,
         "the fixture should expose a DataOnly abstract Initialize for the "
         "schedule-boundary mutation");
  expectRejected(source,
                 "unrelated Initialize metadata must not widen the exact step",
                 [](lang::MirProgram &forged) {
                   const auto &step = plan(forged).steps.front();
                   instruction(*module(forged), step.storageInitialization)
                       ->rawMemoryAccess = true;
                 });
  expectRejected(
      source, "inactive operand union fields cannot ride an Initialize",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto dynamic =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        lang::MirInstruction *initialize =
            instruction(*module(forged), dynamic->storageInitialization);
        initialize->operands.front().place = dynamic->storagePlace;
      });
  expectRejected(
      source, "a dynamic step cannot change its retained HIR identities",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto first =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        const auto second =
            std::find_if(std::next(first), steps.end(), [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        first->statement = second->statement;
        first->initializer = second->initializer;
        first->fullExpression = second->fullExpression;
      });
  expectRejected(
      source, "a dynamic Initialize cannot change its destination",
      [](lang::MirProgram &forged) {
        auto &steps = plan(forged).steps;
        const auto dynamic =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        instruction(*module(forged), dynamic->storageInitialization)
            ->destination = steps.back().storagePlace;
      });

  bool ownStorageRead = false;
  expectRejected(
      source, "a step cannot read its own program storage before publish",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        for (const auto &step : plan(forged).steps) {
          const lang::MirPlace *storage = body->findPlace(step.storagePlace);
          for (lang::MirBlock &block : body->blocks) {
            if (block.programInitializationStep != step.id) {
              continue;
            }
            for (lang::MirInstruction &item : block.instructions) {
              for (lang::MirOperand &operand : item.operands) {
                if (!ownStorageRead && operand.place != 0 &&
                    activePlaceOperand(operand.kind) && storage != nullptr &&
                    operand.type == storage->type) {
                  operand.place = step.storagePlace;
                  ownStorageRead = true;
                }
              }
            }
          }
        }
      },
      "program-storage operand/receiver is not accessed strictly", true);
  expect(ownStorageRead,
         "the fixture should expose a real place read for the own-storage "
         "ordering mutation");

  bool laterStorageRead = false;
  expectRejected(
      source, "a step cannot read later program storage",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &steps = plan(forged).steps;
        for (std::size_t index = 0;
             index + 1 < steps.size() && !laterStorageRead; ++index) {
          for (lang::MirBlock &block : body->blocks) {
            if (block.programInitializationStep != steps[index].id) {
              continue;
            }
            for (lang::MirInstruction &item : block.instructions) {
              for (lang::MirOperand &operand : item.operands) {
                if (operand.place == 0 || !activePlaceOperand(operand.kind)) {
                  continue;
                }
                const auto later =
                    std::find_if(steps.begin() + index + 1, steps.end(),
                                 [&](const auto &candidate) {
                                   const lang::MirPlace *storage =
                                       body->findPlace(candidate.storagePlace);
                                   return storage != nullptr &&
                                          storage->type == operand.type;
                                 });
                if (later != steps.end()) {
                  operand.place = later->storagePlace;
                  laterStorageRead = true;
                  break;
                }
              }
            }
          }
        }
      },
      "program-storage operand/receiver is not accessed strictly", true);
  expect(laterStorageRead,
         "the fixture should expose a same-typed later storage target");

  bool laterStorageAddress = false;
  expectRejected(
      source, "a step cannot take the address of later program storage",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &steps = plan(forged).steps;
        for (std::size_t index = 0;
             index + 1 < steps.size() && !laterStorageAddress; ++index) {
          if (steps[index].role !=
              lang::ProgramInitializationStepRole::Initializer) {
            continue;
          }
          lang::MirInstruction *initialize =
              instruction(*body, steps[index].storageInitialization);
          if (initialize == nullptr || initialize->operands.size() != 1 ||
              initialize->operands.front().kind !=
                  lang::MirOperandKind::Value ||
              initialize->operands.front().type != lang::SemanticType::Double) {
            continue;
          }
          const auto later = std::find_if(
              steps.begin() + index + 1, steps.end(),
              [&](const auto &candidate) {
                const lang::MirPlace *storage =
                    body->findPlace(candidate.storagePlace);
                return storage != nullptr &&
                       storage->type == initialize->operands.front().type;
              });
          if (later == steps.end()) {
            continue;
          }
          initialize->operands.front() = {
              .kind = lang::MirOperandKind::Address,
              .place = later->storagePlace,
              .type = initialize->operands.front().type};
          laterStorageAddress = true;
          (void)lang::rebuildMirValueUses(*body);
        }
      },
      "program-storage operand/receiver is not accessed strictly", true);
  expect(laterStorageAddress,
         "the fixture should expose a body-valid Address operand targeting "
         "same-typed later storage");
}

void testCfgAndPublicationMutations(const lang::FrontendResult &frontend) {
  const lang::MirProgram &source = frontend.mir;
  expectRejected(
      source, "a disconnected tagged block must be rejected",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto &last = plan(forged).steps.back();
        const lang::MirBlockId exit =
            std::find_if(body->blocks.begin(), body->blocks.end(),
                         [](const lang::MirBlock &block) {
                           return block.terminator.kind ==
                                  lang::MirTerminatorKind::Exit;
                         })
                ->id;
        lang::MirBlock extra;
        extra.id = body->blocks.size() + 1;
        extra.programInitializationStep = last.id;
        extra.terminator.kind = lang::MirTerminatorKind::Goto;
        extra.terminator.target = exit;
        body->blocks.push_back(std::move(extra));
      },
      "unreachable from its entry", true);

  bool bypassedInitialize = false;
  expectRejected(
      source, "a path cannot bypass the claimed Initialize",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &steps = plan(forged).steps;
        bool changed = false;
        for (std::size_t index = 0; index + 1 < steps.size() && !changed;
             ++index) {
          lang::MirBlock *initialize =
              instructionBlock(*body, steps[index].storageInitialization);
          if (initialize == nullptr) {
            continue;
          }
          for (lang::MirBlock &block : body->blocks) {
            if (block.id != initialize->id &&
                block.programInitializationStep == steps[index].id &&
                redirectEdge(block.terminator, initialize->id,
                             steps[index + 1].entryBlock)) {
              changed = true;
              bypassedInitialize = true;
              break;
            }
          }
        }
      },
      "does not dominate", true);
  expect(bypassedInitialize,
         "the conditional fixture should expose an actual Initialize-bypass "
         "edge mutation");

  expectRejected(
      source, "a full-expression marker cannot move to another step",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &steps = plan(forged).steps;
        const auto dynamic =
            std::find_if(steps.begin(), steps.end(), [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        std::optional<lang::MirInstruction> marker;
        for (lang::MirBlock &block : body->blocks) {
          const auto found = std::find_if(
              block.instructions.begin(), block.instructions.end(),
              [&](const lang::MirInstruction &item) {
                return item.fullExpressionEnd == dynamic->fullExpression;
              });
          if (found != block.instructions.end()) {
            marker = std::move(*found);
            block.instructions.erase(found);
            break;
          }
        }
        const auto later =
            std::find_if(body->blocks.begin(), body->blocks.end(),
                         [&](const lang::MirBlock &block) {
                           return block.programInitializationStep > dynamic->id;
                         });
        later->instructions.push_back(std::move(*marker));
      },
      "dynamic initialization step lacks", true);
  expectRejected(
      source, "a publication marker cannot retain unrelated metadata",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto dynamic = std::find_if(
            plan(forged).steps.begin(), plan(forged).steps.end(),
            [](const auto &step) {
              return step.role ==
                     lang::ProgramInitializationStepRole::Initializer;
            });
        for (lang::MirBlock &block : body->blocks) {
          for (lang::MirInstruction &item : block.instructions) {
            if (item.fullExpressionEnd == dynamic->fullExpression) {
              item.rawMemoryAccess = true;
            }
          }
        }
      },
      "dynamic initialization step lacks", true);

  expectRejected(
      source, "a dense Goto cannot retain unrelated payload",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto &steps = plan(forged).steps;
        const lang::MirBlockId next = steps[1].entryBlock;
        const auto predecessor = std::find_if(
            body->blocks.begin(), body->blocks.end(),
            [&](const lang::MirBlock &block) {
              return block.terminator.kind == lang::MirTerminatorKind::Goto &&
                     block.terminator.target == next;
            });
        predecessor->terminator.hirStatement = 1;
      },
      "boundary is not the exact dense Goto", true);
  expectRejected(
      source, "the final Exit cannot retain unrelated payload",
      [](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        const auto exit = std::find_if(body->blocks.begin(), body->blocks.end(),
                                       [](const lang::MirBlock &block) {
                                         return block.terminator.kind ==
                                                lang::MirTerminatorKind::Exit;
                                       });
        exit->terminator.hirStatement = 1;
      },
      "boundary is not the exact dense Goto or final Exit", true);

  expectRejected(source, "a dense edge cannot skip an initialization step",
                 [](lang::MirProgram &forged) {
                   lang::MirBody *body = module(forged);
                   const auto &steps = plan(forged).steps;
                   const auto boundary = std::find_if(
                       body->blocks.begin(), body->blocks.end(),
                       [&](const lang::MirBlock &block) {
                         return block.terminator.target == steps[1].entryBlock;
                       });
                   boundary->terminator.target = steps[2].entryBlock;
                 });
  expectRejected(source, "a dense edge cannot go backward to an earlier step",
                 [](lang::MirProgram &forged) {
                   lang::MirBody *body = module(forged);
                   const auto &steps = plan(forged).steps;
                   const auto boundary = std::find_if(
                       body->blocks.begin(), body->blocks.end(),
                       [&](const lang::MirBlock &block) {
                         return block.terminator.target == steps[2].entryBlock;
                       });
                   boundary->terminator.target = steps.front().entryBlock;
                 });
  expectRejected(source, "program initialization cannot Exit before its end",
                 [](lang::MirProgram &forged) {
                   lang::MirBody *body = module(forged);
                   const auto &steps = plan(forged).steps;
                   const auto boundary = std::find_if(
                       body->blocks.begin(), body->blocks.end(),
                       [&](const lang::MirBlock &block) {
                         return block.terminator.target == steps[1].entryBlock;
                       });
                   boundary->terminator = {};
                   boundary->terminator.kind = lang::MirTerminatorKind::Exit;
                 });

  bool reenteredPublication = false;
  expectRejected(
      source, "a same-step edge cannot re-enter a publication block",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &steps = plan(forged).steps;
        for (std::size_t index = 0;
             index + 1 < steps.size() && !reenteredPublication; ++index) {
          lang::MirBlock *published =
              instructionBlock(*body, steps[index].storageInitialization);
          if (published == nullptr ||
              published->terminator.kind != lang::MirTerminatorKind::Goto ||
              published->terminator.target != steps[index + 1].entryBlock) {
            continue;
          }
          const lang::MirBlockId next = published->terminator.target;
          published->terminator = {};
          published->terminator.kind = lang::MirTerminatorKind::Branch;
          published->terminator.value =
              lang::MirOperand{.kind = lang::MirOperandKind::Constant,
                               .literal = lang::Literal{true},
                               .type = lang::SemanticType::Bool};
          published->terminator.target = next;
          published->terminator.elseTarget = published->id;
          reenteredPublication = true;
        }
      },
      "execute storage Initialize more than once", true);
  expect(reenteredPublication,
         "the fixture should expose an actual edge that loops around a "
         "publication block");
}

void testStaticMigrationShells(const lang::FrontendResult &frontend) {
  const std::optional<lang::HirClassInstanceId> registry =
      classFor(frontend, "Registry");
  const std::optional<lang::HirClassInstanceId> plain =
      classFor(frontend, "Plain");
  expect(registry.has_value() && plain.has_value(),
         "fixture class identities should be concrete");

  expectRejected(
      frontend.mir, "a planned static owner cannot restore its legacy body",
      [&](lang::MirProgram &forged) {
        auto &owner = classes(forged)[*registry - 1];
        const lang::PlaceDomain domain =
            owner.staticFieldInitializers.placeDomain;
        owner.staticFieldInitializers = owner.fieldInitializers;
        owner.staticFieldInitializers.kind =
            lang::MirBodyKind::StaticFieldInitializers;
        owner.staticFieldInitializers.placeDomain = domain;
        for (lang::MirPlace &place : owner.staticFieldInitializers.places) {
          if (place.key) {
            place.key->domain = domain;
          }
        }
      });

  lang::MirProgram compatible = frontend.mir;
  auto &unplanned = classes(compatible)[*plain - 1];
  const lang::PlaceDomain domain =
      unplanned.staticFieldInitializers.placeDomain;
  unplanned.staticFieldInitializers = unplanned.fieldInitializers;
  unplanned.staticFieldInitializers.kind =
      lang::MirBodyKind::StaticFieldInitializers;
  unplanned.staticFieldInitializers.placeDomain = domain;
  for (lang::MirPlace &place : unplanned.staticFieldInitializers.places) {
    if (place.key) {
      place.key->domain = domain;
    }
  }
  expect(lang::verifyMirProgram(compatible).valid(),
         "an unplanned class may retain a structurally valid compatibility "
         "static-initializer body");
}

void testLoanStorageOrdering() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-program-initialization/loan.gti", R"(
int32_t& relay(int32_t& value) { return value; }
mut int32_t base = 1;
mut int32_t out = relay(base);
mut int32_t later = 2;
int main() { return out; }
)");
  if (!frontend.mirValid) {
    printDiagnostics(frontend);
    printVerification(lang::verifyMirProgram(frontend.mir));
  }
  expect(frontend.sourceValid && frontend.syntaxValid &&
             frontend.semanticValid && frontend.hirValid &&
             frontend.failureMetadataValid && frontend.mirValid,
         "the loan-order fixture should produce valid MIR");
  if (!frontend.mirValid) {
    return;
  }

  const std::optional<lang::SymbolId> outSymbol = symbolFor(frontend, "out");
  const std::optional<lang::SymbolId> laterSymbol =
      symbolFor(frontend, "later");
  bool changedLoanSource = false;
  expectRejected(
      frontend.mir, "a Loan operand cannot hide a later-storage source",
      [&](lang::MirProgram &forged) {
        lang::MirBody *body = module(forged);
        auto &initialization = plan(forged);
        const lang::MirProgramInitializationStep *out =
            outSymbol ? initialization.findStepForSymbol(*outSymbol) : nullptr;
        const lang::MirProgramInitializationStep *later =
            laterSymbol ? initialization.findStepForSymbol(*laterSymbol)
                        : nullptr;
        const auto loan = std::find_if(body->loans.begin(), body->loans.end(),
                                       [](const lang::MirLoan &candidate) {
                                         return candidate.kind ==
                                                lang::MirLoanKind::CallResult;
                                       });
        if (out == nullptr || later == nullptr || loan == body->loans.end()) {
          return;
        }
        const lang::MirPlaceId oldSource = loan->source;
        loan->source = later->storagePlace;
        for (lang::MirBlock &block : body->blocks) {
          if (block.programInitializationStep != out->id) {
            continue;
          }
          for (lang::MirInstruction &item : block.instructions) {
            for (lang::MirOperand &operand : item.operands) {
              if (operand.kind == lang::MirOperandKind::BorrowRead &&
                  operand.place == oldSource) {
                operand.place = later->storagePlace;
                changedLoanSource = true;
              }
            }
          }
        }
      },
      "program-storage operand/receiver is not accessed strictly", true);
  expect(changedLoanSource,
         "the loan fixture should expose a body-valid CallResult loan/source "
         "retargeting mutation");
}

void testOptimizationCoherence(const lang::FrontendResult &frontend) {
  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility = pipeline.run(
      frontend.hir, lang::OptimizationLevel::O1, lang::TargetInfo::host());
  const lang::OptimizedProgram optimized = pipeline.run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = frontend.mir,
                                .level = lang::OptimizationLevel::O1,
                                .compatibility = &compatibility});
  bool moduleIdentityFold = false;
  bool exactTags = optimized.mir.module().blocks.size() ==
                   optimized.sourceMir.module().blocks.size();
  for (std::size_t index = 0;
       index < optimized.mir.module().blocks.size() && exactTags; ++index) {
    exactTags =
        optimized.mir.module().blocks[index].programInitializationStep ==
        optimized.sourceMir.module().blocks[index].programInitializationStep;
    for (const lang::MirInstruction &item :
         optimized.mir.module().blocks[index].instructions) {
      moduleIdentityFold = moduleIdentityFold ||
                           item.literalProvenance.kind ==
                               lang::MirLiteralProvenanceKind::IdentityFold;
    }
  }
  expect(optimized.valid() && moduleIdentityFold && exactTags &&
             optimized.mir.programInitializationPlan() ==
                 optimized.sourceMir.programInitializationPlan() &&
             lang::verifyMirOptimizationCoherence(optimized.sourceMir,
                                                  optimized.mir)
                 .valid(),
         "O1 should admit an instruction-only Module identity fold while "
         "freezing the plan and every block tag");

  lang::MirProgram changedConstant = frontend.mir;
  auto &steps = plan(changedConstant).steps;
  const auto constant =
      std::find_if(steps.begin(), steps.end(), [](const auto &step) {
        return step.dataInitialization ==
                   lang::MirProgramDataInitializationKind::Constant &&
               step.dataConstant.has_value();
      });
  if (auto *integer =
          std::get_if<lang::ConstantInteger>(&*constant->dataConstant)) {
    ++integer->magnitude;
  }
  expect(
      lang::verifyMirProgram(changedConstant).valid() &&
          !lang::verifyMirOptimizationCoherence(frontend.mir, changedConstant)
               .valid(),
      "optimizer coherence should freeze a valid plan-constant mutation");

  lang::MirProgram changedOwner = frontend.mir;
  const std::optional<lang::HirClassInstanceId> plain =
      classFor(frontend, "Plain");
  auto &ownedSteps = plan(changedOwner).steps;
  const auto staticStep =
      std::find_if(ownedSteps.begin(), ownedSteps.end(), [](const auto &step) {
        return step.storageKind == lang::ProgramStorageKind::StaticField;
      });
  staticStep->ownerClass = plain.value_or(0);
  expect(plain && lang::verifyMirProgram(changedOwner).valid() &&
             !lang::verifyMirOptimizationCoherence(frontend.mir, changedOwner)
                  .valid(),
         "optimizer coherence should freeze a generically valid plan-owner "
         "authority mutation");

  lang::MirProgram changedTag = frontend.mir;
  module(changedTag)->blocks.front().programInitializationStep = 0;
  expect(
      !lang::verifyMirOptimizationCoherence(frontend.mir, changedTag).valid(),
      "optimizer coherence should reject a changed block-step tag");
}

} // namespace

int main() {
  const lang::FrontendResult frontend = analyzeFixture();
  testExactLoweringAndPrinting(frontend);
  if (!frontend.mirValid) {
    return 1;
  }
  testSourceLessSyntheticLowering(frontend);
  testPlanAndStorageMutations(frontend);
  testCfgAndPublicationMutations(frontend);
  testStaticMigrationShells(frontend);
  testLoanStorageOrdering();
  testOptimizationCoherence(frontend);
  return passed ? 0 : 1;
}
