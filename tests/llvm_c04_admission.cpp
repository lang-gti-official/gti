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
      std::string(
          "C04 evaluation supports only a closed scalar/borrowed-cleanup "
          "call graph: ") +
      reason);
}
bool scalar(const SemanticType &type) {
  return type == SemanticType::Int32 || type == SemanticType::Bool;
}
bool scalarOrReference(const SemanticType &type) {
  return scalar(type) ||
         (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
          scalar(type.arguments[0]));
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
  const auto &program = session.program().mir();
  const auto knownClass = [&](const SemanticType &type) {
    return std::any_of(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &instance) { return instance.type == type; });
  };
  std::set<HirClassInstanceId> classes;
  for (const auto &instance : program.classInstances()) {
    if (instance.compilerCapability != CompilerCapabilityTypeKind::None ||
        instance.polymorphic || instance.abstract || instance.cAbiRecord ||
        instance.cOpaqueHandle || instance.unionLayout ||
        !instance.bases.empty() || !instance.structuralBases.empty() ||
        !instance.virtualDispatch.empty() ||
        !std::all_of(instance.declaredFields.begin(),
                     instance.declaredFields.end(), [&](const auto &field) {
                       return scalarOrReference(field.type) ||
                              knownClass(field.type);
                     }))
      reject("class outside ordinary scalar/reference/record-field cleanup "
             "profile");
    classes.insert(instance.id);
  }
  const auto localType = [&](const SemanticType &type) {
    return scalarOrReference(type) || knownClass(type);
  };
  for (auto id : selected) {
    const auto *function = session.program().mir().findFunctionInstance(id);
    if (!function || function->definitionKind != MirDefinitionKind::Source ||
        function->linkage != LanguageLinkage::Gti || function->owner ||
        function->entryKind != ProgramEntryKind::None ||
        function->requiresUnsafe || function->neverReturns ||
        !scalar(function->returnType) ||
        !std::all_of(function->parameterTypes.begin(),
                     function->parameterTypes.end(), scalarOrReference))
      reject("function signature or definition");
  }
  for (const auto &constructor : program.constructorInstances())
    if (!classes.contains(constructor.owner) || constructor.requiresUnsafe ||
        constructor.definitionKind != MirDefinitionKind::Source ||
        !std::all_of(constructor.parameterTypes.begin(),
                     constructor.parameterTypes.end(), scalarOrReference))
      reject("constructor outside scalar/reference profile");
  for (const auto &destructor : program.destructorInstances())
    if (!classes.contains(destructor.owner) ||
        destructor.definitionKind != MirDefinitionKind::Source)
      reject("destructor outside source profile");
  for (auto address : admission.requiredBodies()) {
    const bool helper = address.kind == MirBodyKind::Constructor ||
                        address.kind == MirBodyKind::Destructor ||
                        address.kind == MirBodyKind::FieldInitializers;
    if (!helper && (address.kind != MirBodyKind::Function ||
                    !selected.contains(address.owner)))
      reject("required body outside selected closure");
    const auto &body = *findMirBody(program, address);
    if (!body.rangeIterations.empty())
      reject("range obligation");
    for (const auto &drop : body.dropObligations)
      if (!localType(drop.dropType.type) || !drop.dropType.classInstance ||
          !classes.contains(*drop.dropType.classInstance))
        reject("cleanup outside admitted local classes");
    for (const auto &place : body.places)
      if (!localType(place.type) || place.foreignStorage ||
          place.root == MirPlaceRootKind::Symbol ||
          std::any_of(place.projections.begin(), place.projections.end(),
                      [](const auto &projection) {
                        return projection.kind != MirProjectionKind::Field &&
                               projection.kind !=
                                   MirProjectionKind::Dereference;
                      }))
        reject("place outside scalar/reference/local class profile");
    for (const auto &value : body.values)
      if (!localType(value.info.type))
        reject("value outside scalar/reference/local class profile");
    for (const auto &block : body.blocks) {
      const auto term = block.terminator.kind;
      if (term != MirTerminatorKind::Return &&
          term != MirTerminatorKind::Exit && term != MirTerminatorKind::Goto &&
          term != MirTerminatorKind::Branch &&
          term != MirTerminatorKind::Invoke &&
          term != MirTerminatorKind::ContinueFailure &&
          term != MirTerminatorKind::PropagateFailure &&
          term != MirTerminatorKind::TerminateCleanupFailure)
        reject("terminator outside scalar profile");
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone)
          continue;
        if (instruction.intrinsic != IntrinsicKind::None ||
            instruction.rawMemoryAccess || instruction.elementCleanup ||
            instruction.replacementCleanup ||
            instruction.preparedParameterDrop ||
            (instruction.successResultDrop &&
             instruction.kind != MirInstructionKind::Construct) ||
            instruction.nativeCallbackAdapter || instruction.threadTaskAdapter)
          reject("non-scalar instruction facts");
        switch (instruction.kind) {
        case MirInstructionKind::Lifecycle:
          for (const auto &event : instruction.lifecycle) {
            const auto *source = body.findDropObligation(event.source);
            const bool constructorPublication =
                address.kind == MirBodyKind::Constructor &&
                term == MirTerminatorKind::Return && source &&
                source->kind == MirDropObligationKind::ConstructionRollback &&
                event.target == 0 && !event.conditional &&
                !event.failureCleanup;
            if (event.kind == MirLifecycleEventKind::Replace ||
                (event.kind == MirLifecycleEventKind::TransferOut &&
                 !constructorPublication))
              reject("replacement or escaping owner lifecycle");
          }
          break;
        case MirInstructionKind::Borrow:
        case MirInstructionKind::ActivateBorrow:
        case MirInstructionKind::EndBorrow:
        case MirInstructionKind::Drop:
        case MirInstructionKind::Load:
        case MirInstructionKind::Initialize:
        case MirInstructionKind::Assign:
        case MirInstructionKind::CallInput:
          break;
        case MirInstructionKind::Construct: {
          if (instruction.constructorTarget) {
            if (!program.findConstructorInstance(
                    *instruction.constructorTarget))
              reject("construction outside verified source constructors");
          } else {
            const auto instance = std::find_if(
                program.classInstances().begin(),
                program.classInstances().end(), [&](const auto &item) {
                  return item.type == instruction.info.type;
                });
            if (instance == program.classInstances().end() ||
                instruction.constructorKind != ConstructorKind::Ordinary ||
                !instruction.operands.empty() ||
                !instance->declaredFields.empty() ||
                instance->defaultConstructor !=
                    SpecialMemberStatus::Generated ||
                instance->generatedDefaultRequiresSchedule)
              reject("targetless construction outside empty generated default");
          }
          break;
        }
        case MirInstructionKind::CallBody:
          if (!instruction.bodyTarget ||
              instruction.bodyTarget->kind != MirBodyKind::FieldInitializers ||
              !classes.contains(instruction.bodyTarget->owner))
            reject("body call outside field initialization");
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
