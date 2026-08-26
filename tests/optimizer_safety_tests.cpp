#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/report.h"
#include "gti/optimization/rewrite.h"
#include "gti/optimizer.h"
#include "gti/support.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::HirFunctionInstance *
findFunction(const lang::FrontendResult &frontend, std::string_view name) {
  const auto found =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [&](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == name;
                   });
  return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
}

std::size_t countDischarges(const lang::MirProgram &program,
                            lang::MirSafetyCheckFamily family) {
  std::size_t count = 0;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(program)) {
    const lang::MirBody *body = lang::findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        count += static_cast<std::size_t>(
            std::count_if(instruction.safetyDischarges.begin(),
                          instruction.safetyDischarges.end(),
                          [family](const lang::MirSafetyDischarge &discharge) {
                            return discharge.family == family;
                          }));
      }
    }
  }
  return count;
}

std::size_t functionDischarges(const lang::FrontendResult &frontend,
                               const lang::OptimizedProgram &optimized,
                               std::string_view name,
                               lang::MirSafetyCheckFamily family) {
  const lang::HirFunctionInstance *hir = findFunction(frontend, name);
  const lang::MirFunctionInstance *function =
      hir == nullptr ? nullptr : optimized.mir.findFunctionInstance(hir->id);
  std::size_t count = 0;
  if (function == nullptr) {
    return count;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      count += static_cast<std::size_t>(
          std::count_if(instruction.safetyDischarges.begin(),
                        instruction.safetyDischarges.end(),
                        [family](const lang::MirSafetyDischarge &discharge) {
                          return discharge.family == family;
                        }));
    }
  }
  return count;
}

void testProofCarryingSafetyCheckElimination() {
  const std::string source = R"(
mut int32_t observed = 0;

class MutatesObserved {
public:
  MutatesObserved() { observed = 2147483647; }
};

int32_t fixed_sum() {
  int32_t values[4] = {1, 2, 3, 4};
  mut uint64_t index = 0;
  mut int32_t total = 0;
  while (index < 4) {
    total += values[index];
    index++;
  }
  return total;
}

int32_t dynamic_read(uint64_t index) {
  int32_t values[4] = {1, 2, 3, 4};
  return values[index];
}

int32_t dynamic_add(int32_t left, int32_t right) {
  return left + right;
}

int32_t lifecycle_mutation_barrier() {
  if (observed < 10) {
    MutatesObserved mutation = MutatesObserved();
    return observed + 1;
  }
  return 0;
}

int32_t guarded_math(int32_t value) {
  if (value >= 0) {
    if (value < 10) {
      mut int32_t added = value + 1;
      mut int32_t subtracted = added - 1;
      mut int32_t multiplied = subtracted * 2;
      return -multiplied;
    }
  }
  return 0;
}

int32_t exact_remainder(int32_t value) {
  return value % int32_t(7);
}

int32_t dynamic_remainder(int32_t value, int32_t divisor) {
  return value % divisor;
}

int32_t masked_add(int32_t value) {
  return (value & int32_t(255)) + int32_t(1);
}

extern "C" {
  void analysis_barrier();
}

int64_t bounded_recurrence() {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < 100; index++) {
    total += int64_t(index & uint64_t(255));
  }
  return total;
}

int64_t dynamic_recurrence(uint64_t count) {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < count; index++) {
    total += int64_t(index & uint64_t(255));
  }
  return total;
}

int64_t repeated_recurrence_write() {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < 100; index++) {
    total += int64_t(index & uint64_t(255));
    total += int64_t(1);
  }
  return total;
}

int64_t recurrence_call_barrier() {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < 100; index++) {
    total += int64_t(index & uint64_t(255));
    analysis_barrier();
  }
  return total;
}

int32_t fixed_array_envelope(int32_t value) {
  mut int32_t values[4] = {};
  for (mut uint64_t index = 0; index < 4; index++) {
    values[index] = (value & int32_t(255)) + int32_t(1);
  }
  return values[0] + int32_t(1);
}

int32_t fixed_array_call_barrier(int32_t value) {
  mut int32_t values[4] = {};
  for (mut uint64_t index = 0; index < 4; index++) {
    values[index] = (value & int32_t(255)) + int32_t(1);
  }
  analysis_barrier();
  return values[0] + int32_t(1);
}

int32_t fixed_array_alias_write(int32_t value) {
  mut int32_t values[2] = {1, 2};
  {
    mut int32_t& alias = values[0];
    alias = value;
  }
  return values[1] + int32_t(1);
}

int32_t main() {
  if (fixed_sum() != 10) { return 1; }
  if (guarded_math(4) != -8) { return 2; }
  if (exact_remainder(17) != 3) { return 3; }
  if (masked_add(511) != 256) { return 4; }
  if (bounded_recurrence() != 4950) { return 5; }
  if (fixed_array_envelope(511) != 257) { return 6; }
  return 0;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("proof-carrying-safety.gti", source);
  expect(frontend.canGenerateCode(),
         "proof-carrying safety fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const auto optimize = [&](lang::OptimizationLevel level) {
    return pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
  };
  const lang::OptimizedProgram o0 = optimize(lang::OptimizationLevel::O0);
  const lang::OptimizedProgram o1 = optimize(lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o2 = optimize(lang::OptimizationLevel::O2);
  const lang::OptimizedProgram o3 = optimize(lang::OptimizationLevel::O3);

  expect(o0.valid() && o1.valid() && o2.valid() && o3.valid() &&
             countDischarges(o0.mir, lang::MirSafetyCheckFamily::Bounds) == 0 &&
             countDischarges(o1.mir, lang::MirSafetyCheckFamily::Bounds) == 0 &&
             countDischarges(
                 o0.mir, lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0 &&
             countDischarges(
                 o1.mir, lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
         "O0 and O1 must remain free of safety-check transformations");
  expect(functionDischarges(frontend, o2, "fixed_sum",
                            lang::MirSafetyCheckFamily::Bounds) == 1 &&
             functionDischarges(
                 frontend, o2, "fixed_sum",
                 lang::MirSafetyCheckFamily::ArithmeticOverflow) == 2 &&
             functionDischarges(
                 frontend, o2, "guarded_math",
                 lang::MirSafetyCheckFamily::ArithmeticOverflow) == 4,
         "O2 should discharge the loop bounds/increment and all four guarded "
         "arithmetic forms");
  expect(functionDischarges(frontend, o2, "dynamic_read",
                            lang::MirSafetyCheckFamily::Bounds) == 0 &&
             functionDischarges(
                 frontend, o2, "dynamic_add",
                 lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0 &&
             functionDischarges(
                 frontend, o2, "lifecycle_mutation_barrier",
                 lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
         "dynamic bounds, unconstrained arithmetic, and user lifecycle "
         "mutation barriers must remain checked");
  expect(
      functionDischarges(frontend, o2, "exact_remainder",
                         lang::MirSafetyCheckFamily::ArithmeticDomain) == 1 &&
          functionDischarges(frontend, o2, "dynamic_remainder",
                             lang::MirSafetyCheckFamily::ArithmeticDomain) ==
              0 &&
          functionDischarges(frontend, o2, "masked_add",
                             lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
              1,
      "exact nonzero divisors and exact bit masks should refine integer "
      "facts without proving dynamic divisors");

  const std::size_t boundedRecurrence =
      functionDischarges(frontend, o2, "bounded_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t dynamicRecurrence =
      functionDischarges(frontend, o2, "dynamic_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t repeatedRecurrence =
      functionDischarges(frontend, o2, "repeated_recurrence_write",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t callRecurrence =
      functionDischarges(frontend, o2, "recurrence_call_barrier",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  expect(boundedRecurrence == dynamicRecurrence + 1 &&
             boundedRecurrence == repeatedRecurrence + 1 &&
             boundedRecurrence == callRecurrence + 1,
         "only a finite single-write recurrence without an opaque native "
         "call should gain the accumulator overflow proof");

  const std::size_t arrayEnvelope =
      functionDischarges(frontend, o2, "fixed_array_envelope",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t arrayBarrier =
      functionDischarges(frontend, o2, "fixed_array_call_barrier",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t arrayAliasWrite =
      functionDischarges(frontend, o2, "fixed_array_alias_write",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  expect(arrayEnvelope == arrayBarrier + 1 && arrayAliasWrite == 0,
         "a fixed-array element envelope should prove the later scalar add "
         "and be invalidated by opaque calls or aliasing writes");
  expect(lang::MirPrinter().print(o2.mir) == lang::MirPrinter().print(o3.mir) &&
             countDischarges(o2.mir, lang::MirSafetyCheckFamily::Bounds) ==
                 countDischarges(o3.mir, lang::MirSafetyCheckFamily::Bounds) &&
             countDischarges(o2.mir,
                             lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
                 countDischarges(
                     o3.mir, lang::MirSafetyCheckFamily::ArithmeticOverflow) &&
             countDischarges(o2.mir,
                             lang::MirSafetyCheckFamily::ArithmeticDomain) ==
                 countDischarges(o3.mir,
                                 lang::MirSafetyCheckFamily::ArithmeticDomain),
         "O2 and O3 should derive the same deterministic GTI safety proofs");

  const std::size_t bounds =
      static_cast<std::size_t>(lang::MirSafetyCheckFamily::Bounds);
  const std::size_t overflow =
      static_cast<std::size_t>(lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t domain =
      static_cast<std::size_t>(lang::MirSafetyCheckFamily::ArithmeticDomain);
  expect(o2.report.safety.dischargedByFamily[bounds] > 0 &&
             o2.report.safety.dischargedByFamily[overflow] > 0 &&
             o2.report.safety.dischargedByFamily[domain] > 0 &&
             o2.report.safety.sourceByFamily[bounds] ==
                 o2.report.safety.emittedByFamily[bounds] +
                     o2.report.safety.dischargedByFamily[bounds] &&
             o2.report.safety.sourceByFamily[overflow] ==
                 o2.report.safety.emittedByFamily[overflow] +
                     o2.report.safety.dischargedByFamily[overflow] &&
             o2.report.safety.sourceByFamily[domain] ==
                 o2.report.safety.emittedByFamily[domain] +
                     o2.report.safety.dischargedByFamily[domain],
         "the report should reconcile exact per-family source, emitted, and "
         "discharged checks");
  const std::string report =
      lang::serializeOptimizationReportJson(o2.report, "O2");
  expect(report.find("\"version\":3") != std::string::npos &&
             report.find("\"families\"") != std::string::npos &&
             report.find("\"safetyDischarge\"") != std::string::npos &&
             report.find("\"relatedPlaces\"") != std::string::npos &&
             report.find("\"safetyChecks\":true") != std::string::npos,
         "schema v3 should expose exact safety families, outcomes, and MIR "
         "proof identities plus truthful safety-analysis invalidation");

  const auto firstSafetyRecord = std::find_if(
      o2.report.transformations.begin(), o2.report.transformations.end(),
      [](const lang::MirTransformRecord &record) {
        return record.kind == lang::MirTransformKind::DischargeSafetyCheck;
      });
  expect(firstSafetyRecord != o2.report.transformations.end() &&
             firstSafetyRecord->safetyDischarge.has_value() &&
             firstSafetyRecord->requiredAnalyses.safetyChecks,
         "every removed check should carry one typed outcome and canonical "
         "safety-analysis requirement");
  if (firstSafetyRecord != o2.report.transformations.end()) {
    std::vector<lang::MirTransformRecord> forged = o2.report.transformations;
    const std::size_t index = static_cast<std::size_t>(
        std::distance(o2.report.transformations.begin(), firstSafetyRecord));
    forged[index].relatedInstructions.push_back(
        std::numeric_limits<lang::MirInstructionId>::max());
    std::sort(forged.begin(), forged.end(), lang::mirTransformRecordLess);
    expect(!lang::verifyMirOptimizationCoherence(frontend.mir, o2.mir, forged)
                .valid(),
           "coherence replay should reject forged safety proof evidence");

    forged = o2.report.transformations;
    forged.erase(forged.begin() + static_cast<std::ptrdiff_t>(index));
    expect(!lang::verifyMirOptimizationCoherence(frontend.mir, o2.mir, forged)
                .valid(),
           "coherence replay should reject optimized metadata without its "
           "exact transformation record");
  }

  const auto firstOrdinaryRecord = std::find_if(
      o2.report.transformations.begin(), o2.report.transformations.end(),
      [](const lang::MirTransformRecord &record) {
        return record.kind != lang::MirTransformKind::DischargeSafetyCheck;
      });
  if (firstOrdinaryRecord != o2.report.transformations.end()) {
    std::vector<lang::MirTransformRecord> forged = o2.report.transformations;
    const std::size_t index = static_cast<std::size_t>(
        std::distance(o2.report.transformations.begin(), firstOrdinaryRecord));
    forged[index].relatedPlaces.push_back(
        std::numeric_limits<lang::MirPlaceId>::max());
    std::sort(forged.begin(), forged.end(), lang::mirTransformRecordLess);
    expect(!lang::verifyMirOptimizationCoherence(frontend.mir, o2.mir, forged)
                .valid(),
           "ordinary transform records must reject safety-only proof fields");
  }

  const lang::OptimizedProgram repeated =
      pipeline.run({.hir = frontend.hir,
                    .mir = o2.mir,
                    .level = lang::OptimizationLevel::O2});
  expect(!repeated.valid(),
         "optimized MIR must not be accepted as a fresh canonical source "
         "snapshot");

  const lang::HirFunctionInstance *dynamic =
      findFunction(frontend, "dynamic_read");
  const lang::MirBodyAddress dynamicAddress{
      .kind = lang::MirBodyKind::Function,
      .owner = dynamic == nullptr ? 0 : dynamic->id};
  const lang::MirBody *dynamicBody =
      dynamic == nullptr ? nullptr
                         : lang::findMirBody(frontend.mir, dynamicAddress);
  std::optional<std::pair<lang::MirInstructionId, lang::MirSafetyDischarge>>
      unproven;
  if (dynamicBody != nullptr) {
    for (const lang::MirBlock &block : dynamicBody->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size();
             ++origin) {
          for (const lang::DefinedFailureOutcome outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code == lang::DefinedFailureCode::IndexOutOfBounds &&
                outcome.detail == lang::DefinedFailureDetail::FixedArray) {
              unproven =
                  std::pair{instruction.id,
                            lang::MirSafetyDischarge{
                                .family = lang::MirSafetyCheckFamily::Bounds,
                                .origin = origin,
                                .outcome = outcome}};
            }
          }
        }
      }
    }
  }
  lang::MirProgram editorCandidate = frontend.mir;
  const std::string beforeEditor = lang::MirPrinter().print(editorCandidate);
  lang::MirProgramEditor editor(editorCandidate, "forged-bounds-proof");
  if (unproven) {
    editor.queueSafetyDischarge(
        {.body = dynamicAddress, .instruction = unproven->first},
        unproven->second);
  }
  const lang::MirEditResult rejected = editor.apply();
  expect(unproven.has_value() && !rejected.valid() && !rejected.changed &&
             lang::MirPrinter().print(editorCandidate) == beforeEditor,
         "the controlled editor should atomically reject an unproved dynamic "
         "bounds discharge");
}

} // namespace

int main() {
  lang::installCrashHandlers("gti_optimizer_safety_tests");
  testProofCarryingSafetyCheckElimination();
  if (failures != 0) {
    std::cerr << failures << " optimizer safety test(s) failed\n";
    return 1;
  }
  std::cout << "optimizer safety tests passed\n";
  return 0;
}
