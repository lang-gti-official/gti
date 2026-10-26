#include "../src/compiler/llvm/body.h"

#include <algorithm>
#include <set>

#if !defined(GTI_LLVM_C04_MIR_EVALUATION)
#error "C04 admission is for isolated evaluation targets only"
#endif

namespace lang::native_llvm {
namespace {
[[noreturn]] void reject(const char *reason) {
  throw BackendUnsupportedError(
      std::string("C04 evaluation supports only a closed no-cleanup scalar "
                  "call graph: ") +
      reason);
}
bool scalar(const SemanticType &type) {
  return type == SemanticType::Int32 || type == SemanticType::Bool;
}
} // namespace

void validateC04Evaluation(const Session &session,
                           const LlvmAdmissionPlan &admission,
                           std::span<const HirFunctionInstanceId> functions) {
  if (&session.program() != &admission.program() || !admission.admitted() ||
      session.options().optimization != admission.options().optimization ||
      session.options().debugInfo != admission.options().debugInfo)
    throw BackendUnsupportedError(
        "C04 evaluation requires the exact admitted snapshot and options");
  const std::set<HirFunctionInstanceId> selected(functions.begin(),
                                                 functions.end());
  if (selected.empty() || selected.size() != functions.size() ||
      !session.program().mir().programInitializationPlan().steps.empty())
    reject("empty/duplicate selection or program initialization");
  for (auto id : selected) {
    const auto *function = session.program().mir().findFunctionInstance(id);
    if (!function || function->definitionKind != MirDefinitionKind::Source ||
        function->linkage != LanguageLinkage::Gti || function->owner ||
        function->entryKind != ProgramEntryKind::None ||
        function->requiresUnsafe || function->neverReturns ||
        !scalar(function->returnType) ||
        !std::all_of(function->parameterTypes.begin(),
                     function->parameterTypes.end(), scalar))
      reject("function signature or definition");
    const auto &body = function->body;
    if (!body.dropObligations.empty() || !body.loans.empty() ||
        !body.rangeIterations.empty())
      reject("cleanup, borrowing or range obligation");
    for (const auto &place : body.places)
      if (!scalar(place.type) || !place.projections.empty() ||
          place.foreignStorage ||
          (place.root != MirPlaceRootKind::Binding &&
           place.root != MirPlaceRootKind::Temporary &&
           place.root != MirPlaceRootKind::Value))
        reject("non-scalar or nonlocal place");
    for (const auto &value : body.values)
      if (!scalar(value.info.type))
        reject("non-scalar value");
    for (const auto &block : body.blocks) {
      const auto term = block.terminator.kind;
      if (block.activeFailure &&
          (!block.instructions.empty() ||
           (term != MirTerminatorKind::ContinueFailure &&
            term != MirTerminatorKind::PropagateFailure &&
            term != MirTerminatorKind::Goto)))
        reject("observable failure continuation");
      if (term != MirTerminatorKind::Return &&
          term != MirTerminatorKind::Goto &&
          term != MirTerminatorKind::Branch &&
          term != MirTerminatorKind::Invoke &&
          term != MirTerminatorKind::ContinueFailure &&
          term != MirTerminatorKind::PropagateFailure)
        reject("terminator outside scalar profile");
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone)
          continue;
        if (instruction.intrinsic != IntrinsicKind::None ||
            instruction.rawMemoryAccess || instruction.receiver ||
            instruction.elementCleanup || instruction.replacementCleanup ||
            instruction.preparedParameterDrop ||
            instruction.successResultDrop || instruction.bodyTarget ||
            instruction.nativeCallbackAdapter || instruction.threadTaskAdapter)
          reject("non-scalar instruction facts");
        switch (instruction.kind) {
        case MirInstructionKind::Lifecycle:
          // Empty expression-boundary markers carry no cleanup operation.
          if (!instruction.lifecycle.empty())
            reject("lifecycle event outside scalar profile");
          break;
        case MirInstructionKind::Load:
        case MirInstructionKind::Initialize:
        case MirInstructionKind::Assign:
        case MirInstructionKind::CallInput:
          break;
        case MirInstructionKind::Call:
          if (!instruction.functionTarget ||
              !selected.contains(*instruction.functionTarget) ||
              instruction.dispatch != CallDispatch::Static)
            reject("callee outside selected static closure");
          break;
        case MirInstructionKind::Compute:
          switch (instruction.operation) {
          case MirOperation::Literal:
          case MirOperation::Identity:
          case MirOperation::Convert:
          case MirOperation::Add:
          case MirOperation::Subtract:
          case MirOperation::Multiply:
          case MirOperation::Divide:
          case MirOperation::Remainder:
          case MirOperation::Equal:
          case MirOperation::NotEqual:
          case MirOperation::Less:
          case MirOperation::LessEqual:
          case MirOperation::Greater:
          case MirOperation::GreaterEqual:
            break;
          default:
            reject("operation outside scalar profile");
          }
          break;
        default:
          reject("instruction kind outside scalar profile");
        }
      }
    }
  }
}
} // namespace lang::native_llvm
