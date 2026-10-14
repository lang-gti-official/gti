#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>

int main() {
  const auto exclusive = lang::Frontend().analyze("exclusive-borrowed-callback-mir.gti", R"gti(
int main() {
  mut int value = 4;
  mut auto change = [mut &value]() -> void { value += 1; };
  change();
  return value - 5;
}
)gti");
  if (!exclusive.canGenerateCode()) {
    for (const auto &diagnostic : exclusive.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &lambda : exclusive.mir.lambdaInstances()) {
      for (const auto &place : lambda.body.places) {
        std::cerr << "p" << place.id << " root=" << int(place.root)
                  << " capture=" << place.capture << " symbol=" << place.symbol
                  << " access=" << int(place.access) << " type=" << int(place.type.kind)
                  << " projections=";
        for (const auto &projection : place.projections)
          std::cerr << int(projection.kind) << ',';
        std::cerr << '\n';
      }
      for (const auto &loan : lambda.body.loans)
        std::cerr << "loan " << loan.id << " source=" << loan.source
                  << " kind=" << int(loan.kind) << " access=" << int(loan.access)
                  << " entry=" << loan.entry << '\n';
    }
    return 6;
  }
  const auto analyzed = lang::Frontend().analyze("borrowed-callback-mir.gti", R"gti(
int main() {
  int first = 4;
  int second = 9;
  auto difference = [&first, &second]() -> int { return second - first; };
  return difference() - 5;
}
)gti");
  if (!analyzed.canGenerateCode()) {
    for (const auto &diagnostic : analyzed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  if (!lang::verifyMirProgram(analyzed.mir).valid())
    return 2;
  const auto &lambda = analyzed.mir.lambdaInstances().front();
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = analyzed.mir;
    auto *target = const_cast<lang::MirLambdaInstance *>(changed.findLambda(lambda.id));
    if (mutation == 0)
      target->captureModes.front() = lang::LambdaCaptureMode::Copy;
    else if (mutation == 1)
      target->capability = lang::CallableInvocationCapability::Mutable;
    else
      target->captureTypes.front().referenceAccess = lang::AccessMode::Mutable;
    if (lang::verifyMirProgram(changed).valid()) {
      std::cerr << "accepted forged capture representation " << mutation << '\n';
      return 3;
    }
  }
  bool checked = false;
  for (const auto &function : analyzed.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Closure || !instruction.loan)
          continue;
        checked = true;
        for (int mutation = 0; mutation < 4; ++mutation) {
          auto changed = analyzed.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
              [&](const auto &candidate) { return candidate.id == instruction.id; });
          auto &loan = target->body.loans[*instruction.loan - 1];
          if (mutation == 0)
            selected->loan.reset();
          else if (mutation == 1)
            loan.additionalDependencies.clear();
          else if (mutation == 2)
            loan.additionalDependencies.front().designatedFields = loan.designatedFields;
          else
            selected->operands.front().kind = lang::MirOperandKind::Copy;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged closure dependency " << mutation << '\n';
            return 4;
          }
        }
      }
  return checked ? 0 : 5;
}
