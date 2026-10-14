#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "compiler_test_fixture.h"

#include <algorithm>
#include <iostream>

int main() {
  const auto erased = analyzeTrustedPreludeFixture("erased-user-wrapper-mir.gti", R"gti(
#include <std/memory>
#include <std/functional>
class Inspection {
  std::erased_object& object;
public:
  Inspection(int& object) : object(std::erase_ref(object)) {}
  int read() {
    unsafe { int& value = std::restore_ref<int>(this.object); return value; }
  }
};
int main() {
  int owner = 7; Inspection view{owner};
  auto reader = [&owner](int value) -> int { return owner + value; };
  std::function_ref<int(int)> callback{reader};
  mut int count = 0;
  mut auto writer = [mut &count](int value) -> int { count += value; return count; };
  mut std::function_ref<int(int) mut> update{writer};
  return view.read() + callback(1) + update(2) - 17;
}
)gti");
  if (!erased.canGenerateCode()) {
    for (const auto &diagnostic : erased.diagnostics)
      std::cerr << diagnostic.message << '\n';
    const auto verification = lang::verifyMirProgram(erased.mir);
    for (const auto &error : verification.errors)
      std::cerr << "verify " << int(error.bodyKind) << ':' << error.owner << " b" << error.block << " i" << error.instruction << ' ' << error.message << '\n';
    const auto dump = [&](const auto &instance) {
      const auto &body = instance.body;
      if (std::none_of(verification.errors.begin(), verification.errors.end(),
                      [&](const auto &error) { return error.bodyKind == body.kind && error.owner == instance.id; }))
        return;
      std::cerr << "body " << int(body.kind) << ':' << instance.id << '\n';
      for (const auto &loan : body.loans)
        std::cerr << "  loan " << loan.id << " kind=" << int(loan.kind) << " source=" << loan.source
                  << " parent=" << loan.parent << " access=" << int(loan.access)
                  << " entry=" << loan.entry << " escapes=" << loan.escapes << " producer=" << loan.producedBy << '\n';
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) {
          std::cerr << "  i" << instruction.id << " k=" << int(instruction.kind)
                    << " intrinsic=" << int(instruction.intrinsic) << " op=" << int(instruction.operation)
                    << " result=" << instruction.result.value_or(0) << " loan=" << instruction.loan.value_or(0)
                    << " type=" << int(instruction.info.type.kind) << " operands=";
          for (const auto &operand : instruction.operands)
            std::cerr << int(operand.kind) << ':' << int(operand.type.kind) << ":v" << operand.value << ":p" << operand.place << ":l" << operand.loan << ',';
          std::cerr << '\n';
        }
    };
    for (const auto &instance : erased.mir.functionInstances()) dump(instance);
    for (const auto &instance : erased.mir.constructorInstances()) dump(instance);
    for (const auto &instance : erased.mir.lambdaInstances()) dump(instance);
    return 7;
  }
  bool erasedChecked = false;
  for (const auto &function : erased.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::RestoreReference)
          continue;
        erasedChecked = true;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto changed = erased.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &instructions = target->body.blocks[block.id - 1].instructions;
          auto selected = std::find_if(instructions.begin(), instructions.end(),
              [&](const auto &candidate) { return candidate.id == instruction.id; });
          if (mutation == 0)
            selected->unsafeOperation = lang::UnsafeOperationKind::None;
          else if (mutation == 1)
            selected->parameterTypes.front().referenceAccess = lang::AccessMode::Mutable;
          else
            selected->borrowOrigin = lang::BorrowOriginKind::None;
          if (lang::verifyMirProgram(changed).valid()) {
            std::cerr << "accepted forged erased reference " << mutation << '\n';
            return 8;
          }
        }
      }
  if (!erasedChecked)
    return 9;
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
