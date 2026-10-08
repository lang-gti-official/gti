#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend = lang::Frontend().analyze(
      root / "source-prefix-contract.gti", R"gti(
#include <std/memory>
int main() {
  mut std::detail::prefix_owner<int> source{uint64_t(4)};
  source.append_value(3);
  source.append_value(7);
  mut std::detail::prefix_owner<int> destination{uint64_t(8)};
  source.relocate_to(destination);
  destination.insert(uint64_t(1), 5);
  destination.erase(uint64_t(0));
  return destination.read(uint64_t(0)) == 5 ? 0 : 1;
}
)gti",
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    std::cerr << lang::MirPrinter().print(frontend.mir);
    return 1;
  }
  for (const auto &owner : frontend.mir.classInstances()) {
    const auto source = std::find_if(
        frontend.hir.classInstances().begin(),
        frontend.hir.classInstances().end(),
        [&](const auto &candidate) { return candidate.id == owner.id; });
    if (source != frontend.hir.classInstances().end() && source->source &&
        (source->source->name().lexeme == "prefix_owner" ||
         source->source->name().lexeme == "prefix_slots") &&
        owner.compilerCapability != lang::CompilerCapabilityTypeKind::None) {
      std::cerr << "source prefix unexpectedly acquired compiler capability "
                   "authority\n";
      return 1;
    }
  }
  const auto cpp = gti_test::emitCppText(frontend);
  if (cpp.find("// GTI verified-MIR body:") == std::string::npos)
    return 1;
  auto forged = frontend.mir;
  bool changed = false;
  bool validEffects = true;
  const auto mutate = [&](lang::MirBody &body) {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::RequireAllocation)
          continue;
        const auto effects = lang::effects(instruction);
        if (!effects.mayTrap || effects.allocates ||
            effects.writesUnknownMemory || effects.movesValue)
          validEffects = false;
        instruction.definedFailure.localOrigins.front().outcomes.front() = {
            lang::DefinedFailureCode::KeyNotFound,
            lang::DefinedFailureDetail::KeyLookup};
        changed = true;
      }
  };
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forged.functionInstances()))
    mutate(function.body);
  for (auto &constructor :
       const_cast<std::vector<lang::MirConstructorInstance> &>(
           forged.constructorInstances()))
    mutate(constructor.body);
  if (!validEffects || !changed || lang::verifyMirProgram(forged).valid()) {
    std::cerr << "forged allocation-failure category was not rejected\n";
    return 1;
  }
  const auto hosted = lang::Frontend().analyze(
      root / "source-prefix-hosted-contract.gti", R"gti(
#include <std/string>
#include <std/vector>
int main(int argc, std::vector<std::string> argv) { return argc; }
)gti",
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!hosted.canGenerateCode()) {
    std::cerr << "source prefix hosted argument contract did not lower\n";
    return 1;
  }
  const auto hostedText = gti_test::emitCppText(hosted);
  if (hostedText.find("__gti_cleanup_context(&__gti_failure_record") ==
          std::string::npos ||
      hostedText.find(
          "__gti_arguments).template __gti_mir_unique_cleanup_failure<") ==
          std::string::npos) {
    std::cerr << "hosted argument cleanup did not consume its MIR recipe\n";
    return 1;
  }
  auto forgedHosted = hosted.mir;
  auto &operations = const_cast<std::optional<lang::MirHostedStartupPlan> &>(
                         forgedHosted.hostedStartupPlan())
                         ->operations;
  bool changedCleanup = false;
  for (auto &operation : operations) {
    if (operation.kind ==
            lang::MirHostedStartupOperationKind::DropFailureCleanup &&
        operation.failureBehavior ==
            lang::MirHostedStartupFailureBehavior::Propagate) {
      operation.failureBehavior = lang::MirHostedStartupFailureBehavior::None;
      changedCleanup = true;
      break;
    }
  }
  if (!changedCleanup || lang::verifyMirProgram(forgedHosted).valid()) {
    std::cerr << "forged failure-free hosted cleanup was not rejected\n";
    return 1;
  }
  const auto forwardedPath =
      root / "tests/fixtures/source_prefix_forwarded_result_cleanup.gti";
  std::ifstream forwardedInput(forwardedPath);
  const std::string forwardedSource{
      std::istreambuf_iterator<char>(forwardedInput), {}};
  const auto forwarded = lang::Frontend().analyze(
      forwardedPath, forwardedSource, {root / "stdlib/prelude.gti"}, {},
      {root / "stdlib"});
  if (!forwarded.canGenerateCode() ||
      gti_test::emitCppText(forwarded).find(
          "GTI MIR unpublished return cleanup") == std::string::npos) {
    std::cerr
        << "forwarded expected result did not retain unpublished cleanup\n";
    return 1;
  }
  auto forgedReturn = forwarded.mir;
  bool changedReturn = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forgedReturn.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1)
          continue;
        const auto *drop = function.body.findDropObligation(
            instruction.lifecycle.front().source);
        if (drop != nullptr &&
            drop->kind == lang::MirDropObligationKind::ReturnResult &&
            drop->dropType.type.kind == lang::SemanticType::Expected &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          instruction.definedFailure.propagation =
              lang::FailurePropagationKind::None;
          changedReturn = true;
          break;
        }
      }
      if (changedReturn)
        break;
    }
    if (changedReturn)
      break;
  }
  if (!changedReturn || lang::verifyMirProgram(forgedReturn).valid()) {
    std::cerr
        << "unpublished expected result cannot hide failure-capable cleanup\n";
    return 1;
  }
  const auto payloadPath =
      root / "tests/fixtures/source_prefix_payload_return_cleanup.gti";
  std::ifstream payloadInput(payloadPath);
  const std::string payloadSource{std::istreambuf_iterator<char>(payloadInput),
                                  {}};
  const auto payload = lang::Frontend().analyze(payloadPath, payloadSource,
                                                {root / "stdlib/prelude.gti"},
                                                {}, {root / "stdlib"});
  if (!payload.canGenerateCode() ||
      gti_test::emitCppText(payload).find(
          "GTI MIR unpublished return cleanup") == std::string::npos) {
    std::cerr << "owning enum return lost its unpublished cleanup recipe\n";
    return 1;
  }
  std::size_t returnCleanups = 0;
  for (const auto &function : payload.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1)
          continue;
        const auto *drop = function.body.findDropObligation(
            instruction.lifecycle.front().source);
        if (drop == nullptr ||
            drop->kind != lang::MirDropObligationKind::ReturnResult ||
            drop->dropType.type.kind != lang::SemanticType::Enum)
          continue;
        ++returnCleanups;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = payload.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &member : changed->body.findBlock(block.id)->instructions) {
            if (member.id != instruction.id)
              continue;
            if (mutation == 0)
              member.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            if (mutation == 1)
              member.lifecycle.front().conditional = true;
            if (mutation == 2)
              member.lifecycle.front().source = 0;
          }
          if (lang::verifyMirProgram(malformed).valid()) {
            std::cerr << "owning enum rollback accepted a forged cleanup\n";
            return 1;
          }
        }
      }
    }
  }
  if (returnCleanups == 0) {
    std::cerr << "enum return fixture did not exercise result rollback\n";
    return 1;
  }
  std::cout << "source prefix lifetime/condition authority: passed\n";
  return 0;
}
