#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/optimization/pass.h"
#include "gti/optimization/report.h"
#include "gti/optimizer.h"
#include "gti/support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

lang::FrontendResult analyzeTrusted(std::string_view name,
                                    const std::string &source) {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const std::filesystem::path entry = standardLibrary / std::string(name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend().analyze(entry, source,
                                  {standardLibrary / "prelude.gti", entry},
                                  {{entryKey, source}}, {standardLibrary});
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

std::size_t functionOperationDischarges(const lang::FrontendResult &frontend,
                                        const lang::OptimizedProgram &optimized,
                                        std::string_view name,
                                        lang::MirSafetyCheckFamily family,
                                        lang::MirOperation operation) {
  const lang::HirFunctionInstance *hir = findFunction(frontend, name);
  const lang::MirFunctionInstance *function =
      hir == nullptr ? nullptr : optimized.mir.findFunctionInstance(hir->id);
  if (function == nullptr) {
    return 0;
  }
  std::size_t count = 0;
  for (const lang::MirBlock &block : function->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.operation != operation) {
        continue;
      }
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

std::size_t operatorDischarges(const lang::OptimizedProgram &optimized,
                               lang::OverloadedOperator operation,
                               lang::MirSafetyCheckFamily family) {
  std::size_t count = 0;
  for (const lang::MirFunctionInstance &function :
       optimized.mir.functionInstances()) {
    if (function.overloadedOperator != operation) {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
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

uint64_t exact_loop_bound(uint64_t count) { return count; }

void observe_recurrence(int64_t value) {}

void replace_recurrence(mut int64_t& value, int64_t replacement) {
  value = replacement;
}

int64_t contextual_bound_recurrence() {
  uint64_t count = 100;
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < exact_loop_bound(count); index++) {
    total += int64_t(index & uint64_t(255));
  }
  return total;
}

int64_t bounded_interval_recurrence(uint64_t count) {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < (count & uint64_t(127)); index++) {
    total += int64_t(index & uint64_t(255));
  }
  return total;
}

int64_t exact_observer_recurrence() {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < exact_loop_bound(uint64_t(100));
       index++) {
    observe_recurrence(total);
    total += int64_t(index & uint64_t(255));
  }
  return total;
}

int64_t mutating_closed_call_recurrence() {
  mut int64_t total = 0;
  for (mut uint64_t index = 0; index < exact_loop_bound(uint64_t(100));
       index++) {
    int64_t element = int64_t(index & uint64_t(255));
    replace_recurrence(total, element);
    total += element;
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
  if (contextual_bound_recurrence() != 4950) { return 7; }
  if (exact_observer_recurrence() != 4950) { return 8; }
  if (mutating_closed_call_recurrence() != 198) { return 9; }
  if (bounded_interval_recurrence(uint64_t(100)) != 4950) { return 10; }
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
  const std::size_t contextualRecurrence =
      functionDischarges(frontend, o2, "contextual_bound_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t boundedIntervalRecurrence =
      functionDischarges(frontend, o2, "bounded_interval_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t observerRecurrence =
      functionDischarges(frontend, o2, "exact_observer_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t mutatingCallRecurrence =
      functionDischarges(frontend, o2, "mutating_closed_call_recurrence",
                         lang::MirSafetyCheckFamily::ArithmeticOverflow);
  const std::size_t mutatingAccumulator = functionOperationDischarges(
      frontend, o2, "mutating_closed_call_recurrence",
      lang::MirSafetyCheckFamily::ArithmeticOverflow,
      lang::MirOperation::AddAssign);
  expect(boundedRecurrence == 2 && dynamicRecurrence == 0 &&
             repeatedRecurrence == 1 && callRecurrence == 0 &&
             contextualRecurrence == 2 && boundedIntervalRecurrence == 2 &&
             observerRecurrence == 2 && mutatingCallRecurrence == 1 &&
             mutatingAccumulator == 0,
         "finite exact or interval bounds and unrelated exact calls should "
         "prove independent counter arithmetic while dynamic bounds, repeated "
         "writes, native calls, and exact calls mutating the accumulator keep "
         "the affected arithmetic checked");

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
  if (firstSafetyRecord != o2.report.transformations.end() &&
      firstSafetyRecord->safetyDischarge) {
    lang::MirProgram cachedCandidate = frontend.mir;
    lang::MirBodyAnalysisManager cachedAnalyses;
    const lang::MirClosedCallSafetyAnalysis *cachedSafety =
        cachedAnalyses.closedCallSafety(cachedCandidate);
    lang::MirEditor cachedEditor(cachedCandidate, "cached-safety-replay-test");
    cachedEditor.queueSafetyDischarge(
        {.body = firstSafetyRecord->body,
         .instruction = firstSafetyRecord->instruction},
        *firstSafetyRecord->safetyDischarge);
    const lang::MirEditResult cachedEdit = cachedEditor.apply(cachedAnalyses);
    expect(cachedSafety != nullptr && cachedSafety->converged &&
               cachedEdit.valid() && cachedEdit.changed &&
               cachedEdit.appliedPatches == 1 &&
               cachedEdit.analysesInvalidated &&
               cachedEdit.invalidatedAnalyses.empty() &&
               cachedAnalyses.revision(firstSafetyRecord->body) == 1 &&
               cachedAnalyses.closedCallSafety(cachedCandidate) == cachedSafety,
           "metadata-only safety edits should reuse the manager's canonical "
           "snapshot while advancing the edited body revision");

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
  lang::MirEditor editor(editorCandidate, "forged-bounds-proof");
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

void testPrefixStorageBoundsRelation() {
  const std::string source = R"(
extern "C" {
  void analysis_barrier();
}

int32_t exact_checked_read(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  return gti_internal::prefix_storage_read(values, index);
}

int32_t different_index(uint64_t checked, uint64_t accessed) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(checked, live);
  return gti_internal::prefix_storage_read(values, accessed);
}

int32_t different_storage(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> checked =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  mut gti_internal::prefix_storage<int32_t> accessed =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(checked, int32_t(7));
  gti_internal::prefix_storage_append(accessed, int32_t(8));
  uint64_t live = gti_internal::prefix_storage_length(checked);
  gti_internal::index_bounds_check(index, live);
  return gti_internal::prefix_storage_read(accessed, index);
}

int32_t append_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  gti_internal::prefix_storage_append(values, int32_t(8));
  return gti_internal::prefix_storage_read(values, index);
}

int32_t pop_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  gti_internal::prefix_storage_append(values, int32_t(8));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  gti_internal::prefix_storage_pop(values);
  return gti_internal::prefix_storage_read(values, index);
}

int32_t insert_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  gti_internal::prefix_storage_insert(values, uint64_t(0), int32_t(8));
  return gti_internal::prefix_storage_read(values, index);
}

int32_t erase_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  gti_internal::prefix_storage_append(values, int32_t(8));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  gti_internal::prefix_storage_erase(values, uint64_t(0));
  return gti_internal::prefix_storage_read(values, index);
}

int32_t relocate_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> source =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  mut gti_internal::prefix_storage<int32_t> destination =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(source, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(source);
  gti_internal::index_bounds_check(index, live);
  gti_internal::prefix_storage_relocate(source, destination);
  return gti_internal::prefix_storage_read(source, index);
}

int32_t opaque_call_after_check(uint64_t index) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  analysis_barrier();
  return gti_internal::prefix_storage_read(values, index);
}

int32_t index_write_after_check(mut uint64_t index, uint64_t replacement) {
  mut gti_internal::prefix_storage<int32_t> values =
      gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4));
  gti_internal::prefix_storage_append(values, int32_t(7));
  uint64_t live = gti_internal::prefix_storage_length(values);
  gti_internal::index_bounds_check(index, live);
  index = replacement;
  return gti_internal::prefix_storage_read(values, index);
}

int32_t main() {
  return exact_checked_read(uint64_t(0)) == int32_t(7) ? 0 : 1;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-prefix-bounds.gti", source);
  expect(frontend.canGenerateCode(),
         "prefix-storage bounds relation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const auto optimize = [&](lang::OptimizationLevel level) {
    return pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
  };
  const lang::OptimizedProgram o2 = optimize(lang::OptimizationLevel::O2);
  const lang::OptimizedProgram o3 = optimize(lang::OptimizationLevel::O3);
  const auto bounds = [&](const lang::OptimizedProgram &optimized,
                          std::string_view name) {
    return functionDischarges(frontend, optimized, name,
                              lang::MirSafetyCheckFamily::Bounds);
  };
  expect(o2.valid() && o3.valid() && bounds(o2, "exact_checked_read") == 2 &&
             bounds(o3, "exact_checked_read") == 2,
         "an exact closed-call index range should discharge the public "
         "logical-size check and its matching private prefix read at O2 and "
         "O3");
  for (const std::string_view name :
       {"different_index", "different_storage", "append_after_check",
        "pop_after_check", "insert_after_check", "erase_after_check",
        "relocate_after_check", "opaque_call_after_check",
        "index_write_after_check"}) {
    expect(bounds(o2, name) == 0 && bounds(o3, name) == 0,
           std::string(name) +
               " must retain the private prefix read bounds check");
  }
}

void testClosedIndexedElementEnvelope() {
  const std::string source = R"(
class CheckedBuffer {
  mut gti_internal::prefix_storage<int32_t> data;

public:
  CheckedBuffer()
      : data(gti_internal::allocate_prefix_storage<int32_t>(uint64_t(4))) {
    gti_internal::prefix_storage_append(this.data, int32_t(1));
    gti_internal::prefix_storage_append(this.data, int32_t(2));
    gti_internal::prefix_storage_append(this.data, int32_t(3));
    gti_internal::prefix_storage_append(this.data, int32_t(4));
  }

  uint64_t size() {
    return gti_internal::prefix_storage_length(this.data);
  }

  int32_t& operator[](uint64_t index) {
    uint64_t live = gti_internal::prefix_storage_length(this.data);
    gti_internal::index_bounds_check(index, live);
    return gti_internal::prefix_storage_read(this.data, index);
  }

  mut int32_t& operator[](uint64_t index) mut {
    uint64_t live = gti_internal::prefix_storage_length(this.data);
    gti_internal::index_bounds_check(index, live);
    return gti_internal::prefix_storage_read_mut(this.data, index);
  }
};

void negate(mut CheckedBuffer& values) {
  for (mut uint64_t index = 0; index < values.size(); index++) {
    values[index] *= int32_t(-1);
  }
}

int32_t closed_indexed_sum(CheckedBuffer& values) {
  mut int32_t total = 0;
  for (mut uint64_t index = 0; index < values.size(); index++) {
    total += values[index];
  }
  return total;
}

int32_t unknown_indexed_sum(CheckedBuffer& values) {
  mut int32_t total = 0;
  for (mut uint64_t index = 0; index < values.size(); index++) {
    total += values[index];
  }
  return total;
}

int32_t main() {
  mut CheckedBuffer values = CheckedBuffer();
  negate(values);
  return closed_indexed_sum(values) == int32_t(-10) ? 0 : 1;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-indexed-envelope.gti", source);
  expect(frontend.canGenerateCode(),
         "closed indexed-element envelope fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const auto optimize = [&](lang::OptimizationLevel level) {
    return pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
  };
  const lang::OptimizedProgram o2 = optimize(lang::OptimizationLevel::O2);
  const lang::OptimizedProgram o3 = optimize(lang::OptimizationLevel::O3);
  const auto overflow = [&](const lang::OptimizedProgram &optimized,
                            std::string_view name) {
    return functionDischarges(frontend, optimized, name,
                              lang::MirSafetyCheckFamily::ArithmeticOverflow);
  };

  expect(o2.valid() && o3.valid() && overflow(o2, "closed_indexed_sum") == 2 &&
             overflow(o3, "closed_indexed_sum") == 2,
         "an exact closed call chain should carry the prefix-storage element "
         "envelope through an indexed return borrow and prove both loop "
         "arithmetic checks");
  expect(overflow(o2, "unknown_indexed_sum") == 0 &&
             overflow(o3, "unknown_indexed_sum") == 0,
         "an indexed receiver without a closed object context must retain "
         "both arithmetic checks");
}

void testClosedIndexedRecordFieldEnvelope() {
  const std::string source = R"(
extern "C" {
  void analysis_barrier();
}

struct CheckedRecord {
  CheckedRecord() {}

  mut int32_t broad = int32_t(2147483647);
  mut int32_t narrow = int32_t(0);
};

class CheckedRecordBuffer {
  mut gti_internal::prefix_storage<CheckedRecord> data;

public:
  CheckedRecordBuffer()
      : data(gti_internal::allocate_prefix_storage<CheckedRecord>(uint64_t(2))) {
    gti_internal::prefix_storage_append(this.data, CheckedRecord());
    gti_internal::prefix_storage_append(this.data, CheckedRecord());
  }

  uint64_t size() {
    return gti_internal::prefix_storage_length(this.data);
  }

  CheckedRecord& operator[](uint64_t index) {
    uint64_t live = gti_internal::prefix_storage_length(this.data);
    gti_internal::index_bounds_check(index, live);
    return gti_internal::prefix_storage_read(this.data, index);
  }

  mut CheckedRecord& operator[](uint64_t index) mut {
    uint64_t live = gti_internal::prefix_storage_length(this.data);
    gti_internal::index_bounds_check(index, live);
    return gti_internal::prefix_storage_read_mut(this.data, index);
  }
};

void increment_narrow(mut CheckedRecordBuffer& values) {
  mut CheckedRecord& value = values[uint64_t(0)];
  value.narrow =
      (value.narrow * int32_t(3) + int32_t(1)) % int32_t(17);
}

void repeat_narrow(mut CheckedRecordBuffer& values) {
  for (mut int32_t tick = 0; tick < int32_t(100); tick++) {
    mut CheckedRecord& value = values[uint64_t(0)];
    int32_t phase = tick % int32_t(17);
    value.narrow =
        (value.narrow * int32_t(3) + phase + int32_t(2)) % int32_t(61) -
        int32_t(30);
  }
}

void repeat_unbounded(mut CheckedRecordBuffer& values, int32_t delta) {
  for (mut int32_t tick = 0; tick < int32_t(100); tick++) {
    mut CheckedRecord& value = values[uint64_t(0)];
    value.narrow = value.narrow + delta;
  }
}

int32_t direct_increment_narrow() {
  mut CheckedRecordBuffer values = CheckedRecordBuffer();
  mut CheckedRecord& value = values[uint64_t(0)];
  value.narrow =
      (value.narrow * int32_t(3) + int32_t(1)) % int32_t(17);
  return value.narrow;
}

void barrier_then_increment(mut CheckedRecordBuffer& values) {
  mut CheckedRecord& value = values[uint64_t(0)];
  analysis_barrier();
  value.narrow =
      (value.narrow * int32_t(3) + int32_t(1)) % int32_t(17);
}

void increment_then_barrier(mut CheckedRecordBuffer& values) {
  mut CheckedRecord& value = values[uint64_t(0)];
  value.narrow =
      (value.narrow * int32_t(3) + int32_t(1)) % int32_t(17);
  analysis_barrier();
}

int32_t main() {
  mut CheckedRecordBuffer values = CheckedRecordBuffer();
  int32_t direct = direct_increment_narrow();
  increment_narrow(values);
  repeat_narrow(values);
  increment_then_barrier(values);
  barrier_then_increment(values);
  return direct;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-indexed-record-envelope.gti", source);
  expect(frontend.canGenerateCode(),
         "closed indexed-record envelope fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const auto optimize = [&](lang::OptimizationLevel level) {
    return pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
  };
  const lang::OptimizedProgram o2 = optimize(lang::OptimizationLevel::O2);
  const lang::OptimizedProgram o3 = optimize(lang::OptimizationLevel::O3);
  const auto overflow = [&](const lang::OptimizedProgram &optimized,
                            std::string_view name) {
    return functionDischarges(frontend, optimized, name,
                              lang::MirSafetyCheckFamily::ArithmeticOverflow);
  };

  expect(o2.valid() && o3.valid() && overflow(o2, "increment_narrow") == 2 &&
             overflow(o3, "increment_narrow") == 2,
         "a closed record constructor should preserve independent field "
         "envelopes through a retained indexed mutable reference");
  expect(overflow(o2, "direct_increment_narrow") == 2 &&
             overflow(o3, "direct_increment_narrow") == 2,
         "a direct call-result loan retained by a local reference should "
         "preserve its indexed-record field envelope");
  const std::size_t repeatNarrowO2 = overflow(o2, "repeat_narrow");
  const std::size_t repeatNarrowO3 = overflow(o3, "repeat_narrow");
  const std::size_t repeatUnboundedO2 = overflow(o2, "repeat_unbounded");
  const std::size_t repeatUnboundedO3 = overflow(o3, "repeat_unbounded");
  expect(repeatNarrowO2 == 5 && repeatNarrowO3 == 5,
         "a result-bounded record field update should preserve its envelope "
         "across a finite loop and discharge its field arithmetic plus the "
         "guard-bounded loop increment");
  expect(repeatUnboundedO2 == 0 && repeatUnboundedO3 == 0,
         "an unbounded record field recurrence must retain its field check");
  expect(overflow(o2, "increment_then_barrier") == 2 &&
             overflow(o3, "increment_then_barrier") == 2,
         "a later opaque call must not discard converged contextual proofs "
         "established before the barrier");
  expect(overflow(o2, "barrier_then_increment") == 0 &&
             overflow(o3, "barrier_then_increment") == 0,
         "an opaque call must invalidate retained indexed-record field "
         "envelopes before arithmetic mutation");
}

void testGuardedClosedCallContextWidening() {
  const std::string source = R"(
struct ContextEntity {
  ContextEntity() {}

  mut int32_t position_x = 0;
  mut int32_t position_y = 0;
  mut int32_t velocity_x = 0;
  mut int32_t velocity_y = 0;
};

class ContextBuffer {
  mut gti_internal::prefix_storage<ContextEntity> data;

public:
  ContextBuffer()
      : data(gti_internal::allocate_prefix_storage<ContextEntity>(uint64_t(4))) {
    gti_internal::prefix_storage_append(this.data, ContextEntity());
    gti_internal::prefix_storage_append(this.data, ContextEntity());
    gti_internal::prefix_storage_append(this.data, ContextEntity());
    gti_internal::prefix_storage_append(this.data, ContextEntity());
  }

  uint64_t size() {
    return gti_internal::prefix_storage_length(this.data);
  }

  mut ContextEntity& operator[](uint64_t index) mut {
    uint64_t live = gti_internal::prefix_storage_length(this.data);
    gti_internal::index_bounds_check(index, live);
    return gti_internal::prefix_storage_read_mut(this.data, index);
  }
};

void update(mut ContextBuffer& entities) {
  for (mut uint64_t index = 0; index < uint64_t(4); index++) {
    mut ContextEntity& entity = entities[index];
    entity.position_x = int32_t(index % uint64_t(7)) - int32_t(3);
    entity.position_y = int32_t(index % uint64_t(5)) - int32_t(2);
    entity.velocity_x = int32_t(index % uint64_t(3)) - int32_t(1);
    entity.velocity_y = int32_t(index % uint64_t(2));
  }

  for (mut int32_t tick = 0; tick < int32_t(16); tick++) {
    int32_t phase = tick % int32_t(7);
    for (mut uint64_t index = 0; index < uint64_t(4); index++) {
      mut ContextEntity& entity = entities[index];
      int32_t lane = int32_t(index % uint64_t(3));
      entity.velocity_x =
          (entity.velocity_x * int32_t(3) + phase + lane) % int32_t(17) -
          int32_t(8);
      entity.velocity_y =
          (entity.velocity_y * int32_t(5) + phase + lane) % int32_t(19) -
          int32_t(9);
      entity.position_x =
          (entity.position_x + entity.velocity_x + int32_t(64)) %
              int32_t(127) -
          int32_t(63);
      entity.position_y =
          (entity.position_y + entity.velocity_y + int32_t(64)) %
              int32_t(127) -
          int32_t(63);
    }
  }
}

int32_t main() {
  mut ContextBuffer entities = ContextBuffer();
  update(entities);
  return entities[uint64_t(0)].position_x;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-guarded-call-context.gti", source);
  expect(frontend.canGenerateCode(),
         "guarded closed-call context fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const auto optimize = [&](lang::OptimizationLevel level) {
    return pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
  };
  const lang::OptimizedProgram o2 = optimize(lang::OptimizationLevel::O2);
  const lang::OptimizedProgram o3 = optimize(lang::OptimizationLevel::O3);
  const auto bounds = [&](const lang::OptimizedProgram &optimized) {
    return operatorDischarges(optimized, lang::OverloadedOperator::Subscript,
                              lang::MirSafetyCheckFamily::Bounds);
  };

  expect(o2.valid() && o3.valid() && bounds(o2) == 2 && bounds(o3) == 2,
         "guard-refined loop indices and exact storage lengths should survive "
         "bounded closed-call context widening and discharge both bounds "
         "checks");
}

// A multidimensional fixed-array element access folds every dimension into one
// place and one bounds site, so the site describes the whole chain. Proving the
// last index alone would let the emitted text drop the earlier dimension's
// bound and subscript past the array, so the discharge must hold for every
// dimension or for none.
void testChainedFixedArrayDimensionBounds() {
  const std::string source = R"(
int32_t proven_chain() {
  mut int32_t matrix[2][3] = {};
  mut uint64_t row = uint64_t(0);
  while (row < uint64_t(1)) {
    row = row + uint64_t(1);
  }
  mut uint64_t column = uint64_t(0);
  while (column < uint64_t(2)) {
    column = column + uint64_t(1);
  }
  return matrix[row][column];
}

int32_t dynamic_parent_chain(uint64_t row) {
  mut int32_t matrix[2][3] = {};
  mut uint64_t column = uint64_t(0);
  while (column < uint64_t(2)) {
    column = column + uint64_t(1);
  }
  return matrix[row][column];
}

int32_t dynamic_terminal_chain(uint64_t column) {
  mut int32_t matrix[2][3] = {};
  mut uint64_t row = uint64_t(0);
  while (row < uint64_t(1)) {
    row = row + uint64_t(1);
  }
  return matrix[row][column];
}

// The dynamic entries stay uncalled so no closed-call context can refine
// their parameters into constants; the bounds-site count below proves the
// accesses survive rather than passing vacuously.
int32_t main() { return proven_chain(); }
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("chained-fixed-array-bounds.gti", source);
  expect(frontend.canGenerateCode(),
         "the chained fixed-array bounds fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizedProgram o3 =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O3});
  const auto bounds = [&](std::string_view name) {
    return functionDischarges(frontend, o3, name,
                              lang::MirSafetyCheckFamily::Bounds);
  };
  const auto boundsSites = [&](std::string_view name) {
    const lang::HirFunctionInstance *hir = findFunction(frontend, name);
    const lang::MirFunctionInstance *function =
        hir == nullptr ? nullptr : o3.mir.findFunctionInstance(hir->id);
    std::size_t count = 0;
    if (function == nullptr) {
      return count;
    }
    for (const lang::MirBlock &block : function->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (const lang::DefinedFailureOrigin &origin :
             instruction.definedFailure.localOrigins) {
          count += static_cast<std::size_t>(
              std::count_if(origin.outcomes.begin(), origin.outcomes.end(),
                            [](const lang::DefinedFailureOutcome &outcome) {
                              return outcome.code ==
                                     lang::DefinedFailureCode::IndexOutOfBounds;
                            }));
        }
      }
    }
    return count;
  };

  expect(o3.valid() && boundsSites("proven_chain") == 1 &&
             boundsSites("dynamic_parent_chain") == 1 &&
             boundsSites("dynamic_terminal_chain") == 1,
         "each chained element access should carry exactly one bounds site "
         "for the whole chain");
  expect(o3.valid() && bounds("proven_chain") == 1,
         "a chained element access whose every dimension is proven should "
         "discharge its single bounds site");
  expect(o3.valid() && bounds("dynamic_parent_chain") == 0 &&
             bounds("dynamic_terminal_chain") == 0,
         "an unproven dimension anywhere in the chain must leave the access "
         "checked, whichever dimension it is");
}

} // namespace

int main() {
  lang::installCrashHandlers("gti_optimizer_safety_tests");
  testProofCarryingSafetyCheckElimination();
  testPrefixStorageBoundsRelation();
  testClosedIndexedElementEnvelope();
  testClosedIndexedRecordFieldEnvelope();
  testGuardedClosedCallContextWidening();
  testChainedFixedArrayDimensionBounds();
  if (failures != 0) {
    std::cerr << failures << " optimizer safety test(s) failed\n";
    return 1;
  }
  std::cout << "optimizer safety tests passed\n";
  return 0;
}
