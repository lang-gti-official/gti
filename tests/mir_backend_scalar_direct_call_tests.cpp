#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

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

constexpr std::string_view marker = "// GTI verified-MIR body: scalar-cfg-v1";
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

std::string_view functionDefinition(std::string_view generated,
                                    std::string_view sourceName) {
  const std::string needle = std::string{"_"} + std::string{sourceName} + "(";
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    if (brace == std::string_view::npos ||
        (lineEnd != std::string_view::npos && brace > lineEnd)) {
      continue;
    }
    const std::size_t end = generated.find("\n  }", brace);
    return end == std::string_view::npos
               ? std::string_view{}
               : generated.substr(brace, end + 4 - brace);
  }
  return {};
}

std::string_view failureFunctionDefinition(std::string_view generated,
                                           std::string_view sourceName) {
  const std::string needle =
      std::string{"_"} + std::string{sourceName} + "__gti_mir_failure(";
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    if (brace == std::string_view::npos ||
        (lineEnd != std::string_view::npos && brace > lineEnd)) {
      continue;
    }
    const std::size_t end = generated.find("\n  }", brace);
    return end == std::string_view::npos
               ? std::string_view{}
               : generated.substr(brace, end + 4 - brace);
  }
  return {};
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

lang::MirFunctionInstance *findMirFunction(const lang::HirProgram &hir,
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

const lang::MirFunctionInstance *findMirFunction(const lang::HirProgram &hir,
                                                 const lang::MirProgram &mir,
                                                 std::string_view name) {
  const lang::HirFunctionInstance *source = findHirFunction(hir, name);
  return source == nullptr ? nullptr : mir.findFunctionInstance(source->id);
}

lang::MirInstruction *findCall(lang::MirFunctionInstance *function) {
  if (function == nullptr) {
    return nullptr;
  }
  for (lang::MirBlock &block : function->body.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Call;
        });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

std::vector<lang::MirInstruction *>
findCallInputs(lang::MirFunctionInstance *function) {
  std::vector<lang::MirInstruction *> inputs;
  if (function == nullptr) {
    return inputs;
  }
  for (lang::MirBlock &block : function->body.blocks) {
    for (lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::CallInput) {
        inputs.push_back(&instruction);
      }
    }
  }
  return inputs;
}

lang::MirInstruction *definitionFor(lang::MirFunctionInstance *function,
                                    lang::MirValueId valueId) {
  if (function == nullptr) {
    return nullptr;
  }
  const lang::MirValue *value = function->body.findValue(valueId);
  lang::MirBlock *block =
      value == nullptr || value->definitionBlock == 0 ||
              value->definitionBlock > function->body.blocks.size()
          ? nullptr
          : &function->body.blocks[value->definitionBlock - 1];
  if (value == nullptr || block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const lang::MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility) {
  static_cast<void>(compatibility);
  return gti_test::emitCpp(frontend, frontend.mir, mir);
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

void expectSelectedDefinitions(std::string_view generated) {
  constexpr std::string_view selected[] = {
      "direct_identity",
      "selected_forward",
      "selected_order",
      "selected_void",
      "direct_chain_middle",
      "selected_chain",
      "direct_chain",
      "direct_unused",
      "direct_choose",
      "direct_nested",
      "direct_call_zero",
      "direct_literal",
      "direct_call_void",
      "direct_heterogeneous",
      "direct_loop",
      "direct_cross_namespace",
      // Callers the old whole-graph contract had to reject even though
      // their own bodies are ordinary: their formerly ineligible callees
      // either emit per body themselves (a constexpr declaration is now
      // admissible; GTI constant contexts are frontend-evaluated) or stay
      // on the compatibility path while the calls emit from verified MIR.
      "constexpr_target",
      "compatibility_constexpr_target",
      "compatibility_constexpr_call",
      "pass",
      "compatibility_static_member",
      "compatibility_internal_target",
      "compatibility_internal_call",
      "compatibility_for_target",
      "compatibility_for_call",
  };
  expect(std::count(generated.begin(), generated.end(), '\n') != 0,
         "generated C++ should be non-empty");
  for (const std::string_view name : selected) {
    expect(functionDefinition(generated, name).find(marker) !=
               std::string_view::npos,
           std::string{"the exact static call body should be MIR-emitted: "} +
               std::string{name});
  }
  constexpr std::string_view failureSelected[] = {
      "checked_target", "compatibility_checked_target",
      "compatibility_checked_call", "compatibility_recursive",
      "checked_increment"};
  constexpr std::string_view failureMarker =
      "// GTI verified-MIR body: scalar-cfg-failure-v1";
  for (const std::string_view name : failureSelected) {
    expect(failureFunctionDefinition(generated, name).find(failureMarker) !=
                   std::string_view::npos &&
               functionDefinition(generated, name).empty(),
           std::string{"the failure component should emit only its explicit "
                       "MIR sibling: "} +
               std::string{name});
  }
}

void testFamilyAndSummary(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the scalar direct-call fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const std::vector<bool> derived =
      lang::deriveMirScalarDefinedFailureEffects(frontend.mir);
  expect(derived.size() == frontend.mir.functionInstances().size() &&
             std::equal(derived.begin(), derived.end(),
                        frontend.mir.functionInstances().begin(),
                        [](bool mayRaise, const auto &function) {
                          return mayRaise == function.mayRaiseDefinedFailure;
                        }),
         "final lowering should publish the exact MIR-derived failure vector");

  constexpr std::string_view noFailure[] = {
      "direct_identity_leaf",
      "direct_identity",
      "direct_chain",
      "direct_unused",
      "direct_choose",
      "direct_pair",
      "direct_nested",
      "direct_call_zero",
      "direct_call_void",
      "direct_literal",
      "direct_heterogeneous",
      "direct_loop",
      "direct_cross_namespace",
      "selected_forward",
      "selected_order",
      "selected_void",
      "direct_chain_middle",
      "selected_chain",
      "constexpr_target",
      "compatibility_constexpr_target",
      "compatibility_constexpr_call",
      "compatibility_internal_target",
      "compatibility_internal_call",
      "compatibility_for_target",
      "compatibility_for_call",
      "pass",
      "compatibility_static_member",
  };
  for (const std::string_view name : noFailure) {
    const lang::MirFunctionInstance *function =
        findMirFunction(frontend.hir, frontend.mir, name);
    expect(function != nullptr && !function->mayRaiseDefinedFailure,
           std::string{"the closed scalar graph should be no-failure: "} +
               std::string{name});
    if (function == nullptr) {
      continue;
    }
    expect(function->body.failureRecords.empty() &&
               std::all_of(
                   function->body.blocks.begin(), function->body.blocks.end(),
                   [](const auto &block) {
                     return block.failureParameter == 0 &&
                            block.terminator.kind !=
                                lang::MirTerminatorKind::Invoke &&
                            block.terminator.kind !=
                                lang::MirTerminatorKind::PropagateFailure;
                   }),
           "normalized no-failure bodies must retain no stale failure CFG");
  }
  constexpr std::string_view mayRaise[] = {
      "checked_target",
      "compatibility_checked_target",
      "compatibility_checked_call",
      "compatibility_recursive",
      "checked_increment",
  };
  for (const std::string_view name : mayRaise) {
    const lang::MirFunctionInstance *function =
        findMirFunction(frontend.hir, frontend.mir, name);
    expect(function != nullptr && function->mayRaiseDefinedFailure,
           std::string{"unsupported or cyclic graph must stay conservative: "} +
               std::string{name});
  }
  const lang::MirFunctionInstance *ordered =
      findMirFunction(frontend.hir, frontend.mir, "selected_order");
  std::vector<std::size_t> stagedInputIndices;
  bool orderedCallAfterInputs = false;
  if (ordered != nullptr) {
    for (const lang::MirBlock &block : ordered->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput) {
          stagedInputIndices.push_back(instruction.callInputIndex);
        } else if (instruction.kind == lang::MirInstructionKind::Call) {
          orderedCallAfterInputs = stagedInputIndices.size() == 2;
        }
      }
    }
  }
  expect(stagedInputIndices == std::vector<std::size_t>{0, 1} &&
             orderedCallAfterInputs,
         "assignment arguments should become ordered CallInputs before the "
         "direct Call");
  const std::string dump = lang::MirPrinter().print(frontend.mir);
  expect(dump.starts_with("mir-v38 ") &&
             dump.find("definition=source may-raise-defined-failure=0") !=
                 std::string::npos,
         "mir-v38 should serialize declaration kind and failure effects");

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
           "O0/O1/O3 direct-call MIR should remain verified");
    if (optimized.valid()) {
      expectSelectedDefinitions(
          emit(frontend, optimized.mir, compatibility).contents);
    }
  }
}

void testRuntimeDeclarationSummary() {
  const std::string source = R"(
namespace gti_internal {
namespace runtime {
@runtime("stdin.read_byte")
int32_t read_stdin_byte();
}
}
int main() { return 0; }
)";
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() /
      "gti-mir-scalar-direct-runtime-prelude.gti";
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(entry, source, {entry}, {{entryKey, source}});
  expect(frontend.canGenerateCode(),
         "the trusted scalar runtime-binding fixture should pass");
  const lang::MirFunctionInstance *runtime =
      findMirFunction(frontend.hir, frontend.mir, "read_stdin_byte");
  expect(runtime != nullptr &&
             runtime->definitionKind ==
                 lang::MirFunctionInstance::DefinitionKind::RuntimeBinding &&
             runtime->mayRaiseDefinedFailure,
         "bodyless scalar runtime bindings must remain explicitly "
         "conservative");
  expect(lang::MirPrinter()
                 .print(frontend.mir)
                 .find("definition=runtime may-raise-defined-failure=1") !=
             std::string::npos,
         "mir-v38 should serialize the runtime declaration category");
}

void testMutations(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the mutation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);

  lang::MirProgram falseChecked = frontend.mir;
  if (auto *checked = findMirFunction(frontend.hir, falseChecked,
                                      "compatibility_checked_target")) {
    checked->mayRaiseDefinedFailure = false;
  }
  expect(!lang::verifyMirProgram(falseChecked).valid(),
         "forged no-failure metadata on checked arithmetic must be rejected");

  lang::MirProgram unsupported = frontend.mir;
  lang::MirFunctionInstance *leaf =
      findMirFunction(frontend.hir, unsupported, "direct_pair");
  if (leaf != nullptr) {
    for (lang::MirBlock &block : leaf->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::BitwiseAnd) {
          instruction.operation = lang::MirOperation::Add;
        }
      }
    }
  }
  expect(!lang::verifyMirProgram(unsupported).valid(),
         "unsupported checked operations cannot be hidden by absent failure "
         "metadata");

  lang::MirProgram propagation = frontend.mir;
  if (lang::MirInstruction *call = findCall(
          findMirFunction(frontend.hir, propagation, "direct_identity"))) {
    call->definedFailure.propagation = lang::FailurePropagationKind::DirectCall;
  }
  expect(!lang::verifyMirProgram(propagation).valid(),
         "a normalized call cannot regain DirectCall propagation without its "
         "Invoke record");

  lang::MirProgram staleFailure = frontend.mir;
  if (auto *identity =
          findMirFunction(frontend.hir, staleFailure, "direct_identity")) {
    identity->body.blocks.front().failureParameter = 1;
  }
  expect(!lang::verifyMirProgram(staleFailure).valid(),
         "a stale failure parameter must invalidate a no-failure body");

  lang::MirProgram retargeted = frontend.mir;
  lang::MirInstruction *retargetedCall =
      findCall(findMirFunction(frontend.hir, retargeted, "direct_identity"));
  const lang::HirFunctionInstance *alternate =
      findHirFunction(frontend.hir, "direct_alternate_leaf");
  if (retargetedCall != nullptr && alternate != nullptr) {
    retargetedCall->functionTarget = alternate->id;
  }
  expect(lang::verifyMirProgram(retargeted).valid(),
         "same-signature no-failure retargeting should remain generic MIR");
  expect(emissionRejected(frontend, retargeted, compatibility),
         "backend selection must reject MIR/HIR target identity drift");

  lang::MirProgram substitutedOperation = frontend.mir;
  lang::MirFunctionInstance *pair =
      findMirFunction(frontend.hir, substitutedOperation, "direct_pair");
  bool changedOperation = false;
  if (pair != nullptr) {
    for (lang::MirBlock &block : pair->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::BitwiseAnd) {
          instruction.operation = lang::MirOperation::BitwiseOr;
          changedOperation = true;
          break;
        }
      }
      if (changedOperation) {
        break;
      }
    }
  }
  expect(changedOperation &&
             lang::verifyMirProgram(substitutedOperation).valid(),
         "a same-domain direct-call graph operation substitution should remain "
         "valid generic MIR");
  if (changedOperation &&
      lang::verifyMirProgram(substitutedOperation).valid()) {
    expect(emissionRejected(frontend, substitutedOperation, compatibility),
           "scalar-cfg-v1 must reject an operation substitution that "
           "has no optimizer rewrite provenance");
  }

  lang::MirProgram swappedBranch = frontend.mir;
  lang::MirFunctionInstance *choose =
      findMirFunction(frontend.hir, swappedBranch, "direct_choose");
  bool changedBranch = false;
  if (choose != nullptr) {
    for (lang::MirBlock &block : choose->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Branch) {
        std::swap(block.terminator.target, block.terminator.elseTarget);
        changedBranch = true;
        break;
      }
    }
  }
  expect(changedBranch && lang::verifyMirProgram(swappedBranch).valid(),
         "a type-compatible direct-call graph branch swap should remain valid "
         "generic MIR");
  if (changedBranch && lang::verifyMirProgram(swappedBranch).valid()) {
    expect(emissionRejected(frontend, swappedBranch, compatibility),
           "scalar-cfg-v1 must reject a branch-successor swap that has "
           "no optimizer rewrite provenance");
  }

  lang::MirProgram retargetedInput = frontend.mir;
  lang::MirFunctionInstance *ordered =
      findMirFunction(frontend.hir, retargetedInput, "selected_order");
  std::vector<lang::MirInstruction *> inputs = findCallInputs(ordered);
  bool changedInput = false;
  if (inputs.size() == 2 && inputs[0]->operands.size() == 1 &&
      inputs[1]->operands.size() == 1 &&
      inputs[0]->operands.front().type == inputs[1]->operands.front().type) {
    inputs[1]->operands.front() = inputs[0]->operands.front();
    changedInput =
        ordered != nullptr && lang::rebuildMirValueUses(ordered->body);
  }
  const bool retargetedInputValid =
      changedInput && lang::verifyMirProgram(retargetedInput).valid();
  expect(retargetedInputValid,
         "a same-typed dominating CallInput source retarget should remain "
         "generic MIR");
  if (retargetedInputValid) {
    expect(emissionRejected(frontend, retargetedInput, compatibility),
           "backend selection must bind each CallInput to its exact HIR "
           "argument source");
  }

  lang::MirProgram safeReadInput = frontend.mir;
  lang::MirFunctionInstance *safeOrdered =
      findMirFunction(frontend.hir, safeReadInput, "selected_forward");
  std::vector<lang::MirInstruction *> safeInputs = findCallInputs(safeOrdered);
  bool attachedSafeRead = false;
  if (!safeInputs.empty() && safeInputs.front()->operands.size() == 1 &&
      safeInputs.front()->operands.front().kind ==
          lang::MirOperandKind::Value) {
    lang::MirInstruction *source =
        definitionFor(safeOrdered, safeInputs.front()->operands.front().value);
    if (source != nullptr && source->ownership &&
        source->ownership->kind == lang::OwnershipEventKind::Read) {
      safeInputs.front()->ownership = source->ownership;
      attachedSafeRead = true;
    }
  }
  expect(attachedSafeRead && lang::verifyMirProgram(safeReadInput).valid(),
         "CallInput should accept an exact safe scalar Read ownership event");
  if (attachedSafeRead && lang::verifyMirProgram(safeReadInput).valid()) {
    const std::string generated =
        emit(frontend, safeReadInput, compatibility).contents;
    expect(functionDefinition(generated, "selected_forward").find(marker) !=
               std::string_view::npos,
           "an exact safe CallInput Read should remain in the MIR-emitted "
           "family");
  }

  const lang::OptimizationResult o1Compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o1 =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .compatibility = &o1Compatibility});
  expect(o1.valid() && lang::verifyMirProgram(o1.mir).valid(),
         "the direct-call provenance mutation should begin with verified O1 "
         "MIR");
  if (o1.valid() && lang::verifyMirProgram(o1.mir).valid()) {
    lang::MirProgram launderedFold = o1.mir;
    lang::MirFunctionInstance *literal =
        findMirFunction(frontend.hir, launderedFold, "direct_literal");
    lang::MirInstruction *rewritten = nullptr;
    if (literal != nullptr) {
      for (lang::MirBlock &block : literal->body.blocks) {
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [](const lang::MirInstruction &instruction) {
                           return instruction.literalProvenance.kind ==
                                  lang::MirLiteralProvenanceKind::IdentityFold;
                         });
        if (found != block.instructions.end()) {
          rewritten = &*found;
          break;
        }
      }
    }
    expect(rewritten != nullptr,
           "O1 direct_literal should retain an identity-fold provenance "
           "proof");
    if (rewritten != nullptr) {
      rewritten->literalProvenance = {
          .kind = lang::MirLiteralProvenanceKind::Source};
      expect(lang::verifyMirProgram(launderedFold).valid(),
             "a folded direct-call input relabeled as a source literal "
             "should remain generic valid MIR");
      if (lang::verifyMirProgram(launderedFold).valid()) {
        expect(emissionRejected(frontend, launderedFold, o1Compatibility),
               "scalar-cfg-v1 must reject a grouped argument whose "
               "identity-fold provenance was laundered");
      }
    }
  }

  lang::MirProgram deletedVoidCall = frontend.mir;
  lang::MirFunctionInstance *voidCaller =
      findMirFunction(frontend.hir, deletedVoidCall, "selected_void");
  bool deletedCall = false;
  if (voidCaller != nullptr) {
    for (lang::MirBlock &block : voidCaller->body.blocks) {
      const auto before = block.instructions.size();
      std::erase_if(block.instructions,
                    [](const lang::MirInstruction &instruction) {
                      return instruction.kind == lang::MirInstructionKind::Call;
                    });
      deletedCall = deletedCall || block.instructions.size() != before;
    }
    (void)lang::rebuildMirValueUses(voidCaller->body);
  }
  expect(deletedCall && lang::verifyMirProgram(deletedVoidCall).valid(),
         "deleting a no-result call should remain generic valid MIR");
  if (deletedCall && lang::verifyMirProgram(deletedVoidCall).valid()) {
    expect(emissionRejected(frontend, deletedVoidCall, compatibility),
           "bidirectional HIR/MIR call coverage must reject a deleted void "
           "call");
  }

  lang::MirProgram reordered = frontend.mir;
  if (auto *nested =
          findMirFunction(frontend.hir, reordered, "direct_nested")) {
    for (lang::MirBlock &block : nested->body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.operands.size() == 2;
          });
      if (call != block.instructions.end()) {
        std::swap(call->operands[0], call->operands[1]);
        break;
      }
    }
  }
  expect(!lang::verifyMirProgram(reordered).valid(),
         "ordered CallInput operands cannot be bypassed or reordered");

  lang::MirProgram conservativeRoot = frontend.mir;
  if (auto *unused =
          findMirFunction(frontend.hir, conservativeRoot, "direct_unused")) {
    unused->mayRaiseDefinedFailure = true;
  }
  expect(lang::verifyMirProgram(conservativeRoot).valid(),
         "public verification should permit conservative true summaries");
  expect(emissionRejected(frontend, conservativeRoot, compatibility),
         "an independently eligible direct graph must fail closed when its "
         "exact MIR failure summary drifts");

  lang::MirProgram definitionDrift = frontend.mir;
  if (auto *unused =
          findMirFunction(frontend.hir, definitionDrift, "direct_unused")) {
    unused->definitionKind =
        lang::MirFunctionInstance::DefinitionKind::RuntimeBinding;
    unused->mayRaiseDefinedFailure = true;
  }
  expect(!lang::verifyMirProgram(definitionDrift).valid(),
         "runtime-binding provenance must reject a retained source body");
  expect(emissionRejected(frontend, definitionDrift, compatibility),
         "backend entry must reject forged non-source definition provenance");

  lang::MirProgram invalidDefinition = frontend.mir;
  if (auto *unused =
          findMirFunction(frontend.hir, invalidDefinition, "direct_unused")) {
    unused->definitionKind =
        static_cast<lang::MirFunctionInstance::DefinitionKind>(99);
  }
  expect(!lang::verifyMirProgram(invalidDefinition).valid(),
         "unknown MIR definition-kind values must be rejected");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gti_mir_backend_scalar_direct_call_tests <fixture>\n";
    return 2;
  }
  testFamilyAndSummary(argv[1]);
  testRuntimeDeclarationSummary();
  testMutations(argv[1]);
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  return 0;
}
