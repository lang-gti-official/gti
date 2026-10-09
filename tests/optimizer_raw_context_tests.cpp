#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
void checkAddressFixture(std::string_view filename,
                         std::initializer_list<std::string_view> positiveNames,
                         std::size_t negativeCount) {
  const lang::OptimizationPipeline pipeline;
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    ("fixtures/" + std::string(filename));
  const std::ifstream file(path);
  const std::string text{std::istreambuf_iterator<char>(file.rdbuf()), {}};
  const auto library =
      path.parent_path().parent_path().parent_path() / "stdlib";
  const auto addresses = lang::Frontend().analyze(
      path, text, {library / "prelude.gti", path},
      {{std::filesystem::weakly_canonical(path).string(), text}}, {library});
  expect(addresses.canGenerateCode(),
         "typed scalar address fixture must lower");
  if (!addresses.canGenerateCode()) {
    for (const auto &diagnostic : addresses.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto addressContexts =
      lang::analyzeMirClosedCallSafetyChecks(addresses.mir);
  expect(addressContexts.converged, "typed address contexts must converge");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = pipeline.run(
        {.hir = addresses.hir, .mir = addresses.mir, .level = level});
    expect(optimized.valid(), "typed address facts must independently replay");
    if (level == lang::OptimizationLevel::O0)
      expect(optimized.report.transformations.empty(), "O0 remains unchanged");
    std::size_t positives = 0, negatives = 0;
    for (const auto &instance : addresses.hir.functionInstances()) {
      if (!instance.source)
        continue;
      const auto name = instance.source->name().lexeme;
      const bool positive =
          std::find(positiveNames.begin(), positiveNames.end(), name) !=
          positiveNames.end();
      if (!positive && !name.starts_with("bad_"))
        continue;
      positive ? ++positives : ++negatives;
      const auto *context = addressContexts.find(
          {.kind = lang::MirBodyKind::Function, .owner = instance.id});
      expect(context && context->reached,
             "all address controls must be reached");
      expect(context && !context->closed,
             "actual raw reads remain opaque despite value postconditions");
      std::size_t discharges = 0, rawLoads = 0;
      const auto *body = optimized.mir.findFunctionInstance(instance.id);
      for (const auto &block : body->body.blocks)
        for (const auto &instruction : block.instructions) {
          rawLoads += !instruction.tombstone && instruction.rawMemoryAccess &&
                      instruction.kind == lang::MirInstructionKind::Load;
          discharges += std::count_if(
              instruction.safetyDischarges.begin(),
              instruction.safetyDischarges.end(), [](const auto &proof) {
                return proof.family ==
                       lang::MirSafetyCheckFamily::ArithmeticOverflow;
              });
        }
      expect(rawLoads > 0, "value evidence must not erase actual raw access");
      if (discharges !=
          (positive && level != lang::OptimizationLevel::O0 ? 1 : 0))
        std::cerr << "address control " << name << " discharges=" << discharges
                  << '\n';
      expect(discharges ==
                 (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
             "exact addresses prove positives but not stale/ambiguous values");
    }
    expect(positives == positiveNames.size() && negatives == negativeCount,
           "complete scalar-address census");
  }
  std::size_t forgedAddresses = 0;
  for (const auto &instance : addresses.hir.functionInstances()) {
    if (!instance.source || !instance.source->name().lexeme.starts_with("bad_"))
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = instance.id};
    const auto *body = lang::findMirBody(addresses.mir, address);
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size(); ++origin)
          for (const auto outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
              continue;
            auto forged = addresses.mir;
            lang::MirEditor editor(forged, "forged-scalar-address");
            editor.queueSafetyDischarge(
                {.body = address, .instruction = instruction.id},
                {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                 .origin = origin,
                 .outcome = outcome});
            const auto result = editor.apply();
            expect(!result.valid() && !result.changed,
                   "stale address facts cannot be forged");
            ++forgedAddresses;
          }
  }
  expect(forgedAddresses == negativeCount,
         "every scalar-address negative rejects a forged discharge");
}
} // namespace

int main() {
  const auto frontend = lang::Frontend().analyze("raw-context.gti", R"gti(
mut int global = 4;

int before_raw(mut int& destination, int input) {
  int result = input + 1;
  unsafe { mut int* address = &destination; *address = 2147483647; }
  return result;
}

int after_raw(mut int& destination) {
  unsafe { mut int* address = &destination; *address = 2147483647; }
  return destination + 1;
}

int stable_after_raw(mut int& destination, int input) {
  int copy = input;
  unsafe { int* address = &destination; *address = 2147483647; }
  return input + copy;
}

class Token {
public:
  int value;
  Token(int value) : value(value) {}
};
int consume(Token token) { return token.value + 1; }
int after_owning_argument(int input) {
  Token token{7};
  [[discard]] consume(token);
  return input + 1;
}

unsafe int declared_step(int input) { return input + 1; }
unsafe int declared_raw_step(mut int& destination, int input) {
  unsafe { mut int* address = &destination; *address = 2147483647; }
  return input + 1;
}
int through_declared(mut int& destination, int input) {
  unsafe {
    if (declared_step(input) != 8) return 2;
    return declared_raw_step(destination, input);
  }
}

int conflicting(mut int& destination, int input) {
  int result = input + 1;
  unsafe { mut int* address = &destination; *address = 0; }
  return result;
}

int unreached(mut int& destination, int input) {
  int result = input + 1;
  unsafe { mut int* address = &destination; *address = 0; }
  return result;
}

int after_call() {
  global = 4;
  int result = before_raw(global, 7);
  return global + result;
}

int main() {
  mut int local = 4;
  int result = before_raw(local, 7);
  if (result != 8) return 1;
  if (stable_after_raw(local, 7) != 14) return 2;
  if (after_owning_argument(7) != 8) return 3;
  if (through_declared(local, 7) != 8) return 4;
  int first = conflicting(local, 7);
  int second = conflicting(local, 2147483647);
  int changed = after_raw(local);
  return after_call() + changed + first + second;
}
)gti");
  expect(frontend.canGenerateCode(), "ordinary raw context fixture must lower");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto function = [&](std::string_view name) {
    for (const auto &instance : frontend.hir.functionInstances())
      if (instance.source && instance.source->name().lexeme == name)
        return instance.id;
    return lang::HirFunctionInstanceId{};
  };
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  expect(analysis.converged, "raw contextual safety must converge");
  for (const auto name :
       {"before_raw", "after_raw", "after_call", "stable_after_raw"}) {
    const auto *body = analysis.find(
        {.kind = lang::MirBodyKind::Function, .owner = function(name)});
    expect(body && body->reached && !body->closed,
           "raw execution must not export an effect-closed body summary");
  }
  const lang::OptimizationPipeline pipeline;
  const auto *owning =
      analysis.find({.kind = lang::MirBodyKind::Function,
                     .owner = function("after_owning_argument")});
  expect(
      owning && owning->reached && owning->closed,
      "generated scalar-record copy and cleanup have a closed effect summary");
  const auto *declared = analysis.find({.kind = lang::MirBodyKind::Function,
                                        .owner = function("declared_step")});
  const auto *declaredRaw =
      analysis.find({.kind = lang::MirBodyKind::Function,
                     .owner = function("declared_raw_step")});
  expect(declared && declared->reached && declared->closed && declaredRaw &&
             declaredRaw->reached && !declaredRaw->closed,
         "selected unsafe source calls retain actual body effect authority; "
         "raw writes still prevent effect closure");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "raw proofs must pass independent replay");
    const auto discharges = [&](std::string_view name) {
      std::size_t count = 0;
      const auto *body = optimized.mir.findFunctionInstance(function(name));
      if (body)
        for (const auto &block : body->body.blocks)
          for (const auto &instruction : block.instructions)
            count += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
      return count;
    };
    expect(discharges("before_raw") ==
               (level == lang::OptimizationLevel::O0 ? 0 : 1),
           "exact scalar input proves the arithmetic before raw access");
    expect(discharges("stable_after_raw") ==
               (level == lang::OptimizationLevel::O0 ? 0 : 1),
           "readonly scalar copies retain their exact facts after raw writes");
    expect(discharges("after_owning_argument") ==
                   (level == lang::OptimizationLevel::O0 ? 0 : 1) &&
               discharges("consume") ==
                   (level == lang::OptimizationLevel::O0 ? 0 : 1),
           "a generated scalar-record copy supplies independent callee facts "
           "without hiding the caller's continuation");
    expect(discharges("after_raw") == 0 && discharges("after_call") == 0 &&
               discharges("conflicting") == 0 && discharges("unreached") == 0,
           "raw access and its invocation invalidate later mutable facts; "
           "conflicting or absent contexts cannot prove earlier arithmetic");
    expect(discharges("declared_step") ==
                   (level == lang::OptimizationLevel::O0 ? 0 : 1) &&
               discharges("declared_raw_step") ==
                   (level == lang::OptimizationLevel::O0 ? 0 : 1),
           "an unsafe declaration cannot hide independently proved scalar "
           "arithmetic; actual mutable raw reads remain unproved");
    if (level != lang::OptimizationLevel::O0) {
      auto forged = optimized.report.transformations;
      const auto proof =
          std::find_if(forged.begin(), forged.end(), [](const auto &record) {
            return record.kind == lang::MirTransformKind::DischargeSafetyCheck;
          });
      expect(proof != forged.end(), "the positive must have replay evidence");
      if (proof != forged.end()) {
        proof->relatedInstructions.push_back(
            std::numeric_limits<lang::MirInstructionId>::max());
        expect(!lang::verifyMirOptimizationCoherence(frontend.mir,
                                                     optimized.mir, forged)
                    .valid(),
               "corrupted raw-context proof identities must be rejected");
      }
    }
  }
  bool checkedForgedDischarge = false;
  const lang::MirBodyAddress afterAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = function("after_raw")};
  const auto *after = lang::findMirBody(frontend.mir, afterAddress);
  if (after)
    for (const auto &block : after->blocks)
      for (const auto &instruction : block.instructions)
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size(); ++origin)
          for (const auto outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
              continue;
            auto malformed = frontend.mir;
            lang::MirEditor editor(malformed, "forged-post-raw-proof");
            editor.queueSafetyDischarge(
                {.body = afterAddress, .instruction = instruction.id},
                {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                 .origin = origin,
                 .outcome = outcome});
            const auto rejected = editor.apply();
            expect(!rejected.valid() && !rejected.changed,
                   "the editor must independently reject a post-raw proof");
            checkedForgedDischarge = true;
          }
  expect(checkedForgedDischarge,
         "the raw-write fixture must expose a checked overflow outcome");
  checkAddressFixture("optimizer_scalar_addresses.gti",
                      {"copied_address", "replaced_address", "read_address",
                       "joined_same", "returned_address", "relayed_address",
                       "redirected_result"},
                      10);
  checkAddressFixture("optimizer_constructed_addresses.gti",
                      {"constructed_scalar", "constructed_relay",
                       "constructed_pair", "constructed_pack",
                       "constructed_subtract"},
                      10);
  checkAddressFixture("optimizer_constructed_address_budget.gti",
                      {"within_budget"}, 1);
  return failures == 0 ? 0 : 1;
}
