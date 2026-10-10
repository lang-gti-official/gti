#include "gti/frontend.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
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

void testClosedCallMutableInputInvalidation() {
  const std::string source = R"(
mut int32_t shared = 0;
void set_max(mut int32_t& value) { value = 2147483647; }
void forward_set(mut int32_t& value) { set_max(value); }
void set_global() { shared = 2147483647; }
void observe(int32_t& value) { int32_t copy = value; }
void opaque();
void expose(mut int32_t& value);
int32_t exact_leaf() { return 7; }
int32_t exact_nested() { return exact_leaf(); }

class View {
  mut int32_t& value;
public:
  View(mut int32_t& source) : value(source) {}
  void set_maximum() mut {
    mut int32_t& target = this.value;
    target = 2147483647;
  }
};
class Counter {
  mut int32_t value = 0;
public:
  Counter() {}
  void set(int32_t replacement) mut { this.value = replacement; }
  int32_t get() { return this.value; }
};

int32_t direct() {
  mut int32_t value = 0;
  set_max(value);
  return value + int32_t(1);
}
int32_t forwarded() {
  mut int32_t value = 0;
  forward_set(value);
  return value + int32_t(1);
}
int32_t projected() {
  mut int32_t values[2] = {0, 0};
  set_max(values[1]);
  return values[1] + int32_t(1);
}
int32_t carrier() {
  mut int32_t value = 0;
  { mut View view{value}; view.set_maximum(); }
  return value + int32_t(1);
}
int32_t carrier_shell() {
  mut int32_t value = 0;
  {
    mut View view{value};
    mut View& shell = view;
    shell.set_maximum();
  }
  return value + int32_t(1);
}
int32_t local_reference() {
  mut int32_t value = 0;
  { mut int32_t& alias = value; set_max(alias); }
  return value + int32_t(1);
}
int32_t global() {
  shared = 0;
  set_global();
  return shared + int32_t(1);
}
int32_t global_loop() {
  shared = 0;
  for (mut int32_t index = 0; index < 2; index++) {
    set_global();
    shared = shared + int32_t(1);
  }
  return shared;
}
int32_t unresolved() {
  mut int32_t value = 0;
  opaque();
  return value + int32_t(1);
}
int32_t unresolved_exposed() {
  mut int32_t value = 0;
  expose(value);
  opaque();
  return value + int32_t(1);
}
int32_t unrelated() {
  mut int32_t value = 0;
  mut int32_t other = 0;
  set_max(other);
  return value + int32_t(1);
}
int32_t readonly() {
  mut int32_t value = 0;
  observe(value);
  return value + int32_t(1);
}
int32_t nested_exact() {
  mut int32_t value = 0;
  [[discard]] exact_nested();
  return value + int32_t(1);
}
int32_t object_postcondition() {
  mut Counter counter{};
  counter.set(7);
  return counter.get() + int32_t(1);
}
int32_t main() {
  if (shared == 0) { return direct(); }
  if (shared == 1) { return forwarded(); }
  if (shared == 2) { return projected(); }
  if (shared == 3) { return carrier(); }
  if (shared == 4) { return global(); }
  if (shared == 5) { return global_loop(); }
  if (shared == 6) { return carrier_shell(); }
  if (shared == 7) { return local_reference(); }
  if (shared == 8) { return unrelated(); }
  if (shared == 9) { return readonly(); }
  if (shared == 10) { return nested_exact(); }
  if (shared == 11) { return object_postcondition(); }
  if (shared == 12) { return unresolved_exposed(); }
  return unresolved();
}
)";
  const auto frontend =
      lang::Frontend().analyze("closed-call-mutable-inputs.gti", source);
  expect(frontend.canGenerateCode(),
         "closed mutable-input fixtures must pass source analysis");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto *direct = findFunction(frontend, "direct");
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = direct ? direct->id : 0};
  const auto *body = lang::findMirBody(frontend.mir, address);
  std::optional<lang::MirInstructionId> addition;
  std::optional<lang::MirSafetyDischarge> discharge;
  if (body) {
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Add)
          continue;
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size();
             ++origin) {
          for (const auto outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code == lang::DefinedFailureCode::IntegerOverflow) {
              addition = instruction.id;
              discharge = lang::MirSafetyDischarge{
                  .family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                  .origin = origin,
                  .outcome = outcome};
            }
          }
        }
      }
    }
  }
  expect(addition && discharge,
         "the direct call client must retain an exact overflow origin");
  const auto closed = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  expect(
      closed.converged,
      "closed-call mutable-input analysis must converge within its budget: " +
          std::to_string(closed.iterations) + " maximum per-body analyses");
  for (const std::string_view name :
       {"direct", "forwarded", "projected", "carrier", "carrier_shell",
        "local_reference", "global", "global_loop", "unrelated", "readonly",
        "nested_exact", "object_postcondition"}) {
    const auto *function = findFunction(frontend, name);
    const auto *context =
        function ? closed.find({lang::MirBodyKind::Function, function->id})
                 : nullptr;
    expect(context && context->reached,
           std::string(name) + " must be reached by the contextual analysis");
  }
  const lang::OptimizationPipeline pipeline;
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "mutable call invalidation must remain verified");
    for (const std::string_view name :
         {"direct", "forwarded", "projected", "carrier", "carrier_shell",
          "local_reference", "global", "unresolved_exposed"}) {
      expect(
          functionOperationDischarges(
              frontend, optimized, name,
              lang::MirSafetyCheckFamily::ArithmeticOverflow,
              lang::MirOperation::Add) == 0,
          "mutated referent facts must not discharge the following overflow");
    }
    if (level == lang::OptimizationLevel::O0)
      continue;
    // An unknown callee that never received a local cannot reach it.
    for (const std::string_view name : {"unrelated", "readonly", "nested_exact",
                                        "object_postcondition", "unresolved"}) {
      const auto actual = functionOperationDischarges(
          frontend, optimized, name,
          lang::MirSafetyCheckFamily::ArithmeticOverflow,
          lang::MirOperation::Add);
      expect(actual == 1, std::string(name) +
                              " must preserve its exact "
                              "post-call proof: " +
                              std::to_string(actual) + " actual, 1 expected");
    }
    expect(functionOperationDischarges(
               frontend, optimized, "global_loop",
               lang::MirSafetyCheckFamily::ArithmeticOverflow,
               lang::MirOperation::Add) == 1,
           "global mutation must block the affected recurrence while keeping "
           "the independent loop counter provable");
    expect(lang::verifyMirOptimizationCoherence(
               frontend.mir, optimized.mir, optimized.report.transformations)
               .valid(),
           "mutable call invalidation evidence must independently replay");
    if (addition && discharge) {
      auto forged = optimized.mir;
      for (auto &block : lang::findMirBody(forged, address)->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == *addition)
            instruction.safetyDischarges.push_back(*discharge);
      expect(!lang::verifyMirOptimizationCoherence(
                  frontend.mir, forged, optimized.report.transformations)
                  .valid(),
             "replay must reject a fabricated post-mutation safety discharge");
    }
  }
  if (addition && discharge) {
    auto candidate = frontend.mir;
    const std::string before = lang::MirPrinter().print(candidate);
    lang::MirEditor editor(candidate, "forged-mutable-call-proof");
    editor.queueSafetyDischarge({.body = address, .instruction = *addition},
                                *discharge);
    const auto edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               lang::MirPrinter().print(candidate) == before,
           "the editor must reject stale pre-call scalar facts atomically");

    for (const bool cycle : {false, true}) {
      auto malformed = frontend.mir;
      auto *malformedBody = lang::findMirBody(malformed, address);
      bool changed = false;
      for (auto &block : malformedBody->blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::CallInput ||
              instruction.callInputKind !=
                  lang::HirCallInputKind::MutableBorrow ||
              !instruction.result)
            continue;
          if (cycle) {
            instruction.operands.front() = {.kind = lang::MirOperandKind::Value,
                                            .value = *instruction.result,
                                            .type = instruction.info.type};
          } else {
            instruction.operands.clear();
          }
          changed = true;
        }
      }
      (void)lang::rebuildMirValueUses(*malformedBody);
      expect(changed && !lang::verifyMirProgram(malformed).valid(),
             "malformed staged-input graphs must fail production MIR "
             "verification");
      const auto analysis = lang::analyzeMirClosedCallSafetyChecks(malformed);
      const auto *analyzed = analysis.find(address);
      expect(analyzed == nullptr ||
                 analyzed->safety.find(*addition, *discharge) == nullptr,
             "standalone safety queries must fail closed on unknown or cyclic "
             "staged-input identity without certifying malformed MIR");
    }
  }
}

void testSynchronizingCallInvalidation() {
  const auto frontend = analyzeTrusted("optimizer-sync-call.gti", R"(
#include <std/atomic>
std::atomic<int32_t> signal{0};
void synchronize(std::atomic<int32_t>& signal) {
  [[discard]] signal.load();
}
int32_t synchronized_loop(std::atomic<int32_t>& signal) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 2; index++) {
    synchronize(signal);
    total += 1;
  }
  return total;
}
void synchronized_shared(std::atomic<int32_t>& signal, mut int32_t& total) {
  total = 0;
  synchronize(signal);
  total += 1;
}
int main() {
  mut int32_t shared = 0;
  synchronized_shared(signal, shared);
  return synchronized_loop(signal) + shared;
}
)");
  expect(frontend.canGenerateCode(),
         "a real synchronizing call fixture must pass source/MIR verification");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  bool hasSynchronization = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir))
    for (const auto &block : lang::findMirBody(frontend.mir, address)->blocks)
      for (const auto &instruction : block.instructions)
        hasSynchronization |= instruction.synchronization.kind ==
                              lang::SynchronizationOperationKind::AtomicLoad;
  expect(hasSynchronization,
         "the negative must contain an actual MIR atomic-load boundary");
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  const auto *function = findFunction(frontend, "synchronized_loop");
  const auto *summary =
      function ? analysis.find({lang::MirBodyKind::Function, function->id})
               : nullptr;
  expect(analysis.converged && summary && summary->reached && !summary->closed,
         "an actual synchronization boundary must not export a complete "
         "closed recurrence summary");
  for (const auto level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const auto optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid() &&
               functionDischarges(
                   frontend, optimized, "synchronized_shared",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
           "a synchronizing call must retain checks on storage another "
           "thread may reach");
    const auto local =
        functionDischarges(frontend, optimized, "synchronized_loop",
                           lang::MirSafetyCheckFamily::ArithmeticOverflow);
    // The loop counter is a local no other thread can reach, so its bound
    // survives. The accumulator still needs recurrence invariance, which
    // the call's opaque effects block.
    expect(local == 1, "an uncaptured loop counter keeps its bound across "
                       "synchronization: " +
                           std::to_string(local) + " actual, 1 expected");
  }
}

void testOpaqueRuntimeCallInvalidation() {
  const std::string source = R"(
#include <std/memory>
mut bool inspect_runtime = false;
int32_t inspect_shared(gti_internal::shared_owner<int32_t>& owner, bool inspect) {
  if (inspect) {
    [[discard]] gti_internal::shared_owner_use_count(owner);
  }
  return 1;
}
int32_t transport_result(gti_internal::shared_owner<int32_t>& owner, bool inspect) {
  return inspect_shared(owner, inspect) + 1;
}
int main() {
  auto owner = gti_internal::shared_owner_empty<int32_t>();
  [[discard]] transport_result(owner, inspect_runtime);
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 2; index++) {
    [[discard]] inspect_shared(owner, inspect_runtime);
    total += 1;
  }
  return total;
}
)";
  const auto frontend = analyzeTrusted("optimizer-opaque-runtime.gti", source);
  expect(frontend.canGenerateCode(),
         "an unsupported runtime intrinsic fixture must remain valid GTI/MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto *function = findFunction(frontend, "inspect_shared");
  bool hasOpaqueSynchronization = false;
  if (function)
    for (const auto &block :
         frontend.mir.findFunctionInstance(function->id)->body.blocks)
      for (const auto &instruction : block.instructions)
        hasOpaqueSynchronization |=
            instruction.intrinsic == lang::IntrinsicKind::SharedOwnerUseCount &&
            instruction.synchronization.kind ==
                lang::SynchronizationOperationKind::None &&
            lang::effects(instruction).maySynchronize;
  expect(hasOpaqueSynchronization,
         "the negative must use authoritative runtime synchronization effects "
         "without relying on an explicit AtomicLoad marker");
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  const auto *summary =
      function ? analysis.find({lang::MirBodyKind::Function, function->id})
               : nullptr;
  expect(analysis.converged && summary && summary->reached && !summary->closed,
         "an unsupported runtime operation must not export effect closure "
         "when one successful joined path crossed its memory barrier");
  const auto *transport = findFunction(frontend, "transport_result");
  const auto *transportSummary =
      transport ? analysis.find({lang::MirBodyKind::Function, transport->id})
                : nullptr;
  expect(transportSummary && transportSummary->reached &&
             !transportSummary->closed,
         "opaque successful effects must survive exact nested calls even "
         "when their scalar return facts remain known");
  const auto *main = findFunction(frontend, "main");
  const auto accumulatorStart = source.find("total += 1;");
  expect(accumulatorStart != std::string::npos,
         "the opaque-loop accumulator source must be present");
  if (accumulatorStart == std::string::npos)
    return;
  const int accumulatorLine =
      1 + std::count(source.begin(), source.begin() + accumulatorStart, '\n');
  std::vector<std::pair<lang::MirInstructionId, lang::MirSafetyDischarge>>
      accumulatorChecks;
  if (main)
    for (const auto &block :
         frontend.mir.findFunctionInstance(main->id)->body.blocks)
      for (const auto &instruction : block.instructions)
        for (std::size_t index = 0;
             index < instruction.definedFailure.localOrigins.size(); ++index) {
          const auto &origin = instruction.definedFailure.localOrigins[index];
          if (origin.line != accumulatorLine)
            continue;
          for (const auto &outcome : origin.outcomes)
            if (outcome.code == lang::DefinedFailureCode::IntegerOverflow)
              accumulatorChecks.emplace_back(
                  instruction.id,
                  lang::MirSafetyDischarge{
                      .family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                      .origin = index,
                      .outcome = outcome});
        }
  expect(!accumulatorChecks.empty(),
         "the opaque-loop negative must identify the accumulator's actual "
         "MIR instruction and original integer-overflow outcome by source");
  for (const auto level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const auto optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "opaque-runtime optimization must stay verified");
    for (const auto &[id, check] : accumulatorChecks) {
      const lang::MirInstruction *retained = nullptr;
      for (const auto &block :
           optimized.mir.findFunctionInstance(main->id)->body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.id == id)
            retained = &instruction;
      expect(retained && !retained->tombstone &&
                 retained->definedFailure.localOrigins.size() > check.origin &&
                 retained->definedFailure.localOrigins[check.origin].line ==
                     accumulatorLine &&
                 !lang::hasMirSafetyDischarge(*retained, check),
             "opaque runtime synchronization must retain the exact accumulator "
             "overflow check even when the independent counter is provable");
    }
    expect(functionOperationDischarges(
               frontend, optimized, "transport_result",
               lang::MirSafetyCheckFamily::ArithmeticOverflow,
               lang::MirOperation::Add) == 1,
           "an independently proven scalar result may cross an opaque call "
           "without claiming that its effects are closed");
  }
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
  const auto expectCount = [&](std::string_view name, std::size_t actual,
                               std::size_t expected) {
    expect(actual == expected,
           std::string(name) +
               " must retain its exact recurrence proof count: " +
               std::to_string(actual) + " actual, " + std::to_string(expected) +
               " expected");
  };
  expectCount("bounded_recurrence", boundedRecurrence, 2);
  expectCount("dynamic_recurrence", dynamicRecurrence, 0);
  expectCount("repeated_recurrence_write", repeatedRecurrence, 1);
  expectCount("recurrence_call_barrier", callRecurrence, 0);
  expectCount("contextual_bound_recurrence", contextualRecurrence, 2);
  expectCount("bounded_interval_recurrence", boundedIntervalRecurrence, 2);
  expectCount("exact_observer_recurrence", observerRecurrence, 2);
  expectCount("mutating_closed_call_recurrence", mutatingCallRecurrence, 1);
  expectCount("mutating_closed_call_recurrence accumulator",
              mutatingAccumulator, 0);

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

// Source owners put the second check in an ordinary reference-returning
// accessor rather than an intrinsic in the caller. Count its exact selected
// borrowing call chain, without recognizing a container or member spelling.
std::size_t borrowingCallDischarges(const lang::OptimizedProgram &optimized,
                                    lang::HirFunctionInstanceId root,
                                    lang::MirSafetyCheckFamily family) {
  std::vector<lang::HirFunctionInstanceId> pending{root};
  std::vector<lang::HirFunctionInstanceId> visited;
  std::size_t count = 0;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (std::find(visited.begin(), visited.end(), id) != visited.end())
      continue;
    visited.push_back(id);
    const auto *function = optimized.mir.findFunctionInstance(id);
    if (function == nullptr)
      continue;
    for (const auto &block : function->body.blocks) {
      for (const auto &instruction : block.instructions) {
        count += std::count_if(
            instruction.safetyDischarges.begin(),
            instruction.safetyDischarges.end(),
            [&](const auto &proof) { return proof.family == family; });
        const auto *callee = instruction.functionTarget
                                 ? optimized.mir.findFunctionInstance(
                                       *instruction.functionTarget)
                                 : nullptr;
        if (instruction.kind == lang::MirInstructionKind::Call &&
            callee != nullptr &&
            callee->returnType.kind == lang::SemanticType::Reference)
          pending.push_back(callee->id);
      }
    }
  }
  return count;
}

void testSourcePrefixBoundsRelation() {
  const std::string source = R"(
#include <std/memory>
extern "C" {
  void analysis_barrier();
}

int32_t exact_checked_read(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  return values.read(index);
}

int32_t different_index(uint64_t checked, uint64_t accessed) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(checked, live);
  return values.read(accessed);
}

int32_t different_storage(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> checked =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  mut std::detail::prefix_owner<int32_t> accessed =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  checked.append_value(int32_t(7));
  accessed.append_value(int32_t(8));
  uint64_t live = checked.length();
  gti_internal::index_bounds_check(index, live);
  return accessed.read(index);
}

int32_t append_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  values.append_value(int32_t(8));
  return values.read(index);
}

int32_t pop_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  values.append_value(int32_t(8));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  values.pop();
  return values.read(index);
}

int32_t insert_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  values.insert(uint64_t(0), int32_t(8));
  return values.read(index);
}

int32_t erase_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  values.append_value(int32_t(8));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  values.erase(uint64_t(0));
  return values.read(index);
}

int32_t relocate_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> source =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  mut std::detail::prefix_owner<int32_t> destination =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  source.append_value(int32_t(7));
  uint64_t live = source.length();
  gti_internal::index_bounds_check(index, live);
  source.relocate_to(destination);
  return source.read(index);
}

int32_t opaque_call_after_check(uint64_t index) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  analysis_barrier();
  return values.read(index);
}

int32_t index_write_after_check(mut uint64_t index, uint64_t replacement) {
  mut std::detail::prefix_owner<int32_t> values =
      std::detail::prefix_owner<int32_t>(uint64_t(4));
  values.append_value(int32_t(7));
  uint64_t live = values.length();
  gti_internal::index_bounds_check(index, live);
  index = replacement;
  return values.read(index);
}

int32_t main() {
  return exact_checked_read(uint64_t(0)) == int32_t(7) ? 0 : 1;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-source-prefix-bounds.gti", source);
  expect(frontend.canGenerateCode(),
         "source-prefix bounds relation fixture should pass the frontend");
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
    const auto *function = findFunction(frontend, name);
    return function == nullptr
               ? 0
               : borrowingCallDischarges(optimized, function->id,
                                         lang::MirSafetyCheckFamily::Bounds);
  };
  expect(o2.valid() && o3.valid() && bounds(o2, "exact_checked_read") == 2 &&
             bounds(o3, "exact_checked_read") == 2,
         "an exact closed-call index range should discharge the public "
         "logical-size check and its matching source prefix read at O2 and "
         "O3");
  for (const std::string_view name :
       {"different_index", "different_storage", "append_after_check",
        "pop_after_check", "insert_after_check", "erase_after_check",
        "relocate_after_check", "opaque_call_after_check",
        "index_write_after_check"}) {
    expect(functionDischarges(frontend, o2, name,
                              lang::MirSafetyCheckFamily::Bounds) == 0 &&
               functionDischarges(frontend, o3, name,
                                  lang::MirSafetyCheckFamily::Bounds) == 0,
           std::string(name) +
               " must retain the source prefix read bounds check");
  }
}

void testClosedIndexedElementEnvelope() {
  const std::string source = R"(
#include <std/memory>
class CheckedBuffer {
  mut std::detail::prefix_owner<int32_t> data;

public:
  CheckedBuffer()
      : data(std::detail::prefix_owner<int32_t>(uint64_t(4))) {
    this.data.append_value(int32_t(1));
    this.data.append_value(int32_t(2));
    this.data.append_value(int32_t(3));
    this.data.append_value(int32_t(4));
  }

  uint64_t size() {
    return this.data.length();
  }

  int32_t& operator[](uint64_t index) {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    return this.data.read(index);
  }

  mut int32_t& operator[](uint64_t index) mut {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    return this.data.read_mut(index);
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
         "closed_indexed_sum must preserve its two exact prefix-envelope "
         "proofs: O2=" +
             std::to_string(overflow(o2, "closed_indexed_sum")) +
             ", O3=" + std::to_string(overflow(o3, "closed_indexed_sum")));
  expect(overflow(o2, "unknown_indexed_sum") == 0 &&
             overflow(o3, "unknown_indexed_sum") == 0,
         "an indexed receiver without a closed object context must retain "
         "both arithmetic checks");
}

void testClosedIndexedRecordFieldEnvelope() {
  const std::string source = R"(
#include <std/memory>
extern "C" {
  void analysis_barrier();
}

struct CheckedRecord {
  CheckedRecord() {}

  mut int32_t broad = int32_t(2147483647);
  mut int32_t narrow = int32_t(0);
};

class CheckedRecordBuffer {
  mut std::detail::prefix_owner<CheckedRecord> data;

public:
  CheckedRecordBuffer()
      : data(std::detail::prefix_owner<CheckedRecord>(uint64_t(2))) {
    this.data.append_value(CheckedRecord());
    this.data.append_value(CheckedRecord());
  }

  uint64_t size() {
    return this.data.length();
  }

  CheckedRecord& operator[](uint64_t index) {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    return this.data.read(index);
  }

  mut CheckedRecord& operator[](uint64_t index) mut {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    return this.data.read_mut(index);
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
  // Preserve the original sequential client as a conservative boundary
  // control. Its local owning helper performs cleanup whose authoritative
  // effect summary has unknown writes; the earlier caller buffer cannot keep
  // its field envelope across that call.
  const auto crossingFrontend =
      analyzeTrusted("optimizer-indexed-record-opaque-cleanup.gti", source);
  expect(crossingFrontend.canGenerateCode(),
         "the sequential record cleanup-boundary client must remain valid");
  if (!crossingFrontend.canGenerateCode())
    return;
  const auto crossingAnalysis =
      lang::analyzeMirClosedCallSafetyChecks(crossingFrontend.mir);
  for (const auto name :
       {"increment_narrow", "repeat_narrow", "increment_then_barrier"}) {
    const auto *function = findFunction(crossingFrontend, name);
    const auto *context =
        function
            ? crossingAnalysis.find({lang::MirBodyKind::Function, function->id})
            : nullptr;
    expect(crossingAnalysis.converged && context && context->reached,
           "opaque cleanup must not hide later record clients from analysis");
  }
  for (const auto level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const auto crossing =
        lang::OptimizationPipeline().run({.hir = crossingFrontend.hir,
                                          .mir = crossingFrontend.mir,
                                          .level = level});
    // The constructor's envelope does not survive the opaque cleanup, so
    // the first client keeps both checks. Its own exact write then proves a
    // fresh element-zero postcondition, which later clients may consume.
    expect(crossing.valid() &&
               functionDischarges(
                   crossingFrontend, crossing, "increment_narrow",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0 &&
               functionDischarges(
                   crossingFrontend, crossing, "repeat_narrow",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow) == 5 &&
               functionDischarges(
                   crossingFrontend, crossing, "increment_then_barrier",
                   lang::MirSafetyCheckFamily::ArithmeticOverflow) == 2,
           "opaque cleanup must erase the earlier caller record envelope; "
           "only facts written after the barrier may prove later clients");
    expect(lang::verifyMirOptimizationCoherence(crossingFrontend.mir,
                                                crossing.mir,
                                                crossing.report.transformations)
               .valid(),
           "cleanup-boundary proofs must independently replay");
  }

  // Separately retain every original exact-record positive. Constructing the
  // caller buffer after the unrelated helper cleanup establishes fresh facts
  // without crossing that opaque boundary.
  std::string isolatedSource = source;
  const std::string before =
      "  mut CheckedRecordBuffer values = CheckedRecordBuffer();\n"
      "  int32_t direct = direct_increment_narrow();";
  const std::string after =
      "  int32_t direct = direct_increment_narrow();\n"
      "  mut CheckedRecordBuffer values = CheckedRecordBuffer();";
  const auto position = isolatedSource.find(before);
  expect(position != std::string::npos,
         "the isolated positive must retain the same record call chain");
  if (position == std::string::npos)
    return;
  isolatedSource.replace(position, before.size(), after);
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-indexed-record-envelope.gti", isolatedSource);
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
         "repeat_narrow must preserve its five record-envelope and loop "
         "proofs: O2=" +
             std::to_string(repeatNarrowO2) +
             ", O3=" + std::to_string(repeatNarrowO3));
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

void testSourceRecordSequenceEnvelopes() {
  // An unrelated record sequence built by the public vector size
  // constructor. Each element runs the record's own source constructor in
  // final storage; a write to one field leaves its sibling field envelope
  // intact; a caller-local loop index reads one element of the initialized
  // region without naming an exact cell.
  const std::string source = R"(
#include <std/vector>
extern "C" {
  void analysis_barrier();
}

struct particle {
  particle() {}

  mut int32_t speed = 3;
  mut int32_t heat = 0;
};

struct noisy_particle {
  noisy_particle() { analysis_barrier(); }

  mut int32_t speed = 3;
  mut int32_t heat = 0;
};

int32_t settle(mut std::vector<particle>& values) {
  for (mut int32_t tick = 0; tick < 1000; tick++) {
    for (mut std::size_t index = 0; index < values.size(); index++) {
      mut particle& item = values[index];
      item.heat = (item.heat * 3 + item.speed) % 101 - 50;
    }
  }
  return values[std::size_t(0)].heat;
}

int32_t settle_with_barrier(mut std::vector<particle>& values) {
  for (mut int32_t tick = 0; tick < 1000; tick++) {
    for (mut std::size_t index = 0; index < values.size(); index++) {
      mut particle& item = values[index];
      analysis_barrier();
      item.heat = (item.heat * 3 + item.speed) % 101 - 50;
    }
  }
  return values[std::size_t(0)].heat;
}

int32_t settle_noisy() {
  mut std::vector<noisy_particle> values =
      std::vector<noisy_particle>(std::size_t(16));
  for (mut int32_t tick = 0; tick < 1000; tick++) {
    for (mut std::size_t index = 0; index < values.size(); index++) {
      mut noisy_particle& item = values[index];
      item.heat = (item.heat * 3 + item.speed) % 101 - 50;
    }
  }
  return values[std::size_t(0)].heat;
}

int32_t sum_speeds(std::vector<particle>& values) {
  mut int32_t total = 0;
  for (mut std::size_t index = 0; index < values.size(); index++) {
    total += values[index].speed;
  }
  return total;
}

int32_t sum_past_end(std::vector<particle>& values) {
  mut int32_t total = 0;
  for (mut std::size_t index = 0; index <= values.size(); index++) {
    total += values[index].speed;
  }
  return total;
}

int32_t main() {
  mut std::vector<particle> values = std::vector<particle>(std::size_t(16));
  int32_t speeds = sum_speeds(values);
  int32_t settled = settle(values);
  int32_t barrier = settle_with_barrier(values);
  return settled + barrier + speeds + settle_noisy() + sum_past_end(values);
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-record-sequence.gti", source);
  expect(frontend.canGenerateCode(),
         "the record sequence fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  for (const auto level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    const auto overflow = [&](std::string_view name) {
      return functionDischarges(frontend, optimized, name,
                                lang::MirSafetyCheckFamily::ArithmeticOverflow);
    };
    const std::string counts =
        " settle=" + std::to_string(overflow("settle")) +
        " sum_speeds=" + std::to_string(overflow("sum_speeds")) +
        " barrier=" + std::to_string(overflow("settle_with_barrier")) +
        " noisy=" + std::to_string(overflow("settle_noisy")) +
        " past_end=" + std::to_string(overflow("sum_past_end"));
    expect(optimized.valid() && overflow("settle") == 5 &&
               overflow("sum_speeds") == 2,
           "constructor-built record envelopes must prove both counters and "
           "every field operation:" +
               counts);
    const auto multiply = [&](std::string_view name) {
      return functionOperationDischarges(
          frontend, optimized, name,
          lang::MirSafetyCheckFamily::ArithmeticOverflow,
          lang::MirOperation::Multiply);
    };
    expect(overflow("settle_with_barrier") == 2 &&
               overflow("settle_noisy") == 2 &&
               multiply("settle_with_barrier") == 0 &&
               multiply("settle_noisy") == 0,
           "an opaque call before the write, or an opaque element "
           "constructor, must retain the field checks while the uncaptured "
           "loop counter keeps its bound:" +
               counts);
    expect(overflow("sum_past_end") == 0,
           "an index reaching the initialized length must not read the "
           "element envelope:" +
               counts);
    expect(lang::verifyMirOptimizationCoherence(
               frontend.mir, optimized.mir, optimized.report.transformations)
               .valid(),
           "record sequence proofs must independently replay");
  }
}

void testScalarLeafSequenceStores() {
  // User containers over the source prefix owner. Vacant construction of a
  // floating, enum or record element, movement of a by-value scalar fill and
  // a checked floating store through the returned element reference all keep
  // the container's live length. A caller-local index then proves `at`. An
  // index reaching the length, or an opaque call in the loop, keeps it.
  const std::string containers = R"(
#include <std/memory>
extern "C" {
  void analysis_barrier();
}

class samples<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  samples(uint64_t count) : data(std::detail::prefix_owner<T>(count)) {
    while (this.data.length() < count) {
      unsafe { this.data.append_unchecked(); }
    }
  }

  uint64_t size() {
    return this.data.length();
  }

  mut T& at(uint64_t index) mut {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    unsafe { return this.data.read_mut_unchecked(index); }
  }
};

class filled<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  filled(uint64_t count, T fill) : data(std::detail::prefix_owner<T>(count)) {
    while (this.data.length() < count) {
      unsafe { this.data.append_value_unchecked(fill); }
    }
  }

  uint64_t size() {
    return this.data.length();
  }

  T& at(uint64_t index) {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    unsafe { return this.data.read_unchecked(index); }
  }
};

struct body {
  body() {}

  mut double mass = 1.0d;
  mut int64_t hits = 0;
};

enum class shade : int32_t { dark, light };
)";
  struct Case {
    std::string_view name;
    std::string program;
    std::size_t bounds;
    std::string_view expectation;
  };
  const std::vector<Case> cases = {
      {"floating-store", R"(
void ramp(mut samples<double>& values) {
  for (mut uint64_t index = 0; index < values.size(); index++) {
    values.at(index) = double(index) * 0.5d;
  }
}

int32_t main() {
  mut samples<double> values = samples<double>(uint64_t(16));
  ramp(values);
  for (mut uint64_t index = 0; index < values.size(); index++) {
    values.at(index) += 1.0d;
  }
  return int32_t(values.at(uint64_t(3)));
}
)",
       1,
       "value-initialized floating elements and checked floating stores "
       "must keep the live length"},
      {"floating-field-store", R"(
void weigh(mut samples<body>& bodies) {
  for (mut uint64_t index = 0; index < bodies.size(); index++) {
    bodies.at(index).mass *= 0.5d;
  }
}

int32_t main() {
  mut samples<body> bodies = samples<body>(uint64_t(16));
  weigh(bodies);
  return int32_t(bodies.at(uint64_t(3)).mass);
}
)",
       1,
       "a floating field store through an element reference must keep the "
       "live length"},
      {"scalar-fill", R"(
double total(filled<double>& values) {
  mut double sum = 0.0d;
  for (mut uint64_t index = 0; index < values.size(); index++) {
    sum += values.at(index);
  }
  return sum;
}

int32_t main() {
  filled<double> values = filled<double>(uint64_t(16), 0.5d);
  return int32_t(total(values));
}
)",
       1,
       "moving a by-value floating fill into vacant storage must keep the "
       "live length"},
      {"enum-fill", R"(
int32_t count_dark(filled<shade>& values) {
  mut int32_t dark = 0;
  for (mut uint64_t index = 0; index < values.size(); index++) {
    if (values.at(index) == shade::dark) { dark++; }
  }
  return dark;
}

int32_t main() {
  filled<shade> values = filled<shade>(uint64_t(16), shade::light);
  return count_dark(values);
}
)",
       1, "an integral enum fill shares the scalar construction profile"},
      {"floating-past-end", R"(
void ramp(mut samples<double>& values) {
  for (mut uint64_t index = 0; index <= values.size(); index++) {
    values.at(index) = double(index) * 0.5d;
  }
}

int32_t main() {
  mut samples<double> values = samples<double>(uint64_t(16));
  ramp(values);
  return int32_t(values.at(uint64_t(3)));
}
)",
       0, "an index reaching the live length must keep the check"},
      {"floating-barrier", R"(
void ramp(mut samples<double>& values) {
  for (mut uint64_t index = 0; index < values.size(); index++) {
    values.at(index) = double(index) * 0.5d;
    analysis_barrier();
  }
}

int32_t main() {
  mut samples<double> values = samples<double>(uint64_t(16));
  ramp(values);
  return int32_t(values.at(uint64_t(3)));
}
)",
       0, "an opaque call after the floating store must keep the check"},
  };
  for (const Case &fixture : cases) {
    const lang::FrontendResult frontend = analyzeTrusted(
        "optimizer-scalar-leaf-" + std::string(fixture.name) + ".gti",
        containers + fixture.program);
    expect(frontend.canGenerateCode(), "the " + std::string(fixture.name) +
                                           " fixture should pass the frontend");
    if (!frontend.canGenerateCode()) {
      for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    for (const auto level :
         {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
      const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
          {.hir = frontend.hir, .mir = frontend.mir, .level = level});
      const std::size_t bounds = functionDischarges(
          frontend, optimized, "at", lang::MirSafetyCheckFamily::Bounds);
      expect(optimized.valid() && bounds == fixture.bounds,
             std::string(fixture.name) + ": " +
                 std::string(fixture.expectation) +
                 " (bounds=" + std::to_string(bounds) + ")");
      expect(lang::verifyMirOptimizationCoherence(
                 frontend.mir, optimized.mir, optimized.report.transformations)
                 .valid(),
             std::string(fixture.name) + " proofs must independently replay");
    }
  }
}

void testUncapturedLocalsSurviveUnknownEffects() {
  // An unknown call, a synchronizing call or another allocation can only
  // reach storage whose identity reached it. Locals that no earlier
  // instruction exposed keep their facts; an address passed to native or
  // bodyless code, an aggregate holding a borrow, a constructor registering
  // its own field, or a capture on an earlier loop iteration keeps the check.
  const std::string locals = R"(extern "C" {
  void analysis_barrier();
  void stash(int32_t* address);
}


int32_t uncaptured() {
  mut int32_t value = 0;
  analysis_barrier();
  return value + int32_t(1);
}

int32_t address_passed() {
  mut int32_t value = 0;
  unsafe { stash(&value); }
  analysis_barrier();
  return value + int32_t(1);
}

int32_t captured_before_use() {
  mut int32_t value = 0;
  unsafe { stash(&value); }
  value = 0;
  analysis_barrier();
  return value + int32_t(1);
}

int32_t captured_after_use() {
  mut int32_t value = 0;
  analysis_barrier();
  int32_t result = value + int32_t(1);
  unsafe { stash(&value); }
  return result;
}

class View {
  mut int32_t& value;
public:
  View(mut int32_t& source) : value(source) {}
};

void keep_view(View& view);

int32_t aggregate_captured() {
  mut int32_t value = 0;
  View view{value};
  keep_view(view);
  analysis_barrier();
  return value + int32_t(1);
}

class Registered {
public:
  mut int32_t count = 0;
  Registered() { unsafe { stash(&this.count); } }
};

int32_t registered() {
  mut Registered entry = Registered();
  entry.count = 0;
  analysis_barrier();
  return entry.count + int32_t(1);
}

int32_t loop_carried() {
  mut int32_t value = 0;
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 2; index++) {
    analysis_barrier();
    total = value * int32_t(3);
    unsafe { stash(&value); }
  }
  return total;
}

int32_t loop_uncaptured() {
  mut int32_t value = 0;
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 2; index++) {
    analysis_barrier();
    total = value * int32_t(3);
  }
  return total;
}

int32_t main() {
  return uncaptured() + address_passed() + captured_before_use() +
         captured_after_use() + aggregate_captured() + registered() +
         loop_carried() + loop_uncaptured();
}
)";
  const lang::FrontendResult frontend =
      analyzeTrusted("optimizer-capture-locals.gti", locals);
  expect(frontend.canGenerateCode(),
         "the capture fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const std::string containerHeader = R"(#include <std/memory>
extern "C" {
  void analysis_barrier();

}

class samples<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  samples(uint64_t count) : data(std::detail::prefix_owner<T>(count)) {
    while (this.data.length() < count) {
      unsafe { this.data.append_unchecked(); }
    }
  }

  uint64_t size() {
    return this.data.length();
  }

  mut T& at(uint64_t index) mut {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    unsafe { return this.data.read_mut_unchecked(index); }
  }
};

void keep_samples(samples<double>& values);

int32_t main() {
  mut samples<double> first = samples<double>(uint64_t(16));
  mut samples<double> second = samples<double>(uint64_t(16));
  for (mut uint64_t index = 0; index < uint64_t(16); index++) {
    first.at(index) = double(index) * 0.5d;
  }
  analysis_barrier();
  for (mut uint64_t index = 0; index < uint64_t(16); index++) {
    second.at(index) = first.at(index);
  }
  return int32_t(second.at(uint64_t(3)));
}
)";
  for (const auto level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    const auto discharged = [&](std::string_view name,
                                lang::MirOperation operation) {
      return functionOperationDischarges(
          frontend, optimized, name,
          lang::MirSafetyCheckFamily::ArithmeticOverflow, operation);
    };
    expect(optimized.valid(), "capture fixtures must remain verified");
    for (const std::string_view name : {"uncaptured", "captured_after_use"})
      expect(discharged(name, lang::MirOperation::Add) == 1,
             std::string(name) +
                 ": a local not yet exposed keeps its value across an "
                 "unknown call");
    for (const std::string_view name : {"address_passed", "captured_before_use",
                                        "aggregate_captured", "registered"})
      expect(discharged(name, lang::MirOperation::Add) == 0,
             std::string(name) +
                 ": an exposed local must not keep facts across an unknown "
                 "call");
    expect(discharged("loop_carried", lang::MirOperation::Multiply) == 0,
           "a capture on an earlier iteration must reach the next one");
    expect(discharged("loop_uncaptured", lang::MirOperation::Multiply) == 1,
           "an uncaptured local keeps its value through a loop of unknown "
           "calls");
    expect(lang::verifyMirOptimizationCoherence(
               frontend.mir, optimized.mir, optimized.report.transformations)
               .valid(),
           "capture proofs must independently replay");
  }

  // Building a second container calls the opaque process allocator, and an
  // unknown call separates the loops; neither can reach the first container.
  // Passing it by reference to a bodyless function first exposes it.
  for (const bool exposed : {false, true}) {
    std::string source = containerHeader;
    if (exposed) {
      const std::string barrier = "  analysis_barrier();";
      source.replace(source.find(barrier), barrier.size(),
                     "  keep_samples(first);\n" + barrier);
    }
    const lang::FrontendResult containers =
        analyzeTrusted(exposed ? "optimizer-capture-exposed.gti"
                               : "optimizer-capture-containers.gti",
                       source);
    expect(containers.canGenerateCode(),
           "the capture container fixture should pass the frontend");
    if (!containers.canGenerateCode())
      continue;
    for (const auto level :
         {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
      const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
          {.hir = containers.hir, .mir = containers.mir, .level = level});
      const std::size_t bounds = functionDischarges(
          containers, optimized, "at", lang::MirSafetyCheckFamily::Bounds);
      expect(optimized.valid() && bounds == (exposed ? 0U : 1U),
             std::string(exposed ? "an exposed container must keep its "
                                   "accessor check"
                                 : "a second allocation and an unknown call "
                                   "must not hide an unexposed container's "
                                   "length") +
                 " (bounds=" + std::to_string(bounds) + ")");
      expect(
          lang::verifyMirOptimizationCoherence(containers.mir, optimized.mir,
                                               optimized.report.transformations)
              .valid(),
          "capture container proofs must independently replay");
    }
  }
}

void testGuardedClosedCallContextWidening() {
  const std::string source = R"(
#include <std/memory>
struct ContextEntity {
  ContextEntity() {}

  mut int32_t position_x = 0;
  mut int32_t position_y = 0;
  mut int32_t velocity_x = 0;
  mut int32_t velocity_y = 0;
};

class ContextBuffer {
  mut std::detail::prefix_owner<ContextEntity> data;

public:
  ContextBuffer()
      : data(std::detail::prefix_owner<ContextEntity>(uint64_t(4))) {
    this.data.append_value(ContextEntity());
    this.data.append_value(ContextEntity());
    this.data.append_value(ContextEntity());
    this.data.append_value(ContextEntity());
  }

  uint64_t size() {
    return this.data.length();
  }

  mut ContextEntity& operator[](uint64_t index) mut {
    uint64_t live = this.data.length();
    gti_internal::index_bounds_check(index, live);
    return this.data.read_mut(index);
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
    std::size_t count = 0;
    for (const auto &function : optimized.mir.functionInstances())
      if (function.overloadedOperator == lang::OverloadedOperator::Subscript)
        count += borrowingCallDischarges(optimized, function.id,
                                         lang::MirSafetyCheckFamily::Bounds);
    return count;
  };

  expect(o2.valid() && o3.valid() && bounds(o2) == 2 && bounds(o3) == 2,
         "guard-refined loop indices and exact storage lengths should survive "
         "bounded closed-call context widening and discharge both bounds "
         "checks");
}

// A multidimensional access checks each prefix before evaluating the next
// index, then retains the complete place on its terminal bounds operation.
// Proving only the last index must not discharge that terminal check: every
// dimension must be proven. The early AddressOf preflight currently remains
// conservatively checked and retains its own first-failure source identity.
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
    std::pair<std::size_t, std::size_t> count{};
    if (function == nullptr) {
      return count;
    }
    const lang::MirInstruction *prefix = nullptr;
    const lang::MirInstruction *terminal = nullptr;
    const lang::MirBlock *prefixBlock = nullptr;
    const lang::MirBlock *terminalBlock = nullptr;
    for (const lang::MirBlock &block : function->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (const lang::DefinedFailureOrigin &origin :
             instruction.definedFailure.localOrigins) {
          const auto bounds = static_cast<std::size_t>(
              std::count_if(origin.outcomes.begin(), origin.outcomes.end(),
                            [](const lang::DefinedFailureOutcome &outcome) {
                              return outcome.code ==
                                     lang::DefinedFailureCode::IndexOutOfBounds;
                            }));
          if (bounds == 0)
            continue;
          if (instruction.kind == lang::MirInstructionKind::Compute &&
              instruction.operation == lang::MirOperation::AddressOf) {
            count.first += bounds;
            prefix = &instruction;
            prefixBlock = &block;
          } else {
            count.second += bounds;
            terminal = &instruction;
            terminalBlock = &block;
          }
        }
      }
    }
    const auto dominance = lang::computeMirDominance(function->body);
    expect(prefix && terminal && prefixBlock && terminalBlock && dominance &&
               terminal->kind == lang::MirInstructionKind::Load &&
               prefixBlock->terminator.kind ==
                   lang::MirTerminatorKind::Invoke &&
               prefixBlock->terminator.invokeInstruction == prefix->id &&
               dominance->dominates(prefixBlock->terminator.target,
                                    terminalBlock->id),
           "early prefix selection must succeed before the terminal load");
    expect(prefix && terminal &&
               prefix->definedFailure.localOrigins.size() == 1 &&
               terminal->definedFailure.localOrigins.size() == 1 &&
               prefix->definedFailure.localOrigins !=
                   terminal->definedFailure.localOrigins &&
               prefix->localFailureSites.size() == 1 &&
               terminal->localFailureSites.size() == 1 &&
               prefix->localFailureSites != terminal->localFailureSites &&
               o3.mir.failureMetadata().siteFor(
                   prefix->definedFailure.localOrigins.front()) ==
                   prefix->localFailureSites.front() &&
               o3.mir.failureMetadata().siteFor(
                   terminal->definedFailure.localOrigins.front()) ==
                   terminal->localFailureSites.front(),
           "prefix and terminal retain distinct exact source failure sites");
    return count;
  };

  const std::pair<std::size_t, std::size_t> prefixAndTerminal{1, 1};
  expect(o3.valid() && boundsSites("proven_chain") == prefixAndTerminal &&
             boundsSites("dynamic_parent_chain") == prefixAndTerminal &&
             boundsSites("dynamic_terminal_chain") == prefixAndTerminal,
         "each chained access retains its early prefix site and complete "
         "terminal site independently");
  expect(o3.valid() && bounds("proven_chain") == 1,
         "a chained element access whose every dimension is proven should "
         "discharge its terminal bounds site without erasing the preflight");
  expect(o3.valid() && bounds("dynamic_parent_chain") == 0 &&
             bounds("dynamic_terminal_chain") == 0,
         "an unproven dimension anywhere in the chain must leave the access "
         "checked, whichever dimension it is");
  expect(functionOperationDischarges(frontend, o3, "proven_chain",
                                     lang::MirSafetyCheckFamily::Bounds,
                                     lang::MirOperation::AddressOf) == 0,
         "the proven chain must not claim an unsupported prefix discharge");
}

} // namespace

int main() {
  lang::installCrashHandlers("gti_optimizer_safety_tests");
  testProofCarryingSafetyCheckElimination();
  testClosedCallMutableInputInvalidation();
  testSynchronizingCallInvalidation();
  testOpaqueRuntimeCallInvalidation();
  testSourcePrefixBoundsRelation();
  testClosedIndexedElementEnvelope();
  testClosedIndexedRecordFieldEnvelope();
  testGuardedClosedCallContextWidening();
  testSourceRecordSequenceEnvelopes();
  testScalarLeafSequenceStores();
  testUncapturedLocalsSurviveUnknownEffects();
  testChainedFixedArrayDimensionBounds();
  if (failures != 0) {
    std::cerr << failures << " optimizer safety test(s) failed\n";
    return 1;
  }
  std::cout << "optimizer safety tests passed\n";
  return 0;
}
