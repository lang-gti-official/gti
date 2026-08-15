#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view marker =
    "// GTI verified-MIR body: scalar-failure-callgraph-v1";
constexpr std::string_view lifecycleMarker =
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
      [name](const lang::HirFunctionInstance &function) {
        return function.source != nullptr &&
               function.source->name().lexeme == name;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

const lang::HirClassInstance *findHirClass(const lang::HirProgram &program,
                                           std::string_view name) {
  const auto found = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [name](const lang::HirClassInstance &instance) {
        return instance.source != nullptr &&
               instance.source->name().lexeme == name;
      });
  return found == program.classInstances().end() ? nullptr : &*found;
}

lang::MirFunctionInstance *findFunction(const lang::HirProgram &hir,
                                        lang::MirProgram &mir,
                                        std::string_view name) {
  const lang::HirFunctionInstance *source = findHirFunction(hir, name);
  if (source == nullptr) {
    return nullptr;
  }
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      mir.functionInstances());
  const auto found = std::find_if(
      functions.begin(), functions.end(),
      [&](const auto &function) { return function.id == source->id; });
  return found == functions.end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *findFunction(const lang::HirProgram &hir,
                                              const lang::MirProgram &mir,
                                              std::string_view name) {
  return findFunction(hir, const_cast<lang::MirProgram &>(mir), name);
}

const lang::MirConstructorInstance *
findConstructor(const lang::MirProgram &mir,
                const lang::HirClassInstance *owner) {
  if (owner == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(
      mir.constructorInstances().begin(), mir.constructorInstances().end(),
      [&](const lang::MirConstructorInstance &constructor) {
        return constructor.owner == owner->id;
      });
  return found == mir.constructorInstances().end() ? nullptr : &*found;
}

const lang::MirDestructorInstance *
findDestructor(const lang::MirProgram &mir,
               const lang::HirClassInstance *owner) {
  return owner == nullptr || !owner->destructor
             ? nullptr
             : mir.findDestructorInstance(*owner->destructor);
}

lang::MirInstruction *findInstruction(lang::MirFunctionInstance *function,
                                      lang::MirInstructionKind kind,
                                      lang::FailurePropagationKind propagation =
                                          lang::FailurePropagationKind::None) {
  if (function == nullptr) {
    return nullptr;
  }
  for (lang::MirBlock &block : function->body.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const lang::MirInstruction &instruction) {
          return instruction.kind == kind &&
                 instruction.definedFailure.propagation == propagation &&
                 (!instruction.definedFailure.localOrigins.empty() ||
                  propagation != lang::FailurePropagationKind::None);
        });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

const lang::MirFailureRecord *
recordFor(const lang::MirFunctionInstance &function,
          const lang::MirInstruction &instruction) {
  const auto found = std::find_if(
      function.body.failureRecords.begin(), function.body.failureRecords.end(),
      [&](const lang::MirFailureRecord &record) {
        return record.producerInstruction == instruction.id;
      });
  return found == function.body.failureRecords.end() ? nullptr : &*found;
}

std::size_t failureCleanupDrops(const lang::MirFunctionInstance &function,
                                const lang::MirFailureRecord &record) {
  const lang::MirBlock *cleanup =
      function.body.findBlock(record.parameterBlock);
  return cleanup == nullptr
             ? 0
             : static_cast<std::size_t>(std::count_if(
                   cleanup->instructions.begin(), cleanup->instructions.end(),
                   [](const lang::MirInstruction &instruction) {
                     return instruction.kind ==
                                lang::MirInstructionKind::Drop &&
                            instruction.lifecycle.size() == 1 &&
                            instruction.lifecycle.front().failureCleanup;
                   }));
}

bool exactFailureCleanupDrops(const lang::MirFunctionInstance &function,
                              const lang::MirFailureRecord &record,
                              std::size_t exactDrops) {
  const lang::MirBlock *cleanup =
      function.body.findBlock(record.parameterBlock);
  if (cleanup == nullptr ||
      failureCleanupDrops(function, record) != exactDrops) {
    return false;
  }
  return std::all_of(
      cleanup->instructions.begin(), cleanup->instructions.end(),
      [&](const lang::MirInstruction &instruction) {
        if (instruction.kind != lang::MirInstructionKind::Drop) {
          return true;
        }
        if (instruction.lifecycle.size() != 1 || !instruction.destination) {
          return false;
        }
        const lang::MirLifecycleEvent &event = instruction.lifecycle.front();
        const lang::MirDropObligation *obligation =
            function.body.findDropObligation(event.source);
        return event.kind == lang::MirLifecycleEventKind::Drop &&
               event.failureCleanup && !event.conditional &&
               event.target == 0 && obligation != nullptr &&
               obligation->place == *instruction.destination &&
               obligation->dropType.destructor.has_value() &&
               obligation->dropType.requiresActiveCleanup;
      });
}

bool exactFailureEdge(const lang::MirFunctionInstance &function,
                      const lang::MirInstruction &instruction,
                      std::size_t exactDrops) {
  const lang::MirFailureRecord *record = recordFor(function, instruction);
  const lang::MirBlock *producer =
      record == nullptr ? nullptr
                        : function.body.findBlock(record->producerBlock);
  const lang::MirBlock *cleanup =
      record == nullptr ? nullptr
                        : function.body.findBlock(record->parameterBlock);
  return record != nullptr && producer != nullptr && cleanup != nullptr &&
         !producer->instructions.empty() &&
         producer->instructions.back().id == instruction.id &&
         producer->terminator.kind == lang::MirTerminatorKind::Invoke &&
         producer->terminator.invokeInstruction == instruction.id &&
         producer->terminator.failureRecord == record->id &&
         producer->terminator.target != 0 &&
         producer->terminator.elseTarget == record->parameterBlock &&
         cleanup->failureParameter == record->id &&
         cleanup->terminator.kind ==
             lang::MirTerminatorKind::PropagateFailure &&
         cleanup->terminator.failureRecord == record->id &&
         exactFailureCleanupDrops(function, *record, exactDrops);
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

bool emissionRejected(const lang::FrontendResult &frontend,
                      const lang::MirProgram &mir,
                      const lang::OptimizationResult &compatibility) {
  try {
    (void)emit(frontend, mir, compatibility);
    return false;
  } catch (const std::logic_error &) {
    return true;
  }
}

std::size_t countSubstring(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string_view::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

std::string_view definitionForMarker(std::string_view generated,
                                     std::string_view familyMarker,
                                     std::size_t instance) {
  const std::string exactMarker = std::string(familyMarker) +
                                  " function-instance " +
                                  std::to_string(instance);
  const std::size_t markerAt = generated.find(exactMarker);
  if (markerAt == std::string_view::npos) {
    return {};
  }
  const std::size_t bodyStart = generated.rfind('{', markerAt);
  if (bodyStart == std::string_view::npos) {
    return {};
  }
  const std::size_t signatureStart = generated.rfind('\n', bodyStart);
  std::size_t depth = 0;
  for (std::size_t at = bodyStart; at < generated.size(); ++at) {
    if (generated[at] == '{') {
      ++depth;
    } else if (generated[at] == '}' && --depth == 0) {
      return generated.substr(
          signatureStart == std::string_view::npos ? 0 : signatureStart + 1,
          at -
              (signatureStart == std::string_view::npos ? 0
                                                        : signatureStart + 1) +
              1);
    }
  }
  return {};
}

std::string_view nativeMainDefinition(std::string_view generated) {
  constexpr std::string_view signature = "\nint main() {";
  const std::size_t start = generated.find(signature);
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t bodyStart = start + signature.size() - 1;
  std::size_t depth = 0;
  for (std::size_t at = bodyStart; at < generated.size(); ++at) {
    if (generated[at] == '{') {
      ++depth;
    } else if (generated[at] == '}' && --depth == 0) {
      return generated.substr(start + 1, at - start);
    }
  }
  return {};
}

void expectHiddenFailureAbi(std::string_view definition,
                            std::string_view sourceName,
                            std::size_t expectedReturns) {
  const std::size_t bodyStart = definition.find('{');
  const std::string_view signature = definition.substr(0, bodyStart);
  expect(
      !definition.empty() && bodyStart != std::string_view::npos &&
          signature.starts_with("  bool ") &&
          signature.find(sourceName) != std::string_view::npos &&
          signature.find("std::int32_t *__gti_mir_out_result") !=
              std::string_view::npos &&
          signature.find("::gti_failure_record_v1 *__gti_mir_failure_record") !=
              std::string_view::npos,
      "each selected body should expose the exact bool/out-result/record "
      "hidden ABI");
  expect(expectedReturns != 0 &&
             countSubstring(definition, "// GTI MIR return publication") ==
                 expectedReturns &&
             countSubstring(definition, "*__gti_mir_out_result =") ==
                 expectedReturns &&
             countSubstring(definition, "return true;") == expectedReturns &&
             definition.find("return false;") != std::string_view::npos &&
             definition.find("gti_rt_failure_terminate_v1") ==
                 std::string_view::npos,
         "a hidden body should return success/failure without containing the "
         "hosted process boundary");
  std::size_t returns = 0;
  for (std::size_t publication =
           definition.find("// GTI MIR return publication");
       publication != std::string_view::npos;
       publication =
           definition.find("// GTI MIR return publication", publication + 1)) {
    ++returns;
    const std::size_t nextPublication =
        definition.find("// GTI MIR return publication", publication + 1);
    const std::size_t assignment =
        definition.find("*__gti_mir_out_result =", publication);
    const std::size_t success = definition.find("return true;", assignment);
    expect(assignment != std::string_view::npos &&
               success != std::string_view::npos && publication < assignment &&
               assignment < success &&
               (nextPublication == std::string_view::npos ||
                success < nextPublication),
           "each verified MIR Return should publish the out result immediately "
           "before success");
  }
  expect(returns == expectedReturns,
         "the hidden body should preserve every exact MIR Return path");
}

void expectFailureCleanupBeforeFalse(std::string_view definition,
                                     std::size_t expectedPaths) {
  std::size_t paths = 0;
  for (std::size_t failure = definition.find("return false;");
       failure != std::string_view::npos;
       failure = definition.find("return false;", failure + 1)) {
    ++paths;
    const std::size_t block =
        definition.rfind("// GTI MIR failure-record ", failure);
    const std::string_view failureBlock =
        block == std::string_view::npos
            ? std::string_view{}
            : definition.substr(block, failure - block);
    expect(!failureBlock.empty() &&
               failureBlock.find("// GTI MIR failure cleanup "
                                 "drop-obligation ") !=
                   std::string_view::npos &&
               failureBlock.find(".destroy();") != std::string_view::npos &&
               failureBlock.find("*__gti_mir_out_result") ==
                   std::string_view::npos,
           "each failure block should run its exact MIR drop before returning "
           "false without publishing a result");
  }
  expect(paths == expectedPaths,
         "the emitted body should preserve every exact MIR failure edge");
}

void expectSelected(std::string_view generated,
                    const lang::MirFunctionInstance &leaf,
                    const lang::MirFunctionInstance &caller,
                    const lang::MirFunctionInstance &entry) {
  expect(countSubstring(generated, marker) == 3,
         "the atomic hosted component should select leaf, caller, and entry");
  expect(generated.find("::gti_failure_record_v1 *") !=
                 std::string_view::npos &&
             generated.find("GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1") !=
                 std::string_view::npos &&
             generated.find("GTI_FAILURE_DETAIL_ADDITION_V1") !=
                 std::string_view::npos,
         "the hidden ABI should carry an exact fixed record and detector");
  expect(
      generated.find("gti_rt_failure_terminate_v1") != std::string_view::npos &&
          generated.find("GTI_FAILURE_EXIT_STATUS") != std::string_view::npos &&
          generated.find("catch (...)") != std::string_view::npos,
      "the hosted entry should terminate through the runtime and contain "
      "native exceptions");
  const std::string_view leafDefinition =
      definitionForMarker(generated, marker, leaf.id);
  const std::string_view callerDefinition =
      definitionForMarker(generated, marker, caller.id);
  const std::string_view entryDefinition =
      definitionForMarker(generated, marker, entry.id);
  const auto returnCount = [](const lang::MirFunctionInstance &function) {
    return std::count_if(
        function.body.blocks.begin(), function.body.blocks.end(),
        [](const lang::MirBlock &block) {
          return block.terminator.kind == lang::MirTerminatorKind::Return;
        });
  };
  expectHiddenFailureAbi(leafDefinition, "_overflowing_leaf(",
                         returnCount(leaf));
  expectHiddenFailureAbi(callerDefinition, "_propagate_overflow(",
                         returnCount(caller));
  expectHiddenFailureAbi(entryDefinition, "__gti_entry(", returnCount(entry));
  expectFailureCleanupBeforeFalse(leafDefinition, 1);
  expectFailureCleanupBeforeFalse(callerDefinition, 2);
  expectFailureCleanupBeforeFalse(entryDefinition, 1);
  expect(leafDefinition.find("UINT32_C(1)") != std::string_view::npos &&
             leafDefinition.find("UINT32_C(2)") == std::string_view::npos &&
             callerDefinition.find("UINT32_C(2)") != std::string_view::npos,
         "local detectors should publish their exact original artifact site "
         "without re-siting propagated records");
  expect(callerDefinition.find("__gti_mir_failure_record)") !=
                 std::string_view::npos &&
             entryDefinition.find("__gti_mir_failure_record)") !=
                 std::string_view::npos &&
             callerDefinition.find("&__gti_mir_failure_record") ==
                 std::string_view::npos &&
             entryDefinition.find("&__gti_mir_failure_record") ==
                 std::string_view::npos,
         "propagating bodies should pass the unchanged failure-record pointer "
         "to their callees");

  const std::string_view nativeMain = nativeMainDefinition(generated);
  constexpr std::string_view firewall =
      "catch (...) {\n"
      "    std::_Exit(GTI_FAILURE_EXIT_STATUS);\n"
      "  }";
  expect(!nativeMain.empty() &&
             countSubstring(generated, "::gti_rt_failure_terminate_v1(") == 1 &&
             nativeMain.find(firewall) != std::string_view::npos &&
             nativeMain.find("::gti_failure_record_v1 "
                             "__gti_failure_record{};") !=
                 std::string_view::npos &&
             nativeMain.find("::__gti_program::__gti_entry(") !=
                 std::string_view::npos &&
             nativeMain.find("&__gti_failure_record") != std::string_view::npos,
         "the one hosted boundary should own the record, terminate once, and "
         "immediately firewall an escaping native exception with status 70");
  const std::size_t catchAt = nativeMain.find("catch (...)");
  const std::size_t catchEnd = catchAt == std::string_view::npos
                                   ? std::string_view::npos
                                   : nativeMain.find('}', catchAt);
  const std::string_view catchBody =
      catchEnd == std::string_view::npos
          ? std::string_view{}
          : nativeMain.substr(catchAt, catchEnd - catchAt + 1);
  expect(!catchBody.empty() &&
             catchBody.find("__gti_failure_record") == std::string_view::npos &&
             catchBody.find("gti_rt_failure") == std::string_view::npos,
         "the native-exception firewall must not forge or report a GTI "
         "failure record");
  expect(countSubstring(generated, lifecycleMarker) == 2 &&
             generated.find("constructor-instance") != std::string_view::npos &&
             generated.find("destructor-instance") != std::string_view::npos,
         "the failure component should reuse verified MIR constructor and "
         "destructor bodies");
}

void testFailureGraph(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the failure-callgraph fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(effects.functions.size() == frontend.mir.functionInstances().size() &&
             std::equal(effects.functions.begin(), effects.functions.end(),
                        frontend.mir.functionInstances().begin(),
                        [](bool effect, const auto &function) {
                          return effect == function.mayRaiseDefinedFailure;
                        }),
         "final MIR should retain the independently derived failure effects");

  const lang::MirFunctionInstance *leaf =
      findFunction(frontend.hir, frontend.mir, "overflowing_leaf");
  const lang::MirFunctionInstance *caller =
      findFunction(frontend.hir, frontend.mir, "propagate_overflow");
  const lang::MirFunctionInstance *entry =
      findFunction(frontend.hir, frontend.mir, "main");
  const lang::HirClassInstance *owner =
      findHirClass(frontend.hir, "FailureScope");
  const lang::MirConstructorInstance *constructor =
      findConstructor(frontend.mir, owner);
  const lang::MirDestructorInstance *destructor =
      findDestructor(frontend.mir, owner);
  expect(leaf != nullptr && caller != nullptr && entry != nullptr &&
             leaf->mayRaiseDefinedFailure && caller->mayRaiseDefinedFailure &&
             entry->mayRaiseDefinedFailure &&
             entry->entryKind == lang::ProgramEntryKind::NoArguments,
         "the unique no-argument entry graph should remain failure-capable");
  expect(owner != nullptr && constructor != nullptr && destructor != nullptr &&
             constructor->owner == owner->id &&
             destructor->owner == owner->id &&
             !constructor->mayRaiseDefinedFailure &&
             !destructor->mayRaiseDefinedFailure &&
             constructor->id <= effects.constructors.size() &&
             destructor->id <= effects.destructors.size() &&
             !effects.constructors[constructor->id - 1] &&
             !effects.destructors[destructor->id - 1],
         "the exact constructor and destructor closure should be proved "
         "failure-free in MIR");
  if (leaf == nullptr || caller == nullptr || entry == nullptr ||
      constructor == nullptr || destructor == nullptr) {
    return;
  }

  lang::MirInstruction *detector =
      findInstruction(const_cast<lang::MirFunctionInstance *>(leaf),
                      lang::MirInstructionKind::Compute);
  lang::MirInstruction *propagation = findInstruction(
      const_cast<lang::MirFunctionInstance *>(caller),
      lang::MirInstructionKind::Call, lang::FailurePropagationKind::DirectCall);
  lang::MirInstruction *callerDetector =
      findInstruction(const_cast<lang::MirFunctionInstance *>(caller),
                      lang::MirInstructionKind::Compute);
  lang::MirInstruction *boundary = findInstruction(
      const_cast<lang::MirFunctionInstance *>(entry),
      lang::MirInstructionKind::Call, lang::FailurePropagationKind::DirectCall);

  const bool exactOrigin =
      detector != nullptr && detector->operation == lang::MirOperation::Add &&
      detector->definedFailure.propagation ==
          lang::FailurePropagationKind::None &&
      detector->definedFailure.localOrigins.size() == 1 &&
      detector->definedFailure.localOrigins.front().outcomes ==
          std::vector<lang::DefinedFailureOutcome>{
              {.code = lang::DefinedFailureCode::IntegerOverflow,
               .detail = lang::DefinedFailureDetail::Addition}} &&
      detector->localFailureSites.size() == 1 &&
      frontend.mir.failureMetadata().siteFor(
          detector->definedFailure.localOrigins.front()) ==
          detector->localFailureSites.front();
  expect(exactOrigin && exactFailureEdge(*leaf, *detector, 1),
         "the local detector should bind one canonical site and drop the leaf "
         "owner before propagation");
  expect(propagation != nullptr &&
             propagation->definedFailure.localOrigins.empty() &&
             propagation->localFailureSites.empty() &&
             exactFailureEdge(*caller, *propagation, 1),
         "the caller should preserve the callee record and clean its owner");
  expect(detector != nullptr && callerDetector != nullptr &&
             callerDetector->operation == lang::MirOperation::Subtract &&
             callerDetector->definedFailure.localOrigins.size() == 1 &&
             callerDetector->localFailureSites.size() == 1 &&
             callerDetector->localFailureSites.front() !=
                 detector->localFailureSites.front() &&
             exactFailureEdge(*caller, *callerDetector, 1),
         "the caller-local detector should own a distinct site without "
         "re-siting propagated leaf failure");
  expect(boundary != nullptr && boundary->definedFailure.localOrigins.empty() &&
             boundary->localFailureSites.empty() &&
             exactFailureEdge(*entry, *boundary, 1),
         "the entry should preserve the original record through its final "
         "cleanup edge");

  const lang::FailureMetadata &metadata = frontend.mir.failureMetadata();
  const auto exactOutcomeSite =
      [&](lang::DefinedFailureCode code, lang::DefinedFailureDetail detail,
          std::size_t line, std::size_t start, std::size_t end) {
        return std::count_if(
            metadata.sites().begin(), metadata.sites().end(),
            [&](const lang::FailureSiteDescriptor &site) {
              return site.logicalSource == fixture.filename().string() &&
                     site.line == static_cast<int>(line) &&
                     site.start == start && site.end == end &&
                     site.outcomes == std::vector<lang::DefinedFailureOutcome>{
                                          {.code = code, .detail = detail}};
            });
      };
  expect(lang::verifyFailureMetadata(metadata).valid() &&
             metadata.sites().size() == 2 &&
             !metadata.artifactIdentity().isZero() &&
             exactOutcomeSite(lang::DefinedFailureCode::IntegerOverflow,
                              lang::DefinedFailureDetail::Addition, 20, 432,
                              433) == 1 &&
             exactOutcomeSite(lang::DefinedFailureCode::IntegerOverflow,
                              lang::DefinedFailureDetail::Subtraction, 26, 561,
                              562) == 1,
         "the hosted component should own two exact immutable failure sites");
  expect(lang::MirPrinter().print(frontend.mir).starts_with("mir-v23 "),
         "the failure-capable family should consume the verified v23 schema");

  const lang::OptimizationPipeline pipeline;
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const lang::OptimizationResult compatibility =
        pipeline.run(frontend.hir, level);
    const lang::OptimizedProgram optimized =
        pipeline.run({.hir = frontend.hir,
                      .mir = frontend.mir,
                      .level = level,
                      .compatibility = &compatibility});
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "O0/O1/O3 failure-callgraph MIR should remain verified");
    if (optimized.valid()) {
      const lang::MirFunctionInstance *optimizedLeaf =
          findFunction(frontend.hir, optimized.mir, "overflowing_leaf");
      const lang::MirFunctionInstance *optimizedCaller =
          findFunction(frontend.hir, optimized.mir, "propagate_overflow");
      const lang::MirFunctionInstance *optimizedEntry =
          findFunction(frontend.hir, optimized.mir, "main");
      expect(optimizedLeaf != nullptr && optimizedCaller != nullptr &&
                 optimizedEntry != nullptr,
             "optimized MIR should retain the exact hosted call graph");
      if (optimizedLeaf != nullptr && optimizedCaller != nullptr &&
          optimizedEntry != nullptr) {
        expectSelected(emit(frontend, optimized.mir, compatibility).contents,
                       *optimizedLeaf, *optimizedCaller, *optimizedEntry);
      }
    }
  }
}

void testAtomicNearMiss(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the reverse-caller near miss should remain a valid GTI program");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  const auto functionRaises = [&](const lang::HirFunctionInstance *function) {
    return function != nullptr && function->id != 0 &&
           function->id <= effects.functions.size() &&
           effects.functions[function->id - 1];
  };
  const auto constructorRaises =
      [&](const lang::MirConstructorInstance *constructor) {
        return constructor != nullptr && constructor->id != 0 &&
               constructor->id <= effects.constructors.size() &&
               effects.constructors[constructor->id - 1];
      };
  const auto destructorRaises =
      [&](const lang::MirDestructorInstance *destructor) {
        return destructor != nullptr && destructor->id != 0 &&
               destructor->id <= effects.destructors.size() &&
               effects.destructors[destructor->id - 1];
      };
  const auto bodyCalls = [](const lang::HirBody &body,
                            lang::HirFunctionInstanceId target) {
    return std::any_of(body.values.begin(), body.values.end(),
                       [&](const lang::HirValue &value) {
                         return value.functionTarget == target;
                       });
  };
  const std::string name = fixture.filename().string();
  const lang::HirFunctionInstance *entry =
      findHirFunction(frontend.hir, "main");
  expect(functionRaises(entry),
         "each atomic near miss should retain an otherwise failure-capable "
         "hosted entry");
  if (name.find("reverse_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    const lang::HirFunctionInstance *external =
        findHirFunction(frontend.hir, "compatibility_caller");
    expect(functionRaises(checked) && external != nullptr &&
               bodyCalls(external->body, checked->id) &&
               external->parameterTypes.size() == 1 &&
               external->parameterTypes.front().kind ==
                   lang::SemanticType::Reference,
           "the reverse near miss should be excluded only by its exact "
           "reference-parameter incoming edge");
  } else if (name.find("recursive_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *recursive =
        findHirFunction(frontend.hir, "recursive_checked");
    expect(functionRaises(recursive) &&
               bodyCalls(recursive->body, recursive->id),
           "the recursive near miss should retain an exact may-fail cycle");
  } else if (name.find("checked_ctor_near_miss") != std::string::npos ||
             name.find("checked_dtor_near_miss") != std::string::npos ||
             name.find("cleanup_near_miss") != std::string::npos) {
    const std::string_view ownerName =
        name.find("checked_ctor") != std::string::npos
            ? "CheckedConstructor"
            : (name.find("checked_dtor") != std::string::npos
                   ? "CheckedDestructor"
                   : "FailureCleanup");
    const lang::HirClassInstance *owner = findHirClass(frontend.hir, ownerName);
    const lang::MirConstructorInstance *constructor =
        findConstructor(frontend.mir, owner);
    const lang::MirDestructorInstance *destructor =
        findDestructor(frontend.mir, owner);
    const bool expectsConstructorFailure =
        name.find("checked_dtor") == std::string::npos;
    const bool expectsDestructorFailure =
        name.find("checked_ctor") == std::string::npos;
    expect(owner != nullptr &&
               constructorRaises(constructor) == expectsConstructorFailure &&
               destructorRaises(destructor) == expectsDestructorFailure,
           "the cleanup near miss should expose the exact failure-capable "
           "constructor/destructor exclusion");
  } else if (name.find("virtual_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    const lang::HirFunctionInstance *method =
        findHirFunction(frontend.hir, "Call");
    expect(checked != nullptr && method != nullptr && method->virtualMethod &&
               bodyCalls(method->body, checked->id),
           "the polymorphic near miss should retain its virtual incoming "
           "edge into the component");
  } else if (name.find("lambda_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    expect(checked != nullptr &&
               std::any_of(frontend.hir.lambdaInstances().begin(),
                           frontend.hir.lambdaInstances().end(),
                           [&](const lang::HirLambda &lambda) {
                             return bodyCalls(lambda.body, checked->id);
                           }),
           "the lambda near miss should retain its exact incoming lambda-body "
           "edge");
  } else if (name.find("dynamic_global_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    expect(checked != nullptr && bodyCalls(frontend.hir.module(), checked->id),
           "the initializer near miss should execute a checked call before "
           "the hosted boundary");
  } else if (name.find("failure_free_helper_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    const lang::HirFunctionInstance *helper =
        findHirFunction(frontend.hir, "failure_free_identity");
    expect(functionRaises(checked) && helper != nullptr &&
               !functionRaises(helper) && bodyCalls(checked->body, helper->id),
           "the cross-family near miss should call an exact failure-free "
           "normal-ABI helper");
  } else if (name.find("no_detector_entry_near_miss") != std::string::npos) {
    const bool entryHasFailureSite =
        entry != nullptr &&
        std::any_of(entry->body.values.begin(), entry->body.values.end(),
                    [](const lang::HirValue &value) {
                      return !value.definedFailure.localOrigins.empty() ||
                             (value.functionTarget &&
                              !value.constructorTarget &&
                              value.intrinsic == lang::IntrinsicKind::None);
                    });
    expect(functionRaises(entry) && !entryHasFailureSite,
           "the no-detector entry near miss should retain a conservative "
           "effect without a local detector or propagation edge");
  } else if (name.find("c_linkage_near_miss") != std::string::npos) {
    const lang::HirFunctionInstance *checked =
        findHirFunction(frontend.hir, "checked_increment");
    const lang::HirFunctionInstance *native =
        findHirFunction(frontend.hir, "native_identity");
    expect(functionRaises(checked) && native != nullptr &&
               native->linkage == lang::LanguageLinkage::C &&
               !native->externalSymbol.empty() &&
               bodyCalls(checked->body, native->id),
           "the native near miss should retain an exact outgoing C-linkage "
           "declaration edge from the checked graph");
  } else if (name.find("array_user_near_miss") != std::string::npos) {
    const lang::HirClassInstance *selected =
        findHirClass(frontend.hir, "FailureScope");
    const lang::HirFunctionInstance *external =
        findHirFunction(frontend.hir, "compatibility_array_user");
    const lang::SemanticType *parameter =
        external == nullptr || external->parameterTypes.size() != 1
            ? nullptr
            : &external->parameterTypes.front();
    const bool directBodyUse =
        selected != nullptr && external != nullptr &&
        (std::any_of(external->body.values.begin(), external->body.values.end(),
                     [&](const lang::HirValue &value) {
                       return value.info.type == selected->type;
                     }) ||
         std::any_of(external->body.dropObligations.begin(),
                     external->body.dropObligations.end(),
                     [&](const lang::HirDropObligation &drop) {
                       return drop.dropType.type == selected->type;
                     }));
    expect(selected != nullptr && parameter != nullptr &&
               parameter->kind == lang::SemanticType::Array &&
               parameter->arguments.size() == 1 &&
               parameter->arguments.front() == selected->type && !directBodyUse,
           "the array-user near miss should expose selected-class ownership "
           "only through its nested array element type");
  } else if (name.find("generic_owner_near_miss") != std::string::npos) {
    const lang::HirClassInstance *selected =
        findHirClass(frontend.hir, "FailureScope");
    const lang::HirClassInstance *holder =
        findHirClass(frontend.hir, "CompatibilityOwner");
    const bool nestedField =
        selected != nullptr && holder != nullptr &&
        std::any_of(
            holder->fields.begin(), holder->fields.end(),
            [&](const lang::HirClassField &field) {
              return field.info.type.kind == lang::SemanticType::Array &&
                     field.info.type.arguments.size() == 1 &&
                     field.info.type.arguments.front() == selected->type;
            });
    const bool directField =
        selected != nullptr && holder != nullptr &&
        std::any_of(holder->fields.begin(), holder->fields.end(),
                    [&](const lang::HirClassField &field) {
                      return field.info.type == selected->type;
                    });
    expect(selected != nullptr && holder != nullptr &&
               holder->typeArguments.size() == 1 &&
               holder->typeArguments.front() == selected->type && nestedField &&
               !directField,
           "the generic-owner near miss should expose selected-class storage "
           "only through its instantiated nested field type");
  } else if (name.find("class_field_near_miss") != std::string::npos) {
    const lang::HirClassInstance *selected =
        findHirClass(frontend.hir, "FailureScope");
    const lang::HirClassInstance *holder =
        findHirClass(frontend.hir, "CompatibilityHolder");
    expect(selected != nullptr && holder != nullptr &&
               std::any_of(holder->fields.begin(), holder->fields.end(),
                           [&](const lang::HirClassField &field) {
                             return field.info.type == selected->type;
                           }) &&
               !constructorRaises(findConstructor(frontend.mir, selected)) &&
               !destructorRaises(findDestructor(frontend.mir, selected)),
           "the class-field near miss should cross an otherwise proved "
           "selected lifecycle representation");
  } else if (name.find("lifecycle_user_near_miss") != std::string::npos) {
    const lang::HirClassInstance *selected =
        findHirClass(frontend.hir, "FailureScope");
    const lang::HirClassInstance *user =
        findHirClass(frontend.hir, "CompatibilityLifecycleUser");
    const auto mentions = [&](const lang::HirBody &body) {
      if (selected == nullptr) {
        return false;
      }
      const bool binding =
          std::any_of(body.bindings.begin(), body.bindings.end(),
                      [&](const lang::HirBinding &candidate) {
                        return candidate.info.type == selected->type;
                      });
      const bool value =
          std::any_of(body.values.begin(), body.values.end(),
                      [&](const lang::HirValue &candidate) {
                        return candidate.info.type == selected->type;
                      });
      const bool drop =
          std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                      [&](const lang::HirDropObligation &candidate) {
                        return candidate.dropType.type == selected->type;
                      });
      return binding || value || drop;
    };
    const auto constructor =
        user == nullptr
            ? frontend.hir.constructorInstances().end()
            : std::find_if(frontend.hir.constructorInstances().begin(),
                           frontend.hir.constructorInstances().end(),
                           [&](const lang::HirConstructorInstance &candidate) {
                             return candidate.owner == user->id &&
                                    candidate.source != nullptr &&
                                    mentions(candidate.body);
                           });
    const lang::HirDestructorInstance *destructor =
        user == nullptr || !user->destructor
            ? nullptr
            : frontend.hir.findDestructorInstance(*user->destructor);
    expect(selected != nullptr && user != nullptr &&
               constructor != frontend.hir.constructorInstances().end() &&
               destructor != nullptr && mentions(constructor->body) &&
               mentions(destructor->body),
           "the lifecycle-user near miss should mention the selected class "
           "from both compatibility constructor and destructor bodies");
  }
  const lang::OptimizationPipeline pipeline;
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const lang::OptimizationResult compatibility =
        pipeline.run(frontend.hir, level);
    const lang::OptimizedProgram optimized =
        pipeline.run({.hir = frontend.hir,
                      .mir = frontend.mir,
                      .level = level,
                      .compatibility = &compatibility});
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "O0/O1/O3 near-miss MIR should remain verified");
    if (optimized.valid()) {
      const std::string generated =
          emit(frontend, optimized.mir, compatibility).contents;
      expect(generated.find(marker) == std::string::npos,
             "an open or cyclic may-fail component must remain wholly on the "
             "compatibility ABI");
    }
  }
}

void testNonOwningClassMention(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the nonowning selected-class fixture should remain valid");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::HirClassInstance *selected =
      findHirClass(frontend.hir, "FailureScope");
  const lang::HirFunctionInstance *external =
      findHirFunction(frontend.hir, "compatibility_nonowning");
  const lang::HirFunctionInstance *checked =
      findHirFunction(frontend.hir, "checked_increment");
  const lang::HirFunctionInstance *entry =
      findHirFunction(frontend.hir, "main");
  const auto refersToSelected = [&](const lang::SemanticType &type) {
    return selected != nullptr && type.arguments.size() == 1 &&
           type.arguments.front() == selected->type;
  };
  expect(
      selected != nullptr && external != nullptr && checked != nullptr &&
          entry != nullptr && external->parameterTypes.size() == 2 &&
          external->parameterTypes[0].kind == lang::SemanticType::Reference &&
          external->parameterTypes[1].kind == lang::SemanticType::RawPointer &&
          refersToSelected(external->parameterTypes[0]) &&
          refersToSelected(external->parameterTypes[1]),
      "reference/raw-pointer-only mentions should retain exact nonowning "
      "selected-class types");

  const lang::OptimizationPipeline pipeline;
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const lang::OptimizationResult compatibility =
        pipeline.run(frontend.hir, level);
    const lang::OptimizedProgram optimized =
        pipeline.run({.hir = frontend.hir,
                      .mir = frontend.mir,
                      .level = level,
                      .compatibility = &compatibility});
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "O0/O1/O3 nonowning fixture MIR should remain verified");
    if (optimized.valid()) {
      const std::string generated =
          emit(frontend, optimized.mir, compatibility).contents;
      expect(countSubstring(generated, marker) == 2,
             "nonowning reference/raw-pointer mentions should not demote the "
             "otherwise closed checked graph");
    }
  }
}

void testMutations(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the failure-callgraph mutation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);

  lang::MirProgram missingSite = frontend.mir;
  if (lang::MirInstruction *detector = findInstruction(
          findFunction(frontend.hir, missingSite, "overflowing_leaf"),
          lang::MirInstructionKind::Compute)) {
    detector->localFailureSites.clear();
  }
  expect(!lang::verifyMirProgram(missingSite).valid(),
         "a detector cannot lose its canonical artifact site");

  lang::MirProgram staleRecord = frontend.mir;
  if (lang::MirFunctionInstance *leaf =
          findFunction(frontend.hir, staleRecord, "overflowing_leaf");
      leaf != nullptr && !leaf->body.failureRecords.empty()) {
    ++leaf->body.failureRecords.front().producerInstruction;
  }
  expect(!lang::verifyMirProgram(staleRecord).valid(),
         "a failure record cannot drift from its producer instruction");

  lang::MirProgram missingDrop = frontend.mir;
  bool erasedDrop = false;
  if (lang::MirFunctionInstance *caller =
          findFunction(frontend.hir, missingDrop, "propagate_overflow")) {
    lang::MirInstruction *call =
        findInstruction(caller, lang::MirInstructionKind::Call,
                        lang::FailurePropagationKind::DirectCall);
    const lang::MirFailureRecord *record =
        call == nullptr ? nullptr : recordFor(*caller, *call);
    lang::MirBlock *cleanup =
        record == nullptr || record->parameterBlock == 0 ||
                record->parameterBlock > caller->body.blocks.size()
            ? nullptr
            : &caller->body.blocks[record->parameterBlock - 1];
    if (cleanup != nullptr) {
      const auto before = cleanup->instructions.size();
      std::erase_if(
          cleanup->instructions, [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Drop &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().failureCleanup;
          });
      erasedDrop = cleanup->instructions.size() != before;
      (void)lang::rebuildMirValueUses(caller->body);
    }
  }
  expect(erasedDrop && (!lang::verifyMirProgram(missingDrop).valid() ||
                        emissionRejected(frontend, missingDrop, compatibility)),
         "missing failure cleanup must fail closed before production");

  lang::MirProgram retargeted = frontend.mir;
  lang::MirFunctionInstance *caller =
      findFunction(frontend.hir, retargeted, "propagate_overflow");
  const lang::HirFunctionInstance *callerSource =
      findHirFunction(frontend.hir, "propagate_overflow");
  if (lang::MirInstruction *call =
          findInstruction(caller, lang::MirInstructionKind::Call,
                          lang::FailurePropagationKind::DirectCall);
      call != nullptr && callerSource != nullptr) {
    call->functionTarget = callerSource->id;
  }
  expect(lang::verifyMirProgram(retargeted).valid(),
         "a same-signature conservative recursive retarget should remain "
         "generic valid MIR");
  if (lang::verifyMirProgram(retargeted).valid()) {
    expect(emissionRejected(frontend, retargeted, compatibility),
           "the backend must bind the selected call graph to exact HIR call "
           "identities");
  }

  lang::MirProgram headerDrift = frontend.mir;
  if (lang::MirFunctionInstance *leaf =
          findFunction(frontend.hir, headerDrift, "overflowing_leaf")) {
    leaf->staticMember = true;
  }
  expect(lang::verifyMirProgram(headerDrift).valid(),
         "a conservative function-header mutation should remain generic MIR");
  if (lang::verifyMirProgram(headerDrift).valid()) {
    expect(emissionRejected(frontend, headerDrift, compatibility),
           "the hosted selector must reject exact source/MIR header drift");
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 18) {
    std::cerr << "usage: gti_mir_backend_scalar_failure_callgraph_tests "
                 "<failure-fixture> <reverse-near-miss> "
                 "<recursive-near-miss> <cleanup-near-miss> "
                 "<checked-ctor-near-miss> <checked-dtor-near-miss> "
                 "<virtual-near-miss> <lambda-near-miss> "
                 "<dynamic-global-near-miss> "
                 "<failure-free-helper-near-miss> "
                 "<c-linkage-near-miss> "
                 "<class-field-near-miss> <lifecycle-user-near-miss> "
                 "<array-user-near-miss> <generic-owner-near-miss> "
                 "<no-detector-entry-near-miss> "
                 "<nonowning-user>\n";
    return 2;
  }
  testFailureGraph(argv[1]);
  testAtomicNearMiss(argv[2]);
  testAtomicNearMiss(argv[3]);
  testAtomicNearMiss(argv[4]);
  testAtomicNearMiss(argv[5]);
  testAtomicNearMiss(argv[6]);
  testAtomicNearMiss(argv[7]);
  testAtomicNearMiss(argv[8]);
  testAtomicNearMiss(argv[9]);
  testAtomicNearMiss(argv[10]);
  testAtomicNearMiss(argv[11]);
  testAtomicNearMiss(argv[12]);
  testAtomicNearMiss(argv[13]);
  testAtomicNearMiss(argv[14]);
  testAtomicNearMiss(argv[15]);
  testAtomicNearMiss(argv[16]);
  testNonOwningClassMention(argv[17]);
  testMutations(argv[1]);
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  return 0;
}
