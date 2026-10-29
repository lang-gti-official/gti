#include "gti/mir_printer.h"

#include "gti/mir_effects.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace lang {
namespace {

template <typename Enum> [[nodiscard]] constexpr auto number(Enum value) {
  // Byte-backed enum identities must serialize as numbers, never characters.
  return +static_cast<std::underlying_type_t<Enum>>(value);
}

[[nodiscard]] constexpr std::string_view
callableBoundaryName(CallableBoundary boundary) {
  switch (boundary) {
  case CallableBoundary::Confined:
    return "confined";
  case CallableBoundary::Owned:
    return "owned";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
callableCapabilityName(CallableInvocationCapability capability) {
  switch (capability) {
  case CallableInvocationCapability::Read:
    return "read";
  case CallableInvocationCapability::Mutable:
    return "mut";
  case CallableInvocationCapability::Once:
    return "once";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
ownedTransportName(CallableOwnedTransportKind kind) {
  switch (kind) {
  case CallableOwnedTransportKind::ExactReturn:
    return "exact-return";
  case CallableOwnedTransportKind::ExactField:
    return "exact-field";
  case CallableOwnedTransportKind::ThreadTask:
    return "thread-task";
  case CallableOwnedTransportKind::Value:
    return "owned-value";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
lambdaCaptureModeName(LambdaCaptureMode mode) {
  switch (mode) {
  case LambdaCaptureMode::Copy:
    return "copy";
  case LambdaCaptureMode::Move:
    return "move";
  case LambdaCaptureMode::BorrowRead:
    return "borrow-read";
  case LambdaCaptureMode::BorrowMut:
    return "borrow-mut";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
callInputRoleName(MirCallInputRole role) {
  switch (role) {
  case MirCallInputRole::Receiver:
    return "receiver";
  case MirCallInputRole::Argument:
    return "argument";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
callInputKindName(HirCallInputKind kind) {
  switch (kind) {
  case HirCallInputKind::Value:
    return "value";
  case HirCallInputKind::CopyValue:
    return "copy-value";
  case HirCallInputKind::MoveValue:
    return "move-value";
  case HirCallInputKind::RetainedReceiver:
    return "retained-receiver";
  case HirCallInputKind::ReadBorrow:
    return "read-borrow";
  case HirCallInputKind::MutableBorrow:
    return "mutable-borrow";
  case HirCallInputKind::ReadTemporaryBorrow:
    return "read-temporary-borrow";
  case HirCallInputKind::MutableTemporaryBorrow:
    return "mutable-temporary-borrow";
  case HirCallInputKind::MoveBorrowedTemporary:
    return "move-borrowed-temporary";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
failurePropagationName(FailurePropagationKind kind) {
  switch (kind) {
  case FailurePropagationKind::None:
    return "none";
  case FailurePropagationKind::DirectCall:
    return "direct-call";
  case FailurePropagationKind::VirtualCall:
    return "virtual-call";
  case FailurePropagationKind::Constructor:
    return "constructor";
  case FailurePropagationKind::Callable:
    return "callable";
  case FailurePropagationKind::TaskJoin:
    return "task-join";
  case FailurePropagationKind::BodyCall:
    return "body-call";
  case FailurePropagationKind::Destructor:
    return "destructor";
  case FailurePropagationKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
programStorageName(ProgramStorageKind kind) {
  switch (kind) {
  case ProgramStorageKind::NamespaceGlobal:
    return "namespace-global";
  case ProgramStorageKind::StaticField:
    return "static-field";
  case ProgramStorageKind::FunctionLocal:
    return "function-local";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
programInitializationRoleName(ProgramInitializationStepRole role) {
  switch (role) {
  case ProgramInitializationStepRole::DataOnly:
    return "data-only";
  case ProgramInitializationStepRole::Initializer:
    return "initializer";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
programDataInitializationName(MirProgramDataInitializationKind kind) {
  switch (kind) {
  case MirProgramDataInitializationKind::None:
    return "none";
  case MirProgramDataInitializationKind::ImplicitZero:
    return "implicit-zero";
  case MirProgramDataInitializationKind::Constant:
    return "constant";
  case MirProgramDataInitializationKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
hostedStartupExitPolicyName(MirHostedStartupExitPolicy policy) {
  switch (policy) {
  case MirHostedStartupExitPolicy::ImmediateExit70:
    return "immediate-exit-70";
  case MirHostedStartupExitPolicy::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
hostedStartupFailureBehaviorName(MirHostedStartupFailureBehavior behavior) {
  switch (behavior) {
  case MirHostedStartupFailureBehavior::None:
    return "none";
  case MirHostedStartupFailureBehavior::Detect:
    return "detect";
  case MirHostedStartupFailureBehavior::Propagate:
    return "propagate";
  case MirHostedStartupFailureBehavior::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view
hostedStartupOperationName(MirHostedStartupOperationKind kind) {
  switch (kind) {
  case MirHostedStartupOperationKind::ValidateArgumentCount:
    return "validate-argument-count";
  case MirHostedStartupOperationKind::ConvertArgumentCount:
    return "convert-argument-count";
  case MirHostedStartupOperationKind::CallProgramInitialization:
    return "call-program-initialization";
  case MirHostedStartupOperationKind::ConstructArgumentVector:
    return "construct-argument-vector";
  case MirHostedStartupOperationKind::InitializeArgumentIndex:
    return "initialize-argument-index";
  case MirHostedStartupOperationKind::EnterArgumentLoop:
    return "enter-argument-loop";
  case MirHostedStartupOperationKind::LoadArgumentIndex:
    return "load-argument-index";
  case MirHostedStartupOperationKind::TestArgumentIndex:
    return "test-argument-index";
  case MirHostedStartupOperationKind::BranchArgumentLoop:
    return "branch-argument-loop";
  case MirHostedStartupOperationKind::ReadArgumentView:
    return "read-argument-view";
  case MirHostedStartupOperationKind::PrepareStringConstructorArgument:
    return "prepare-string-constructor-argument";
  case MirHostedStartupOperationKind::ConstructArgumentString:
    return "construct-argument-string";
  case MirHostedStartupOperationKind::PrepareAppendReceiver:
    return "prepare-append-receiver";
  case MirHostedStartupOperationKind::PrepareAppendArgumentMove:
    return "prepare-append-argument-move";
  case MirHostedStartupOperationKind::CallAppend:
    return "call-append";
  case MirHostedStartupOperationKind::ComputeNextArgumentIndex:
    return "compute-next-argument-index";
  case MirHostedStartupOperationKind::AdvanceArgumentIndex:
    return "advance-argument-index";
  case MirHostedStartupOperationKind::ContinueArgumentLoop:
    return "continue-argument-loop";
  case MirHostedStartupOperationKind::PrepareEntryCount:
    return "prepare-entry-count";
  case MirHostedStartupOperationKind::PrepareEntryArgumentsMove:
    return "prepare-entry-arguments-move";
  case MirHostedStartupOperationKind::CallEntry:
    return "call-entry";
  case MirHostedStartupOperationKind::ReturnEntry:
    return "return-entry";
  case MirHostedStartupOperationKind::RouteOperationFailure:
    return "route-operation-failure";
  case MirHostedStartupOperationKind::DropFailureCleanup:
    return "drop-failure-cleanup";
  case MirHostedStartupOperationKind::RouteCleanupFailure:
    return "route-cleanup-failure";
  case MirHostedStartupOperationKind::EndFailureCleanup:
    return "end-failure-cleanup";
  case MirHostedStartupOperationKind::ContainFailure:
    return "contain-failure";
  case MirHostedStartupOperationKind::TerminateCleanupFailure:
    return "terminate-cleanup-failure";
  case MirHostedStartupOperationKind::Count:
    return "invalid";
  case MirHostedStartupOperationKind::BeginCThreadRuntime:
    return "begin-cthread-runtime";
  case MirHostedStartupOperationKind::EndCThreadRuntime:
    return "end-cthread-runtime";
  case MirHostedStartupOperationKind::DrainThreadLocals:
    return "drain-thread-locals";
  case MirHostedStartupOperationKind::BeginThreadLocals:
    return "begin-thread-locals";
  case MirHostedStartupOperationKind::RetireProgramStorage:
    return "retire-program-storage";
  }
  return "invalid";
}

class Printer {
public:
  explicit Printer(std::ostream &output) : output(output) {}

  void print(const MirProgram &program) {
    const bool classAliases = std::any_of(
        program.classInstances().begin(), program.classInstances().end(),
        [](const auto &instance) { return !instance.typeAliases.empty(); });
    output << "mir-v118 valid=" << program.valid() << '\n';
    for (const auto &storage : program.foreignStorageDeclarations()) {
      output << "foreign-storage symbol=" << storage.symbol
             << " external=" << std::quoted(storage.externalSymbol)
             << " access=" << number(storage.access) << " type=";
      type(storage.type);
      output << '\n';
    }
    if (const std::optional<MirEmbeddingPlan> &embedding =
            program.embeddingPlan()) {
      output << "program-kind library embedding-abi=" << embedding->abiVersion
             << " exports=" << embedding->exports.size() << '\n';
      for (const MirEmbeddingExport &selected : embedding->exports) {
        output << "embedding-export symbol=" << std::quoted(selected.symbol)
               << " function=" << std::quoted(selected.qualifiedName)
               << " declaration=" << selected.declaration
               << " instance=" << selected.function << " return=";
        type(selected.returnType);
        output << " parameters=[";
        for (std::size_t index = 0; index < selected.parameterTypes.size();
             ++index) {
          separator(index);
          type(selected.parameterTypes[index]);
        }
        output << "] request=" << std::quoted(selected.requestSource.source)
               << ':' << selected.requestSource.start << ':'
               << selected.requestSource.end << ':'
               << selected.requestSource.line << '\n';
      }
    }
    const auto &target = program.target();
    output << "source-debug-profile="
           << sourceDebugProfileName(program.sourceDebugProfile()) << '\n';
    output << "target os=" << target.os << " vendor=" << target.vendor
           << " arch=" << target.arch
           << " pointer-width=" << target.dataLayout.pointerWidthBits()
           << " execution-profile=" << number(target.executionProfile) << '\n';
    for (const MirNativeCallbackAdapter &adapter :
         program.nativeCallbackAdapters()) {
      output << "native-callback @" << adapter.id
             << " target=" << adapter.target << " cthread-callback="
             << static_cast<unsigned>(adapter.cThreadCallback)
             << " may-raise=" << adapter.targetMayRaiseDefinedFailure
             << " failure-policy=" << number(adapter.failurePolicy)
             << " catches-native=" << adapter.catchesNativeExceptions
             << " type=";
      type(adapter.type);
      output << '\n';
    }
    output << "failure-metadata artifact="
           << program.failureMetadata().artifactIdentity().hex()
           << " descriptor-bytes="
           << program.failureMetadata().descriptorBytes().size()
           << " sites=" << program.failureMetadata().sites().size() << '\n';
    for (const FailureSiteDescriptor &site :
         program.failureMetadata().sites()) {
      output << "failure-site @" << site.id
             << " source=" << site.logicalSource.size() << ':'
             << site.logicalSource << " line=" << site.line
             << " span=" << site.start << ".." << site.end << " outcomes=[";
      for (std::size_t index = 0; index < site.outcomes.size(); ++index) {
        separator(index);
        const DefinedFailureOutcome outcome = site.outcomes[index];
        output << static_cast<std::uint16_t>(outcome.code) << ':'
               << definedFailureCodeName(outcome.code) << ':'
               << definedFailureDetailName(outcome.detail);
      }
      output << "]\n";
    }
    for (const CallerLocationAssignment &assignment :
         program.failureMetadata().callerAssignments()) {
      output << "caller-location-site @" << assignment.site
             << " source-unit=" << assignment.origin.sourceUnit
             << " line=" << assignment.origin.line
             << " span=" << assignment.origin.start << ".."
             << assignment.origin.end << '\n';
    }
    programInitialization(program.programInitializationPlan());
    output << "thread-local-storage " << program.threadLocalStorage().size()
           << '\n';
    for (const auto &storage : program.threadLocalStorage()) {
      output << "thread-local-storage @" << storage.symbol
             << " source-unit=" << storage.sourceUnit << " type=";
      type(storage.type);
      output << " access=" << number(storage.access)
             << " active-cleanup=" << storage.requiresActiveCleanup << ' ';
      traits(storage.traits);
      output << " data-initializer=";
      literal(storage.dataInitializer);
      effectSummary(storage.effectSummary);
      output << '\n';
      dropObligation(MirDropObligation{.dropType = storage.cleanup});
      body(storage.initializer, storage.symbol);
    }
    hostedStartup(program.hostedStartupPlan());
    output << "thread-task-adapters " << program.threadTaskAdapters().size()
           << '\n';
    for (const MirThreadTaskAdapter &adapter : program.threadTaskAdapters()) {
      output << "thread-task-adapter @" << adapter.id << " type=";
      type(adapter.taskType);
      output << " spawn=v" << adapter.spawnValue
             << " source=" << number(adapter.sourceBody.kind) << ':'
             << adapter.sourceBody.owner << ":i" << adapter.spawnInstruction;
      if (adapter.functionTarget) {
        output << " entry=function@" << *adapter.functionTarget;
      } else if (adapter.lambdaTarget) {
        output << " entry=lambda@" << *adapter.lambdaTarget;
      } else {
        output << " entry=missing";
      }
      output << " entry-capability="
             << callableCapabilityName(adapter.targetInvocation)
             << " invocation=" << callableCapabilityName(adapter.invocation)
             << " scoped=" << adapter.scoped
             << " scope-argument=" << adapter.scopeArgument
             << " worker-failure="
             << (adapter.catchesWorkerFailure ? "catch" : "leak")
             << " native-exception="
             << (adapter.catchesNativeExceptions ? "catch" : "leak") << '\n';
    }
    output << "integral-enums " << program.integralEnums().size() << '\n';
    for (const MirIntegralEnum &enumeration : program.integralEnums()) {
      output << "integral-enum @" << enumeration.declaration
             << " source-unit=" << enumeration.sourceUnit << " type=";
      type(enumeration.type);
      output << " underlying=";
      type(enumeration.underlyingType);
      output << '\n';
    }
    output << "payload-enums " << program.payloadEnums().size() << '\n';
    for (const MirPayloadEnum &enumeration : program.payloadEnums()) {
      output << "payload-enum @" << enumeration.declaration
             << " source-unit=" << enumeration.sourceUnit << " type=";
      type(enumeration.type);
      output << " active-cleanup=" << enumeration.requiresActiveCleanup << ' ';
      traits(enumeration.traits);
      output << " variants=[";
      for (std::size_t variantIndex = 0;
           variantIndex < enumeration.variants.size(); ++variantIndex) {
        separator(variantIndex);
        const MirPayloadEnumVariant &variant =
            enumeration.variants[variantIndex];
        output << "{index=" << variant.variantIndex << ",payload=[";
        for (std::size_t fieldIndex = 0;
             fieldIndex < variant.payloadTypes.size(); ++fieldIndex) {
          separator(fieldIndex);
          type(variant.payloadTypes[fieldIndex]);
          output << ":active="
                 << (fieldIndex < variant.payloadRequiresActiveCleanup.size()
                         ? variant.payloadRequiresActiveCleanup[fieldIndex]
                         : false);
        }
        output << "]}";
      }
      output << "]\n";
    }
    output << "module\n";
    body(program.module(), 0);

    for (const MirClassInstance &instance : program.classInstances()) {
      output << "class @" << instance.id
             << " source-unit=" << instance.sourceUnit
             << " declaration=" << instance.declaration << " type=";
      type(instance.type);
      output << ' ';
      traits(instance.traits);
      output << " transfer-policy=" << number(instance.transferPolicy)
             << " share-policy=" << number(instance.sharePolicy);
      if (instance.transferCondition) {
        output << " transfer-condition=";
        output << '[';
        for (const auto &node : instance.transferCondition->nodes) {
          output << number(node.kind) << ':';
          type(node.type);
          output << ':' << node.left << ':' << node.right << ';';
        }
        output << ']';
      }
      if (instance.shareCondition) {
        output << " share-condition=";
        output << '[';
        for (const auto &node : instance.shareCondition->nodes) {
          output << number(node.kind) << ':';
          type(node.type);
          output << ':' << node.left << ':' << node.right << ';';
        }
        output << ']';
      }
      output << " kind=" << number(instance.kind)
             << " compiler-capability=" << number(instance.compilerCapability)
             << " lifecycle-bound-borrow-carrier="
             << instance.lifecycleBoundBorrowCarrier
             << " mutex-guard-borrow-carrier="
             << instance.mutexGuardBorrowCarrier
             << " abstract=" << instance.abstract
             << " polymorphic=" << instance.polymorphic
             << " c-abi=" << instance.cAbiRecord
             << " c-opaque=" << instance.cOpaqueHandle
             << " special-members={default="
             << number(instance.defaultConstructor)
             << ",generated-default-requires-schedule="
             << instance.generatedDefaultRequiresSchedule
             << ",copy-constructor=" << number(instance.copyConstructor)
             << ",move-constructor=" << number(instance.moveConstructor)
             << ",copy-assignment=" << number(instance.copyAssignment)
             << ",move-assignment=" << number(instance.moveAssignment)
             << ",destructor=" << number(instance.destructorStatus) << '}';
      if (instance.cAbiLayout) {
        output << " layout={size=" << instance.cAbiLayout->sizeBytes
               << ",align=" << instance.cAbiLayout->abiAlignmentBytes
               << ",fields=[";
        for (std::size_t index = 0; index < instance.cAbiLayout->fields.size();
             ++index) {
          separator(index);
          const MirCAbiRecordFieldLayout &field =
              instance.cAbiLayout->fields[index];
          output << "{symbol=" << field.field << ",type=";
          type(field.type);
          output << ",offset=" << field.offsetBytes
                 << ",size=" << field.sizeBytes
                 << ",align=" << field.abiAlignmentBytes << '}';
        }
        output << "]}";
      }
      if (instance.unionLayout) {
        output << " union-layout={size=" << instance.unionLayout->sizeBytes
               << ",align=" << instance.unionLayout->abiAlignmentBytes
               << ",fields=[";
        for (std::size_t index = 0; index < instance.unionLayout->fields.size();
             ++index) {
          separator(index);
          const MirUnionFieldLayout &field =
              instance.unionLayout->fields[index];
          output << "{symbol=" << field.field << ",type=";
          type(field.type);
          output << ",size=" << field.sizeBytes
                 << ",align=" << field.abiAlignmentBytes << '}';
        }
        output << "]}";
      }
      output << " destructor=";
      optional(instance.destructor);
      output << " active-drop=" << instance.requiresActiveDropState
             << " active-cleanup=" << instance.requiresActiveCleanup
             << " bases=[";
      for (std::size_t index = 0; index < instance.bases.size(); ++index) {
        separator(index);
        const HirBaseInstance &base = instance.bases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "] virtual-dispatch=[";
      for (std::size_t index = 0; index < instance.virtualDispatch.size();
           ++index) {
        separator(index);
        const auto &selected = instance.virtualDispatch[index];
        output << "{root=" << selected.root << ",target=" << selected.target
               << ",root-owner=" << selected.rootOwner
               << ",target-owner=" << selected.targetOwner << ",root-path=[";
        list(selected.rootPath);
        output << "],target-path=[";
        list(selected.targetPath);
        output << "]}";
      }
      output << "] structural-bases=[";
      for (std::size_t index = 0; index < instance.structuralBases.size();
           ++index) {
        separator(index);
        const HirBaseInstance &base = instance.structuralBases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "]";
      if (classAliases) {
        output << " aliases=[";
        for (std::size_t index = 0; index < instance.typeAliases.size();
             ++index) {
          separator(index);
          output << "{declaration=" << instance.typeAliases[index].declaration
                 << ",type=";
          type(instance.typeAliases[index].type);
          output << '}';
        }
        output << "]";
      }
      output << " declared-fields=[";
      for (std::size_t index = 0; index < instance.declaredFields.size();
           ++index) {
        separator(index);
        const MirClassFieldInfo &field = instance.declaredFields[index];
        output << "{field=" << field.field << ",symbol=" << field.symbol
               << ",type=";
        type(field.type);
        output << ",access=" << number(field.access)
               << ",drop=" << number(field.dropKind)
               << ",active-cleanup=" << field.requiresActiveCleanup << '}';
      }
      output << "] lifecycle-fields=[";
      for (std::size_t index = 0; index < instance.fields.size(); ++index) {
        separator(index);
        const MirClassFieldLifecycle &field = instance.fields[index];
        output << "{field=" << field.field << ",symbol=" << field.symbol
               << ",type=";
        type(field.type);
        output << ",drop=" << number(field.dropKind)
               << ",active-cleanup=" << field.requiresActiveCleanup << '}';
      }
      output << "] drops=[";
      for (std::size_t index = 0; index < instance.fieldDropOrder.size();
           ++index) {
        separator(index);
        const MirFieldDrop &drop = instance.fieldDropOrder[index];
        output << "{field=" << drop.field << ",symbol=" << drop.symbol
               << ",type=";
        type(drop.type);
        output << ",active-cleanup=" << drop.requiresActiveCleanup << '}';
      }
      output << "]\nclass-fields\n";
      body(instance.fieldInitializers, instance.id);
      output << "class-static-fields\n";
      body(instance.staticFieldInitializers, instance.id);
    }

    for (const MirFunctionInstance &instance : program.functionInstances()) {
      output << "function @" << instance.id
             << " declaration=" << instance.declaration << " type-arguments=[";
      for (std::size_t index = 0; index < instance.typeArguments.size();
           ++index) {
        separator(index);
        type(instance.typeArguments[index]);
      }
      output << "] value-arguments=[";
      for (std::size_t index = 0; index < instance.valueArguments.size();
           ++index) {
        separator(index);
        const CompileTimeValue &argument = instance.valueArguments[index];
        output << '{' << number(argument.kind) << ':' << argument.value << ':'
               << argument.parameterId << '}';
      }
      output << "] owner=";
      optional(instance.owner);
      output << " static=" << instance.staticMember
             << " entry=" << number(instance.entryKind) << ':'
             << (instance.entryArgumentAppendTarget
                     ? *instance.entryArgumentAppendTarget
                     : 0)
             << " return=";
      type(instance.returnType);
      output << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        output << "{binding="
               << (index < instance.parameterBindings.size()
                       ? instance.parameterBindings[index]
                       : 0)
               << ",type=";
        type(instance.parameterTypes[index]);
        output << '}';
      }
      output << "] parameter-bindings=[";
      for (std::size_t index = 0; index < instance.parameterBindings.size();
           ++index) {
        separator(index);
        output << 'b' << instance.parameterBindings[index];
      }
      output << "] ownerless-text-parameters=[";
      for (std::size_t index = 0;
           index < instance.ownerlessTextParameters.size(); ++index) {
        separator(index);
        output << instance.ownerlessTextParameters[index];
      }
      output << "] ownerless-text-fields=[";
      for (std::size_t index = 0; index < instance.ownerlessTextFields.size();
           ++index) {
        separator(index);
        const auto &field = instance.ownerlessTextFields[index];
        if (field.receiver)
          output << "receiver";
        else
          output << field.parameter;
        output << ':' << field.field;
      }
      output << "] published-text-fields=[";
      for (std::size_t index = 0; index < instance.publishedTextFields.size();
           ++index) {
        separator(index);
        output << instance.publishedTextFields[index].parameter << ':'
               << instance.publishedTextFields[index].field;
      }
      output << "] explicit-return-borrow="
             << instance.explicitReturnBorrowOrigin
             << " return-borrow=" << number(instance.returnBorrowOrigin) << ':'
             << instance.returnBorrowParameter << ':'
             << number(instance.returnBorrowAccess)
             << " return-borrow-parameters=[";
      for (std::size_t index = 0;
           index < instance.returnBorrowParameters.size(); ++index) {
        separator(index);
        output << instance.returnBorrowParameters[index];
      }
      output << "] return-borrow-place=";
      borrowOriginPlace(instance.returnBorrowPlace);
      output << " return-borrow-fields=";
      borrowFields(instance.returnBorrowFields);
      output << " complete-return-borrow=";
      if (!instance.completeReturnBorrow) {
        output << "absent";
      } else {
        output << "complete";
        borrowFields(instance.completeReturnBorrow->dependencies);
        output << " alternatives=[";
        for (const auto &alternative :
             instance.completeReturnBorrow->possibleAlternatives)
          borrowConditions(alternative.conditions);
        output << ']';
      }
      output << " restores-reference=" << instance.restoresReferenceResult
             << " restored-fields=";
      restoredFields(instance.restoredBorrowFields);
      output << " owned-pointee=";
      if (instance.ownedPointeeProjection) {
        output << number(instance.ownedPointeeProjection->borrowIntrinsic)
               << ":[";
        for (std::size_t index = 0;
             index < instance.ownedPointeeProjection->fields.size(); ++index) {
          separator(index);
          output << instance.ownedPointeeProjection->fields[index];
        }
        output << ']';
      } else {
        output << "none";
      }
      output << " linkage="
             << (instance.linkage == LanguageLinkage::C ? "c" : "gti")
             << " symbol=" << instance.externalSymbol << " c-array-count=";
      optional(instance.cArrayCountParameter);
      output << " receiver=" << number(instance.receiverMutability)
             << " operator=";
      if (instance.overloadedOperator) {
        output << number(*instance.overloadedOperator);
      } else {
        output << '-';
      }
      output << " virtual=" << instance.virtualMethod
             << " pure=" << instance.pureVirtual
             << " override=" << instance.overrideMethod << " definition=";
      switch (instance.definitionKind) {
      case MirFunctionInstance::DefinitionKind::Source:
        output << "source";
        break;
      case MirFunctionInstance::DefinitionKind::RuntimeBinding:
        output << "runtime";
        break;
      case MirFunctionInstance::DefinitionKind::Declaration:
        output << "declaration";
        break;
      }
      output << " may-raise-defined-failure=" << instance.mayRaiseDefinedFailure
             << " requires-unsafe=" << instance.requiresUnsafe
             << " noreturn=" << instance.neverReturns
             << " track-caller=" << instance.tracksCaller
             << " thread-confined-callback=" << instance.threadConfinedCallback
             << " cthread-operation="
             << static_cast<unsigned>(instance.cThreadOperation)
             << " process-memory-operation="
             << static_cast<unsigned>(instance.processMemoryOperation);
      effectSummary(instance.effectSummary);
      output << " roots=[";
      list(instance.virtualRoots);
      output << "] callables=[";
      for (std::size_t index = 0; index < instance.callableParameters.size();
           ++index) {
        separator(index);
        callableParameter(instance.callableParameters[index]);
      }
      output << "]\n";
      body(instance.body, instance.id);
    }

    for (const MirConstructorInstance &instance :
         program.constructorInstances()) {
      output << "constructor @" << instance.id << " owner=" << instance.owner
             << " constexpr=" << instance.constexprConstructor
             << " requires-unsafe=" << instance.requiresUnsafe
             << " generated-default=" << instance.generatedDefault
             << " type-arguments=[";
      for (std::size_t index = 0; index < instance.typeArguments.size();
           ++index) {
        separator(index);
        type(instance.typeArguments[index]);
      }
      output << "] value-arguments=[";
      for (std::size_t index = 0; index < instance.valueArguments.size();
           ++index) {
        separator(index);
        const CompileTimeValue &argument = instance.valueArguments[index];
        output << '{' << number(argument.kind) << ':' << argument.value << ':'
               << argument.parameterId << '}';
      }
      output << "] parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        output << "{binding="
               << (index < instance.parameterBindings.size()
                       ? instance.parameterBindings[index]
                       : 0)
               << ",type=";
        type(instance.parameterTypes[index]);
        output << '}';
      }
      output << "] parameter-bindings=[";
      for (std::size_t index = 0; index < instance.parameterBindings.size();
           ++index) {
        separator(index);
        output << 'b' << instance.parameterBindings[index];
      }
      output << "] borrow=" << number(instance.borrowOrigin) << ':'
             << instance.borrowParameter << ':' << number(instance.borrowAccess)
             << " borrow-parameters=[";
      for (std::size_t index = 0; index < instance.borrowParameters.size();
           ++index) {
        separator(index);
        output << instance.borrowParameters[index];
      }
      output << "] complete-borrow=";
      if (!instance.completeBorrow) {
        output << "unknown";
      } else {
        borrowFields(instance.completeBorrow->dependencies);
        output << " alternatives=[";
        for (const auto &alternative :
             instance.completeBorrow->possibleAlternatives)
          borrowConditions(alternative.conditions);
        output << ']';
      }
      output << " definition=";
      switch (instance.definitionKind) {
      case MirDefinitionKind::Source:
        output << "source";
        break;
      case MirDefinitionKind::RuntimeBinding:
        output << "runtime";
        break;
      case MirDefinitionKind::Declaration:
        output << "declaration";
        break;
      }
      output << " may-raise-defined-failure="
             << instance.mayRaiseDefinedFailure;
      effectSummary(instance.effectSummary);
      output << " dispatch-state=";
      if (instance.dispatchState)
        output << instance.dispatchState->activeClass << ':'
               << instance.dispatchState->afterBaseInitialization;
      else
        output << "unrepresented";
      output << " initializers=[";
      for (std::size_t index = 0; index < instance.initializers.size();
           ++index) {
        separator(index);
        constructorInitializer(instance.initializers[index]);
      }
      output << "]\n";
      body(instance.body, instance.id);
    }

    for (const MirDestructorInstance &instance :
         program.destructorInstances()) {
      output << "destructor @" << instance.id << " owner=" << instance.owner
             << " definition=";
      switch (instance.definitionKind) {
      case MirDefinitionKind::Source:
        output << "source";
        break;
      case MirDefinitionKind::RuntimeBinding:
        output << "runtime";
        break;
      case MirDefinitionKind::Declaration:
        output << "declaration";
        break;
      }
      output << " may-raise-defined-failure="
             << instance.mayRaiseDefinedFailure;
      effectSummary(instance.effectSummary);
      output << " dispatch-state=";
      if (instance.dispatchState)
        output << instance.dispatchState->activeClass;
      else
        output << '-';
      output << '\n';
      body(instance.body, instance.id);
    }

    for (const MirLambdaInstance &instance : program.lambdaInstances()) {
      output << "lambda @" << instance.id << " type=";
      type(instance.type);
      output << " returns=";
      type(instance.returnType);
      output << " invocation=" << number(instance.capability)
             << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        type(instance.parameterTypes[index]);
      }
      output << "] parameter-bindings=[";
      for (std::size_t index = 0; index < instance.parameterBindings.size();
           ++index) {
        separator(index);
        output << 'b' << instance.parameterBindings[index];
      }
      output << "] captures=[";
      for (std::size_t index = 0; index < instance.captureTypes.size();
           ++index) {
        separator(index);
        output << "{type=";
        type(instance.captureTypes[index]);
        output << ",mode="
               << (index < instance.captureModes.size()
                       ? lambdaCaptureModeName(instance.captureModes[index])
                       : "invalid")
               << ",symbol="
               << (index < instance.captureSymbols.size()
                       ? instance.captureSymbols[index]
                       : 0)
               << ",active-cleanup="
               << (index < instance.captureRequiresActiveCleanup.size()
                       ? instance.captureRequiresActiveCleanup[index]
                       : false)
               << '}';
      }
      output << "] lifecycle-bound-borrow-carrier="
             << instance.lifecycleBoundBorrowCarrier;
      effectSummary(instance.effectSummary);
      output << '\n';
      body(instance.body, instance.id);
    }
    if (program.hostedStartupPlan() && program.hostedStartup() != nullptr) {
      output << "hosted-startup-body\n";
      body(*program.hostedStartup(), program.hostedStartupPlan()->entry);
    }
  }

  void print(const MirBody &value) {
    output << "mir-body-v93\n";
    body(value, 0);
  }

private:
  void effectSummary(const MirEffectSummary &summary) {
    const MirEffectTraits &traits = summary.traits;
    output << " effects={reads-place=" << traits.readsPlace
           << ",writes-place=" << traits.writesPlace
           << ",reads-memory=" << traits.readsUnknownMemory
           << ",writes-memory=" << traits.writesUnknownMemory
           << ",allocates=" << traits.allocates
           << ",runtime=" << traits.invokesRuntime
           << ",traps=" << traits.mayTrap
           << ",starts-loan=" << traits.startsLoan
           << ",ends-loan=" << traits.endsLoan
           << ",depends-loan=" << traits.dependsOnLoan
           << ",copies=" << traits.copiesValue << ",moves=" << traits.movesValue
           << ",initializes=" << traits.initializesValue
           << ",drops=" << traits.dropsValue
           << ",user-code=" << traits.invokesUserCode
           << ",target-dependent=" << traits.targetDependent
           << ",synchronizes=" << traits.maySynchronize
           << ",speculatable=" << traits.speculatable
           << ",removable=" << traits.removableWhenUnused
           << ",reorderable=" << traits.reorderable
           << ",receiver-escapes=" << summary.receiverEscapes
           << ",arguments-escape=[";
    for (std::size_t index = 0; index < summary.argumentsEscape.size();
         ++index) {
      separator(index);
      output << summary.argumentsEscape[index];
    }
    output << "]}";
  }

  void programInitialization(const MirProgramInitializationPlan &plan) {
    output << "program-initialization units=" << plan.units.size()
           << " steps=" << plan.steps.size() << '\n';
    for (const MirProgramInitializationUnit &unit : plan.units) {
      output << "  unit source=" << unit.sourceUnit << " steps=[";
      list(unit.steps);
      output << "]\n";
    }
    for (const MirProgramInitializationStep &step : plan.steps) {
      output << "  step @" << step.id << " source=" << step.sourceUnit
             << " storage=" << programStorageName(step.storageKind)
             << " role=" << programInitializationRoleName(step.role)
             << " symbol=" << step.symbol << " owner-class=" << step.ownerClass
             << " active-cleanup=" << step.requiresActiveCleanup
             << " native-retirement=" << step.nativeRetirement.has_value()
             << " binding=" << step.binding << " place=p" << step.storagePlace
             << " entry=bb" << step.entryBlock << " initialize=i"
             << step.storageInitialization << " data-kind="
             << programDataInitializationName(step.dataInitialization)
             << " data=";
      if (step.dataConstant) {
        constant(*step.dataConstant);
      } else {
        output << '-';
      }
      output << " statement=" << step.statement << " initializer=v"
             << step.initializer << " full-expression=" << step.fullExpression
             << " locals=[";
      for (std::size_t index = 0; index < step.locals.size(); ++index) {
        if (index != 0)
          output << ',';
        const auto &local = step.locals[index];
        output << "local(binding=" << local.binding
               << ";symbol=" << local.symbol << ";place=p" << local.place
               << ')';
      }
      output << "] nested-full-expressions=[";
      list(step.nestedFullExpressions);
      output << "]\n";
      if (step.nativeRetirement)
        dropObligation(MirDropObligation{.dropType = *step.nativeRetirement});
    }
  }

  void hostedStartup(const std::optional<MirHostedStartupPlan> &plan) {
    if (!plan) {
      output << "hosted-startup none\n";
      return;
    }
    output << "hosted-startup kind=" << number(plan->kind)
           << " entry=" << plan->entry << " append=" << plan->appendFunction
           << " vector-constructor=" << plan->vectorConstructor
           << " string-constructor=" << plan->stringConstructor
           << " source=unit" << plan->sourceAnchor.sourceUnit << ':'
           << plan->sourceAnchor.line << '@' << plan->sourceAnchor.start << ".."
           << plan->sourceAnchor.end << " program-initialization="
           << number(plan->programInitializationTarget.kind) << ':'
           << plan->programInitializationTarget.owner
           << " exit=" << hostedStartupExitPolicyName(plan->exitPolicy)
           << " index-place=p" << plan->argumentIndexPlace << " vector-place=p"
           << plan->argumentVectorPlace << " count=%" << plan->stabilizedCount
           << " vector=%" << plan->argumentVector << " result=%"
           << plan->entryResult << " cthread-begin=" << plan->cThreadBegin
           << " cthread-end=" << plan->cThreadEnd
           << " operations=" << plan->operations.size() << '\n';
    for (const MirHostedStartupOperation &operation : plan->operations) {
      output << "  hosted-operation @" << operation.id
             << " kind=" << hostedStartupOperationName(operation.kind)
             << " failure="
             << hostedStartupFailureBehaviorName(operation.failureBehavior)
             << " block=bb" << operation.block << " instruction=i"
             << operation.instruction << " place=p" << operation.place
             << " value=%" << operation.value << " drop=drop"
             << operation.dropObligation << " failure-record=fail"
             << operation.failureRecord << " cleanup-boundary=cleanup"
             << operation.cleanupBoundary
             << " terminator=" << operation.terminator << '\n';
    }
  }

  template <typename Value> void list(const std::vector<Value> &values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      separator(index);
      output << values[index];
    }
  }

  void separator(std::size_t index) {
    if (index != 0) {
      output << ',';
    }
  }

  template <typename Value> void optional(const std::optional<Value> &value) {
    if (value) {
      output << *value;
    } else {
      output << '-';
    }
  }

  void bytes(const std::string &value) {
    output << value.size() << ':' << std::hex << std::setfill('0');
    for (const unsigned char byte : value) {
      output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    output << std::dec << std::setfill(' ');
  }

  void literal(const std::optional<Literal> &value) {
    if (!value) {
      output << '-';
      return;
    }
    std::visit(
        [this](const auto &literalValue) {
          using Value = std::decay_t<decltype(literalValue)>;
          if constexpr (std::is_same_v<Value, std::monostate>) {
            output << "unset";
          } else if constexpr (std::is_same_v<Value, std::nullptr_t>) {
            output << "null";
          } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
            output << "u64:" << literalValue;
          } else if constexpr (std::is_same_v<Value, BinaryFloat>) {
            const bool binary64 =
                literalValue.format == BinaryFloatFormat::Binary64;
            output << (binary64 ? "f64:0x" : "f32:0x") << std::hex
                   << std::setw(binary64 ? 16 : 8) << std::setfill('0')
                   << literalValue.bits << std::dec << std::setfill(' ');
          } else if constexpr (std::is_same_v<Value, CharacterLiteral>) {
            output << "char:" << static_cast<unsigned int>(literalValue.value);
          } else if constexpr (std::is_same_v<Value, std::string>) {
            output << "string:";
            bytes(literalValue);
          } else if constexpr (std::is_same_v<Value, bool>) {
            output << "bool:" << literalValue;
          }
        },
        *value);
  }

  void constant(const ConstantValue &value) {
    std::visit(
        [this](const auto &constantValue) {
          using Value = std::decay_t<decltype(constantValue)>;
          if constexpr (std::is_same_v<Value, ConstantInteger>) {
            output << "integer:" << constantValue.negative << ':'
                   << constantValue.magnitude << ':'
                   << static_cast<unsigned int>(constantValue.domain.width)
                   << ':' << constantValue.domain.signedValue;
          } else if constexpr (std::is_same_v<Value, NullConstant>) {
            output << "null";
          } else if constexpr (std::is_same_v<Value, ConstantEnum>) {
            output << "enum:" << constantValue.enumId << ':'
                   << constantValue.value.negative << ':'
                   << constantValue.value.magnitude;
          } else if constexpr (std::is_same_v<Value, ConstantAggregateValue>) {
            if (constantValue.value == nullptr) {
              output << "aggregate:null";
              return;
            }
            output << "aggregate:"
                   << static_cast<unsigned int>(constantValue.value->kind)
                   << ':' << constantValue.value->identity << ':';
            if (constantValue.value->kind == ConstantAggregateKind::PayloadEnum)
              output << "variant=" << constantValue.value->variant << ':';
            if (constantValue.value->exactType) {
              type(*constantValue.value->exactType);
            } else {
              output << "type:null";
            }
            output << ":[";
            for (std::size_t index = 0;
                 index < constantValue.value->elements.size(); ++index) {
              if (index != 0) {
                output << ',';
              }
              constant(constantValue.value->elements[index]);
            }
            output << "]:{";
            for (std::size_t index = 0;
                 index < constantValue.value->fields.size(); ++index) {
              if (index != 0) {
                output << ',';
              }
              output << constantValue.value->fields[index].symbol << '=';
              constant(constantValue.value->fields[index].value);
            }
            output << '}';
          } else {
            literal(std::optional<Literal>{Literal{constantValue}});
          }
        },
        value);
  }

  void type(const SemanticType &value) {
    output << "type(" << number(value.kind) << ";class=" << value.classId
           << ";enum=" << value.enumId
           << ";parameter=" << value.genericParameterId
           << ";native-c=" << static_cast<unsigned>(value.nativeCScalar);
    if (value.functionId != 0) {
      output << ";function=" << value.functionId;
    }
    output << ";lambda=" << value.lambdaId
           << ";lambda-parameters=" << value.lambdaParameterCount
           << ";lambda-captures=" << value.lambdaCaptureCount
           << ";length=" << value.arrayLength
           << ";length-parameter=" << value.arrayLengthParameterId
           << ";access=" << number(value.referenceAccess)
           << ";pointer-access=" << number(value.pointerAccess)
           << ";signature-receiver=" << number(value.signatureReceiver)
           << ";concrete-pack=" << value.concretePack << ";args=[";
    for (std::size_t index = 0; index < value.arguments.size(); ++index) {
      separator(index);
      type(value.arguments[index]);
    }
    output << "];values=[";
    for (std::size_t index = 0; index < value.valueArguments.size(); ++index) {
      separator(index);
      const CompileTimeValue &argument = value.valueArguments[index];
      output << '{' << number(argument.kind) << ':' << argument.value << ':'
             << argument.parameterId << '}';
    }
    output << "];lambda-class-types=[";
    for (std::size_t index = 0; index < value.lambdaEnclosingClassTypes.size();
         ++index) {
      separator(index);
      type(value.lambdaEnclosingClassTypes[index]);
    }
    output << "];lambda-function-types=[";
    for (std::size_t index = 0;
         index < value.lambdaEnclosingFunctionTypes.size(); ++index) {
      separator(index);
      type(value.lambdaEnclosingFunctionTypes[index]);
    }
    output << "];lambda-class-values=[";
    for (std::size_t index = 0; index < value.lambdaEnclosingClassValues.size();
         ++index) {
      separator(index);
      const CompileTimeValue &argument =
          value.lambdaEnclosingClassValues[index];
      output << '{' << number(argument.kind) << ':' << argument.value << ':'
             << argument.parameterId << '}';
    }
    output << "];lambda-function-values=[";
    for (std::size_t index = 0;
         index < value.lambdaEnclosingFunctionValues.size(); ++index) {
      separator(index);
      const CompileTimeValue &argument =
          value.lambdaEnclosingFunctionValues[index];
      output << '{' << number(argument.kind) << ':' << argument.value << ':'
             << argument.parameterId << '}';
    }
    output << "])";
  }

  void traits(const SemanticTypeTraits &value) {
    output << "traits(" << number(value.ownership) << ',' << number(value.drop)
           << ',' << value.copyable << ',' << value.movable << ','
           << value.copyAssignable << ',' << value.moveAssignable << ','
           << value.containsBorrowedState << ',' << value.transferCapable << ','
           << value.shareCapable << ','
           << static_cast<unsigned>(value.relocation) << ')';
  }

  void info(const ExpressionInfo &value) {
    output << "info(";
    type(value.type);
    output << ",category=" << number(value.category)
           << ",access=" << number(value.access) << ',';
    traits(value.traits);
    output << ')';
  }

  void callableSignature(const MirCallableSignature &value) {
    output << "signature(return=";
    type(value.returnType);
    output << ";parameters=[";
    for (std::size_t index = 0; index < value.parameterTypes.size(); ++index) {
      separator(index);
      type(value.parameterTypes[index]);
    }
    output << "];function=";
    optional(value.functionTarget);
    output << ";lambda=";
    optional(value.lambdaTarget);
    if (value.nativeCalleeType) {
      output << ";native-callee=";
      type(*value.nativeCalleeType);
    }
    output << ";required=" << callableCapabilityName(value.requiredCapability)
           << ";selected=";
    if (value.selectedCapability) {
      output << callableCapabilityName(*value.selectedCapability);
    } else {
      output << '-';
    }
    output << ')';
  }

  void callableForwarding(const MirCallableForwarding &value) {
    output << "forward(parameter=" << value.parameterIndex << ";function=";
    optional(value.functionTarget);
    output << ')';
  }

  void callableParameter(const MirCallableParameter &value) {
    output << "callable(parameter=" << value.parameterIndex << ";type=";
    type(value.callableType);
    output << ";access=" << number(value.access)
           << ";boundary=" << callableBoundaryName(value.boundary)
           << ";transport=";
    if (value.ownedTransport) {
      output << ownedTransportName(value.ownedTransport->kind) << ':';
      type(value.ownedTransport->destinationType);
      output << ':' << value.ownedTransport->field;
    } else {
      output << '-';
    }
    output << ";signatures=[";
    for (std::size_t index = 0; index < value.signatures.size(); ++index) {
      separator(index);
      callableSignature(value.signatures[index]);
    }
    output << "];forwardings=[";
    for (std::size_t index = 0; index < value.forwardings.size(); ++index) {
      separator(index);
      callableForwarding(value.forwardings[index]);
    }
    output << "])";
  }

  void operand(const MirOperand &value) {
    output << "operand(" << number(value.kind) << ";value=" << value.value
           << ";place=" << value.place << ";loan=" << value.loan << ";literal=";
    literal(value.literal);
    output << ";type=";
    type(value.type);
    output << ')';
  }

  void placeDomain(const PlaceDomain &value) {
    // Snapshot identities are process-local stale-key guards. Normalize a
    // live snapshot so equivalent programs retain deterministic text dumps.
    output << (value.snapshot == 0 ? 0 : 1) << ':' << value.body << ':'
           << value.revision;
  }

  void placeProjection(const PlaceProjection &value) {
    output << "projection(" << number(value.kind) << ";field=" << value.field
           << ";index=" << value.index << ";selection=" << value.selection
           << ')';
  }

  void placeKey(const PlaceKey &value) {
    output << "key(domain=";
    placeDomain(value.domain);
    output << ";root=" << value.root << ";receiver=" << value.receiver
           << ";projections=[";
    for (std::size_t index = 0; index < value.projections.size(); ++index) {
      separator(index);
      placeProjection(value.projections[index]);
    }
    output << "])";
  }

  void borrowOriginPlace(const std::optional<BorrowOriginPlace> &value) {
    if (!value) {
      output << '-';
      return;
    }
    output << "origin(root=" << value->root << ";projections=[";
    for (std::size_t index = 0; index < value->projections.size(); ++index) {
      separator(index);
      placeProjection(value->projections[index]);
    }
    output << "])";
  }

  void
  borrowConditions(const std::vector<BorrowDependencyCondition> &conditions) {
    output << '[';
    for (std::size_t index = 0; index < conditions.size(); ++index) {
      separator(index);
      const auto &condition = conditions[index];
      output << "{path=[";
      list(condition.subobject);
      output << "];enum=";
      type(condition.enumeration);
      output << ";variant=" << condition.variant << '}';
    }
    output << ']';
  }

  void borrowFields(const std::vector<BorrowedFieldOrigin> &fields) {
    output << '[';
    for (std::size_t index = 0; index < fields.size(); ++index) {
      separator(index);
      const auto &field = fields[index];
      output << "field(result=[";
      for (std::size_t path = 0; path < field.resultFields.size(); ++path) {
        separator(path);
        output << field.resultFields[path];
      }
      output << "];origin=" << number(field.origin) << ':' << field.parameter;
      if (field.globalRoot != 0)
        output << ";global=" << field.globalRoot;
      output << ";source=[";
      for (std::size_t path = 0; path < field.sourceFields.size(); ++path) {
        separator(path);
        output << field.sourceFields[path];
      }
      output << "];access=" << number(field.access) << ";result-conditions=";
      borrowConditions(field.resultConditions);
      output << ";source-conditions=";
      borrowConditions(field.sourceConditions);
      output << ')';
    }
    output << ']';
  }

  void restoredFields(const std::vector<RestoredBorrowField> &fields) {
    output << '[';
    for (std::size_t index = 0; index < fields.size(); ++index) {
      separator(index);
      output << "{path=[";
      for (std::size_t part = 0; part < fields[index].fields.size(); ++part) {
        separator(part);
        output << fields[index].fields[part];
      }
      output << "];type=";
      type(fields[index].referenceType);
      output << '}';
    }
    output << ']';
  }

  void ownershipEvent(const OwnershipEvent &value) {
    output << "event(kind=" << number(value.kind) << ";place=";
    placeKey(value.place);
    output << ";before=" << static_cast<unsigned int>(value.before.bits)
           << ";after=" << static_cast<unsigned int>(value.after.bits)
           << ";reachable=" << value.reachable << ')';
  }

  void lifecycleEvent(const MirLifecycleEvent &value) {
    output << "lifecycle(kind=" << number(value.kind) << ";source=drop"
           << value.source << ";target=drop" << value.target
           << ";conditional=" << value.conditional
           << ";failure-cleanup=" << value.failureCleanup << ')';
  }

  void dropObligation(const MirDropObligation &value) {
    output << "  drop" << value.id
           << " hosted-operation=" << value.hostedStartupOperation
           << " hir=" << value.hirObligation
           << " construction-order=" << value.constructionOrder
           << " kind=" << number(value.kind) << " place=p" << value.place
           << " binding=" << value.binding << " value=v" << value.value
           << " generated-value=%" << value.generatedValue
           << " hir-full-expression=" << value.hirFullExpression
           << " full-expression=" << value.fullExpression << " type=";
    type(value.dropType.type);
    output << " class=";
    optional(value.dropType.classInstance);
    output << " lambda=";
    optional(value.dropType.lambdaInstance);
    output << " destructor=";
    optional(value.dropType.destructor);
    output << " fixed-array=";
    if (!value.dropType.fixedArray) {
      output << '-';
    } else {
      output << "schedule(extents=[";
      list(value.dropType.fixedArray->extents);
      output << "];leaf=";
      type(value.dropType.fixedArray->leafType);
      output << ";class=";
      optional(value.dropType.fixedArray->leafClassInstance);
      output << ')';
    }
    output << " unique-pointee=";
    optional(value.dropType.uniquePointeeClass);
    output << " polymorphic-cleanup=" << value.dropType.polymorphicCleanup
           << " unique-field=" << value.dropType.uniqueOwnerField
           << " unique-base=";
    optional(value.dropType.uniqueOwnerBase);
    output << " capture=";
    optional(value.dropType.captureIndex);
    output << " payload-variant=";
    optional(value.dropType.payloadVariant);
    output << " payload-field=";
    optional(value.dropType.payloadField);
    output << " recursive=" << value.dropType.recursiveCleanup
           << " aggregate=[";
    for (const auto &step : value.dropType.aggregate)
      aggregateDropStep(step);
    output << "] active-cleanup=" << value.dropType.requiresActiveCleanup
           << " initially-active=" << value.initiallyActive << '\n';
  }

  void aggregateDropStep(const MirDropType &step) {
    output << "step(type=";
    type(step.type);
    output << ";class=";
    optional(step.classInstance);
    output << ";destructor=";
    optional(step.destructor);
    output << ";pointee=";
    optional(step.uniquePointeeClass);
    output << ";polymorphic-cleanup=" << step.polymorphicCleanup
           << ";field=" << step.uniqueOwnerField << ";base=";
    optional(step.uniqueOwnerBase);
    output << ";capture=";
    optional(step.captureIndex);
    output << ";payload-variant=";
    optional(step.payloadVariant);
    output << ";payload-field=";
    optional(step.payloadField);
    output << ";recursive=" << step.recursiveCleanup
           << ";active=" << step.requiresActiveCleanup << ";children=[";
    for (const auto &child : step.aggregate)
      aggregateDropStep(child);
    output << "])";
  }

  void fullExpression(const MirFullExpression &value) {
    output << "  full-expression" << value.id << " hir=" << value.hirExpression
           << " statement=" << value.statement
           << " constructor-initializer=" << value.constructorInitializer
           << " roots=[";
    list(value.roots);
    output << "]\n";
  }

  void projection(const MirPlaceProjection &value) {
    output << "projection(" << number(value.kind) << ";field=" << value.field
           << ";index=" << value.index << ";constant=";
    optional(value.constantIndex);
    output << ";selection=" << value.selection << ')';
  }

  void place(const MirPlace &value) {
    output << "  p" << value.id
           << " hosted-operation=" << value.hostedStartupOperation
           << " root=" << number(value.root) << " binding=" << value.binding
           << " symbol=" << value.symbol << " capture=" << value.capture
           << " temporary=" << value.temporary << " value=" << value.value
           << " loan=" << value.loan << " projections=[";
    for (std::size_t index = 0; index < value.projections.size(); ++index) {
      separator(index);
      projection(value.projections[index]);
    }
    output << "] type=";
    type(value.type);
    output << " access=" << number(value.access) << ' ';
    traits(value.traits);
    output << " source=v" << value.sourceValue << " key=";
    if (value.key) {
      placeKey(*value.key);
    } else {
      output << '-';
    }
    output << " foreign-storage=" << value.foreignStorage;
    output << " initially-available=" << value.initiallyAvailable << '\n';
  }

  void loan(const MirLoan &value) {
    output << "  loan" << value.id << " kind=" << number(value.kind)
           << " semantic=" << value.semanticLoan << " parent=loan"
           << value.parent << " stored-dependency=loan"
           << value.storedDependency << " source=p" << value.source
           << " access=" << number(value.access) << " produced=v"
           << value.producedBy << " carriers=[";
    for (std::size_t index = 0; index < value.carriers.size(); ++index) {
      separator(index);
      output << value.carriers[index];
    }
    output << "] field=" << value.storedField << " designated-fields=[";
    for (std::size_t index = 0; index < value.designatedFields.size();
         ++index) {
      separator(index);
      output << value.designatedFields[index];
    }
    output << "] conditions=";
    borrowConditions(value.conditions);
    output << " reference-selection=";
    if (value.referenceSelection) {
      const auto &selection = *value.referenceSelection;
      output << "{value=%" << selection.value << " carrier=p"
             << selection.carrier << " transferred-from=l"
             << selection.transferredFrom << " fields=[";
      for (std::size_t index = 0; index < selection.fields.size(); ++index) {
        separator(index);
        output << selection.fields[index];
      }
      output << "] conditions=";
      borrowConditions(selection.conditions);
      output << " reference=";
      type(selection.reference);
      output << '}';
    } else {
      output << "absent";
    }
    output << " lifecycle-bound=" << value.lifecycleBound
           << " additional-dependencies=[";
    for (std::size_t index = 0; index < value.additionalDependencies.size();
         ++index) {
      separator(index);
      const auto &dependency = value.additionalDependencies[index];
      output << "{source=p" << dependency.source << " parent=l"
             << dependency.parent << " access=" << number(dependency.access)
             << " fields=[";
      for (std::size_t field = 0; field < dependency.designatedFields.size();
           ++field) {
        separator(field);
        output << dependency.designatedFields[field];
      }
      output << "];conditions=";
      borrowConditions(dependency.conditions);
      output << '}';
    }
    output << ']' << " two-phase=" << value.twoPhase << " entry=" << value.entry
           << " escapes=" << value.escapes << '\n';
  }

  void value(const MirValue &value) {
    output << "  %" << value.id
           << " hosted-operation=" << value.hostedStartupOperation
           << " source=v" << value.sourceValue << ' ';
    info(value.info);
    output << " defined=bb" << value.definitionBlock << ":i" << value.definition
           << " dependency-set=";
    if (!value.borrowDependencies) {
      output << "absent";
    } else {
      output << "complete[";
      for (const auto &dependency : value.borrowDependencies->dependencies) {
        output << "p" << dependency.source << ":loan" << dependency.parent
               << ":fields=";
        list(dependency.designatedFields);
        output << ":conditions=";
        borrowConditions(dependency.conditions);
        output << ';';
      }
      output << "] alternatives=[";
      for (const auto &alternative :
           value.borrowDependencies->possibleAlternatives) {
        borrowConditions(alternative.conditions);
      }
      output << ']';
    }
    output << '\n';
  }

  void instruction(const MirInstruction &value) {
    output << "    i" << value.id << ' ' << name(value.kind)
           << " tombstone=" << value.tombstone
           << " hosted-operation=" << value.hostedStartupOperation
           << " hir-value=" << value.hirValue
           << " thread-local-storage=" << value.threadLocalStorage
           << " hir-statement=" << value.hirStatement
           << " call-site=" << value.callSite
           << " constructor-initializer=" << value.constructorInitializer
           << " call-input-role=";
    if (value.callInputRole) {
      output << callInputRoleName(*value.callInputRole);
    } else {
      output << '-';
    }
    output << " call-input-index=" << value.callInputIndex
           << " call-input-kind=" << callInputKindName(value.callInputKind)
           << " default-argument=" << value.defaultArgument
           << " prepared-parameter-drop=";
    optional(value.preparedParameterDrop);
    output << " retained-owner-place=";
    optional(value.retainedOwnerPlace);
    output << " success-result-drop=";
    optional(value.successResultDrop);
    output << " success-result-destination=";
    optional(value.successResultDestination);
    output << " unsafe-operation=" << number(value.unsafeOperation)
           << " raw-memory=" << value.rawMemoryAccess << " result=";
    optional(value.result);
    output << " destination=";
    optional(value.destination);
    output << " receiver=";
    if (value.receiver) {
      operand(*value.receiver);
    } else {
      output << '-';
    }
    output << " element-cleanup=";
    if (value.elementCleanup) {
      aggregateDropStep(*value.elementCleanup);
    } else {
      output << '-';
    }
    output << " replacement-pair=" << value.replacementPair
           << " replacement-cleanup=";
    if (value.replacementCleanup) {
      aggregateDropStep(*value.replacementCleanup);
    } else {
      output << '-';
    }
    output << " operands=[";
    for (std::size_t index = 0; index < value.operands.size(); ++index) {
      separator(index);
      operand(value.operands[index]);
    }
    output << "] parameters=[";
    for (std::size_t index = 0; index < value.parameterTypes.size(); ++index) {
      separator(index);
      type(value.parameterTypes[index]);
    }
    output << "] object-layout-type=";
    if (value.objectLayoutType)
      type(*value.objectLayoutType);
    else
      output << '-';
    output << " closure-captures=[";
    for (std::size_t index = 0; index < value.closureCaptureTypes.size();
         ++index) {
      separator(index);
      type(value.closureCaptureTypes[index]);
    }
    output << "] closure-capture-modes=[";
    for (std::size_t index = 0; index < value.closureCaptureModes.size();
         ++index) {
      separator(index);
      output << lambdaCaptureModeName(value.closureCaptureModes[index]);
    }
    output << "] loan=";
    optional(value.loan);
    output << " borrow-origin=" << number(value.borrowOrigin)
           << " borrow-argument=" << value.borrowArgument
           << " borrow-access=" << number(value.borrowAccess)
           << " borrow-arguments=[";
    for (std::size_t index = 0; index < value.borrowArguments.size(); ++index) {
      separator(index);
      output << value.borrowArguments[index];
    }
    output << "] borrow-place=";
    borrowOriginPlace(value.borrowPlace);
    output << " borrow-fields=";
    borrowFields(value.borrowFields);
    output << " dependent-text=" << value.dependentTextPublication
           << " provisional-text-relays=" << value.provisionalTextRelays
           << " restores-reference=" << value.restoresReferenceResult
           << " restored-fields=";
    restoredFields(value.restoredBorrowFields);
    output << " operation=" << name(value.operation) << " literal=";
    literal(value.literal);
    output << " literal-provenance=";
    switch (value.literalProvenance.kind) {
    case MirLiteralProvenanceKind::None:
      output << '-';
      break;
    case MirLiteralProvenanceKind::Source:
      output << "source";
      break;
    case MirLiteralProvenanceKind::IdentityFold:
      output << "identity-fold:v" << value.literalProvenance.sourceValue;
      break;
    case MirLiteralProvenanceKind::ComputeFold: {
      output << "compute-fold:"
             << name(value.literalProvenance.sourceOperation);
      for (std::size_t index = 0;
           index < value.literalProvenance.sourceValues.size(); ++index) {
        output << (index == 0 ? ':' : ',') << 'v'
               << value.literalProvenance.sourceValues[index];
      }
      break;
    }
    case MirLiteralProvenanceKind::SparseConstant: {
      output << "sparse-constant:"
             << name(value.literalProvenance.sourceOperation);
      for (std::size_t index = 0;
           index < value.literalProvenance.sourceValues.size(); ++index) {
        output << (index == 0 ? ':' : ',') << 'v'
               << value.literalProvenance.sourceValues[index];
      }
      break;
    }
    case MirLiteralProvenanceKind::Count:
      output << "invalid:v" << value.literalProvenance.sourceValue;
      break;
    }
    output << " program-constant-substitution="
           << value.programConstantSubstitution;
    output << " enum-owner=";
    optional(value.enumOwner);
    output << " enum-value=";
    if (value.enumValue) {
      output << value.enumValue->negative << ':' << value.enumValue->magnitude;
    } else {
      output << '-';
    }
    output << " enum-variant=";
    optional(value.enumVariant);
    output << " payload-index=";
    optional(value.payloadIndex);
    output << " payload-extraction=";
    if (value.payloadExtractionKind)
      output << static_cast<unsigned>(*value.payloadExtractionKind);
    else
      output << '-';
    output << " intrinsic=" << name(value.intrinsic)
           << " function-pointer-function=";
    optional(value.functionPointerFunctionTarget);
    output << " function-pointer-lambda=";
    optional(value.functionPointerLambdaTarget);
    output << " noreturn=" << value.neverReturns
           << " no-normal-borrow-result=" << value.noNormalBorrowResult
           << " track-caller=" << value.tracksCaller
           << " caller-site=" << value.callerLocationSite
           << " forward-caller=" << value.forwardsCallerLocation
           << " caller-origin=";
    if (value.callerLocation) {
      output << value.callerLocation->sourceUnit << ':'
             << value.callerLocation->start << ':' << value.callerLocation->end
             << ':' << value.callerLocation->line;
    } else {
      output << '-';
    }
    output << " source-location=";
    if (value.sourceLocation) {
      output << value.sourceLocation->file.size() << ':'
             << value.sourceLocation->file << ','
             << value.sourceLocation->function.size() << ':'
             << value.sourceLocation->function << ','
             << value.sourceLocation->line << ':'
             << value.sourceLocation->column;
    } else {
      output << '-';
    }
    if (!value.scopedThreadTasks.empty()) {
      output << " scoped-tasks=[";
      for (const auto &task : value.scopedThreadTasks)
        output << "(adapter=" << task.adapter << ";loan=" << task.dependency
               << ")";
      output << ']';
    }
    output << " synchronization=" << name(value.synchronization.kind)
           << " atomic-order=";
    if (value.synchronization.order) {
      output << name(*value.synchronization.order);
    } else {
      output << '-';
    }
    output << " failure-order=";
    if (value.synchronization.failureOrder) {
      output << name(*value.synchronization.failureOrder);
    } else {
      output << '-';
    }
    output << " failure-origins=[";
    for (std::size_t originIndex = 0;
         originIndex < value.definedFailure.localOrigins.size();
         ++originIndex) {
      separator(originIndex);
      const DefinedFailureOrigin &origin =
          value.definedFailure.localOrigins[originIndex];
      output << "unit" << origin.sourceUnit << ':' << origin.line << '@'
             << origin.start << ".." << origin.end << '{';
      for (std::size_t outcomeIndex = 0; outcomeIndex < origin.outcomes.size();
           ++outcomeIndex) {
        separator(outcomeIndex);
        const DefinedFailureOutcome outcome = origin.outcomes[outcomeIndex];
        output << static_cast<std::uint16_t>(outcome.code) << ':'
               << definedFailureCodeName(outcome.code) << ':'
               << definedFailureDetailName(outcome.detail);
      }
      output << '}';
    }
    output << ']';
    output << " failure-sites=[";
    list(value.localFailureSites);
    output << ']';
    output << " safety-discharges=[";
    for (std::size_t index = 0; index < value.safetyDischarges.size();
         ++index) {
      separator(index);
      const MirSafetyDischarge &discharge = value.safetyDischarges[index];
      output << mirSafetyCheckFamilyName(discharge.family) << ":origin"
             << discharge.origin << ':'
             << definedFailureCodeName(discharge.outcome.code) << ':'
             << definedFailureDetailName(discharge.outcome.detail);
    }
    output << ']';
    output << " failure-propagation="
           << failurePropagationName(value.definedFailure.propagation);
    output << " dispatch=" << number(value.dispatch) << " dispatch-owner=";
    type(value.dispatchOwner);
    output << " function=";
    optional(value.functionTarget);
    if (value.nativeCalleeType) {
      output << " native-callee=";
      type(*value.nativeCalleeType);
    }
    output << " constructor=";
    optional(value.constructorTarget);
    output << " object-allocation=" << value.objectAllocation;
    output << " body-target=";
    if (value.bodyTarget) {
      output << number(value.bodyTarget->kind) << ':'
             << value.bodyTarget->owner;
    } else {
      output << '-';
    }
    output << " constructor-kind=" << number(value.constructorKind)
           << " lambda=";
    optional(value.lambdaTarget);
    output << " callable-boundary=";
    if (value.callableBoundary) {
      output << callableBoundaryName(*value.callableBoundary);
    } else {
      output << '-';
    }
    output << " callable-invocation=";
    if (value.callableInvocation) {
      output << callableCapabilityName(*value.callableInvocation);
    } else {
      output << '-';
    }
    output << " callable-arguments=[";
    for (std::size_t index = 0; index < value.callableArguments.size();
         ++index) {
      separator(index);
      const CallableArgumentBoundary &argument = value.callableArguments[index];
      output << "{parameter=" << argument.parameterIndex
             << ",boundary=" << callableBoundaryName(argument.boundary) << '}';
    }
    output << ']';
    output << " ownership=";
    if (value.ownership) {
      ownershipEvent(*value.ownership);
    } else {
      output << '-';
    }
    output << " lifecycle=[";
    for (std::size_t index = 0; index < value.lifecycle.size(); ++index) {
      separator(index);
      lifecycleEvent(value.lifecycle[index]);
    }
    output << "] full-expression-end=" << value.fullExpressionEnd
           << " early-exit-full-expression-end="
           << value.earlyExitFullExpressionEnd
           << " cleanup-boundary-end=" << value.cleanupBoundaryEnd;
    output << ' ';
    info(value.info);
    output << '\n';
  }

  void switchCase(const SwitchCaseValue &value) {
    output << "case(" << number(value.kind) << ";type=";
    type(value.type);
    output << ";negative=" << value.value.negative
           << ";magnitude=" << value.value.magnitude
           << ";enum=" << value.enumOwner << ')';
  }

  void terminator(const MirTerminator &value) {
    output << "    term " << number(value.kind)
           << " hosted-operation=" << value.hostedStartupOperation
           << " hir-value=" << value.hirValue
           << " hir-statement=" << value.hirStatement << " value=";
    if (value.value) {
      operand(*value.value);
    } else {
      output << '-';
    }
    output << " return-loan=";
    optional(value.returnLoan);
    output << " invoke=i" << value.invokeInstruction << " failure=fail"
           << value.failureRecord << " target=bb" << value.target << " else=bb"
           << value.elseTarget << " target-arguments=[";
    for (std::size_t index = 0; index < value.targetArguments.size(); ++index) {
      separator(index);
      operand(value.targetArguments[index]);
    }
    output << "] else-arguments=[";
    for (std::size_t index = 0; index < value.elseArguments.size(); ++index) {
      separator(index);
      operand(value.elseArguments[index]);
    }
    output << "] success-lifecycle=[";
    for (std::size_t index = 0; index < value.successLifecycle.size();
         ++index) {
      separator(index);
      lifecycleEvent(value.successLifecycle[index]);
    }
    output << "] switches=[";
    for (std::size_t index = 0; index < value.switchTargets.size(); ++index) {
      separator(index);
      const MirSwitchTarget &target = value.switchTargets[index];
      output << '{';
      if (target.value) {
        switchCase(*target.value);
      } else {
        output << "default";
      }
      output << "->bb" << target.target << " arguments=[";
      for (std::size_t argument = 0; argument < target.arguments.size();
           ++argument) {
        separator(argument);
        operand(target.arguments[argument]);
      }
      output << "]}";
    }
    output << "] provenance=";
    switch (value.provenance.kind) {
    case MirTerminatorProvenanceKind::None:
      output << '-';
      break;
    case MirTerminatorProvenanceKind::BranchFold:
      output << "branch-fold:v" << value.provenance.foldSourceValue;
      break;
    case MirTerminatorProvenanceKind::Count:
      output << "invalid:v" << value.provenance.foldSourceValue;
      break;
    }
    output << '\n';
  }

  void block(const MirBlock &value) {
    output << "  bb" << value.id << " reachable=" << value.reachable
           << " program-initialization-step=" << value.programInitializationStep
           << " failure-parameter=fail" << value.failureParameter
           << " active-failure=fail" << value.activeFailure << " parameters=[";
    for (std::size_t index = 0; index < value.parameters.size(); ++index) {
      separator(index);
      const MirBlockParameter &parameter = value.parameters[index];
      output << "{%" << parameter.value << ":";
      type(parameter.type);
      output << ";source=v" << parameter.sourceValue
             << ";statement=" << parameter.sourceStatement << '}';
    }
    output << "]\n";
    for (const MirInstruction &item : value.instructions) {
      instruction(item);
    }
    terminator(value.terminator);
  }

  void use(const MirValueUse &value) {
    output << "use(" << number(value.kind) << ";value=" << value.value
           << ";block=" << value.block << ";instruction=" << value.instruction
           << ";place=" << value.place << ";edge-target=" << value.edgeTarget
           << ";edge=" << value.edgeIndex << ";operand=" << value.operandIndex
           << ')';
  }

  void body(const MirBody &value, std::size_t owner) {
    output << "body kind=" << number(value.kind) << " owner=" << owner
           << " caller-parameter=" << value.hasCallerLocation
           << " compiler-trusted-synchronization="
           << value.compilerTrustedSynchronization << " domain=";
    placeDomain(value.placeDomain);
    output << " entry=bb" << value.entry << " return=";
    type(value.returnType);
    output << '\n';
    output << " places " << value.places.size() << '\n';
    for (const MirPlace &item : value.places) {
      place(item);
    }
    output << " loans " << value.loans.size() << '\n';
    for (const MirLoan &item : value.loans) {
      loan(item);
    }
    output << " full-expressions " << value.fullExpressions.size() << '\n';
    for (const MirFullExpression &item : value.fullExpressions) {
      fullExpression(item);
    }
    output << " cleanup-boundaries " << value.cleanupBoundaries.size() << '\n';
    for (const MirCleanupBoundary &item : value.cleanupBoundaries) {
      output << "  cleanup-boundary" << item.id
             << " hosted-operation=" << item.hostedStartupOperation
             << " kind=" << number(item.kind) << " obligations=[";
      list(item.obligations);
      output << "]\n";
    }
    output << " drops " << value.dropObligations.size() << '\n';
    for (const MirDropObligation &item : value.dropObligations) {
      dropObligation(item);
    }
    output << " failure-records " << value.failureRecords.size() << '\n';
    for (const MirFailureRecord &item : value.failureRecords) {
      output << "  fail" << item.id << " kind="
             << (item.kind == MirFailureRecordKind::Continuation
                     ? "continuation"
                     : "producer")
             << " hosted-operation=" << item.hostedStartupOperation
             << " producer=bb" << item.producerBlock << ":i"
             << item.producerInstruction << " parameter=bb"
             << item.parameterBlock << '\n';
    }
    output << " program-constant-substitutions "
           << value.programConstantSubstitutions.size() << '\n';
    for (const MirProgramConstantSubstitution &item :
         value.programConstantSubstitutions) {
      output << "  hir-value=" << item.hirValue << " constant=";
      constant(item.constant);
      output << '\n';
    }
    output << " range-iterations " << value.rangeIterations.size() << '\n';
    for (const MirRangeIteration &item : value.rangeIterations) {
      output << "  range" << item.id << " hir=" << item.hirStatement
             << " core=" << item.coreLoop
             << " strategy=" << number(item.strategy) << " range=";
      type(item.rangeType);
      output << " iterator=";
      type(item.iteratorType);
      output << " sentinel=";
      type(item.sentinelType);
      output << " element=";
      type(item.elementType);
      output << " access=" << number(item.elementAccess) << " owner=p"
             << item.owner << " carrier=p" << item.rangeCarrier
             << " iterator-place=p" << item.iterator << " sentinel-place=p"
             << item.sentinel << " element-place=p" << item.element
             << " element-owner=p" << item.elementOwner
             << " element-owner-statement=s" << item.elementOwnerStatement
             << " element-owner-loan=loan" << item.elementOwnerLoan
             << " element-access=%" << item.elementAccessValue
             << " element-access-source=v" << item.elementAccessSource
             << " dereference-function=f" << item.dereferenceFunction
             << " element-owner-drop=drop" << item.elementOwnerDrop
             << " element-source-drop=drop" << item.elementSourceDrop
             << " range-loan=loan" << item.rangeLoan << " element-loan=loan"
             << item.elementLoan << " owner-drop=drop" << item.ownerDrop
             << " blocks=bb" << item.conditionEntryBlock << ",bb"
             << item.conditionBlock << ",bb" << item.bodyBlock << ",bb"
             << item.incrementBlock << ",bb" << item.cleanupBlock << ",bb"
             << item.exitBlock << " extent=" << item.arrayLength
             << " owns-temporary=" << item.ownsRangeTemporary;
      output << " materializes-element-value=" << item.materializesElementValue;
      if (item.endpointParking) {
        const auto &parking = *item.endpointParking;
        output << " endpoint-parking=loan" << parking.cursor
               << " end-hir=" << parking.endValue
               << " begin-inst=" << parking.beginCall
               << " end-inst=" << parking.endCall
               << " receiver-inst=" << parking.endReceiver
               << " park-inst=" << parking.park
               << " restore-inst=" << parking.restore << " failure-restores=[";
        for (std::size_t index = 0; index < parking.failureRestores.size();
             ++index) {
          if (index != 0)
            output << ',';
          output << parking.failureRestores[index];
        }
        output << ']';
      }
      output << '\n';
    }
    output << " values " << value.values.size() << '\n';
    for (const MirValue &item : value.values) {
      this->value(item);
    }
    output << " blocks " << value.blocks.size() << '\n';
    for (const MirBlock &item : value.blocks) {
      block(item);
    }
    output << " uses " << value.valueUses.size() << '\n';
    for (std::size_t index = 0; index < value.valueUses.size(); ++index) {
      output << "  %" << index + 1 << "=[";
      for (std::size_t useIndex = 0; useIndex < value.valueUses[index].size();
           ++useIndex) {
        separator(useIndex);
        use(value.valueUses[index][useIndex]);
      }
      output << "]\n";
    }
    output << " source-debug="
           << sourceDebugCollectionName(value.sourceDebug.collection)
           << " tables=" << static_cast<bool>(value.sourceDebug.tables) << '\n';
    if (value.sourceDebug.tables) {
      const auto &tables = *value.sourceDebug.tables;
      output << "  source-domain=";
      placeDomain(tables.domain);
      output << '\n';
      const auto span = [&](const SourceSpan &source) {
        output << std::quoted(source.source) << ':' << source.start << ':'
               << source.end << ':' << source.line;
      };
      for (const auto &scope : tables.scopes) {
        output << "  source-scope=" << scope.id << " parent=" << scope.parent
               << " unit=" << scope.sourceUnit
               << " expansion=" << scope.expansion << " span=";
        span(scope.source);
        output << '\n';
      }
      for (const auto &local : tables.locals) {
        const auto &declaration = local.declaration;
        output << "  source-local=" << declaration.id
               << " scope=" << declaration.scope
               << " binding=" << declaration.binding
               << " symbol=" << declaration.symbol
               << " name=" << std::quoted(declaration.name)
               << " parameter=" << declaration.parameter << " type=";
        type(declaration.type);
        output << " span=";
        span(declaration.declaration);
        output << " place=" << local.place << " policy=" << number(local.policy)
               << '\n';
      }
      for (const auto &statement : tables.statements) {
        output << "  source-statement=" << statement.id
               << " scope=" << statement.scope << " span=";
        span(statement.source);
        output << '\n';
      }
      for (const auto &sourceValue : tables.values) {
        output << "  source-value=" << sourceValue.id
               << " scope=" << sourceValue.scope << " span=";
        span(sourceValue.source);
        output << '\n';
      }
      for (const auto &end : tables.localEnds)
        output << "  source-end=" << end.local << " block=" << end.point.block
               << " instruction=" << end.point.instruction
               << " after=" << end.point.after
               << " entry=" << end.point.blockEntry << '\n';
      for (const auto &event : tables.valueEvents)
        output << "  source-value-event=" << event.local
               << " value=" << event.value << " block=" << event.point.block
               << " instruction=" << event.point.instruction
               << " after=" << event.point.after << '\n';
    }
    output << "end-body\n";
  }

  void constructorInitializer(const MirConstructorInitializer &value) {
    output << "initializer(kind=" << number(value.kind) << ";type=";
    type(value.targetType);
    output << ";field=" << value.field << ";base=";
    optional(value.base);
    output << ";constructor=";
    optional(value.constructorTarget);
    output << ";arguments=[";
    list(value.arguments);
    output << "];explicit-arguments=" << value.explicitArgumentCount
           << ";stores-reference=" << value.storesReference
           << ";borrow-access=" << number(value.borrowAccess)
           << ";generated=" << value.generatedDefault << ";owned-parameter=";
    optional(value.ownedParameter);
    output << ')';
  }

  std::ostream &output;
};

} // namespace

std::string MirPrinter::print(const MirProgram &program) const {
  std::ostringstream output;
  print(program, output);
  return output.str();
}

std::string MirPrinter::print(const MirBody &body) const {
  std::ostringstream output;
  Printer(output).print(body);
  return output.str();
}

void MirPrinter::print(const MirProgram &program, std::ostream &output) const {
  Printer(output).print(program);
}

} // namespace lang
