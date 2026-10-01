#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/optimization/coverage.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Enum> void census(Enum last) {
  for (int index = 0; index <= static_cast<int>(last); ++index) {
    // Code 14 is the reserved cleanup emergency envelope, not an outcome.
    if constexpr (std::is_same_v<Enum, lang::DefinedFailureCode>) {
      if (index == 14)
        continue;
    }
    const auto coverage = lang::optimizationCoverage(static_cast<Enum>(index));
    expect(coverage.classified() && !coverage.consumer.empty(),
           "every vocabulary member must identify a consumer or barrier");
  }
  expect(!lang::optimizationCoverage(static_cast<Enum>(255)).classified(),
         "unknown vocabulary must fail closed");
}

void testCensus() {
  using namespace lang;
  census(MirInstructionKind::CleanupBeforeAssign);
  census(MirOperation::PostDecrement);
  census(IntrinsicKind::SourceColumn);
  census(MirTerminatorKind::TerminateCleanupFailure);
  census(MirProjectionKind::Base);
  census(MirPlaceRootKind::Loan);
  census(MirBodyKind::HostedStartup);
  census(MirOperandKind::Loan);
  census(MirLifecycleEventKind::Drop);
  census(DefinedFailureCode::InvalidContainerArgument);
  census(DefinedFailureDetail::SharedOwner);
  census(FailurePropagationKind::Destructor);
  census(SynchronizationOperationKind::ConditionNotify);
  census(AtomicMemoryOrder::SequentiallyConsistent);
  census(UnsafeOperationKind::DeclaredUnsafeCall);
  census(MirAnalysisKind::SparseConstants);
  census(MirTransformKind::DevirtualizeCall);
  census(MirTransformProofKind::ExactReceiver);
  census(MirSafetyCheckFamily::ArithmeticDomain);
  expect(
      !optimizationCoverage(static_cast<DefinedFailureCode>(14)).classified(),
      "reserved cleanup failure envelope is not a local outcome");

  for (const auto effect :
       {effects(static_cast<MirInstructionKind>(255)),
        effects(static_cast<MirOperation>(255)),
        effects(static_cast<IntrinsicKind>(255)),
        effects(static_cast<SynchronizationOperationKind>(255))}) {
    expect(effect.mayTrap && effect.writesUnknownMemory &&
               effect.maySynchronize && effect.dropsValue &&
               effect.movesValue && effect.startsLoan && !effect.speculatable &&
               !effect.removableWhenUnused,
           "unknown effects retain every relevant optimization barrier");
  }
}

lang::MirBodyAddress functionAddress(const lang::FrontendResult &frontend,
                                     std::string_view name) {
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name)
      return {lang::MirBodyKind::Function, function.id};
  }
  expect(false, "fixture function must exist");
  return {};
}

void testUnknownVocabulary(const lang::FrontendResult &frontend) {
  using namespace lang;
  const auto address = functionAddress(frontend, "constant");
  const std::vector<std::function<void(MirBody &)>> mutations = {
      [](MirBody &body) { body.kind = static_cast<MirBodyKind>(255); },
      [](MirBody &body) {
        body.blocks.front().terminator.kind =
            static_cast<MirTerminatorKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().kind =
            static_cast<MirInstructionKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().operation =
            static_cast<MirOperation>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().intrinsic =
            static_cast<IntrinsicKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().unsafeOperation =
            static_cast<UnsafeOperationKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().synchronization.kind =
            static_cast<SynchronizationOperationKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().synchronization.order =
            static_cast<AtomicMemoryOrder>(255);
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().operands.push_back(
            {.kind = static_cast<MirOperandKind>(255)});
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().lifecycle.push_back(
            {.kind = static_cast<MirLifecycleEventKind>(255)});
      },
      [](MirBody &body) {
        body.blocks.front().instructions.front().definedFailure.propagation =
            static_cast<FailurePropagationKind>(255);
      },
      [](MirBody &body) {
        body.blocks.front()
            .instructions.front()
            .definedFailure.localOrigins.push_back(
                {.outcomes = {{static_cast<DefinedFailureCode>(255),
                               DefinedFailureDetail::Addition}}});
      },
      [](MirBody &body) {
        body.blocks.front()
            .instructions.front()
            .definedFailure.localOrigins.push_back(
                {.outcomes = {{DefinedFailureCode::IntegerOverflow,
                               static_cast<DefinedFailureDetail>(255)}}});
      },
      [](MirBody &body) {
        body.places.push_back({.root = static_cast<MirPlaceRootKind>(255)});
      },
      [](MirBody &body) {
        body.places.push_back(
            {.projections = {{.kind = static_cast<MirProjectionKind>(255)}}});
      },
  };
  for (const auto &mutate : mutations) {
    MirProgram program = frontend.mir;
    MirBody *body = findMirBody(program, address);
    if (body == nullptr || body->blocks.front().instructions.empty()) {
      expect(false, "constant fixture must contain an instruction");
      return;
    }
    mutate(*body);
    expect(!verifyMirBody(*body).valid() && !verifyMirProgram(program).valid(),
           "unknown family cannot enter verified MIR/backend input");
    expect(!computeMirDominance(*body) &&
               !analyzeMirSccp(program, address).complete &&
               !analyzeMirClosedCallSafetyChecks(program).converged,
           "unknown families cannot seed dominance, sparse or range proofs");
    MirBodyAnalysisManager analyses;
    expect(analyses.cfg(program, address) == nullptr,
           "unknown terminators cannot be mistaken for CFG exits");
    expect(!verifyMirOptimizationCoherence(program, program, {}).valid(),
           "identity replay must still reject unknown vocabulary");
    auto result = OptimizationPipeline().run({.hir = frontend.hir,
                                              .mir = program,
                                              .level = OptimizationLevel::O2,
                                              .options = {.verifyMir = false}});
    expect(
        !result.valid() && result.report.transformations.empty() &&
            result.report.passes.empty(),
        "disabling optional verification cannot bypass vocabulary admission");
  }

  MirPlace left{.id = 1,
                .root = MirPlaceRootKind::Binding,
                .binding = 1,
                .projections = {{.kind = static_cast<MirProjectionKind>(255)}}};
  MirPlace right{.id = 2, .root = MirPlaceRootKind::Binding, .binding = 2};
  expect(mirPlaceRelation(frontend.mir, left, right) ==
                 MirAliasRelation::MayAlias &&
             mirPlaceRelation(frontend.mir, left, left) ==
                 MirAliasRelation::MayAlias,
         "unknown projections never establish disjoint or exact alias facts");
}

void testPreservation(const lang::FrontendResult &frontend) {
  using namespace lang;
  const auto unknown = static_cast<MirAnalysisKind>(255);
  expect(
      !MirPreservedAnalyses::all().preserves(unknown) &&
          !MirPreservedAnalyses::only({unknown}).preserves(unknown) &&
          !MirPreservedAnalyses::all().preserves(MirAnalysisKind::Count),
      "unknown preservation queries fail closed without oversized bit shifts");
  const auto address = functionAddress(frontend, "constant");
  MirBodyAnalysisManager analyses;
  expect(analyses.safetyChecks(frontend.mir, address) != nullptr,
         "populate local and closed-call safety caches");
  const auto invalidated = analyses.invalidate(
      address, MirPreservedAnalyses::only({MirAnalysisKind::ClosedCallSafety}));
  expect(std::find(invalidated.begin(), invalidated.end(),
                   name(MirAnalysisKind::SafetyChecks)) != invalidated.end(),
         "preserving closed-call results must not implicitly preserve a local "
         "safety cache");
  expect(std::find(invalidated.begin(), invalidated.end(),
                   name(MirAnalysisKind::ClosedCallSafety)) ==
             invalidated.end(),
         "explicitly preserved closed-call cache remains preserved");
}

void testComputeEffects(const lang::FrontendResult &frontend) {
  using namespace lang;
  const auto address = functionAddress(frontend, "guarded_array");
  const auto localProofs = [&](const MirProgram &program) {
    return analyzeMirSafetyChecks(program, address,
                                  analyzeMirPlaceAliasEscape(program, address));
  };
  const auto bounds = [](const MirSafetyAnalysis &analysis) {
    return std::count_if(analysis.proofs.begin(), analysis.proofs.end(),
                         [](const MirSafetyProof &proof) {
                           return proof.discharge.family ==
                                  MirSafetyCheckFamily::Bounds;
                         });
  };
  expect(bounds(localProofs(frontend.mir)) != 0,
         "baseline place facts must prove the later array read");
  MirProgram changed = frontend.mir;
  MirBody *body = findMirBody(changed, address);
  bool mutated = false;
  for (auto &block : body->blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.literal &&
          std::get_if<bool>(&*instruction.literal) != nullptr) {
        // Model a future Compute family carrying a newly classified memory
        // effect. Direct range analysis must honor the effects rather than
        // trusting the Compute tag or requiring a matching transfer function.
        instruction.rawMemoryAccess = true;
        mutated = true;
      }
    }
  }
  expect(mutated && bounds(localProofs(changed)) == 0,
         "effectful Compute is a memory barrier even before scalar "
         "interpretation");
}

void testKnownBarriers(const lang::FrontendResult &frontend) {
  using namespace lang;
  const auto *mainBody =
      findMirBody(frontend.mir, functionAddress(frontend, "main"));
  bool checkedExactCall = false;
  for (const auto &block : mainBody->blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Call &&
          exactMirInvocationSummary(frontend.mir, *mainBody, instruction)) {
        const auto reason =
            mirOptimizationBarrierReason(frontend.mir, instruction, mainBody);
        expect(reason.find("without a closed effect summary") ==
                   std::string::npos,
               "known exact calls must not be misreported as unknown calls");
        checkedExactCall = true;
      }
    }
  }
  expect(checkedExactCall, "fixture must exercise a known exact call");
  const auto baseline = MirPrinter().print(frontend.mir);
  const auto o0 = OptimizationPipeline().run({.hir = frontend.hir,
                                              .mir = frontend.mir,
                                              .level = OptimizationLevel::O0});
  expect(o0.valid() && o0.report.passes.empty() &&
             o0.report.transformations.empty() &&
             MirPrinter().print(o0.mir) == baseline,
         "O0 remains transformation-free and structurally identical");
  for (const auto level : {OptimizationLevel::O2, OptimizationLevel::O3}) {
    const auto optimized = OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid() &&
               verifyMirOptimizationCoherence(frontend.mir, optimized.mir,
                                              optimized.report.transformations)
                   .valid(),
           "supported proofs independently replay beside conservative "
           "user-type barriers");
    const auto address = functionAddress(frontend, "opaque_boundary");
    const auto *body = findMirBody(optimized.mir, address);
    expect(body != nullptr, "opaque user-type fixture body remains present");
    if (body != nullptr) {
      std::size_t calls = 0;
      std::size_t discharged = 0;
      for (const auto &block : body->blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind == MirInstructionKind::Call)
            ++calls;
          discharged += instruction.safetyDischarges.size();
        }
      }
      expect(calls != 0 && discharged == 0,
             "native boundary on unrelated user record retains calls and "
             "unproved checks");
    }
    const std::string remarks =
        formatOptimizationRemarks(optimized.report, "test");
    expect(remarks.find("call without a closed effect summary") !=
                   std::string::npos &&
               remarks.find("unknown memory/alias effects") !=
                   std::string::npos &&
               remarks.find("unsupported failure discharge family") !=
                   std::string::npos,
           "missed remarks explain unknown-call, alias and unsupported failure "
           "boundaries");
    const auto repeated = OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(serializeOptimizationReportJson(optimized.report, "test") ==
               serializeOptimizationReportJson(repeated.report, "test"),
           "barrier reports remain deterministic");
  }

  MirInstruction raw;
  raw.rawMemoryAccess = true;
  raw.unsafeOperation = UnsafeOperationKind::RawDereference;
  expect(mirOptimizationBarrierReason(frontend.mir, raw)
                 .find("unsafe/raw-memory") != std::string::npos,
         "raw-memory reason derives from MIR metadata");
  MirInstruction synchronization;
  synchronization.kind = MirInstructionKind::Call;
  synchronization.synchronization.kind =
      SynchronizationOperationKind::AtomicLoad;
  expect(mirOptimizationBarrierReason(frontend.mir, synchronization)
                 .find("synchronization") != std::string::npos,
         "even an atomic load is a synchronization barrier");
  MirInstruction cleanup;
  cleanup.kind = MirInstructionKind::Drop;
  expect(mirOptimizationBarrierReason(frontend.mir, cleanup)
                 .find("cleanup/lifecycle") != std::string::npos,
         "cleanup reason derives from the authoritative effect model");
}
} // namespace

int main() {
  testCensus();
  const auto frontend =
      lang::Frontend().analyze("optimizer-conformance.gti", R"(
struct UserRecord { mut int32_t value = 1; };
extern "C" { void analysis_barrier(); }
int constant() { return (42); }
int guarded_array() {
  mut int index = 1;
  int values[2] = {3, 4};
  bool fence = true;
  return values[index];
}
int32_t opaque_boundary(mut UserRecord& record, int32_t divisor) {
  if (record.value >= 0 && record.value < 10) {
    analysis_barrier();
    return (record.value + 1) / divisor;
  }
  return 0;
}
int main() { return constant(); }
)");
  expect(frontend.canGenerateCode(),
         "conformance fixture must lower successfully");
  if (frontend.canGenerateCode()) {
    testUnknownVocabulary(frontend);
    testPreservation(frontend);
    testComputeEffects(frontend);
    testKnownBarriers(frontend);
  }
  return failures == 0 ? 0 : 1;
}
