#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/native_c_contract.h"
#include "../src/compiler/mir_verification_queries.h"

#include <algorithm>
#include <functional>
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
bool observerPointer(const SemanticType &type) {
  return type.kind == SemanticType::RawPointer && type.arguments.size() == 1 &&
         type.arguments.front() == SemanticType::Int32;
}
bool scalarInput(const SemanticType &type) {
  return scalarOrReference(type) || observerPointer(type);
}
} // namespace

std::vector<MirBodyAddress>
validateC04Evaluation(const Session &session,
                      const LlvmAdmissionPlan &admission,
                      std::span<const HirFunctionInstanceId> functions,
                      C04EvaluationProfile profile) {
  if (profile != C04EvaluationProfile::ScalarCleanup &&
      profile != C04EvaluationProfile::NativeBoundary &&
      profile != C04EvaluationProfile::DynamicCalls &&
      profile != C04EvaluationProfile::ScopedWorkers)
    reject("unknown evaluation profile");
  const bool workers = profile == C04EvaluationProfile::ScopedWorkers;
  const bool dynamic = profile == C04EvaluationProfile::DynamicCalls || workers;
  if (&session.program() != &admission.program() ||
      (!dynamic && !admission.admitted()) ||
      session.options().optimization != admission.options().optimization ||
      session.options().debugInfo != admission.options().debugInfo)
    throw BackendUnsupportedError(
        "C04 evaluation requires the exact admitted snapshot and options");
  std::set<HirFunctionInstanceId> selected(functions.begin(), functions.end());
  if (selected.empty() || selected.size() != functions.size() ||
      !session.program().mir().programInitializationPlan().steps.empty())
    reject("empty/duplicate selection or program initialization");
  const auto &program = session.program().mir();
  std::vector<MirBodyAddress> bodies(admission.requiredBodies().begin(),
                                     admission.requiredBodies().end());
  std::set<HirClassInstanceId> demandedClasses;
  if (dynamic) {
    // This is private evaluation demand, not a modified production admission
    // plan. Every demanded operation is checked below against the sealed MIR.
    bodies.clear();
    const auto demand = [&](MirBodyAddress address) {
      if (!findMirBody(program, address))
        reject("dynamic dependency has no sealed body");
      if (std::find(bodies.begin(), bodies.end(), address) == bodies.end())
        bodies.push_back(address);
    };
    std::function<void(const SemanticType &)> type;
    type = [&](const SemanticType &value) {
      for (const auto &argument : value.arguments)
        type(argument);
      for (const auto &instance : program.classInstances())
        if (instance.type == value &&
            demandedClasses.insert(instance.id).second)
          for (const auto &field : instance.declaredFields)
            type(field.type);
    };
    std::function<void(const MirDropType &)> cleanup;
    cleanup = [&](const MirDropType &drop) {
      type(drop.type);
      if (drop.destructor)
        demand({MirBodyKind::Destructor, *drop.destructor});
      for (const auto &child : drop.aggregate)
        cleanup(child);
    };
    for (const auto root : functions)
      demand({MirBodyKind::Function, root});
    for (std::size_t next = 0; next < bodies.size(); ++next) {
      const auto address = bodies[next];
      const auto &body = *findMirBody(program, address);
      if (address.kind == MirBodyKind::Function) {
        const auto *function = program.findFunctionInstance(address.owner);
        if (!function || function->definitionKind != MirDefinitionKind::Source)
          reject("dynamic closure has a non-source function");
        selected.insert(function->id);
        type(function->returnType);
        for (const auto &parameter : function->parameterTypes)
          type(parameter);
        if (function->owner)
          type(program.findClassInstance(*function->owner)->type);
      }
      for (const auto &place : body.places)
        type(place.type);
      for (const auto &value : body.values)
        type(value.info.type);
      for (const auto &drop : body.dropObligations)
        cleanup(drop.dropType);
      for (const auto &block : body.blocks)
        for (const auto &stage : block.instructions) {
          if (stage.tombstone)
            continue;
          if (stage.functionTarget && stage.intrinsic == IntrinsicKind::None)
            demand({MirBodyKind::Function, *stage.functionTarget});
          if (stage.functionPointerFunctionTarget)
            demand(
                {MirBodyKind::Function, *stage.functionPointerFunctionTarget});
          if (workers && stage.intrinsic == IntrinsicKind::ThreadScope)
            for (auto target : c04ScopedTargets(session, address, stage))
              demand({MirBodyKind::Function, target});
          if (stage.constructorTarget)
            demand({MirBodyKind::Constructor, *stage.constructorTarget});
          if (stage.bodyTarget)
            demand(*stage.bodyTarget);
          if (stage.replacementCleanup)
            cleanup(*stage.replacementCleanup);
        }
    }
  }
  const auto knownClass = [&](const SemanticType &type) {
    return std::any_of(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &instance) {
          return instance.type == type &&
                 (!dynamic || demandedClasses.contains(instance.id));
        });
  };
  std::function<bool(const SemanticType &)> passiveEnum;
  passiveEnum = [&](const SemanticType &type) {
    if (!workers || type.kind != SemanticType::Enum)
      return false;
    const auto *payload = program.findPayloadEnum(type);
    if (!payload)
      return true;
    if (payload->requiresActiveCleanup)
      return false;
    for (const auto &variant : payload->variants)
      for (const auto &element : variant.payloadTypes)
        if (!scalar(element) && !passiveEnum(element))
          return false;
    return true;
  };
  std::set<HirClassInstanceId> classes;
  for (const auto &instance : program.classInstances()) {
    if (dynamic && !demandedClasses.contains(instance.id))
      continue;
    const auto *pointer =
        dynamic ? mir_detail::functionPointerSignature(program, instance.type)
                : nullptr;
    if (pointer && (pointer->arguments.empty() ||
                    !std::all_of(pointer->arguments.begin(),
                                 pointer->arguments.end(), scalarOrReference)))
      reject("dynamic invocation signature outside scalar/reference profile");
    if ((instance.compilerCapability != CompilerCapabilityTypeKind::None &&
         !pointer) ||
        instance.polymorphic || instance.abstract || instance.cAbiRecord ||
        instance.cOpaqueHandle || instance.unionLayout ||
        !instance.bases.empty() || !instance.structuralBases.empty() ||
        !instance.virtualDispatch.empty() ||
        !std::all_of(instance.declaredFields.begin(),
                     instance.declaredFields.end(), [&](const auto &field) {
                       return scalarInput(field.type) ||
                              knownClass(field.type) || passiveEnum(field.type);
                     }))
      reject("class outside ordinary scalar/reference/observer/record-field "
             "cleanup "
             "profile");
    classes.insert(instance.id);
  }
  std::function<bool(const SemanticType &)> localType;
  localType = [&](const SemanticType &type) {
    return scalarInput(type) || type == SemanticType::NullPtr ||
           knownClass(type) || passiveEnum(type) ||
           (workers && type == SemanticType::Void) ||
           (dynamic &&
            ((type.kind == SemanticType::Reference &&
              type.arguments.size() == 1 &&
              knownClass(type.arguments.front())) ||
             type.hasFunctionItemShape() ||
             (type.kind == SemanticType::TypePack && type.concretePack &&
              std::all_of(type.arguments.begin(), type.arguments.end(),
                          [&](const auto &element) {
                            return workers ? localType(element) &&
                                                 element.kind !=
                                                     SemanticType::TypePack
                                           : scalarOrReference(element);
                          }))));
  };
  const auto recordDestination = [&](const MirPlace *place) {
    return place && place->root == MirPlaceRootKind::Binding &&
           !place->foreignStorage && knownClass(place->type) &&
           std::all_of(place->projections.begin(), place->projections.end(),
                       [](const auto &projection) {
                         return projection.kind == MirProjectionKind::Field;
                       });
  };
  for (auto id : selected) {
    const auto *function = session.program().mir().findFunctionInstance(id);
    if (!function || function->definitionKind != MirDefinitionKind::Source ||
        function->linkage != LanguageLinkage::Gti ||
        (!dynamic && function->owner) ||
        function->entryKind != ProgramEntryKind::None ||
        function->requiresUnsafe || function->neverReturns ||
        (!scalar(function->returnType) && !knownClass(function->returnType) &&
         !(workers && (function->returnType == SemanticType::Void ||
                       passiveEnum(function->returnType)))) ||
        !std::all_of(function->parameterTypes.begin(),
                     function->parameterTypes.end(), localType))
      reject("function signature or definition");
  }
  for (const auto &constructor : program.constructorInstances()) {
    if (dynamic && std::find(bodies.begin(), bodies.end(),
                             MirBodyAddress{MirBodyKind::Constructor,
                                            constructor.id}) == bodies.end())
      continue;
    if (!classes.contains(constructor.owner) || constructor.requiresUnsafe ||
        constructor.definitionKind != MirDefinitionKind::Source ||
        !std::all_of(constructor.parameterTypes.begin(),
                     constructor.parameterTypes.end(), [&](const auto &type) {
                       return dynamic ? localType(type) : scalarInput(type);
                     }))
      reject("constructor outside scalar/reference profile");
  }
  for (const auto &destructor : program.destructorInstances()) {
    if (dynamic && !classes.contains(destructor.owner))
      continue;
    if (!classes.contains(destructor.owner) ||
        destructor.definitionKind != MirDefinitionKind::Source)
      reject("destructor outside source profile");
  }
  unsigned formations = 0, invocations = 0, scopes = 0;
  for (auto address : bodies) {
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
                      [&](const auto &projection) {
                        return projection.kind != MirProjectionKind::Field &&
                               projection.kind !=
                                   MirProjectionKind::Dereference &&
                               projection.kind !=
                                   MirProjectionKind::RawDereference &&
                               !(dynamic && projection.kind ==
                                                MirProjectionKind::PackElement);
                      }))
        throw BackendUnsupportedError(
            "C04 evaluation place outside scalar/reference/local class "
            "profile: body " +
            std::to_string(static_cast<unsigned>(address.kind)) + "/" +
            std::to_string(address.owner) + " place " +
            std::to_string(place.id) + " type " +
            std::to_string(static_cast<unsigned>(place.type.kind)) + " root " +
            std::to_string(static_cast<unsigned>(place.root)));
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
        const bool pointerOperation =
            dynamic &&
            (instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem ||
             instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke) &&
            mir_detail::functionPointerOperation(program, instruction);
        const bool scopeOperation =
            workers && instruction.intrinsic == IntrinsicKind::ThreadScope;
        if (scopeOperation) {
          (void)c04ScopedTargets(session, address, instruction);
          ++scopes;
        }
        const bool moveOperation =
            workers && instruction.intrinsic == IntrinsicKind::Move &&
            instruction.kind == MirInstructionKind::Move &&
            !instruction.receiver && !instruction.functionTarget &&
            instruction.operands.size() == 1 &&
            instruction.parameterTypes.empty() &&
            instruction.info.type == instruction.operands[0].type &&
            localType(instruction.info.type);
        if (pointerOperation) {
          formations +=
              instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem;
          invocations +=
              instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke;
        }
        if ((instruction.intrinsic != IntrinsicKind::None &&
             !pointerOperation && !scopeOperation && !moveOperation) ||
            instruction.elementCleanup || instruction.nativeCallbackAdapter ||
            instruction.threadTaskAdapter)
          throw BackendUnsupportedError(
              "C04 evaluation non-scalar instruction facts: " +
              std::to_string(static_cast<unsigned>(address.kind)) + "/" +
              std::to_string(address.owner) + " instruction " +
              std::to_string(instruction.id) + " intrinsic " +
              std::to_string(static_cast<unsigned>(instruction.intrinsic)));
        if (instruction.rawMemoryAccess &&
            (instruction.unsafeOperation !=
                 UnsafeOperationKind::RawDereference ||
             !scalar(instruction.info.type) ||
             (instruction.kind != MirInstructionKind::Load &&
              instruction.kind != MirInstructionKind::Assign)))
          reject("raw access outside explicit unsafe scalar observation");
        if (instruction.replacementCleanup) {
          const auto *destination =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          if (instruction.kind != MirInstructionKind::CleanupBeforeAssign ||
              !instruction.replacementPair || !recordDestination(destination) ||
              destination->type != instruction.replacementCleanup->type)
            reject("replacement outside local ordinary record or field");
        }
        if (instruction.successResultDrop &&
            instruction.kind != MirInstructionKind::Construct) {
          const auto *target =
              instruction.functionTarget
                  ? program.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          if (instruction.kind != MirInstructionKind::Call || !target ||
              !selected.contains(target->id) ||
              instruction.dispatch != CallDispatch::Static ||
              !knownClass(target->returnType))
            reject("success result outside ordinary record call profile");
        }
        if (instruction.preparedParameterDrop) {
          const auto *prepared =
              body.findDropObligation(*instruction.preparedParameterDrop);
          if (instruction.kind != MirInstructionKind::CallInput || !prepared ||
              prepared->kind != MirDropObligationKind::PreparedParameter ||
              !knownClass(prepared->dropType.type))
            reject("prepared parameter outside ordinary record profile");
        }
        for (const auto &event : instruction.lifecycle) {
          const auto *source = body.findDropObligation(event.source);
          const bool publication = event.target == 0 && !event.conditional &&
                                   !event.failureCleanup && source;
          const bool constructorPublication =
              publication && address.kind == MirBodyKind::Constructor &&
              term == MirTerminatorKind::Return &&
              source->kind == MirDropObligationKind::ConstructionRollback;
          const bool fieldInitializerPublication =
              publication && address.kind == MirBodyKind::FieldInitializers &&
              term == MirTerminatorKind::Exit &&
              source->kind == MirDropObligationKind::ConstructionRollback;
          const bool parameterTransfer =
              publication && instruction.kind == MirInstructionKind::Call &&
              instruction.functionTarget &&
              selected.contains(*instruction.functionTarget) &&
              instruction.dispatch == CallDispatch::Static &&
              source->kind == MirDropObligationKind::PreparedParameter;
          const bool scopeTransfer =
              publication && scopeOperation &&
              source->kind == MirDropObligationKind::PreparedParameter &&
              std::any_of(instruction.operands.begin(),
                          instruction.operands.end(), [&](const auto &operand) {
                            return operand.type == source->dropType.type;
                          });
          const bool workerConstructorTransfer =
              workers && publication &&
              instruction.kind == MirInstructionKind::Construct &&
              instruction.constructorTarget &&
              std::find(bodies.begin(), bodies.end(),
                        MirBodyAddress{MirBodyKind::Constructor,
                                       *instruction.constructorTarget}) !=
                  bodies.end() &&
              source->kind == MirDropObligationKind::PreparedParameter &&
              std::any_of(instruction.parameterTypes.begin(),
                          instruction.parameterTypes.end(),
                          [&](const auto &type) {
                            return type == source->dropType.type;
                          });
          const bool resultPublication =
              publication && address.kind == MirBodyKind::Function &&
              term == MirTerminatorKind::Return &&
              (source->kind == MirDropObligationKind::ReturnResult ||
               source->kind == MirDropObligationKind::Value) &&
              knownClass(source->dropType.type);
          const auto *destination =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          // A field remains owned by its containing binding. Its prepared RHS
          // transfers into that field, rather than replacing the parent's drop.
          const bool fieldReplacement =
              publication && instruction.kind == MirInstructionKind::Assign &&
              instruction.replacementPair && recordDestination(destination) &&
              !destination->projections.empty() &&
              source->kind == MirDropObligationKind::Value &&
              source->dropType.type == destination->type;
          if (event.kind == MirLifecycleEventKind::Replace &&
              !fieldReplacement) {
            const auto *target = body.findDropObligation(event.target);
            if (instruction.kind != MirInstructionKind::Assign ||
                !instruction.replacementPair || !source || !target ||
                event.conditional || event.failureCleanup ||
                target->kind != MirDropObligationKind::Binding ||
                instruction.destination != target->place ||
                !knownClass(source->dropType.type) ||
                source->dropType.type != target->dropType.type)
              reject("replacement outside exact ordinary record publication");
          }
          if (event.kind == MirLifecycleEventKind::TransferOut &&
              !constructorPublication && !fieldInitializerPublication &&
              !parameterTransfer && !workerConstructorTransfer &&
              !scopeTransfer && !resultPublication)
            throw BackendUnsupportedError(
                "C04 evaluation replacement or escaping owner lifecycle: " +
                std::to_string(static_cast<unsigned>(address.kind)) + "/" +
                std::to_string(address.owner) + " instruction " +
                std::to_string(instruction.id) + " intrinsic " +
                std::to_string(static_cast<unsigned>(instruction.intrinsic)) +
                " source-kind " +
                std::to_string(source ? static_cast<unsigned>(source->kind)
                                      : 999));
        }
        switch (instruction.kind) {
        case MirInstructionKind::Lifecycle:
          break;
        case MirInstructionKind::Borrow:
        case MirInstructionKind::ActivateBorrow:
        case MirInstructionKind::EndBorrow:
        case MirInstructionKind::Drop:
        case MirInstructionKind::CleanupBeforeAssign:
        case MirInstructionKind::Load:
        case MirInstructionKind::Initialize:
        case MirInstructionKind::Assign:
        case MirInstructionKind::CallInput:
          break;
        case MirInstructionKind::Move:
          if (!workers)
            reject("move outside worker evaluation profile");
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
        case MirInstructionKind::Call: {
          if (pointerOperation || scopeOperation || moveOperation)
            break;
          const auto *callee =
              instruction.functionTarget
                  ? program.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          if (callee && callee->linkage == LanguageLinkage::C &&
              profile == C04EvaluationProfile::NativeBoundary) {
            const auto *declaration =
                session.program().findFunctionDeclaration(callee->declaration);
            const bool pointer =
                std::any_of(callee->parameterTypes.begin(),
                            callee->parameterTypes.end(), observerPointer);
            if (!declaration || declaration->linkage != LanguageLinkage::C ||
                declaration->definitionKind != MirDefinitionKind::Declaration ||
                declaration->externalSymbol != callee->externalSymbol ||
                declaration->returnType != callee->returnType ||
                !std::equal(callee->parameterTypes.begin(),
                            callee->parameterTypes.end(),
                            declaration->parameters.begin(),
                            declaration->parameters.end(),
                            [](const auto &type, const auto &parameter) {
                              return type == parameter.type;
                            }) ||
                instruction.dispatch != CallDispatch::Static ||
                !nativeCDirectCallContract(program, session.program().target(),
                                           body, instruction) ||
                !scalar(callee->returnType) ||
                !std::all_of(callee->parameterTypes.begin(),
                             callee->parameterTypes.end(),
                             [](const auto &type) {
                               return scalar(type) || observerPointer(type);
                             }) ||
                instruction.unsafeOperation !=
                    (pointer ? UnsafeOperationKind::ForeignPointerCall
                             : UnsafeOperationKind::None))
              reject("native call lost exact scalar/observer signature, stage "
                     "or unsafe authority");
            break;
          }
          if (!instruction.functionTarget ||
              !selected.contains(*instruction.functionTarget) ||
              instruction.dispatch != CallDispatch::Static ||
              instruction.nativeCalleeType ||
              instruction.unsafeOperation != UnsafeOperationKind::None)
            reject("callee outside selected static closure");
          break;
        }
        case MirInstructionKind::Compute:
          if (workers && passiveEnum(instruction.info.type) &&
              (instruction.operation == MirOperation::EnumConstant ||
               instruction.operation == MirOperation::PayloadConstruct))
            break;
          if (dynamic && instruction.operation == MirOperation::FunctionItem &&
              instruction.info.type.hasFunctionItemShape())
            break;
          switch (instruction.operation) {
          case MirOperation::Literal:
          case MirOperation::Identity:
          case MirOperation::Convert:
          case MirOperation::AddressOf:
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
  if (workers && !scopes)
    reject("worker profile requires an actual scoped task group");
  if (dynamic && !workers && (!formations || !invocations))
    reject("dynamic profile requires actual formation and invocation");
  return bodies;
}
} // namespace lang::native_llvm
