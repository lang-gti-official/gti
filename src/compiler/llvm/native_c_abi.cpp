#include "native_c_abi.h"
#include "debug_info.h"
#include "failure.h"
#include "native_c_contract.h"
#include "runtime_abi.h"
#include "thread_lifecycle.h"

#include <llvm/IR/Constants.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <array>
#include <functional>
#include <set>

namespace lang::native_llvm {

bool nativeCScalarValueContract(const TargetInfo &target,
                                const SemanticType &type) {
  if (!((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")) ||
      type.nativeCScalar == NativeCScalarKind::None)
    return false;
  const auto canonical =
      SemanticType::nativeCScalarType(type.nativeCScalar, target);
  return canonical && *canonical == type;
}

bool nativeCValueContract(const MirProgram &program, const TargetInfo &target,
                          const SemanticType &type, NativeCValueRole role) {
  if (!((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")))
    return false;
  std::set<HirClassInstanceId> visiting;
  const auto value = [&](const auto &self, const SemanticType &selected,
                         NativeCValueRole position, unsigned depth) -> bool {
    if (depth > 256)
      return false;
    if (selected.nativeCScalar != NativeCScalarKind::None &&
        !nativeCScalarValueContract(target, selected))
      return false;
    switch (selected.kind) {
    case SemanticType::Void:
      return position == NativeCValueRole::Result;
    case SemanticType::Bool:
    case SemanticType::Int8:
    case SemanticType::UInt8:
    case SemanticType::Int16:
    case SemanticType::UInt16:
    case SemanticType::Int32:
    case SemanticType::UInt32:
    case SemanticType::Int64:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::CString:
      return true;
    case SemanticType::StringView:
      return position == NativeCValueRole::Argument;
    case SemanticType::RawPointer:
      if (selected.arguments.size() != 1)
        return false;
      if (selected.arguments.front().kind == SemanticType::Class) {
        return std::any_of(program.classInstances().begin(),
                           program.classInstances().end(),
                           [&](const auto &record) {
                             return record.type == selected.arguments.front() &&
                                    (record.cAbiRecord || record.cOpaqueHandle);
                           });
      }
      return self(self, selected.arguments.front(), NativeCValueRole::Result,
                  depth + 1);
    case SemanticType::NativeFunction:
      if (!selected.hasNativeFunctionShape() ||
          !self(self, *selected.nativeFunctionReturnType(),
                NativeCValueRole::Result, depth + 1))
        return false;
      return std::all_of(selected.nativeFunctionParameterTypes().begin(),
                         selected.nativeFunctionParameterTypes().end(),
                         [&](const auto &parameter) {
                           return self(self, parameter,
                                       NativeCValueRole::Argument, depth + 1);
                         });
    case SemanticType::Array:
      return position == NativeCValueRole::Storage &&
             selected.arguments.size() == 1 && selected.arrayLength > 0 &&
             !selected.arrayLengthParameterId &&
             self(self, selected.arguments.front(), NativeCValueRole::Storage,
                  depth + 1);
    case SemanticType::Class: {
      const MirClassInstance *record = nullptr;
      for (const auto &candidate : program.classInstances())
        if (candidate.type == selected) {
          if (record)
            return false;
          record = &candidate;
        }
      if (!record || !record->cAbiRecord || !record->cAbiLayout ||
          record->cOpaqueHandle || record->polymorphic || record->abstract ||
          !record->bases.empty() || !record->structuralBases.empty() ||
          record->requiresActiveCleanup || record->requiresActiveDropState ||
          record->declaredFields.empty() ||
          record->cAbiLayout->sizeBytes == 0 ||
          record->cAbiLayout->abiAlignmentBytes == 0 ||
          record->cAbiLayout->abiAlignmentBytes > 8 ||
          record->cAbiLayout->fields.size() != record->declaredFields.size() ||
          !visiting.insert(record->id).second)
        return false;
      bool valid = true;
      for (std::size_t index = 0; index < record->declaredFields.size();
           ++index) {
        const auto &field = record->declaredFields[index];
        const auto &layout = record->cAbiLayout->fields[index];
        valid = valid && field.symbol == layout.field &&
                field.type == layout.type && !field.requiresActiveCleanup &&
                self(self, field.type, NativeCValueRole::Storage, depth + 1);
      }
      visiting.erase(record->id);
      return valid;
    }
    default:
      return false;
    }
  };
  return value(value, type, role, 0);
}

bool nativeCOpaquePointerReferentContract(const MirProgram &program,
                                          const TargetInfo &target,
                                          const SemanticType &pointer) {
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.arguments.size() != 1 ||
      pointer.arguments.front().kind != SemanticType::Class ||
      !nativeCValueContract(program, target, pointer,
                            NativeCValueRole::Argument))
    return false;
  const MirClassInstance *identity = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == pointer.arguments.front()) {
      if (identity)
        return false;
      identity = &candidate;
    }
  return identity && identity->cOpaqueHandle && !identity->cAbiRecord &&
         !identity->cAbiLayout && !identity->unionLayout &&
         identity->declaredFields.empty() && identity->bases.empty() &&
         identity->structuralBases.empty() && !identity->polymorphic &&
         !identity->abstract && !identity->requiresActiveCleanup &&
         !identity->requiresActiveDropState;
}

bool nativeCDeclarationContract(const MirProgram &program,
                                const TargetInfo &target,
                                const MirFunctionInstance &function) {
  const auto *canonical = program.findFunctionInstance(function.id);
  if (!canonical || *canonical != function ||
      function.definitionKind != MirDefinitionKind::Declaration ||
      function.linkage != LanguageLinkage::C ||
      function.externalSymbol.empty() ||
      function.externalSymbol.find('\0') != std::string::npos ||
      function.owner || function.tracksCaller ||
      !function.typeArguments.empty() || !function.valueArguments.empty() ||
      function.virtualMethod ||
      function.processMemoryOperation != ProcessMemoryRuntimeOperation::None ||
      (function.cThreadOperation != CThreadRuntimeOperation::None &&
       !nativeCThreadOperationBinding(program, function)) ||
      !function.callableParameters.empty() ||
      !nativeCValueContract(program, target, function.returnType,
                            NativeCValueRole::Result))
    return false;
  return std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), [&](const auto &parameter) {
                       return nativeCValueContract(program, target, parameter,
                                                   NativeCValueRole::Argument);
                     });
}

namespace {
bool scalarBoundaryType(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Void:
  case SemanticType::Bool:
  case SemanticType::Int8:
  case SemanticType::UInt8:
  case SemanticType::Int16:
  case SemanticType::UInt16:
  case SemanticType::Int32:
  case SemanticType::UInt32:
  case SemanticType::Int64:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
  case SemanticType::CString:
  case SemanticType::RawPointer:
    return true;
  default:
    return false;
  }
}

// Structural family shared by passive C imports and foreign object storage.
// Canonical layout, field identities and target validity are checked
// separately.
bool passiveNativeCStorageType(const MirProgram &program,
                               const SemanticType &type, unsigned depth = 0) {
  if (depth > 256)
    return false;
  if (type.kind == SemanticType::Class) {
    for (const auto &record : program.classInstances())
      if (record.type == type)
        return std::all_of(record.declaredFields.begin(),
                           record.declaredFields.end(), [&](const auto &field) {
                             return passiveNativeCStorageType(
                                 program, field.type, depth + 1);
                           });
    return false;
  }
  if (type.kind == SemanticType::Array)
    return type.arguments.size() == 1 &&
           passiveNativeCStorageType(program, type.arguments[0], depth + 1);
  return scalarBoundaryType(type) && type.kind != SemanticType::Void &&
         type.kind != SemanticType::CString;
}
} // namespace

bool nativeCScalarImportContract(const MirProgram &program,
                                 const TargetInfo &target,
                                 const MirFunctionInstance &function) {
  return nativeCDeclarationContract(program, target, function) &&
         scalarBoundaryType(function.returnType) &&
         std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), scalarBoundaryType);
}

bool nativeCCountedInputContract(const MirProgram &program,
                                 const TargetInfo &target,
                                 const MirFunctionInstance &function) {
  return nativeCDeclarationContract(program, target, function) &&
         scalarBoundaryType(function.returnType) &&
         std::any_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(),
                     [](const auto &type) {
                       return type.kind == SemanticType::StringView;
                     }) &&
         std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), [](const auto &type) {
                       return type.kind == SemanticType::StringView ||
                              scalarBoundaryType(type);
                     });
}

bool nativeCAggregateImportContract(const MirProgram &program,
                                    const TargetInfo &target,
                                    const MirFunctionInstance &function) {
  if (!nativeCDeclarationContract(program, target, function))
    return false;
  // The canonical value contract owns layout/identity/target validity. This
  // profile only bounds the already verified representation family. Existing
  // immutable counted inputs compose; native function fields remain excluded.
  bool aggregate = false;
  const auto boundary = [&](const SemanticType &type, bool input) {
    if (input && type.kind == SemanticType::StringView)
      return true;
    if (scalarBoundaryType(type))
      return true;
    if (type.kind != SemanticType::Class)
      return false;
    aggregate = true;
    return passiveNativeCStorageType(program, type);
  };
  return boundary(function.returnType, false) &&
         std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(),
                     [&](const auto &type) { return boundary(type, true); }) &&
         aggregate;
}

bool nativeCScalarFunctionValueContract(const MirProgram &program,
                                        const TargetInfo &target,
                                        const SemanticType &type) {
  return type.kind == SemanticType::NativeFunction &&
         type.hasNativeFunctionShape() &&
         nativeCValueContract(program, target, type,
                              NativeCValueRole::Argument) &&
         scalarBoundaryType(*type.nativeFunctionReturnType()) &&
         std::all_of(type.nativeFunctionParameterTypes().begin(),
                     type.nativeFunctionParameterTypes().end(),
                     scalarBoundaryType);
}

bool nativeCFunctionValueContract(const MirProgram &program,
                                  const TargetInfo &target,
                                  const SemanticType &type) {
  return type.kind == SemanticType::NativeFunction &&
         type.hasNativeFunctionShape() &&
         nativeCValueContract(program, target, type,
                              NativeCValueRole::Argument);
}

bool nativeCScalarFunctionImportContract(const MirProgram &program,
                                         const TargetInfo &target,
                                         const MirFunctionInstance &function) {
  return nativeCDeclarationContract(program, target, function) &&
         nativeCScalarFunctionValueContract(program, target,
                                            function.returnType) &&
         std::all_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), scalarBoundaryType);
}

bool nativeCScalarIndirectCallContract(const MirProgram &program,
                                       const TargetInfo &target,
                                       const MirBody &body,
                                       const MirInstruction &instruction) {
  return nativeCIndirectCallContract(program, target, body, instruction) &&
         nativeCScalarFunctionValueContract(program, target,
                                            *instruction.nativeCalleeType);
}

bool nativeCScalarReceiverInputContract(const MirProgram &program,
                                        const TargetInfo &target,
                                        const MirBody &body,
                                        const MirInstruction &instruction) {
  return nativeCScalarFunctionValueContract(program, target,
                                            instruction.info.type) &&
         nativeCReceiverInputContract(program, target, body, instruction);
}

bool nativeCReceiverInputContract(const MirProgram &program,
                                  const TargetInfo &target, const MirBody &body,
                                  const MirInstruction &instruction) {
  if (instruction.tombstone ||
      instruction.kind != MirInstructionKind::CallInput ||
      !instruction.result ||
      instruction.callInputRole != MirCallInputRole::Receiver ||
      instruction.callInputKind != HirCallInputKind::Value ||
      instruction.callInputIndex != 0 || instruction.callSite == 0 ||
      !nativeCFunctionValueContract(program, target, instruction.info.type))
    return false;
  const auto *value = body.findValue(*instruction.result);
  if (!value || value->definition != instruction.id)
    return false;
  const MirInstruction *canonical = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions)
      if (stage.id == instruction.id)
        canonical = &stage;
  if (!canonical || *canonical != instruction)
    return false;
  for (const auto &block : body.blocks)
    for (const auto &call : block.instructions)
      if (call.callSite == instruction.callSite && call.receiver &&
          call.receiver->kind == MirOperandKind::Value &&
          call.receiver->value == *instruction.result &&
          call.receiver->type == instruction.info.type &&
          nativeCIndirectCallContract(program, target, body, call))
        return true;
  return false;
}

bool nativeCFunctionBorrowInputContract(const MirProgram &program,
                                        const TargetInfo &target,
                                        const MirBody &body,
                                        const MirInstruction &instruction) {
  const auto &type = instruction.info.type;
  const bool mutableBorrow =
      instruction.callInputKind == HirCallInputKind::MutableBorrow;
  if (instruction.tombstone ||
      instruction.kind != MirInstructionKind::CallInput ||
      instruction.callInputRole != MirCallInputRole::Argument ||
      (!mutableBorrow &&
       instruction.callInputKind != HirCallInputKind::ReadBorrow) ||
      !instruction.result || !instruction.callSite ||
      instruction.operands.size() != 1 || instruction.rawMemoryAccess ||
      instruction.receiver || instruction.functionTarget ||
      instruction.lambdaTarget || instruction.constructorTarget ||
      instruction.nativeCalleeType || instruction.nativeCallbackAdapter ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      type.kind != SemanticType::Reference || type.arguments.size() != 1 ||
      type.referenceAccess !=
          (mutableBorrow ? AccessMode::Mutable : AccessMode::ReadOnly) ||
      !nativeCFunctionValueContract(program, target, type.arguments.front()))
    return false;
  const auto &input = instruction.operands.front();
  const auto *place = body.findPlace(input.place);
  const auto *value = body.findValue(*instruction.result);
  if (input.type != type || input.value || input.literal || input.loan ||
      input.kind != (mutableBorrow ? MirOperandKind::BorrowWrite
                                   : MirOperandKind::BorrowRead) ||
      !place || place->type != type.arguments.front() ||
      (mutableBorrow && place->access != AccessMode::Mutable) || !value ||
      value->definition != instruction.id || value->info.type != type)
    return false;
  bool canonical = false, consumed = false;
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      if (stage.id == instruction.id)
        canonical = stage == instruction;
      if (stage.tombstone || stage.kind != MirInstructionKind::Call ||
          stage.callSite != instruction.callSite || !stage.functionTarget)
        continue;
      const auto *callee = program.findFunctionInstance(*stage.functionTarget);
      const auto index = instruction.callInputIndex;
      if (!callee || callee->linkage != LanguageLinkage::Gti ||
          callee->definitionKind != MirDefinitionKind::Source ||
          index >= callee->parameterTypes.size() ||
          index >= stage.operands.size() ||
          stage.parameterTypes != callee->parameterTypes ||
          callee->parameterTypes[index] != type)
        continue;
      const auto &argument = stage.operands[index];
      consumed |= argument.kind == MirOperandKind::Value &&
                  argument.value == *instruction.result &&
                  argument.type == type;
    }
  return canonical && consumed;
}

bool nativeCResultInputContract(const MirProgram &program,
                                const TargetInfo &target, const MirBody &body,
                                const MirInstruction &instruction) {
  if (instruction.tombstone ||
      instruction.kind != MirInstructionKind::CallInput ||
      instruction.callInputRole != MirCallInputRole::Argument ||
      (instruction.callInputKind != HirCallInputKind::Value &&
       instruction.callInputKind != HirCallInputKind::MoveValue) ||
      instruction.unsafeOperation != UnsafeOperationKind::ForeignPointerCall ||
      instruction.rawMemoryAccess || !instruction.result ||
      instruction.receiver || instruction.functionTarget ||
      instruction.lambdaTarget || instruction.constructorTarget ||
      instruction.nativeCalleeType || instruction.nativeCallbackAdapter ||
      instruction.intrinsic != IntrinsicKind::None || !instruction.callSite ||
      !instruction.hirValue || instruction.operands.size() != 1 ||
      !nativeCValueContract(program, target, instruction.info.type,
                            NativeCValueRole::Result))
    return false;
  const auto &input = instruction.operands.front();
  const auto *value = input.kind == MirOperandKind::Value
                          ? body.findValue(input.value)
                          : nullptr;
  const auto *result = body.findValue(*instruction.result);
  if (!value || input.type != instruction.info.type ||
      value->info.type != input.type || !result ||
      result->info.type != input.type || result->definition != instruction.id)
    return false;
  bool canonical = false, produced = false, consumed = false;
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      if (stage.id == instruction.id)
        canonical = stage == instruction;
      if (stage.id == value->definition)
        produced = stage.result == input.value &&
                   stage.info.type == input.type &&
                   stage.hirValue == instruction.hirValue &&
                   (nativeCDirectCallContract(program, target, body, stage) ||
                    nativeCIndirectCallContract(program, target, body, stage));
      const auto index = instruction.callInputIndex;
      if (!stage.tombstone &&
          (stage.kind == MirInstructionKind::Call ||
           stage.kind == MirInstructionKind::Construct) &&
          stage.callSite == instruction.callSite &&
          index < stage.parameterTypes.size() &&
          index < stage.operands.size()) {
        const auto &argument = stage.operands[index];
        consumed |= stage.parameterTypes[index] == instruction.info.type &&
                    argument.kind == MirOperandKind::Value &&
                    argument.value == *instruction.result &&
                    argument.type == instruction.info.type;
      }
    }
  // The unsafe effect belongs to the exact producing C call. Staging its
  // already-produced value executes no second foreign access; destination and
  // prepared-parameter cleanup remain separate verified MIR obligations.
  return canonical && produced && consumed;
}

bool nativeCDirectCallContract(const MirProgram &program,
                               const TargetInfo &target, const MirBody &body,
                               const MirInstruction &instruction) {
  const auto *function =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  if (!function || !nativeCDeclarationContract(program, target, *function) ||
      instruction.kind != MirInstructionKind::Call || instruction.tombstone ||
      instruction.intrinsic != IntrinsicKind::None || instruction.receiver ||
      instruction.nativeCalleeType || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget ||
      instruction.nativeCallbackAdapter || instruction.callableInvocation ||
      !instruction.callableArguments.empty() || instruction.tracksCaller ||
      instruction.callSite == 0 || instruction.rawMemoryAccess ||
      (instruction.unsafeOperation != UnsafeOperationKind::ForeignPointerCall &&
       instruction.unsafeOperation != UnsafeOperationKind::None) ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      instruction.parameterTypes != function->parameterTypes ||
      instruction.info.type != function->returnType ||
      instruction.operands.size() != function->parameterTypes.size())
    return false;
  bool selected = false;
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.id == instruction.id)
        selected = candidate.kind == MirInstructionKind::Call &&
                   candidate.callSite == instruction.callSite &&
                   candidate.functionTarget == instruction.functionTarget;
  if (!selected)
    return false;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const auto &operand = instruction.operands[index];
    const auto *value = operand.kind == MirOperandKind::Value
                            ? body.findValue(operand.value)
                            : nullptr;
    const auto &type = function->parameterTypes[index];
    if (!value || operand.type != type || value->info.type != type)
      return false;
    bool staged = false;
    for (const auto &block : body.blocks)
      for (const auto &input : block.instructions)
        if (input.id == value->definition)
          staged = input.kind == MirInstructionKind::CallInput &&
                   input.callSite == instruction.callSite &&
                   input.callInputRole == MirCallInputRole::Argument &&
                   input.callInputIndex == index && input.info.type == type;
    if (!staged)
      return false;
  }
  return true;
}

bool nativeCIndirectCallContract(const MirProgram &program,
                                 const TargetInfo &target, const MirBody &body,
                                 const MirInstruction &instruction) {
  return !instruction.tombstone &&
         isMirNativeIndirectCall(program, body, instruction) &&
         nativeCValueContract(program, target, *instruction.nativeCalleeType,
                              NativeCValueRole::Argument);
}

bool nativeCCallbackContract(const MirProgram &program,
                             const TargetInfo &target,
                             const MirNativeCallbackAdapter &adapter) {
  const auto *canonical = program.findNativeCallbackAdapter(adapter.id);
  const auto *function = program.findFunctionInstance(adapter.target);
  if (!canonical || *canonical != adapter || !function || function->owner ||
      function->requiresUnsafe || function->tracksCaller ||
      function->definitionKind != MirDefinitionKind::Source ||
      function->linkage != LanguageLinkage::Gti ||
      !function->typeArguments.empty() || !function->valueArguments.empty() ||
      !nativeCFunctionValueContract(program, target, adapter.type) ||
      adapter.targetMayRaiseDefinedFailure !=
          function->mayRaiseDefinedFailure ||
      adapter.failurePolicy !=
          MirNativeCallbackFailurePolicy::TerminateInvocation ||
      !adapter.catchesNativeExceptions)
    return false;
  const auto parameters = adapter.type.nativeFunctionParameterTypes();
  if (function->returnType != *adapter.type.nativeFunctionReturnType() ||
      !std::equal(function->parameterTypes.begin(),
                  function->parameterTypes.end(), parameters.begin(),
                  parameters.end()))
    return false;
  const auto attachment =
      program.executionProfile() == ExecutionProfile::Concurrent
          ? (function->threadConfinedCallback
                 ? CThreadCallbackKind::ThreadConfined
                 : adapter.type.cThreadCallbackKind())
          : CThreadCallbackKind::None;
  return adapter.cThreadCallback == attachment &&
         (program.executionProfile() != ExecutionProfile::Concurrent ||
          (attachment != CThreadCallbackKind::None &&
           target.runtimeCapabilities.threads));
}

bool nativeCCallbackStageContract(const MirProgram &program,
                                  const TargetInfo &target, const MirBody &body,
                                  const MirInstruction &instruction) {
  const auto *adapter = instruction.nativeCallbackAdapter
                            ? program.findNativeCallbackAdapter(
                                  *instruction.nativeCallbackAdapter)
                            : nullptr;
  if (!adapter || !nativeCCallbackContract(program, target, *adapter) ||
      instruction.tombstone ||
      instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::NativeCallback ||
      !instruction.result || instruction.info.type != adapter->type ||
      !instruction.operands.empty() || instruction.receiver ||
      instruction.destination || instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.nativeCalleeType || instruction.threadTaskAdapter ||
      instruction.callableInvocation ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget || instruction.tracksCaller)
    return false;
  const auto *value = body.findValue(*instruction.result);
  if (!value || value->definition != instruction.id ||
      value->info.type != adapter->type)
    return false;
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.id == instruction.id)
        return candidate == instruction;
  return false;
}

bool nativeCStorageContract(const MirProgram &program, const TargetInfo &target,
                            const ForeignStorageInfo &storage) {
  const auto *canonical = program.findForeignStorage(storage.symbol);
  return canonical && *canonical == storage &&
         !storage.externalSymbol.empty() &&
         storage.externalSymbol.find('\0') == std::string::npos &&
         storage.type.kind != SemanticType::Array &&
         nativeCValueContract(program, target, storage.type,
                              NativeCValueRole::Storage);
}

bool nativeCStorageImportContract(const MirProgram &program,
                                  const TargetInfo &target,
                                  const ForeignStorageInfo &storage) {
  return nativeCStorageContract(program, target, storage);
}

namespace {

// A representation query over verified places. Traversing an untagged union
// does not prove native initialization or an active member; source unsafe
// permission remains a separate requirement of the accessing instruction. The
// result reports whether the final selection is a union member.
std::optional<bool> nativeCStorageUnionProjection(const MirProgram &program,
                                                  const TargetInfo &target,
                                                  const MirBody &body,
                                                  const MirPlace &place) {
  const auto *selected = body.findPlace(place.id);
  const auto *storage = program.findForeignStorage(place.symbol);
  if (!selected || *selected != place || !place.foreignStorage ||
      place.root != MirPlaceRootKind::Symbol || place.capture != 0 ||
      place.binding != 0 || !place.initiallyAvailable || !storage ||
      !nativeCStorageImportContract(program, target, *storage))
    return std::nullopt;
  const bool root = std::any_of(
      body.places.begin(), body.places.end(), [&](const auto &root) {
        return root.foreignStorage && root.root == MirPlaceRootKind::Symbol &&
               root.symbol == storage->symbol && root.capture == 0 &&
               root.projections.empty() && root.type == storage->type;
      });
  if (!root)
    return std::nullopt;
  SemanticType type = storage->type;
  auto access = storage->access;
  bool unionMember = false;
  for (const auto &projection : place.projections) {
    if (projection.kind == MirProjectionKind::Field) {
      const MirClassInstance *record = nullptr;
      for (const auto &candidate : program.classInstances())
        if (candidate.type == type) {
          if (record)
            return std::nullopt;
          record = &candidate;
        }
      if (!record || !record->cAbiRecord || !record->cAbiLayout)
        return std::nullopt;
      const MirClassFieldInfo *field = nullptr;
      for (std::size_t index = 0; index < record->declaredFields.size();
           ++index)
        if (record->declaredFields[index].symbol == projection.field) {
          if (field || index >= record->cAbiLayout->fields.size())
            return std::nullopt;
          field = &record->declaredFields[index];
          const auto &layout = record->cAbiLayout->fields[index];
          if (layout.field != field->symbol || layout.type != field->type)
            return std::nullopt;
        }
      if (!field)
        return std::nullopt;
      // MIR labels an access a union-member read exactly when its final
      // selection is a union member; a field of a selected member, such as
      // `storage.member.field`, is an ordinary foreign-storage access.
      unionMember = record->unionLayout.has_value();
      if (field->access == AccessMode::ReadOnly)
        access = AccessMode::ReadOnly;
      type = field->type;
    } else if (projection.kind == MirProjectionKind::Index) {
      if (type.kind != SemanticType::Array || type.arguments.size() != 1 ||
          type.arrayLength == 0 || type.arrayLengthParameterId ||
          (projection.constantIndex &&
           *projection.constantIndex >= type.arrayLength))
        return std::nullopt;
      // The MIR verifier owns the bounds-check edge. This query only binds
      // the already evaluated index to this body's exact integer value.
      if (!projection.constantIndex) {
        const auto *index = body.findValue(projection.index);
        if (!index || !constantIntegerDomain(index->info.type))
          return std::nullopt;
      }
      auto element = type.arguments.front();
      type = std::move(element);
      unionMember = false;
    } else {
      return std::nullopt;
    }
  }
  if (type != place.type ||
      (place.access == AccessMode::Mutable && access != AccessMode::Mutable))
    return std::nullopt;
  return unionMember;
}

bool nativeCStorageAccess(const MirProgram &program, const TargetInfo &target,
                          const MirBody &body,
                          const MirInstruction &instruction,
                          UnsafeOperationKind permission) {
  const bool address = instruction.kind == MirInstructionKind::Compute &&
                       instruction.operation == MirOperation::AddressOf &&
                       !instruction.rawMemoryAccess;
  // A by-value argument copied out of C storage reads the object exactly as a
  // load does; its prepared temporary is an ordinary local destination.
  const bool copiedArgument =
      instruction.kind == MirInstructionKind::CallInput &&
      instruction.callInputRole == MirCallInputRole::Argument &&
      instruction.callInputKind == HirCallInputKind::CopyValue &&
      instruction.operands.size() == 1 &&
      instruction.operands.front().kind == MirOperandKind::Copy;
  if (instruction.tombstone || instruction.unsafeOperation != permission ||
      (!instruction.rawMemoryAccess && !address) || instruction.receiver ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.nativeCalleeType || instruction.nativeCallbackAdapter ||
      instruction.threadTaskAdapter || instruction.callableInvocation ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget ||
      (instruction.kind != MirInstructionKind::Load &&
       instruction.kind != MirInstructionKind::Assign &&
       instruction.kind != MirInstructionKind::Modify && !address &&
       !copiedArgument))
    return false;
  bool selected = false;
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.id == instruction.id)
        selected = candidate == instruction;
  if (!selected)
    return false;
  if (instruction.kind == MirInstructionKind::Assign ||
      instruction.kind == MirInstructionKind::Modify) {
    const auto *destination = instruction.destination
                                  ? body.findPlace(*instruction.destination)
                                  : nullptr;
    if (!destination || destination->access != AccessMode::Mutable)
      return false;
  }
  if (address) {
    if (!instruction.result || instruction.destination ||
        instruction.operands.size() != 1 ||
        instruction.operands.front().kind != MirOperandKind::Address ||
        instruction.info.type.kind != SemanticType::RawPointer ||
        instruction.info.type.arguments.size() != 1)
      return false;
    const auto &operand = instruction.operands.front();
    const auto *place = body.findPlace(operand.place);
    const auto *result = body.findValue(*instruction.result);
    const auto projection =
        place ? nativeCStorageUnionProjection(program, target, body, *place)
              : std::nullopt;
    return projection && result && result->definition == instruction.id &&
           result->info.type == instruction.info.type &&
           operand.type == place->type &&
           instruction.info.type.arguments.front() == place->type &&
           (instruction.info.type.pointerAccess != AccessMode::Mutable ||
            place->access == AccessMode::Mutable) &&
           *projection == (permission == UnsafeOperationKind::UnionMember);
  }
  bool foreign = false, unionMember = false;
  const auto place = [&](MirPlaceId id) {
    const auto *value = body.findPlace(id);
    if (!value || value->capture != 0)
      return false;
    if (value->foreignStorage) {
      foreign = true;
      const auto projection =
          nativeCStorageUnionProjection(program, target, body, *value);
      if (!projection)
        return false;
      unionMember |= *projection;
      return true;
    }
    // A valid foreign object never authorizes a second raw access. Ordinary
    // local sources/destinations retain their independently verified policy.
    return std::all_of(value->projections.begin(), value->projections.end(),
                       [](const auto &projection) {
                         return projection.kind == MirProjectionKind::Field ||
                                projection.kind == MirProjectionKind::Index;
                       });
  };
  if (instruction.destination && !place(*instruction.destination))
    return false;
  for (const auto &operand : instruction.operands) {
    if (operand.place && !place(operand.place))
      return false;
    if (operand.type == SemanticType::NullPtr &&
        instruction.kind == MirInstructionKind::Assign &&
        instruction.operands.size() == 1 && instruction.destination) {
      const auto *destination = body.findPlace(*instruction.destination);
      const auto *value = operand.kind == MirOperandKind::Value
                              ? body.findValue(operand.value)
                              : nullptr;
      if (destination && value && value->info.type == operand.type &&
          (destination->type.kind == SemanticType::RawPointer ||
           destination->type.kind == SemanticType::NativeFunction))
        continue;
      return false;
    }
    if (!nativeCValueContract(program, target, operand.type,
                              NativeCValueRole::Storage))
      return false;
  }
  return foreign &&
         unionMember == (permission == UnsafeOperationKind::UnionMember);
}

} // namespace

bool nativeCStoragePlaceContract(const MirProgram &program,
                                 const TargetInfo &target, const MirBody &body,
                                 const MirPlace &place) {
  return nativeCStorageUnionProjection(program, target, body, place)
      .has_value();
}

bool nativeCStorageAccessContract(const MirProgram &program,
                                  const TargetInfo &target, const MirBody &body,
                                  const MirInstruction &instruction) {
  return nativeCStorageAccess(program, target, body, instruction,
                              UnsafeOperationKind::ForeignStorage);
}

bool nativeCStorageUnionAccessContract(const MirProgram &program,
                                       const TargetInfo &target,
                                       const MirBody &body,
                                       const MirInstruction &instruction) {
  return nativeCStorageAccess(program, target, body, instruction,
                              UnsafeOperationKind::UnionMember);
}

namespace {

[[noreturn]] void unsupported() {
  throw BackendUnsupportedError(
      "LLVM native C ABI requires a verified natural passive C value on "
      "Darwin AArch64 or Linux x86_64 System V");
}

bool integerField(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Bool:
  case SemanticType::Int8:
  case SemanticType::UInt8:
  case SemanticType::Int16:
  case SemanticType::UInt16:
  case SemanticType::Int32:
  case SemanticType::UInt32:
  case SemanticType::Int64:
  case SemanticType::UInt64:
  case SemanticType::RawPointer:
  case SemanticType::CString:
  case SemanticType::NativeFunction:
    return true;
  default:
    return false;
  }
}

bool darwinTarget(Session &session) {
  const llvm::Triple triple(session.module().getTargetTriple());
  const auto &target = session.program().target();
  const bool darwin = triple.getArch() == llvm::Triple::aarch64 &&
                      triple.isOSDarwin() && triple.isOSBinFormatMachO() &&
                      target.os == "macos" && target.arch == "arm64";
  const bool sysv = triple.getArch() == llvm::Triple::x86_64 &&
                    triple.isOSLinux() && triple.isOSBinFormatELF() &&
                    target.os == "linux" && target.arch == "x86_64";
  if ((!darwin && !sysv) || !session.dataLayout().isLittleEndian() ||
      session.dataLayout().getPointerSize() != 8)
    unsupported();
  // A caller may still have an unterminated IR block. Session verifies the
  // completed module; classification only checks immutable target/layout facts.
  verifyTargetLayout(target, session.dataLayout(), session.context());
  return darwin;
}

struct Homogeneous {
  llvm::Type *base = nullptr;
  unsigned count = 0;
};

Homogeneous inspect(Session &session, const SemanticType &type,
                    unsigned depth = 0) {
  if (depth > 256)
    unsupported();
  if (integerField(type))
    return {};
  if (type.kind == SemanticType::Float || type.kind == SemanticType::Double)
    return {session.layouts().get(type).storage, 1};
  if (type.kind == SemanticType::StringView)
    return {};
  if (type.kind == SemanticType::Array) {
    if (type.arguments.size() != 1 || !type.arrayLength ||
        type.arrayLengthParameterId)
      unsupported();
    const auto element = inspect(session, type.arguments[0], depth + 1);
    if (!element.base || type.arrayLength > 4 / element.count)
      return {};
    return {element.base,
            static_cast<unsigned>(type.arrayLength * element.count)};
  }
  if (type.kind != SemanticType::Class)
    unsupported();
  const auto &record = session.layouts().instance(type);
  if (!record.cAbiRecord || !record.cAbiLayout || record.cOpaqueHandle ||
      !record.bases.empty() || !record.structuralBases.empty() ||
      record.polymorphic || record.requiresActiveCleanup ||
      record.requiresActiveDropState || record.declaredFields.empty())
    unsupported();
  const auto storage = session.layouts().get(type);
  if (!storage.size || storage.alignment > 8)
    unsupported();
  Homogeneous hfa;
  bool homogeneous = true;
  for (const auto &field : record.declaredFields) {
    const auto next = inspect(session, field.type, depth + 1);
    if (!next.base || (hfa.base && hfa.base != next.base))
      homogeneous = false;
    if (!hfa.base)
      hfa.base = next.base;
    hfa.count = record.unionLayout ? std::max(hfa.count, next.count)
                                   : hfa.count + next.count;
    if (hfa.count > 4)
      homogeneous = false;
  }
  if (!homogeneous || !hfa.base || !hfa.count ||
      session.dataLayout().getTypeAllocSize(hfa.base).getFixedValue() *
              hfa.count !=
          storage.size)
    return {};
  return hfa;
}

void leaves(
    Session &session, const SemanticType &type, std::uint64_t at,
    const std::function<void(const SemanticType &, std::uint64_t)> &visit) {
  if (type.kind == SemanticType::Array) {
    const auto stride = session.layouts().get(type).stride;
    for (std::uint64_t i = 0; i < type.arrayLength; ++i)
      leaves(session, type.arguments[0], at + stride * i, visit);
  } else if (type.kind == SemanticType::Class) {
    const auto &record = session.layouts().instance(type);
    const auto storage = session.layouts().get(type);
    for (std::size_t i = 0; i < record.declaredFields.size(); ++i)
      leaves(session, record.declaredFields[i].type,
             at + storage.fieldOffsets.at(i), visit);
  } else if (type.kind == SemanticType::StringView) {
    visit(SemanticType::rawPointerTo(SemanticType::Void), at);
    visit(SemanticType::UInt64, at + 8);
  } else {
    visit(type, at);
  }
}

llvm::Type *joined(llvm::LLVMContext &context,
                   const std::vector<NativeCAbiPiece> &pieces) {
  if (pieces.size() == 1)
    return pieces.front().type;
  std::vector<llvm::Type *> types;
  for (const auto &piece : pieces)
    types.push_back(piece.type);
  return llvm::StructType::get(context, types);
}

NativeCRecordAbi aggregate(Session &session, const SemanticType &type,
                           NativeCRegisterBudget available) {
  const bool darwin = darwinTarget(session);
  if (available.integers > 6 || available.vectors > 8)
    unsupported();
  const auto hfa = inspect(session, type);
  NativeCRecordAbi plan;
  plan.storage = session.layouts().get(type);
  plan.remaining = available;
  auto &context = session.context();
  if (darwin && hfa.base) {
    // AAPCS64 6.8/6.9: flatten HFAs only after the complete aggregate layout.
    plan.argument.coercion = llvm::ArrayType::get(hfa.base, hfa.count);
    plan.result.coercion = plan.argument.coercion;
  } else if (plan.storage.size > 16) {
    plan.argument.coercion = session.layouts().pointer();
    plan.argument.indirect = true;
    plan.argument.byValue = !darwin;
    plan.argument.stackAlignment =
        darwin ? plan.storage.alignment
               : std::max<std::uint64_t>(8, plan.storage.alignment);
    plan.result.coercion = llvm::Type::getVoidTy(context);
    plan.result.indirect = true;
    return plan;
  } else if (darwin) {
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    plan.argument.coercion =
        plan.storage.size <= 8 ? i64 : llvm::ArrayType::get(i64, 2);
    plan.result.coercion =
        plan.storage.size <= 8
            ? static_cast<llvm::Type *>(
                  llvm::IntegerType::get(context, plan.storage.size * 8))
            : llvm::ArrayType::get(i64, 2);
  } else {
    // AMD64 psABI 3.2.3: INTEGER dominates SSE, including overlapping union
    // alternatives. Track occupied float lanes, not number of source fields.
    struct Eightbyte {
      bool integer = false;
      bool doubleField = false;
      std::array<bool, 2> floats{};
      std::uint64_t end = 0;
    };
    std::array<Eightbyte, 2> chunks{};
    leaves(session, type, 0, [&](const SemanticType &leaf, std::uint64_t at) {
      auto &chunk = chunks.at(at / 8);
      const auto layout = session.layouts().get(leaf);
      if (at % layout.alignment != 0 || at % 8 + layout.size > 8)
        unsupported();
      chunk.integer |= integerField(leaf);
      chunk.doubleField |= leaf.kind == SemanticType::Double;
      if (leaf.kind == SemanticType::Float)
        chunk.floats[(at % 8) / 4] = true;
      chunk.end = std::max(chunk.end, at + layout.size);
    });
    unsigned integers = 0, vectors = 0;
    for (std::uint64_t index = 0; index < (plan.storage.size + 7) / 8;
         ++index) {
      const auto &chunk = chunks[index];
      llvm::Type *carrier = nullptr;
      std::uint64_t at = index * 8;
      if (chunk.integer) {
        ++integers;
        carrier = llvm::IntegerType::get(context, (chunk.end - at) * 8);
      } else {
        ++vectors;
        if (chunk.doubleField)
          carrier = llvm::Type::getDoubleTy(context);
        else if (chunk.floats[0] && chunk.floats[1])
          carrier =
              llvm::FixedVectorType::get(llvm::Type::getFloatTy(context), 2);
        else if (chunk.floats[0] || chunk.floats[1]) {
          carrier = llvm::Type::getFloatTy(context);
          if (!chunk.floats[0])
            at += 4;
        } else
          unsupported();
      }
      plan.result.pieces.push_back({carrier, at});
    }
    plan.result.coercion = joined(context, plan.result.pieces);
    if (integers > available.integers || vectors > available.vectors) {
      plan.argument.coercion = session.layouts().pointer();
      plan.argument.indirect = true;
      plan.argument.byValue = true;
      plan.argument.stackAlignment =
          std::max<std::uint64_t>(8, plan.storage.alignment);
    } else {
      plan.argument = plan.result;
      plan.argument.expand = true;
      plan.remaining.integers -= integers;
      plan.remaining.vectors -= vectors;
    }
    return plan;
  }
  plan.argument.pieces = {{plan.argument.coercion, 0}};
  plan.result.pieces = {{plan.result.coercion, 0}};
  return plan;
}

AbiExtension extension(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Bool:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
    return AbiExtension::Unsigned;
  case SemanticType::Int8:
  case SemanticType::Int16:
    return AbiExtension::Signed;
  default:
    return AbiExtension::None;
  }
}

llvm::AllocaInst *temporary(llvm::IRBuilder<> &ir, llvm::Type *type,
                            llvm::Align alignment = llvm::Align(1)) {
  auto &entry = ir.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> allocate(&entry, entry.begin());
  auto *slot = allocate.CreateAlloca(type);
  slot->setAlignment(alignment);
  return slot;
}

llvm::Value *addressAt(llvm::IRBuilder<> &ir, llvm::Value *address,
                       std::uint64_t at) {
  return ir.CreateGEP(ir.getInt8Ty(), address, ir.getInt64(at));
}

llvm::Value *pack(Session &session, llvm::IRBuilder<> &ir, llvm::Value *storage,
                  const NativeCAbiPiece &piece, std::uint64_t size) {
  auto *slot = temporary(ir, piece.type,
                         session.dataLayout().getABITypeAlign(piece.type));
  ir.CreateStore(llvm::Constant::getNullValue(piece.type), slot);
  const auto bytes =
      session.dataLayout().getTypeStoreSize(piece.type).getFixedValue();
  ir.CreateMemCpy(slot, llvm::MaybeAlign(1),
                  addressAt(ir, storage, piece.offset), llvm::MaybeAlign(1),
                  std::min(bytes, size - piece.offset));
  return ir.CreateLoad(piece.type, slot);
}

void unpack(Session &session, llvm::IRBuilder<> &ir, llvm::Value *storage,
            llvm::Value *value, std::uint64_t at, std::uint64_t size) {
  auto *slot =
      temporary(ir, value->getType(),
                session.dataLayout().getABITypeAlign(value->getType()));
  ir.CreateStore(value, slot);
  const auto bytes =
      session.dataLayout().getTypeStoreSize(value->getType()).getFixedValue();
  ir.CreateMemCpy(addressAt(ir, storage, at), llvm::MaybeAlign(1), slot,
                  llvm::MaybeAlign(1), std::min(bytes, size - at));
}

} // namespace

NativeCRecordAbi classifyNativeCRecord(Session &session,
                                       const SemanticType &type,
                                       NativeCRegisterBudget available) {
  if (type.kind != SemanticType::Class)
    unsupported();
  return aggregate(session, type, available);
}

NativeCFunctionAbi
planNativeCFunction(Session &session, const SemanticType &result,
                    std::span<const SemanticType> parameters) {
  const bool darwin = darwinTarget(session);
  if (!nativeCValueContract(session.program().mir(), session.program().target(),
                            result, NativeCValueRole::Result) ||
      !std::all_of(
          parameters.begin(), parameters.end(), [&](const auto &parameter) {
            return nativeCValueContract(session.program().mir(),
                                        session.program().target(), parameter,
                                        NativeCValueRole::Argument);
          }))
    unsupported();
  NativeCFunctionAbi plan;
  NativeCRegisterBudget available;
  std::vector<llvm::Type *> types;
  const auto describe = [&](const SemanticType &type, bool output) {
    NativeCValueAbi value;
    value.type = type;
    value.storage = session.layouts().get(type);
    value.aggregate = type.kind == SemanticType::Class ||
                      type.kind == SemanticType::StringView;
    if (value.aggregate) {
      if (output && type.kind == SemanticType::StringView)
        unsupported();
      const auto record = aggregate(session, type, available);
      value.transport = output ? record.result : record.argument;
      if (!output)
        available = record.remaining;
    } else {
      if (!integerField(type) && type.kind != SemanticType::Float &&
          type.kind != SemanticType::Double &&
          !(output && type.kind == SemanticType::Void))
        unsupported();
      value.transport.coercion = value.storage.value;
      if (!output && !darwin) {
        auto &count = type.kind == SemanticType::Float ||
                              type.kind == SemanticType::Double
                          ? available.vectors
                          : available.integers;
        if (count)
          --count;
      }
    }
    return value;
  };
  plan.result = describe(result, true);
  if (plan.result.transport.indirect) {
    plan.signature.resultDestination = 0;
    types.push_back(session.layouts().pointer());
    plan.signature.parameters.push_back(
        {AbiChannel::ResultDestination, 0, true});
    plan.signature.nativeMemory.push_back({0, plan.result.storage.storage,
                                           plan.result.storage.alignment, true,
                                           false});
    if (!darwin)
      --available.integers;
  } else if (!plan.result.aggregate) {
    plan.signature.resultExtension = extension(result);
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    auto value = describe(parameters[index], false);
    value.firstParameter = types.size();
    if (value.transport.expand) {
      for (const auto &piece : value.transport.pieces) {
        types.push_back(piece.type);
        plan.signature.parameters.push_back({AbiChannel::Argument, index});
      }
    } else {
      types.push_back(value.transport.coercion);
      plan.signature.parameters.push_back(
          {AbiChannel::Argument, index, value.transport.indirect,
           value.aggregate ? AbiExtension::None : extension(value.type)});
    }
    if (value.transport.indirect)
      plan.signature.nativeMemory.push_back(
          {value.firstParameter, value.storage.storage,
           value.transport.stackAlignment, false, value.transport.byValue});
    plan.arguments.push_back(std::move(value));
  }
  plan.signature.type =
      llvm::FunctionType::get(plan.result.transport.coercion, types, false);
  return plan;
}

void verifyNativeCImport(const llvm::Function &function,
                         const FunctionAbi &plan) {
  if (function.getFunctionType() != plan.type || !function.isDeclaration() ||
      !function.hasExternalLinkage() || function.getAddressSpace() != 0 ||
      function.getCallingConv() != llvm::CallingConv::C)
    throw std::logic_error("conflicting LLVM native C import: " +
                           function.getName().str());
  // Derive attributes from the same implementation used by calls and new
  // declarations, so extension and memory-transport rules cannot diverge.
  const std::unique_ptr<llvm::Function> expected(llvm::Function::Create(
      plan.type, llvm::GlobalValue::ExternalLinkage, ""));
  plan.applyTo(*expected);
  if (function.getAttributes() != expected->getAttributes())
    throw std::logic_error("conflicting LLVM native C import attributes: " +
                           function.getName().str());
}

llvm::Value *emitNativeCCall(Session &session, llvm::IRBuilder<> &ir,
                             const NativeCFunctionAbi &plan,
                             llvm::Value *callee,
                             std::span<llvm::Value *const> arguments,
                             llvm::Value *resultDestination) {
  if (arguments.size() != plan.arguments.size())
    throw std::logic_error(
        "native C call disagrees with its exact argument plan");
  if (plan.result.aggregate && !resultDestination)
    resultDestination = temporary(ir, plan.result.storage.storage,
                                  llvm::Align(plan.result.storage.alignment));
  std::vector<llvm::Value *> lowered;
  if (plan.result.transport.indirect)
    lowered.push_back(resultDestination);
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &value = plan.arguments[index];
    auto *input = arguments[index];
    if (!value.aggregate) {
      lowered.push_back(input);
      continue;
    }
    if (value.type.kind == SemanticType::StringView) {
      auto *slot = temporary(ir, value.storage.storage,
                             llvm::Align(value.storage.alignment));
      ir.CreateStore(input, slot);
      input = slot;
    }
    if (value.transport.indirect) {
      // An indirect C value is still a copy, not a borrow of the GTI source.
      const auto alignment =
          std::max(value.storage.alignment, value.transport.stackAlignment);
      auto *copy = temporary(ir, value.storage.storage, llvm::Align(alignment));
      ir.CreateMemCpy(copy, llvm::MaybeAlign(alignment), input,
                      llvm::MaybeAlign(1), value.storage.size);
      lowered.push_back(copy);
    } else {
      for (const auto &piece : value.transport.pieces)
        lowered.push_back(pack(session, ir, input, piece, value.storage.size));
    }
  }
  auto *call = ir.CreateCall(plan.signature.type, callee, lowered);
  plan.signature.applyTo(*call);
  if (!plan.result.aggregate)
    return call->getType()->isVoidTy() ? nullptr : call;
  if (!plan.result.transport.indirect) {
    const auto &pieces = plan.result.transport.pieces;
    if (pieces.size() == 1)
      unpack(session, ir, resultDestination, call, pieces[0].offset,
             plan.result.storage.size);
    else
      for (unsigned i = 0; i < pieces.size(); ++i)
        unpack(session, ir, resultDestination, ir.CreateExtractValue(call, i),
               pieces[i].offset, plan.result.storage.size);
  }
  return resultDestination;
}

llvm::Value *emitNativeCDirectCall(Session &session, llvm::IRBuilder<> &ir,
                                   const MirBody &body,
                                   const MirInstruction &instruction,
                                   std::span<llvm::Value *const> arguments,
                                   llvm::Value *resultDestination) {
  if (!nativeCDirectCallContract(session.program().mir(),
                                 session.program().target(), body, instruction))
    throw BackendUnsupportedError("native C call requires its exact sealed "
                                  "declaration and staged signature");
  const auto &target = *session.program().mir().findFunctionInstance(
      *instruction.functionTarget);
  const auto plan =
      planNativeCFunction(session, target.returnType, target.parameterTypes);
  return emitNativeCCall(session, ir, plan,
                         session.abi().declareFunction(target.id), arguments,
                         resultDestination);
}

llvm::Value *emitNativeCIndirectCall(Session &session, llvm::IRBuilder<> &ir,
                                     const MirBody &body,
                                     const MirInstruction &instruction,
                                     llvm::Value *callee,
                                     std::span<llvm::Value *const> arguments,
                                     llvm::Value *resultDestination) {
  if (!nativeCIndirectCallContract(session.program().mir(),
                                   session.program().target(), body,
                                   instruction))
    throw BackendUnsupportedError(
        "native C indirect call requires its exact staged native signature");
  if (!callee || callee->getType() != session.layouts().pointer())
    throw std::logic_error("native C indirect callee is not a native address");
  const auto &signature = *instruction.nativeCalleeType;
  const auto plan =
      planNativeCFunction(session, *signature.nativeFunctionReturnType(),
                          signature.nativeFunctionParameterTypes());
  return emitNativeCCall(session, ir, plan, callee, arguments,
                         resultDestination);
}

std::vector<llvm::Value *>
decodeNativeCArguments(Session &session, llvm::IRBuilder<> &ir,
                       const NativeCFunctionAbi &plan,
                       llvm::Function &function) {
  if (function.getFunctionType() != plan.signature.type)
    throw std::logic_error("native C entry disagrees with its argument plan");
  std::vector<llvm::Value *> values;
  for (const auto &argument : plan.arguments) {
    auto *input = function.getArg(argument.firstParameter);
    if (!argument.aggregate) {
      values.push_back(input);
      continue;
    }
    auto *storage = temporary(ir, argument.storage.storage,
                              llvm::Align(argument.storage.alignment));
    if (argument.transport.indirect) {
      ir.CreateMemCpy(storage, llvm::MaybeAlign(argument.storage.alignment),
                      input,
                      llvm::MaybeAlign(argument.transport.stackAlignment),
                      argument.storage.size);
    } else {
      ir.CreateStore(llvm::Constant::getNullValue(argument.storage.storage),
                     storage);
      for (unsigned index = 0; index < argument.transport.pieces.size();
           ++index)
        unpack(session, ir, storage,
               function.getArg(argument.firstParameter + index),
               argument.transport.pieces[index].offset, argument.storage.size);
    }
    llvm::Value *bodyValue = storage;
    if (argument.type.kind == SemanticType::StringView)
      bodyValue = ir.CreateLoad(argument.storage.value, storage);
    values.push_back(bodyValue);
  }
  return values;
}

void emitNativeCReturn(Session &session, llvm::IRBuilder<> &ir,
                       const NativeCFunctionAbi &plan, llvm::Function &function,
                       llvm::Value *value) {
  if (function.getFunctionType() != plan.signature.type)
    throw std::logic_error("native C entry disagrees with its return plan");
  if (!plan.result.aggregate) {
    if (plan.signature.type->getReturnType()->isVoidTy())
      ir.CreateRetVoid();
    else if (value)
      ir.CreateRet(value);
    else
      throw std::logic_error("native C return lost its scalar result");
    return;
  }
  if (!value)
    throw std::logic_error("native C return lost its aggregate storage");
  if (plan.result.transport.indirect) {
    ir.CreateMemCpy(function.getArg(*plan.signature.resultDestination),
                    llvm::MaybeAlign(plan.result.storage.alignment), value,
                    llvm::MaybeAlign(plan.result.storage.alignment),
                    plan.result.storage.size);
    ir.CreateRetVoid();
    return;
  }
  const auto &pieces = plan.result.transport.pieces;
  if (pieces.size() == 1) {
    ir.CreateRet(
        pack(session, ir, value, pieces.front(), plan.result.storage.size));
    return;
  }
  llvm::Value *result = llvm::PoisonValue::get(plan.result.transport.coercion);
  for (unsigned index = 0; index < pieces.size(); ++index)
    result = ir.CreateInsertValue(
        result,
        pack(session, ir, value, pieces[index], plan.result.storage.size),
        index);
  ir.CreateRet(result);
}

void declareNativeCStorage(
    Session &session, std::map<SymbolId, llvm::GlobalVariable *> &globals) {
  for (const auto &foreign :
       session.program().mir().foreignStorageDeclarations()) {
    if (!nativeCStorageContract(session.program().mir(),
                                session.program().target(), foreign) ||
        globals.contains(foreign.symbol) ||
        session.module().getNamedValue(foreign.externalSymbol))
      throw std::logic_error("foreign C storage lost its unique sealed symbol");
    if (foreign.type.kind == SemanticType::Class)
      static_cast<void>(inspect(session, foreign.type));
    else if (!integerField(foreign.type) &&
             foreign.type.kind != SemanticType::Float &&
             foreign.type.kind != SemanticType::Double)
      unsupported();
    const auto layout = session.layouts().get(foreign.type);
    // Even a read-only source declaration does not prove native value
    // stability.
    auto *storage = new llvm::GlobalVariable(
        session.module(), layout.storage, false,
        llvm::GlobalValue::ExternalLinkage, nullptr, foreign.externalSymbol);
    storage->setAlignment(llvm::Align(layout.alignment));
    globals.emplace(foreign.symbol, storage);
  }
}

namespace {

std::string callbackName(MirNativeCallbackAdapterId identity) {
  return "__gti_native_c_callback_" + std::to_string(identity);
}

llvm::Function *nativeService(Session &session, const char *name,
                              llvm::Type *result,
                              llvm::ArrayRef<llvm::Type *> parameters) {
  auto *type = llvm::FunctionType::get(result, parameters, false);
  if (auto *previous = session.module().getNamedValue(name)) {
    auto *function = llvm::dyn_cast<llvm::Function>(previous);
    if (!function || function->getFunctionType() != type)
      throw std::logic_error("native callback runtime signature conflict");
    return function;
  }
  return llvm::Function::Create(type, llvm::Function::ExternalLinkage, name,
                                session.module());
}

// A foreign exception at a callback boundary or a confined callback bound on
// a second thread ends the process through std::terminate, as the generated
// C++ adapters do, so a host terminate handler observes it.
void terminateNativeBoundary(Session &session, llvm::IRBuilder<> &ir) {
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::ContractTerminate));
  ir.CreateUnreachable();
}

llvm::GlobalVariable *callbackOwner(Session &session,
                                    MirNativeCallbackAdapterId identity) {
  const auto name = callbackName(identity) + ".owner";
  if (auto *previous = session.module().getNamedGlobal(name))
    return previous;
  auto *type = llvm::Type::getInt64Ty(session.context());
  auto *owner = new llvm::GlobalVariable(session.module(), type, false,
                                         llvm::GlobalValue::InternalLinkage,
                                         llvm::ConstantInt::get(type, 0), name);
  owner->setAlignment(llvm::Align(8));
  return owner;
}

void callbackAttachment(Session &session, llvm::IRBuilder<> &ir,
                        const MirNativeCallbackAdapter &adapter,
                        llvm::Function &wrapper, bool entering) {
  const char *symbol = nullptr;
  llvm::Value *argument = &wrapper;
  switch (adapter.cThreadCallback) {
  case CThreadCallbackKind::None:
    return;
  case CThreadCallbackKind::ThreadEntry:
    symbol = entering ? "gti_rt_cthread_enter_v1" : "gti_rt_cthread_leave_v1";
    break;
  case CThreadCallbackKind::OnceInitializer:
    symbol = entering ? "gti_rt_once_enter_v1" : "gti_rt_once_leave_v1";
    break;
  case CThreadCallbackKind::ThreadConfined:
    if (!entering) {
      ir.CreateCall(nativeService(session, "gti_rt_confined_callback_leave_v1",
                                  ir.getVoidTy(), {}));
      return;
    }
    symbol = "gti_rt_confined_callback_enter_v1";
    auto *owner =
        ir.CreateLoad(ir.getInt64Ty(), callbackOwner(session, adapter.id));
    owner->setAtomic(llvm::AtomicOrdering::Monotonic);
    argument = owner;
    break;
  }
  // Signatures are the versioned C declarations in
  // runtime/include/gti/cthread.h.
  ir.CreateCall(
      nativeService(session, symbol, ir.getVoidTy(), {argument->getType()}),
      {argument});
}

} // namespace

void emitNativeCCallbacks(Session &session, Failures &failures) {
  std::vector<MirNativeCallbackAdapterId> required;
  for (const auto &adapter : session.program().mir().nativeCallbackAdapters())
    required.push_back(adapter.id);
  emitNativeCCallbacks(session, failures, required);
}

void emitNativeCCallbacks(
    Session &session, Failures &failures,
    std::span<const MirNativeCallbackAdapterId> required) {
  if (required.empty())
    return;
  static_cast<void>(darwinTarget(session));
  for (const auto &adapter : session.program().mir().nativeCallbackAdapters()) {
    if (std::find(required.begin(), required.end(), adapter.id) ==
        required.end())
      continue;
    const auto *target =
        session.program().mir().findFunctionInstance(adapter.target);
    const auto *resultType = adapter.type.nativeFunctionReturnType();
    const auto parameters = adapter.type.nativeFunctionParameterTypes();
    if (!nativeCCallbackContract(session.program().mir(),
                                 session.program().target(), adapter))
      throw std::logic_error(
          "native callback lost its sealed exact target contract");
    const auto name = callbackName(adapter.id);
    if (session.module().getNamedValue(name))
      throw std::logic_error("native callback emitted more than once");
    const auto native = planNativeCFunction(session, *resultType, parameters);
    const auto gti = session.abi().gti(*resultType, parameters,
                                       target->mayRaiseDefinedFailure);
    auto *wrapper = llvm::Function::Create(native.signature.type,
                                           llvm::Function::InternalLinkage,
                                           name, session.module());
    native.signature.applyTo(*wrapper);
    wrapper->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", wrapper));
    if (auto *debug = session.debugInfo())
      debug->beginNativeCallback(adapter.id, ir);
    callbackAttachment(session, ir, adapter, *wrapper, true);
    // A native callback is terminal containment even on a managed worker; a
    // cleanup emergency inside it is never published as worker output.
    auto *previousWorker = failures.suspendWorker(ir);
    const auto inputs = decodeNativeCArguments(session, ir, native, *wrapper);
    llvm::Value *output = nullptr;
    if (gti.resultDestination)
      output = temporary(ir, native.result.storage.storage,
                         llvm::Align(native.result.storage.alignment));
    llvm::Value *failure = nullptr;
    if (gti.failureDestination) {
      failure = temporary(
          ir, session.abi().failureChannel(),
          session.dataLayout().getABITypeAlign(session.abi().failureChannel()));
      ir.CreateStore(
          llvm::Constant::getNullValue(session.abi().failureChannel()),
          failure);
    }
    std::vector<llvm::Value *> arguments;
    for (const auto &parameter : gti.parameters) {
      switch (parameter.channel) {
      case AbiChannel::ResultDestination:
        arguments.push_back(output);
        break;
      case AbiChannel::FailureDestination:
        arguments.push_back(failure);
        break;
      case AbiChannel::Argument:
        if (auto *input = inputs.at(parameter.sourceIndex);
            parameter.indirect && !input->getType()->isPointerTy()) {
          const auto layout =
              session.layouts().get(parameters[parameter.sourceIndex]);
          auto *slot =
              temporary(ir, layout.storage, llvm::Align(layout.alignment));
          ir.CreateStore(input, slot);
          arguments.push_back(slot);
        } else {
          arguments.push_back(input);
        }
        break;
      default:
        throw std::logic_error(
            "native callback acquired an unverified hidden channel");
      }
    }
    // This personality contains exceptions from native dependencies only. GTI
    // defined failure still uses the unchanged centrally planned private ABI.
    auto personality = session.module().getOrInsertFunction(
        "__gxx_personality_v0", llvm::FunctionType::get(ir.getInt32Ty(), true));
    wrapper->setPersonalityFn(
        llvm::cast<llvm::Constant>(personality.getCallee()));
    auto *normal =
        llvm::BasicBlock::Create(session.context(), "target.returned", wrapper);
    auto *caught = llvm::BasicBlock::Create(session.context(),
                                            "native.exception", wrapper);
    auto *call = ir.CreateInvoke(session.abi().declareFunction(target->id),
                                 normal, caught, arguments);
    gti.applyTo(*call);
    ir.SetInsertPoint(caught);
    auto *landingType =
        llvm::StructType::get(session.layouts().pointer(), ir.getInt32Ty());
    auto *landing = ir.CreateLandingPad(landingType, 1);
    landing->addClause(
        llvm::ConstantPointerNull::get(session.layouts().pointer()));
    ir.CreateCall(nativeService(session, "__cxa_begin_catch",
                                session.layouts().pointer(),
                                {session.layouts().pointer()}),
                  {ir.CreateExtractValue(landing, 0)});
    terminateNativeBoundary(session, ir);
    ir.SetInsertPoint(normal);
    if (gti.statusResult) {
      auto *success = llvm::BasicBlock::Create(session.context(),
                                               "target.success", wrapper);
      auto *failed =
          llvm::BasicBlock::Create(session.context(), "target.failed", wrapper);
      ir.CreateCondBr(call, success, failed);
      ir.SetInsertPoint(failed);
      failures.terminate(ir, failure);
      ir.SetInsertPoint(success);
    }
    failures.restoreWorker(ir, previousWorker);
    callbackAttachment(session, ir, adapter, *wrapper, false);
    llvm::Value *result = output;
    if (!native.result.storage.addressOnly && output)
      result = ir.CreateLoad(native.result.storage.value, output);
    if (!gti.resultDestination && !call->getType()->isVoidTy())
      result = call;
    emitNativeCReturn(session, ir, native, *wrapper, result);
  }
}

llvm::Value *nativeCCallbackAddress(Session &session, llvm::IRBuilder<> &ir,
                                    MirNativeCallbackAdapterId identity) {
  const auto *adapter =
      session.program().mir().findNativeCallbackAdapter(identity);
  auto *wrapper = session.module().getFunction(callbackName(identity));
  if (!adapter || !wrapper || wrapper->isDeclaration())
    throw std::logic_error(
        "native callback address requires its emitted adapter");
  if (adapter->cThreadCallback != CThreadCallbackKind::ThreadConfined)
    return wrapper;
  auto *current = ir.CreateCall(nativeService(
      session, "gti_rt_confined_callback_thread_v1", ir.getInt64Ty(), {}));
  auto *owner = callbackOwner(session, identity);
  auto *observed = ir.CreateLoad(ir.getInt64Ty(), owner);
  observed->setAtomic(llvm::AtomicOrdering::Monotonic);
  auto *function = ir.GetInsertBlock()->getParent();
  auto *ready =
      llvm::BasicBlock::Create(session.context(), "callback.bound", function);
  auto *bind =
      llvm::BasicBlock::Create(session.context(), "callback.bind", function);
  auto *invalid = llvm::BasicBlock::Create(session.context(),
                                           "callback.wrong.thread", function);
  ir.CreateCondBr(ir.CreateICmpEQ(observed, current), ready, bind);
  ir.SetInsertPoint(bind);
  auto *claim = ir.CreateAtomicCmpXchg(
      owner, ir.getInt64(0), current, llvm::MaybeAlign(8),
      llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
  ir.CreateCondBr(
      ir.CreateOr(ir.CreateExtractValue(claim, 1),
                  ir.CreateICmpEQ(ir.CreateExtractValue(claim, 0), current)),
      ready, invalid);
  ir.SetInsertPoint(invalid);
  terminateNativeBoundary(session, ir);
  ir.SetInsertPoint(ready);
  return wrapper;
}

} // namespace lang::native_llvm
