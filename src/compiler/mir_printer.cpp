#include "gti/mir_printer.h"

#include "gti/optimization/effects.h"

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
  return static_cast<std::underlying_type_t<Enum>>(value);
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
  case HirCallInputKind::ReadBorrow:
    return "read-borrow";
  case HirCallInputKind::MutableBorrow:
    return "mutable-borrow";
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
  }
  return "invalid";
}

class Printer {
public:
  [[nodiscard]] std::string print(const MirProgram &program) {
    output << "mir-v30 valid=" << program.valid() << '\n';
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
    programInitialization(program.programInitializationPlan());
    hostedStartup(program.hostedStartupPlan());
    output << "module\n";
    body(program.module(), 0);

    for (const MirClassInstance &instance : program.classInstances()) {
      output << "class @" << instance.id
             << " declaration=" << instance.declaration << " type=";
      type(instance.type);
      output << " kind=" << number(instance.kind)
             << " abstract=" << instance.abstract
             << " polymorphic=" << instance.polymorphic
             << " c-abi=" << instance.cAbiRecord;
      if (instance.cAbiLayout) {
        output << " layout={size=" << instance.cAbiLayout->sizeBytes
               << ",align=" << instance.cAbiLayout->abiAlignmentBytes
               << ",fields=[";
        for (std::size_t index = 0; index < instance.cAbiLayout->fields.size();
             ++index) {
          separator(index);
          const CAbiRecordFieldLayout &field =
              instance.cAbiLayout->fields[index];
          output << "{name="
                 << (field.declaration == nullptr
                         ? std::string_view{"?"}
                         : std::string_view{field.declaration->name().lexeme})
                 << ",type=";
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
          const UnionFieldLayout &field = instance.unionLayout->fields[index];
          output << "{name="
                 << (field.declaration == nullptr
                         ? std::string_view{"?"}
                         : std::string_view{field.declaration->name().lexeme})
                 << ",type=";
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
      output << "] structural-bases=[";
      for (std::size_t index = 0; index < instance.structuralBases.size();
           ++index) {
        separator(index);
        const HirBaseInstance &base = instance.structuralBases[index];
        output << "{instance=" << base.instance << ",type=";
        type(base.type);
        output << ",interface=" << base.interface << '}';
      }
      output << "] declared-fields=[";
      for (std::size_t index = 0; index < instance.declaredFields.size();
           ++index) {
        separator(index);
        const MirClassFieldInfo &field = instance.declaredFields[index];
        output << "{field=" << field.field << ",symbol=" << field.symbol
               << ",type=";
        type(field.type);
        output << ",drop=" << number(field.dropKind)
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
             << " declaration=" << instance.declaration << " owner=";
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
      output << "] return-borrow=" << number(instance.returnBorrowOrigin) << ':'
             << instance.returnBorrowParameter << ':'
             << number(instance.returnBorrowAccess) << " return-borrow-place=";
      borrowOriginPlace(instance.returnBorrowPlace);
      output << " linkage="
             << (instance.linkage == LanguageLinkage::C ? "c" : "gti")
             << " symbol=" << instance.externalSymbol
             << " receiver=" << number(instance.receiverMutability)
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
             << " roots=[";
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
             << " parameters=[";
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
      output << "] borrow=" << number(instance.borrowOrigin) << ':'
             << instance.borrowParameter << ':' << number(instance.borrowAccess)
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
      output << " may-raise-defined-failure=" << instance.mayRaiseDefinedFailure
             << " initializers=[";
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
      output << " may-raise-defined-failure=" << instance.mayRaiseDefinedFailure
             << '\n';
      body(instance.body, instance.id);
    }

    for (const MirLambdaInstance &instance : program.lambdaInstances()) {
      output << "lambda @" << instance.id << " type=";
      type(instance.type);
      output << " returns=";
      type(instance.returnType);
      output << " parameters=[";
      for (std::size_t index = 0; index < instance.parameterTypes.size();
           ++index) {
        separator(index);
        type(instance.parameterTypes[index]);
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
      output << "]\n";
      body(instance.body, instance.id);
    }
    if (program.hostedStartupPlan() && program.hostedStartup() != nullptr) {
      output << "hosted-startup-body\n";
      body(*program.hostedStartup(), program.hostedStartupPlan()->entry);
    }
    return output.str();
  }

  [[nodiscard]] std::string print(const MirBody &value) {
    output << "mir-body-v30\n";
    body(value, 0);
    return output.str();
  }

private:
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
             << '\n';
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
           << plan->entryResult << " operations=" << plan->operations.size()
           << '\n';
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
          } else if constexpr (std::is_same_v<Value,
                                              ConstantCheckedIntegerResult>) {
            output << "checked:"
                   << static_cast<unsigned int>(constantValue.domain.width)
                   << ':' << constantValue.domain.signedValue << ':'
                   << constantValue.value.has_value();
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
           << ";lambda=" << value.lambdaId
           << ";lambda-parameters=" << value.lambdaParameterCount
           << ";lambda-captures=" << value.lambdaCaptureCount
           << ";length=" << value.arrayLength
           << ";length-parameter=" << value.arrayLengthParameterId
           << ";access=" << number(value.referenceAccess)
           << ";pointer-access=" << number(value.pointerAccess)
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
           << value.shareCapable << ')';
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
    output << " active-cleanup=" << value.dropType.requiresActiveCleanup
           << " initially-active=" << value.initiallyActive << '\n';
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
    output << " initially-available=" << value.initiallyAvailable << '\n';
  }

  void loan(const MirLoan &value) {
    output << "  loan" << value.id << " kind=" << number(value.kind)
           << " semantic=" << value.semanticLoan << " parent=loan"
           << value.parent << " source=p" << value.source
           << " access=" << number(value.access) << " produced=v"
           << value.producedBy << " carriers=[";
    for (std::size_t index = 0; index < value.carriers.size(); ++index) {
      separator(index);
      output << value.carriers[index];
    }
    output << "] field=" << value.storedField << " entry=" << value.entry
           << " escapes=" << value.escapes << '\n';
  }

  void value(const MirValue &value) {
    output << "  %" << value.id
           << " hosted-operation=" << value.hostedStartupOperation
           << " source=v" << value.sourceValue << ' ';
    info(value.info);
    output << " defined=bb" << value.definitionBlock << ":i" << value.definition
           << '\n';
  }

  void instruction(const MirInstruction &value) {
    output << "    i" << value.id << ' ' << name(value.kind)
           << " hosted-operation=" << value.hostedStartupOperation
           << " hir-value=" << value.hirValue
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
           << " prepared-parameter-drop=";
    optional(value.preparedParameterDrop);
    output << " success-result-drop=";
    optional(value.successResultDrop);
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
    output << "] closure-captures=[";
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
    output << "] pack-fold-symbol=" << value.packFoldSymbol
           << " pack-fold-parameter=" << value.packFoldParameter
           << " pack-fold-function=" << value.packFoldFunction
           << " pack-fold-argument=" << value.packFoldArgument
           << " pack-fold-fixed-places=[";
    for (std::size_t index = 0; index < value.packFoldFixedPlaces.size();
         ++index) {
      separator(index);
      output << value.packFoldFixedPlaces[index];
    }
    output << "]"
           << " pack-fold-elements=[";
    for (std::size_t index = 0; index < value.packFoldElements.size();
         ++index) {
      separator(index);
      const MirPackFoldElement &element = value.packFoldElements[index];
      output << "{type=";
      type(element.elementType);
      output << ",function=" << element.functionTarget << ",parameters=[";
      for (std::size_t parameter = 0; parameter < element.parameterTypes.size();
           ++parameter) {
        separator(parameter);
        type(element.parameterTypes[parameter]);
      }
      output << "]}";
    }
    output << "] loan=";
    optional(value.loan);
    output << " borrow-origin=" << number(value.borrowOrigin)
           << " borrow-argument=" << value.borrowArgument
           << " borrow-access=" << number(value.borrowAccess)
           << " borrow-place=";
    borrowOriginPlace(value.borrowPlace);
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
    output << " intrinsic=" << name(value.intrinsic)
           << " synchronization=" << name(value.synchronization.kind)
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
    output << " failure-propagation="
           << failurePropagationName(value.definedFailure.propagation);
    output << " dispatch=" << number(value.dispatch) << " dispatch-owner=";
    type(value.dispatchOwner);
    output << " function=";
    optional(value.functionTarget);
    output << " constructor=";
    optional(value.constructorTarget);
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
           << value.elseTarget << " success-lifecycle=[";
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
      output << "->bb" << target.target << '}';
    }
    output << "]\n";
  }

  void block(const MirBlock &value) {
    output << "  bb" << value.id << " reachable=" << value.reachable
           << " program-initialization-step=" << value.programInitializationStep
           << " failure-parameter=fail" << value.failureParameter
           << " active-failure=fail" << value.activeFailure << '\n';
    for (const MirInstruction &item : value.instructions) {
      instruction(item);
    }
    terminator(value.terminator);
  }

  void use(const MirValueUse &value) {
    output << "use(" << number(value.kind) << ";value=" << value.value
           << ";block=" << value.block << ";instruction=" << value.instruction
           << ";place=" << value.place << ";operand=" << value.operandIndex
           << ')';
  }

  void body(const MirBody &value, std::size_t owner) {
    output << "body kind=" << number(value.kind) << " owner=" << owner
           << " domain=";
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
      output << "  fail" << item.id
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
    output << "];stores-reference=" << value.storesReference
           << ";borrow-access=" << number(value.borrowAccess)
           << ";generated=" << value.generatedDefault << ";owned-parameter=";
    optional(value.ownedParameter);
    output << ')';
  }

  std::ostringstream output;
};

} // namespace

std::string MirPrinter::print(const MirProgram &program) const {
  return Printer().print(program);
}

std::string MirPrinter::print(const MirBody &body) const {
  return Printer().print(body);
}

} // namespace lang
