#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

int checkTerminalContexts() {
  int failures = 0;
  const auto expect = [&](bool condition, std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optimizer_terminal_context.gti";
  const std::ifstream file(path);
  const std::string source{std::istreambuf_iterator<char>(file.rdbuf()), {}};
  const auto library =
      path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      path, source, {library / "prelude.gti", path},
      {{std::filesystem::weakly_canonical(path).string(), source}}, {library});
  expect(frontend.canGenerateCode(), "terminal context fixture must lower");
  if (!frontend.canGenerateCode())
    return failures;
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  expect(analysis.converged,
         "terminal contexts converge without return summaries");
  std::size_t observed = 0;
  for (const auto &instance : frontend.hir.functionInstances()) {
    if (!instance.source)
      continue;
    const auto name = instance.source->name().lexeme;
    const bool positive =
        name == "after_terminal_branch" || name == "after_terminal_constructor";
    const bool negative = name == "bad_surviving_value";
    const bool terminal = name == "stop_inner" || name == "stop_outer";
    if (!positive && !negative && !terminal)
      continue;
    ++observed;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = instance.id};
    const auto *context = analysis.find(address);
    expect(context && context->reached,
           "terminal branches still discover their callees");
    if (terminal)
      expect(context && !context->returnInterval,
             "terminal calls publish no fabricated successful return value");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = frontend.hir, .mir = frontend.mir, .level = level});
      expect(optimized.valid(), "terminal branch facts independently replay");
      if (level == lang::OptimizationLevel::O0)
        expect(optimized.report.transformations.empty(),
               "terminal O0 is unchanged");
      std::size_t proofs = 0;
      for (const auto &block :
           lang::findMirBody(optimized.mir, address)->blocks)
        for (const auto &instruction : block.instructions)
          for (const auto &proof : instruction.safetyDischarges)
            proofs +=
                proof.family == lang::MirSafetyCheckFamily::ArithmeticOverflow;
      expect(proofs ==
                 (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
             "only bounded surviving arithmetic can discard its check");
    }
    if (negative) {
      std::size_t rejected = 0;
      for (const auto &block : lang::findMirBody(frontend.mir, address)->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = frontend.mir;
              lang::MirEditor editor(forged, "forged-terminal-survivor");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "terminal alternative cannot justify overflowing "
                     "surviving arithmetic");
              ++rejected;
            }
      expect(rejected == 1, "surviving overflow exercises canonical rejection");
    }
  }
  expect(observed == 5, "terminal context census");
  return failures;
}

int main() {
  const auto sourcePath = std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/optimizer_object_context.gti";
  const std::ifstream sourceFile(sourcePath);
  const std::string source{std::istreambuf_iterator<char>(sourceFile.rdbuf()),
                           {}};
  const auto frontend = lang::Frontend().analyze(sourcePath, source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  int failures = checkTerminalContexts();
  const auto expect = [&](bool valid, std::string_view message) {
    if (!valid) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  const auto count = [&](const lang::OptimizedProgram &optimized,
                         std::string_view name, std::string_view owner) {
    std::size_t result = 0;
    for (const auto &instance : frontend.hir.functionInstances()) {
      if (!instance.source || instance.source->name().lexeme != name)
        continue;
      const lang::HirClassInstance *classInstance = nullptr;
      if (instance.owner)
        for (const auto &candidate : frontend.hir.classInstances())
          if (candidate.id == *instance.owner)
            classInstance = &candidate;
      if (!owner.empty() &&
          (!classInstance || classInstance->source->name().lexeme != owner))
        continue;
      const auto *function = optimized.mir.findFunctionInstance(instance.id);
      if (function)
        for (const auto &block : function->body.blocks)
          for (const auto &instruction : block.instructions)
            result += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
    }
    return result;
  };
  const auto contextual = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  expect(contextual.converged, "ordinary object contexts must converge");
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source)
      continue;
    const auto name = function.source->name().lexeme;
    if (name != "changed" && name != "raw_changed" && name != "unknown")
      continue;
    const auto *context = contextual.find(
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    expect(context && context->reached == (name != "unknown"),
           "mutation negatives are reached; only the explicit unknown control "
           "is unreached");
  }
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "ordinary object field proofs must replay");
    for (const auto &record : optimized.report.transformations)
      expect(std::find(record.relatedPlaces.begin(), record.relatedPlaces.end(),
                       0) == record.relatedPlaces.end(),
             "implicit receiver roots must never be published as MIR place "
             "identities");
    const std::size_t expected = level == lang::OptimizationLevel::O0 ? 0 : 1;
    expect(count(optimized, "read", "Gauge") == expected,
           "parameterized constructor and owned result fields reach an "
           "implicit receiver");
    expect(count(optimized, "after_address", "Gauge") == expected,
           "inert pointer arithmetic must preserve unrelated field facts "
           "without proving any pointee or address invariant");
    expect(count(optimized, "bump", "Gauge") == 2 * expected,
           "a field mutation retains an independent declaration-default field");
    expect(count(optimized, "read", "Leaf") == expected,
           "a nested field constructor publishes facts relative to its exact "
           "subobject");
    expect(
        count(optimized, "before", "Aliased") == expected,
        "the alias-write negative starts from an independently proved field");
    expect(count(optimized, "read", "Hazard") == 0 &&
               count(optimized, "changed", "Hazard") == 0 &&
               count(optimized, "raw_changed", "Hazard") == 0 &&
               count(optimized, "changed", "Aliased") == 0 &&
               count(optimized, "read", "RawConstructed") == 0 &&
               count(optimized, "unknown", "") == 0,
           "overflowing, raw-mutated and unreached object fields stay checked");
    if (level != lang::OptimizationLevel::O0) {
      auto forged = optimized.report.transformations;
      const auto proof =
          std::find_if(forged.begin(), forged.end(), [](const auto &record) {
            return record.kind == lang::MirTransformKind::DischargeSafetyCheck;
          });
      expect(proof != forged.end(),
             "object facts require durable proof evidence");
      if (proof != forged.end()) {
        proof->relatedPlaces.push_back(
            std::numeric_limits<lang::MirPlaceId>::max());
        expect(!lang::verifyMirOptimizationCoherence(frontend.mir,
                                                     optimized.mir, forged)
                    .valid(),
               "forged object-field evidence must not replay");
      }
    }
  }
  bool testedForgery = false;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "raw_changed")
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const auto *body = lang::findMirBody(frontend.mir, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size(); ++origin)
          for (const auto &outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
              continue;
            auto malformed = frontend.mir;
            lang::MirEditor editor(malformed, "forged-post-raw-object-field");
            editor.queueSafetyDischarge(
                {.body = address, .instruction = instruction.id},
                {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                 .origin = origin,
                 .outcome = outcome});
            const auto result = editor.apply();
            expect(!result.valid() && !result.changed,
                   "the canonical editor rejects stale constructor facts after "
                   "raw writes");
            testedForgery = true;
          }
  }
  expect(testedForgery,
         "the raw-field negative must exercise canonical rejection");
  for (const auto name :
       {"optimizer_nested_write", "optimizer_reference_write",
        "optimizer_nested_raw", "optimizer_scalar_construction",
        "optimizer_scalar_construction_paths",
        "optimizer_record_construction"}) {
    const auto path = sourcePath.parent_path() / (std::string(name) + ".gti");
    const std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file.rdbuf()), {}};
    const auto library =
        sourcePath.parent_path().parent_path().parent_path() / "stdlib";
    const auto changed = lang::Frontend().analyze(
        path, text, {library / "prelude.gti", path},
        {{std::filesystem::weakly_canonical(path).string(), text}}, {library});
    expect(changed.canGenerateCode(),
           "nested and reference-field mutation controls must lower");
    if (!changed.canGenerateCode()) {
      for (const auto &diagnostic : changed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    if (name == std::string_view("optimizer_record_construction")) {
      std::size_t constructions = 0;
      for (const auto address : lang::enumerateMirBodyAddresses(changed.mir))
        for (const auto &block :
             lang::findMirBody(changed.mir, address)->blocks)
          for (const auto &instruction : block.instructions) {
            if (lang::isMirScalarFinalAddressConstruction(instruction) ||
                !lang::isMirStructuralFinalAddressConstruction(changed.mir,
                                                               instruction))
              continue;
            ++constructions;
            const auto effects = lang::effects(changed.mir, instruction);
            expect(effects.writesUnknownMemory && effects.initializesValue &&
                       !effects.invokesUserCode && !effects.maySynchronize &&
                       !effects.removableWhenUnused,
                   "generated final-address construction retains raw lifetime "
                   "effects without inventing source execution");
            auto malformed = instruction;
            malformed.lifecycle.push_back(
                {.kind = lang::MirLifecycleEventKind::Replace});
            expect(!lang::isMirStructuralFinalAddressConstruction(changed.mir,
                                                                  malformed),
                   "replacement is not vacant record construction");
            malformed = instruction;
            malformed.constructorTarget = 1;
            expect(!lang::isMirStructuralFinalAddressConstruction(changed.mir,
                                                                  malformed),
                   "a selected constructor cannot claim generated transport");
            malformed = instruction;
            malformed.rawMemoryAccess = true;
            expect(!lang::isMirStructuralFinalAddressConstruction(changed.mir,
                                                                  malformed),
                   "raw memory access is not vacant record construction");
          }
      expect(constructions > 0,
             "the record fixture must exercise the shared profile");
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = changed.hir, .mir = changed.mir, .level = level});
      expect(optimized.valid(), "nested mutation controls must replay");
      for (const auto &function : changed.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto functionName = function.source->name().lexeme;
        const bool construction = functionName == "construct_other" ||
                                  functionName == "nested_direct" ||
                                  functionName == "nested_reconstruct" ||
                                  functionName == "byte_direct" ||
                                  functionName == "array_direct";
        if (functionName != "read" && functionName != "reconstruct" &&
            !construction)
          continue;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        std::size_t proved = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proved += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        expect(
            proved ==
                (construction && level != lang::OptimizationLevel::O0 ? 1 : 0),
            "nested, returned and borrowed-field aliases cannot retain "
            "stale constructor envelopes after mutation; structural vacant "
            "construction preserves unrelated active fields: " +
                std::string(name) + "/" + std::string(functionName) + "=" +
                std::to_string(proved));
      }
    }
  }
  const auto witnessPath =
      sourcePath.parent_path() / "optimizer_witness_formation.gti";
  const std::ifstream witnessFile(witnessPath);
  const std::string witnessSource{
      std::istreambuf_iterator<char>(witnessFile.rdbuf()), {}};
  const auto witness = lang::Frontend().analyze(witnessPath, witnessSource);
  expect(witness.canGenerateCode(), "owner-witness formation must lower");
  if (witness.canGenerateCode()) {
    const auto contexts = lang::analyzeMirClosedCallSafetyChecks(witness.mir);
    expect(contexts.converged, "owner-witness contexts must converge");
    for (const auto &function : witness.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (name != "formed" && name != "changed" && name != "other_value")
        continue;
      const auto *context = contexts.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      expect(context && context->reached,
             "witness positives and negatives must have actual call contexts");
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = witness.hir, .mir = witness.mir, .level = level});
      expect(optimized.valid(), "witness formation proofs must replay");
      if (level == lang::OptimizationLevel::O0)
        expect(optimized.report.transformations.empty(),
               "witness formation leaves O0 transformation-free");
      for (const auto &function : witness.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        if (name != "formed" && name != "changed" && name != "other_value")
          continue;
        std::size_t proofs = 0;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        for (const auto &block : body->body.blocks)
          for (const auto &instruction : block.instructions)
            proofs += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
        expect(
            proofs == (name == "formed" && level != lang::OptimizationLevel::O0
                           ? 1
                           : 0),
            std::string("witness formation preserves only unrelated memory: ") +
                std::string(name));
      }
    }
    std::size_t negatives = 0;
    for (const auto &function : witness.hir.functionInstances()) {
      if (!function.source || (function.source->name().lexeme != "changed" &&
                               function.source->name().lexeme != "other_value"))
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(witness.mir, address);
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = witness.mir;
              lang::MirEditor editor(forged, "forged-witness-pointee-value");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "witness identity alone cannot prove its pointee's value");
              ++negatives;
            }
    }
    expect(negatives == 2, "both witness negatives require replay rejection");
  }
  const auto ownedPath =
      sourcePath.parent_path() / "optimizer_owned_value_transfer.gti";
  const std::ifstream ownedFile(ownedPath);
  const std::string ownedSource{
      std::istreambuf_iterator<char>(ownedFile.rdbuf()), {}};
  const auto ownedLibrary =
      sourcePath.parent_path().parent_path().parent_path() / "stdlib";
  const auto owned = lang::Frontend().analyze(
      ownedPath, ownedSource, {ownedLibrary / "prelude.gti", ownedPath},
      {{std::filesystem::weakly_canonical(ownedPath).string(), ownedSource}},
      {ownedLibrary});
  expect(owned.canGenerateCode(), "ordinary owned value transfers must lower");
  if (!owned.canGenerateCode())
    for (const auto &diagnostic : owned.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (owned.canGenerateCode()) {
    std::size_t transfers = 0;
    std::size_t drops = 0;
    std::size_t sourceDrops = 0;
    for (const auto address : lang::enumerateMirBodyAddresses(owned.mir))
      for (const auto &block : lang::findMirBody(owned.mir, address)->blocks)
        for (const auto &instruction : block.instructions) {
          const bool transfer =
              lang::isMirStructuralValueTransfer(owned.mir, instruction);
          const bool drop =
              lang::isMirStructuralValueDrop(owned.mir, instruction);
          if (instruction.kind == lang::MirInstructionKind::Drop &&
              instruction.info.type.kind == lang::SemanticType::Class)
            for (const auto &owner : owned.mir.classInstances())
              if (owner.type == instruction.info.type && owner.destructor) {
                ++sourceDrops;
                expect(
                    !drop &&
                        lang::effects(owned.mir, instruction).invokesUserCode,
                    "actual user cleanup retains its selected effects");
              }
          if (!transfer && !drop)
            continue;
          transfers += transfer;
          drops += drop;
          const auto effects = lang::effects(owned.mir, instruction);
          expect(
              !effects.readsUnknownMemory && !effects.writesUnknownMemory &&
                  !effects.invokesUserCode && !effects.invokesRuntime &&
                  !effects.maySynchronize && !effects.removableWhenUnused,
              "generated record transport/cleanup has precise shared effects "
              "without erasing lifecycle observability");
          auto malformed = instruction;
          malformed.rawMemoryAccess = true;
          expect(!lang::isMirStructuralValueTransfer(owned.mir, malformed) &&
                     !lang::isMirStructuralValueDrop(owned.mir, malformed),
                 "a raw operation cannot claim structural record effects");
          malformed = instruction;
          malformed.lifecycle.push_back(
              {.kind = lang::MirLifecycleEventKind::Replace});
          expect(!lang::isMirStructuralValueTransfer(owned.mir, malformed) &&
                     !lang::isMirStructuralValueDrop(owned.mir, malformed),
                 "replacement cannot claim construction-only effects");
        }
    expect(transfers > 0 && drops > 0 && sourceDrops > 0,
           "the fixture must exercise both shared structural effect profiles");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = owned.hir, .mir = owned.mir, .level = level});
      expect(optimized.valid(), "owned value facts must independently replay");
      if (level == lang::OptimizationLevel::O0)
        expect(optimized.report.transformations.empty(),
               "owned transport cannot introduce O0 transformations");
      for (const auto &function : owned.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        const bool positive = name == "copied" ||
                              name == "original_after_copy" ||
                              name == "prepared_copy" || name == "consumed" ||
                              name == "custom_cleanup";
        if (!positive && name != "original_after_sanitize")
          continue;
        std::size_t proofs = 0;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proofs += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        expect(proofs ==
                   (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
               std::string("independent owned field transfer: ") +
                   std::string(name));
      }
    }
    std::size_t negatives = 0;
    for (const auto &function : owned.hir.functionInstances()) {
      if (!function.source ||
          function.source->name().lexeme != "original_after_sanitize")
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(owned.mir, address);
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = owned.mir;
              lang::MirEditor editor(forged, "forged-copy-postcondition");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "callee copy mutation cannot prove caller storage safe");
              ++negatives;
            }
    }
    expect(negatives == 1, "copy independence needs a negative replay oracle");
  }
  const auto movePath = sourcePath.parent_path() / "optimizer_scalar_move.gti";
  const std::ifstream moveFile(movePath);
  const std::string moveSource{std::istreambuf_iterator<char>(moveFile.rdbuf()),
                               {}};
  const auto library =
      sourcePath.parent_path().parent_path().parent_path() / "stdlib";
  const auto moved = lang::Frontend().analyze(
      movePath, moveSource, {library / "prelude.gti", movePath},
      {{std::filesystem::weakly_canonical(movePath).string(), moveSource}},
      {library});
  expect(moved.canGenerateCode(), "ordinary scalar movement must lower");
  if (!moved.canGenerateCode())
    for (const auto &diagnostic : moved.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (moved.canGenerateCode()) {
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = moved.hir, .mir = moved.mir, .level = level});
      expect(optimized.valid(),
             "scalar movement proofs must independently replay");
      for (const auto &function : moved.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        const bool positive = name == "parameter" || name == "result" ||
                              name == "sibling" || name == "read";
        if (!positive && name != "replace_parameter" && name != "field")
          continue;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        std::size_t proved = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proved += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        expect(
            proved ==
                (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
            "moved integer results and independent owned receiver fields "
            "retain facts, but reinitialized sources never regain stale facts");
      }
      for (const auto &record : optimized.report.transformations)
        expect(std::find(record.relatedPlaces.begin(),
                         record.relatedPlaces.end(),
                         0) == record.relatedPlaces.end(),
               "scalar movement cannot publish an implicit receiver identity");
    }
    std::size_t rejectedStaleMoves = 0;
    for (const auto &function : moved.hir.functionInstances()) {
      if (!function.source ||
          (function.source->name().lexeme != "replace_parameter" &&
           function.source->name().lexeme != "field"))
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(moved.mir, address);
      if (!body)
        continue;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = moved.mir;
              lang::MirEditor editor(forged, "forged-retired-move-envelope");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "canonical replay rejects stale moved-source envelopes "
                     "after parameter or receiver-field reinitialization");
              ++rejectedStaleMoves;
            }
    }
    expect(rejectedStaleMoves == 2,
           "both scalar source reinitialization negatives exercise replay");
  }
  const auto stagePath =
      sourcePath.parent_path() / "optimizer_value_staging.gti";
  const std::ifstream stageFile(stagePath);
  const std::string stageSource{
      std::istreambuf_iterator<char>(stageFile.rdbuf()), {}};
  const auto staged = lang::Frontend().analyze(stagePath, stageSource);
  expect(staged.canGenerateCode(), "ordinary raw value staging must lower");
  if (!staged.canGenerateCode())
    for (const auto &diagnostic : staged.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (staged.canGenerateCode()) {
    const auto stagingContexts =
        lang::analyzeMirClosedCallSafetyChecks(staged.mir);
    expect(stagingContexts.converged, "address staging contexts must converge");
    std::size_t neutralStages = 0;
    bool actualArithmetic = false;
    bool actualRead = false;
    bool actualWrite = false;
    bool actualUnsafeCall = false;
    for (const auto &function : staged.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputKind == lang::HirCallInputKind::Value) {
            ++neutralStages;
            const auto effect = lang::effects(instruction);
            expect(instruction.unsafeOperation ==
                           lang::UnsafeOperationKind::None &&
                       !instruction.rawMemoryAccess &&
                       !effect.readsUnknownMemory &&
                       !effect.writesUnknownMemory && !effect.invokesUserCode &&
                       !effect.maySynchronize,
                   "SSA value staging does not repeat the producer's unsafe "
                   "operation or memory effects");
          }
          actualArithmetic |=
              instruction.operation == lang::MirOperation::PointerAdd &&
              instruction.unsafeOperation ==
                  lang::UnsafeOperationKind::PointerArithmetic;
          actualRead |= instruction.kind == lang::MirInstructionKind::Load &&
                        instruction.rawMemoryAccess;
          actualWrite |= instruction.kind == lang::MirInstructionKind::Assign &&
                         instruction.rawMemoryAccess;
          actualUnsafeCall |=
              instruction.kind == lang::MirInstructionKind::Call &&
              instruction.unsafeOperation ==
                  lang::UnsafeOperationKind::DeclaredUnsafeCall;
        }
    expect(neutralStages == 4 && actualArithmetic && actualRead &&
               actualWrite && actualUnsafeCall,
           "neutral staging controls retain actual address/read/write/call "
           "producers, not a bypass of unsafe operations");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = staged.hir, .mir = staged.mir, .level = level});
      expect(optimized.valid(), "neutral staging proofs independently replay");
      for (const auto &function : staged.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        if (name != "after_stage" && name != "after_raw_read" &&
            name != "after_raw_write" && name != "after_unsafe_call")
          continue;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        std::size_t proved = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proved += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        // The actual +0 address now remains exact, so the read preserves its
        // known pointee as well as the unrelated field. It remains a raw,
        // opaque operation; writes still invalidate the field's old value.
        expect(proved == ((name == "after_stage" || name == "after_raw_read") &&
                                  level != lang::OptimizationLevel::O0
                              ? 1
                              : 0),
               "neutral staging and exact reads preserve fields; raw and "
               "unsafe callee writes still invalidate them");
        if (name == "after_raw_read") {
          const auto *context = stagingContexts.find(
              {.kind = lang::MirBodyKind::Function, .owner = function.id});
          expect(context && context->reached && !context->closed,
                 "an exact raw read does not establish effect closure");
          std::size_t rawLoads = 0;
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              rawLoads += !instruction.tombstone &&
                          instruction.rawMemoryAccess &&
                          instruction.kind == lang::MirInstructionKind::Load;
          expect(rawLoads == 1, "the exact raw read must remain executable");
        }
      }
    }
  }
  for (const auto fixture :
       {"optimizer_getter_fields", "optimizer_global_getter_alias"}) {
    const auto path =
        sourcePath.parent_path() / (std::string(fixture) + ".gti");
    const std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file.rdbuf()), {}};
    const auto library =
        sourcePath.parent_path().parent_path().parent_path() / "stdlib";
    const auto getters = lang::Frontend().analyze(
        path, text, {library / "prelude.gti", path},
        {{std::filesystem::weakly_canonical(path).string(), text}}, {library});
    expect(getters.canGenerateCode(), "ordinary getter controls must lower");
    if (!getters.canGenerateCode()) {
      for (const auto &diagnostic : getters.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(getters.mir);
    expect(analysis.converged, "ordinary getter identities must converge");
    std::size_t observedCounter = 0;
    std::size_t reachedControls = 0;
    for (const auto &function : getters.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      const auto *context = analysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      if (name == "observe_counter") {
        ++observedCounter;
        expect(context && context->reached && context->returnInterval &&
                   context->returnInterval->lower ==
                       lang::CheckedIntegerValue{.magnitude = 4} &&
                   context->returnInterval->upper ==
                       lang::CheckedIntegerValue{.magnitude = 4},
               "a bounded constructor loop using forwarded readonly getters "
               "publishes its exact exit field");
      }
      if (name == "guarded" || name == "guarded_nested" || name == "disjoint" ||
          name == "order_changed" || name == "cached_local" ||
          name == "ambiguous" || name == "direct_changed" ||
          name == "mutable_changed" || name == "raw_changed" ||
          name == "copy_snapshot" || name.starts_with("case_")) {
        ++reachedControls;
        expect(context && context->reached,
               "getter mutation controls must be reached, not vacuous");
      }
    }
    expect(observedCounter ==
                   (fixture == std::string_view("optimizer_getter_fields")
                        ? 1
                        : 0) &&
               reachedControls ==
                   (fixture == std::string_view("optimizer_getter_fields") ? 10
                                                                           : 8),
           "getter and global-alias inventories must remain complete");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = getters.hir, .mir = getters.mir, .level = level});
      expect(optimized.valid(), "getter field proofs independently replay");
      for (const auto &record : optimized.report.transformations)
        expect(std::find(record.relatedPlaces.begin(),
                         record.relatedPlaces.end(),
                         0) == record.relatedPlaces.end(),
               "getter origins cannot publish an implicit receiver identity");
      for (const auto &function : getters.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        const bool positive = name == "guarded" || name == "guarded_nested" ||
                              name == "disjoint" ||
                              name == "case_independent_snapshot";
        const bool negative =
            name == "order_changed" || name == "cached_local" ||
            name == "ambiguous" || name == "direct_changed" ||
            name == "mutable_changed" || name == "raw_changed" ||
            name == "copy_snapshot" || name.starts_with("case_global_");
        if (!positive && !negative)
          continue;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        std::size_t proved = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proved += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        if (proved !=
            (positive && level != lang::OptimizationLevel::O0 ? 1 : 0))
          std::cerr << "getter control: " << name
                    << " level=" << static_cast<int>(level)
                    << " proofs=" << proved << '\n';
        expect(proved ==
                   (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
               "current getter fields and immutable snapshots retain bounds; "
               "changed, copied or ambiguous referents stay checked");
      }
    }
    std::size_t rejected = 0;
    for (const auto &function : getters.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (name != "order_changed" && name != "cached_local" &&
          name != "ambiguous" && name != "direct_changed" &&
          name != "mutable_changed" && name != "raw_changed" &&
          name != "copy_snapshot" && !name.starts_with("case_global_"))
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(getters.mir, address);
      if (!body)
        continue;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = getters.mir;
              lang::MirEditor editor(forged, "forged-stale-getter-origin");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "canonical replay rejects stale getter/global aliases");
              ++rejected;
            }
    }
    expect(rejected == 7,
           "every getter mutation negative exercises canonical rejection");
  }
  const auto nestedLoopPath =
      sourcePath.parent_path() / "optimizer_nested_getter_loop.gti";
  const std::ifstream nestedLoopFile(nestedLoopPath);
  const std::string nestedLoopSource{
      std::istreambuf_iterator<char>(nestedLoopFile.rdbuf()), {}};
  const auto nestedLoop = lang::Frontend().analyze(
      nestedLoopPath, nestedLoopSource,
      {library / "prelude.gti", nestedLoopPath},
      {{std::filesystem::weakly_canonical(nestedLoopPath).string(),
        nestedLoopSource}},
      {library});
  expect(nestedLoop.canGenerateCode(), "nested getter loops must lower");
  if (nestedLoop.canGenerateCode()) {
    const auto analysis =
        lang::analyzeMirClosedCallSafetyChecks(nestedLoop.mir);
    expect(analysis.converged, "nested getter loops must converge");
    std::size_t observations = 0;
    for (const auto &function : nestedLoop.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (name != "observe_nested_counter" && name != "observe_vector_length")
        continue;
      ++observations;
      const auto *fact = analysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      expect(fact && fact->reached && fact->returnInterval &&
                 fact->returnInterval->lower ==
                     lang::CheckedIntegerValue{.magnitude = 4} &&
                 fact->returnInterval->upper ==
                     lang::CheckedIntegerValue{.magnitude = 4},
             std::string(name) + " must publish the exact nested loop exit");
    }
    expect(observations == 2, "both nested loop outcomes must be inspected");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = nestedLoop.hir, .mir = nestedLoop.mir, .level = level});
      expect(optimized.valid(), "nested loop exit proofs independently replay");
      if (level == lang::OptimizationLevel::O0)
        expect(optimized.report.transformations.empty(),
               "nested object refinement cannot introduce O0 transformations");
    }
    std::size_t rejected = 0;
    for (const auto &function : nestedLoop.hir.functionInstances()) {
      if (!function.source ||
          function.source->name().lexeme != "changed_nested")
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(nestedLoop.mir, address);
      if (!body)
        continue;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = nestedLoop.mir;
              lang::MirEditor editor(forged, "forged-nested-loop-exit-fact");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "a nested write retires the refined constructor result");
              ++rejected;
            }
    }
    expect(rejected == 1, "nested mutation must exercise canonical rejection");
  } else {
    for (const auto &diagnostic : nestedLoop.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto prefixPath =
      sourcePath.parent_path() / "optimizer_source_prefix_fields.gti";
  const std::ifstream prefixFile(prefixPath);
  const std::string prefixSource{
      std::istreambuf_iterator<char>(prefixFile.rdbuf()), {}};
  const auto prefix = lang::Frontend().analyze(
      prefixPath, prefixSource, {library / "prelude.gti", prefixPath},
      {{std::filesystem::weakly_canonical(prefixPath).string(), prefixSource}},
      {library});
  expect(prefix.canGenerateCode(), "source prefix publication must lower");
  if (prefix.canGenerateCode()) {
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(prefix.mir);
    expect(analysis.converged, "source prefix field publication must converge");
    std::size_t observations = 0;
    for (const auto &function : prefix.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (name != "initial_length" && name != "reserved_capacity" &&
          name != "appended_length")
        continue;
      ++observations;
      const auto *fact = analysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      const std::uint64_t value = name == "initial_length" ? 0 : 4;
      expect(fact && fact->reached && fact->returnInterval &&
                 fact->returnInterval->lower ==
                     lang::CheckedIntegerValue{.magnitude = value} &&
                 fact->returnInterval->upper ==
                     lang::CheckedIntegerValue{.magnitude = value},
             "ordinary source fields publish after temporary allocation-result "
             "cleanup without inferring fresh or initialized raw memory");
    }
    expect(
        observations == 3,
        "source capacity, empty and repeatedly appended lengths are observed");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3})
      expect(lang::OptimizationPipeline()
                 .run({.hir = prefix.hir, .mir = prefix.mir, .level = level})
                 .valid(),
             "source prefix field publication independently replays");
  }
  const std::string invalidMove =
      "#include <std/memory>\nint main() { int source = 7; "
      "int result = std::move(source); return source + result; }\n";
  const auto invalidPath = sourcePath.parent_path() / "moved-source-read.gti";
  const auto invalid = lang::Frontend().analyze(
      invalidPath, invalidMove, {library / "prelude.gti", invalidPath},
      {{std::filesystem::weakly_canonical(invalidPath).string(), invalidMove}},
      {library});
  expect(!invalid.canGenerateCode() &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2018";
                         }),
         "copying a scalar envelope never restores moved-source availability");
  constexpr std::size_t chainLength = 128;
  std::string chain;
  for (std::size_t index = chainLength; index-- > 0;) {
    chain += "int step" + std::to_string(index) + "(int value) { return ";
    chain += index + 1 == chainLength
                 ? "value"
                 : "step" + std::to_string(index + 1) + "(value)";
    chain += " + 1; }\n";
  }
  chain += "int main() { return step0(7) == 135 ? 0 : 1; }\n";
  const auto deep = lang::Frontend().analyze("object-summary-chain.gti", chain);
  expect(deep.canGenerateCode(), "the long ordinary source chain must lower");
  if (deep.canGenerateCode()) {
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(deep.mir);
    expect(analysis.converged && analysis.iterations <= 32,
           "fixed facts traverse 128 bodies within the per-body work budget");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = deep.hir, .mir = deep.mir, .level = level});
      expect(optimized.valid(),
             "long-chain contextual proofs independently replay");
      std::size_t proved = 0;
      for (const auto &function : optimized.mir.functionInstances())
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions)
            proved += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
      expect(proved == (level == lang::OptimizationLevel::O0 ? 0 : chainLength),
             "every exact addition in the long chain is proved; O0 stays "
             "unchanged");
    }
  }
  // Each successive exact callee summary makes one more call reachable. This
  // deliberately consumes the per-body analysis budget, after the first
  // contextual arithmetic proof has already been observed.
  std::string budget;
  for (std::size_t index = 0; index < 40; ++index)
    budget += "int child" + std::to_string(index) + "() { return 0; }\n";
  budget += "int budget(int input) { int first = input + 1;\n";
  for (std::size_t index = 0; index < 40; ++index)
    budget += "[[discard]] child" + std::to_string(index) + "();\n";
  budget += "return first; }\n"
            "int independent(int input) { return input + 1; }\n"
            "int main() { int first = budget(7); int second = independent(7); "
            "return first == 8 && second == 8 ? 0 : 1; }\n";
  const auto bounded =
      lang::Frontend().analyze("object-summary-budget.gti", budget);
  expect(bounded.canGenerateCode(), "the work-budget control must lower");
  if (bounded.canGenerateCode()) {
    lang::HirFunctionInstanceId budgetOwner = 0;
    lang::HirFunctionInstanceId independentOwner = 0;
    for (const auto &function : bounded.hir.functionInstances()) {
      if (!function.source)
        continue;
      if (function.source->name().lexeme == "budget")
        budgetOwner = function.id;
      if (function.source->name().lexeme == "independent")
        independentOwner = function.id;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = budgetOwner};
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(bounded.mir);
    const auto *context = analysis.find(address);
    expect(
        analysis.converged && analysis.iterations <= 32 && context &&
            !context->closed && context->safety.proofs.empty(),
        "budget rejection withdraws earlier contextual proofs and converges");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = bounded.hir, .mir = bounded.mir, .level = level});
      expect(optimized.valid(), "budget rejection must independently replay");
      const auto countProofs = [&](lang::HirFunctionInstanceId owner) {
        std::size_t count = 0;
        const auto *body = optimized.mir.findFunctionInstance(owner);
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              count += instruction.safetyDischarges.size();
        return count;
      };
      expect(countProofs(budgetOwner) == 0 &&
                 countProofs(independentOwner) ==
                     (level == lang::OptimizationLevel::O0 ? 0 : 1),
             "rejecting one body's work budget cannot keep its stale proof or "
             "hide a subsequent independent exact client");
    }
    bool rejected = false;
    if (const auto *body = lang::findMirBody(bounded.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto malformed = bounded.mir;
              lang::MirEditor editor(malformed, "forged-rejected-budget-proof");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "the canonical editor rejects a withdrawn budget proof");
              rejected = true;
            }
    expect(rejected, "budget rejection exercises canonical proof rejection");
  }
  const auto updatesPath =
      sourcePath.parent_path() / "optimizer_receiver_updates.gti";
  const std::ifstream updatesFile(updatesPath);
  const std::string updatesSource{
      std::istreambuf_iterator<char>(updatesFile.rdbuf()), {}};
  const auto updates = lang::Frontend().analyze(
      updatesPath, updatesSource, {library / "prelude.gti", updatesPath},
      {{std::filesystem::weakly_canonical(updatesPath).string(),
        updatesSource}},
      {library});
  expect(updates.canGenerateCode(),
         "entry-relative receiver updates must lower");
  if (updates.canGenerateCode()) {
    const auto contexts = lang::analyzeMirClosedCallSafetyChecks(updates.mir);
    expect(contexts.converged, "entry-relative receiver updates must converge");
    std::size_t conditionalBodies = 0;
    for (const auto &function : updates.hir.functionInstances())
      if (function.source && function.source->name().lexeme == "conditional") {
        const auto *context = contexts.find(
            {.kind = lang::MirBodyKind::Function, .owner = function.id});
        expect(context && context->reached && context->closed,
               "the conditional update must exercise an admitted joined body");
        ++conditionalBodies;
      }
    expect(conditionalBodies == 1,
           "one admitted conditional-update control is required");
    std::size_t observations = 0;
    std::size_t negatives = 0;
    for (const auto &function : updates.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (!name.starts_with("observe_") && !name.starts_with("bad_"))
        continue;
      const auto *context = contexts.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      expect(context && context->reached,
             "receiver update controls must have reached contexts: " +
                 std::string(name));
      if (name.starts_with("observe_")) {
        ++observations;
        expect(
            context && context->returnInterval &&
                context->returnInterval->lower == lang::CheckedIntegerValue{} &&
                context->returnInterval->upper == lang::CheckedIntegerValue{},
            "exact repeated/nested/decremented field result: " +
                std::string(name));
      } else
        ++negatives;
    }
    expect(observations == 3 && negatives == 6,
           "the complete receiver update inventory is checked");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = updates.hir, .mir = updates.mir, .level = level});
      expect(optimized.valid(),
             "entry-relative field evidence independently replays");
      if (level == lang::OptimizationLevel::O0)
        expect(optimized.report.transformations.empty(),
               "receiver updates do not transform O0");
      for (const auto &record : optimized.report.transformations)
        expect(std::find(record.relatedPlaces.begin(),
                         record.relatedPlaces.end(),
                         0) == record.relatedPlaces.end(),
               "receiver updates publish no implicit MIR root");
      for (const auto &function : updates.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto name = function.source->name().lexeme;
        const bool positive = name.starts_with("observe_");
        if (!positive && !name.starts_with("bad_") && name != "up")
          continue;
        std::size_t proved = 0;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        for (const auto &block : body->body.blocks)
          for (const auto &instruction : block.instructions)
            proved += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
        expect(proved ==
                   (positive && level != lang::OptimizationLevel::O0 ? 1 : 0),
               "receiver updates preserve negative checks: " +
                   std::string(name));
      }
    }
    std::size_t rejected = 0;
    for (const auto &function : updates.hir.functionInstances()) {
      if (!function.source)
        continue;
      const auto name = function.source->name().lexeme;
      if (!name.starts_with("bad_") && name != "up")
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto *body = lang::findMirBody(updates.mir, address);
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto forged = updates.mir;
              lang::MirEditor editor(forged, "forged-receiver-update");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "canonical replay rejects a stale/conditional receiver "
                     "update");
              ++rejected;
            }
    }
    expect(rejected == 6, "all five mutation/branch negatives and first "
                          "failure reject forged proofs");
  } else {
    for (const auto &diagnostic : updates.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  return failures == 0 ? 0 : 1;
}
