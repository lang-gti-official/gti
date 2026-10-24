#include "gti/llvm_admission.h"

#include "coverage.h"

#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lang {
namespace {

using llvm_detail::coverage;
using llvm_detail::Shape;

// One representative witness and count per requirement/body/context. Repeated
// scalar operations do not allocate a duplicate diagnostic record per use.
class Inventory final {
public:
  explicit Inventory(std::vector<LlvmCoverageUse> &uses) : uses(uses) {}

  std::optional<MirBodyAddress> body;
  SourceSpan source;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  bool required = true;
  bool executable = false;

  template <class T> void add(T value) {
    const LlvmCoverageEntry &entry = coverage(value);
    const auto key = std::tuple(entry.family, entry.name, body.has_value(),
                                body ? body->kind : MirBodyKind::Module,
                                body ? body->owner : 0, required, executable);
    auto [found, inserted] = indices.try_emplace(key, uses.size());
    if (inserted) {
      uses.push_back(
          {&entry, body, source, block, instruction, 0, required, executable});
    }
    ++uses[found->second].occurrences;
  }

  void type(const SemanticType &root) {
    std::vector<const SemanticType *> pending{&root};
    while (!pending.empty()) {
      const SemanticType &type = *pending.back();
      pending.pop_back();
      add(type.kind);
      if (type.nativeCScalar != NativeCScalarKind::None) {
        add(type.nativeCScalar);
        add(Shape::NativeScalar);
      }
      if (type.kind == SemanticType::Array)
        add(Shape::FixedArray);
      if (!type.arguments.empty())
        add(Shape::TypeArguments);
      for (const SemanticType &argument : type.arguments)
        pending.push_back(&argument);
      for (const SemanticType &argument : type.lambdaEnclosingClassTypes)
        pending.push_back(&argument);
      for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes)
        pending.push_back(&argument);
    }
  }

  void traits(const SemanticTypeTraits &traits, const SemanticType &value) {
    if (traits.containsBorrowedState)
      add(Shape::BorrowedCarrier);
    add(traits.ownership);
    add(traits.drop);
    if (traits.drop != DropKind::Trivial) {
      add(Shape::CleanupValue);
    }
    if (value.kind == SemanticType::Class ||
        value.kind == SemanticType::Lambda) {
      add(traits.copyable  ? Shape::CopyableValue
          : traits.movable ? Shape::MoveOnlyValue
                           : Shape::ImmovableValue);
    }
  }

  void operand(const MirOperand &operand) {
    add(operand.kind);
    type(operand.type);
  }

  void cleanup(const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &drop = *pending.back();
      pending.pop_back();
      type(drop.type);
      if (drop.fixedArray)
        add(Shape::ArrayDrop);
      if (drop.destructor)
        add(Shape::UserDestructor);
      if (drop.polymorphicCleanup)
        add(Shape::PolymorphicClass);
      if (drop.payloadVariant)
        add(Shape::PayloadEnum);
      if (!drop.aggregate.empty())
        add(Shape::FieldDrop);
      if (drop.requiresActiveCleanup)
        add(Shape::CleanupValue);
      for (const auto &child : drop.aggregate)
        pending.push_back(&child);
    }
  }

  void lifecycle(const MirLifecycleEvent &event) {
    add(event.kind);
    if (event.conditional)
      add(Shape::ConditionalDrop);
  }

  void inspect(const LoweredBody &row, const MirBody &mir) {
    body = row.identity.address;
    source = row.identity.source;
    required = row.role == LoweredBodyRole::SourceExecutable;
    executable = false;
    block = 0;
    instruction = 0;
    add(row.role);
    add(row.identity.definition);
    add(mir.kind);
    if (!required)
      return;
    executable = true;
    type(mir.returnType);
    add(mir.returnType.kind == SemanticType::Reference ? Shape::BorrowReturn
        : mir.returnType.kind == SemanticType::Class ||
                mir.returnType.kind == SemanticType::Lambda
            ? Shape::OwnedReturn
            : Shape::DirectReturn);
    if (mir.hasCallerLocation)
      add(Shape::CallerLocation);
    for (const auto &value : mir.values) {
      type(value.info.type);
      traits(value.info.traits, value.info.type);
    }
    for (const auto &place : mir.places) {
      add(place.root);
      add(place.access);
      type(place.type);
      traits(place.traits, place.type);
      for (const auto &projection : place.projections)
        add(projection.kind);
      if (place.foreignStorage)
        add(Shape::NativeCall);
    }
    for (const auto &loan : mir.loans) {
      add(loan.kind);
      add(loan.access);
      if (loan.referenceSelection)
        add(Shape::SelectedReferenceAddress);
      if (loan.twoPhase)
        add(Shape::TwoPhaseBorrow);
      if (!loan.additionalDependencies.empty())
        add(Shape::MultipleBorrowOrigins);
      if (!loan.conditions.empty())
        add(Shape::ConditionalBorrow);
      for (const auto &dependency : loan.additionalDependencies) {
        add(dependency.access);
        if (!dependency.conditions.empty())
          add(Shape::ConditionalBorrow);
      }
    }
    for (const auto &drop : mir.dropObligations) {
      add(drop.kind);
      cleanup(drop.dropType);
    }
    for (const auto &boundary : mir.cleanupBoundaries)
      add(boundary.kind);
    for (const auto &record : mir.failureRecords)
      add(record.kind);

    std::map<std::pair<MirBlockId, MirInstructionId>, const SourceSpan *> spans;
    for (const auto &location : row.sourceLocations)
      spans.emplace(std::pair(location.block, location.instruction),
                    &location.source);
    for (const auto &mirBlock : mir.blocks) {
      block = mirBlock.id;
      for (const auto &stage : mirBlock.instructions) {
        instruction = stage.id;
        const auto found = spans.find({block, instruction});
        source = found == spans.end() ? row.identity.source : *found->second;
        if (stage.tombstone) {
          add(Shape::Tombstone);
          continue;
        }
        add(stage.kind);
        if (stage.callInputRole) {
          add(*stage.callInputRole);
          add(stage.callInputKind);
        }
        if (stage.operation != MirOperation::None)
          add(stage.operation);
        if (stage.intrinsic != IntrinsicKind::None)
          add(stage.intrinsic);
        if (stage.receiver)
          operand(*stage.receiver);
        for (const auto &input : stage.operands)
          operand(input);
        for (const auto &type : stage.parameterTypes)
          this->type(type);
        for (const auto &type : stage.closureCaptureTypes)
          this->type(type);
        for (auto mode : stage.closureCaptureModes)
          add(mode);
        if (stage.objectLayoutType)
          type(*stage.objectLayoutType);
        if (stage.nativeCalleeType) {
          type(*stage.nativeCalleeType);
          add(Shape::NativeCall);
          add(Shape::IndirectCall);
        }
        if (stage.dispatch == CallDispatch::Virtual)
          add(Shape::VirtualCall);
        if (stage.functionTarget || stage.constructorTarget ||
            stage.lambdaTarget)
          add(stage.dispatch);
        if (stage.constructorTarget)
          add(stage.constructorKind);
        if (stage.lambdaTarget || stage.functionPointerFunctionTarget ||
            stage.functionPointerLambdaTarget)
          add(Shape::IndirectCall);
        if (stage.nativeCallbackAdapter)
          add(Shape::NativeCallback);
        if (stage.threadTaskAdapter)
          add(Shape::ThreadTask);
        if (!stage.scopedThreadTasks.empty())
          add(Shape::ScopedThreadTasks);
        if (stage.preparedParameterDrop)
          add(Shape::PreparedOwnerInput);
        if (stage.successResultDrop || stage.successResultDestination)
          add(Shape::SuccessResultPublication);
        if (stage.tracksCaller || stage.callerLocation ||
            stage.sourceLocation || stage.forwardsCallerLocation)
          add(Shape::CallerLocation);
        if (stage.rawMemoryAccess)
          add(Shape::RawMemory);
        if (stage.unsafeOperation != UnsafeOperationKind::None)
          add(stage.unsafeOperation);
        if (stage.payloadExtractionKind)
          add(*stage.payloadExtractionKind);
        if (stage.callableBoundary)
          add(*stage.callableBoundary);
        if (stage.callableInvocation)
          add(*stage.callableInvocation);
        if (stage.synchronization.kind != SynchronizationOperationKind::None) {
          add(stage.synchronization.kind);
          if (stage.synchronization.order)
            add(*stage.synchronization.order);
          if (stage.synchronization.failureOrder)
            add(*stage.synchronization.failureOrder);
        }
        if (!stage.definedFailure.empty()) {
          add(Shape::CheckedFailure);
          add(stage.definedFailure.propagation);
          for (const auto &origin : stage.definedFailure.localOrigins)
            for (const auto &outcome : origin.outcomes) {
              add(outcome.code);
              add(outcome.detail);
            }
        }
        for (const auto &event : stage.lifecycle)
          lifecycle(event);
        if (stage.elementCleanup)
          cleanup(*stage.elementCleanup);
        if (stage.replacementCleanup)
          cleanup(*stage.replacementCleanup);
        for (const auto &proof : stage.safetyDischarges)
          add(proof.family);
        if (stage.literalProvenance.kind != MirLiteralProvenanceKind::None)
          add(stage.literalProvenance.kind);
      }
      instruction = 0;
      const auto found = spans.find({block, 0});
      source = found == spans.end() ? row.identity.source : *found->second;
      add(mirBlock.terminator.kind);
      if (mirBlock.terminator.kind == MirTerminatorKind::Invoke)
        add(Shape::FailureReturn);
      if (mirBlock.terminator.value)
        operand(*mirBlock.terminator.value);
      for (const auto &argument : mirBlock.terminator.targetArguments)
        operand(argument);
      for (const auto &argument : mirBlock.terminator.elseArguments)
        operand(argument);
      for (const auto &target : mirBlock.terminator.switchTargets)
        for (const auto &argument : target.arguments)
          operand(argument);
      for (const auto &event : mirBlock.terminator.successLifecycle)
        lifecycle(event);
      for (const auto &parameter : mirBlock.parameters)
        type(parameter.type);
    }
  }

private:
  using Key = std::tuple<std::string_view, std::string_view, bool, MirBodyKind,
                         std::size_t, bool, bool>;
  std::vector<LlvmCoverageUse> &uses;
  std::map<Key, std::size_t> indices;
};

} // namespace

LlvmAdmissionPlan::LlvmAdmissionPlan(VerifiedLoweredProgram program,
                                     LlvmBackendOptions options)
    : input(std::move(program)), policy(options) {}

const LlvmCoverageUse *LlvmAdmissionPlan::firstUnsupported() const {
  const LlvmCoverageUse *fallback = nullptr;
  for (const auto &use : uses) {
    if (!use.required)
      continue;
    const auto &entry = *use.entry;
    const bool unsupported =
        entry.state == LlvmCoverageState::Pending ||
        (use.executable &&
         entry.nonExecuting == LlvmNonExecutingRule::MustBeAbsent);
    if (!unsupported)
      continue;
    if (fallback == nullptr)
      fallback = &use;
    if (use.body && use.instruction != 0 && !use.source.source.empty())
      return &use;
  }
  return fallback;
}

std::string LlvmAdmissionPlan::limitation() const {
  const auto *use = firstUnsupported();
  if (use == nullptr)
    return {};
  std::ostringstream message;
  message << "executable LLVM lowering is not implemented yet for "
          << use->entry->key() << " (O-LLVM-" << std::setfill('0')
          << std::setw(2) << use->entry->milestone << ')';
  if (use->body)
    message << " in " << coverage(use->body->kind).name << '/'
            << use->body->owner;
  if (use->block != 0)
    message << ", block " << use->block;
  if (use->instruction != 0)
    message << ", instruction " << use->instruction;
  return message.str();
}

LlvmAdmissionPlan planLlvmAdmission(VerifiedLoweredProgram program,
                                    LlvmBackendOptions options) {
  LlvmAdmissionPlan plan(std::move(program), options);
  const auto &input = plan.program();
  const auto &mir = input.mir();
  Inventory inventory(plan.uses);
  inventory.add(Shape::ObjectArtifact);
  inventory.add(input.programKind());
  inventory.add(mir.executionProfile());
  for (const auto &body : input.bodies()) {
    const auto *bodyMir = findMirBody(mir, body.identity.address);
    if (bodyMir == nullptr)
      throw std::logic_error("verified LLVM input lost an inventoried body");
    if (body.role == LoweredBodyRole::SourceExecutable)
      plan.bodies.push_back(body.identity.address);
    inventory.inspect(body, *bodyMir);
  }
  inventory.body.reset();
  inventory.block = 0;
  inventory.instruction = 0;
  inventory.required = true;
  inventory.executable = true;
  for (const auto &instance : mir.classInstances()) {
    const auto *row = input.findClassInstance(instance.id);
    inventory.source = row == nullptr ? SourceSpan{} : row->source;
    inventory.type(instance.type);
    inventory.traits(instance.traits, instance.type);
    inventory.add(instance.kind);
    if (instance.compilerCapability != CompilerCapabilityTypeKind::None)
      inventory.add(instance.compilerCapability);
    if (!instance.declaredFields.empty())
      inventory.add(Shape::ClassFields);
    for (const auto &field : instance.declaredFields)
      inventory.type(field.type);
    if (!instance.bases.empty() || !instance.structuralBases.empty())
      inventory.add(Shape::ClassBases);
    if (instance.polymorphic)
      inventory.add(Shape::PolymorphicClass);
    if (instance.cAbiLayout)
      inventory.add(Shape::NativeRecord);
    if (instance.unionLayout)
      inventory.add(Shape::NativeUnion);
    if (instance.cOpaqueHandle)
      inventory.add(Shape::OpaqueNativeHandle);
  }
  for (const auto &function : mir.functionInstances()) {
    inventory.body = MirBodyAddress{MirBodyKind::Function, function.id};
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    inventory.add(function.definitionKind);
    inventory.add(function.linkage);
    inventory.add(function.entryKind);
    if (function.owner)
      inventory.add(function.receiverMutability);
    inventory.type(function.returnType);
    for (const auto &type : function.parameterTypes)
      inventory.type(type);
    for (const auto &parameter : function.callableParameters) {
      inventory.type(parameter.callableType);
      for (const auto &signature : parameter.signatures) {
        inventory.type(signature.returnType);
        for (const auto &type : signature.parameterTypes)
          inventory.type(type);
        inventory.add(signature.requiredCapability);
        if (signature.selectedCapability)
          inventory.add(*signature.selectedCapability);
        if (signature.nativeCalleeType)
          inventory.type(*signature.nativeCalleeType);
      }
    }
    if (function.cThreadOperation != CThreadRuntimeOperation::None)
      inventory.add(function.cThreadOperation);
    if (function.processMemoryOperation != ProcessMemoryRuntimeOperation::None)
      inventory.add(function.processMemoryOperation);
    if (function.definitionKind == MirDefinitionKind::RuntimeBinding)
      inventory.add(Shape::RuntimeBinding);
    if (function.definitionKind == MirDefinitionKind::Declaration)
      inventory.add(Shape::LibraryDeclaration);
    if (function.linkage == LanguageLinkage::C)
      inventory.add(Shape::NativeCall);
    if (function.mayRaiseDefinedFailure)
      inventory.add(Shape::FailureReturn);
    if (function.receiverMutability != ReceiverMutability::ReadOnly)
      inventory.add(Shape::MutableReceiver);
  }
  inventory.body.reset();
  for (const auto &storage : mir.foreignStorageDeclarations()) {
    inventory.type(storage.type);
    inventory.add(Shape::NativeCall);
  }
  for (const auto &payload : mir.payloadEnums()) {
    inventory.type(payload.type);
    inventory.add(Shape::PayloadEnum);
    for (const auto &variant : payload.variants)
      for (const auto &type : variant.payloadTypes)
        inventory.type(type);
  }
  for (const auto &storage : mir.threadLocalStorage()) {
    inventory.body =
        MirBodyAddress{MirBodyKind::ThreadLocalInitializer, storage.symbol};
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    inventory.type(storage.type);
    inventory.add(storage.dataInitializer ? Shape::ThreadLocalPassive
                                          : Shape::ThreadLocalDynamic);
    if (storage.requiresActiveCleanup)
      inventory.cleanup(storage.cleanup);
  }
  inventory.body.reset();
  inventory.source = {};
  if (!mir.programInitializationPlan().steps.empty())
    inventory.add(Shape::ProgramInitialization);
  for (const auto &step : mir.programInitializationPlan().steps) {
    inventory.add(step.dataInitialization);
    inventory.add(step.storageKind);
    inventory.add(step.role);
  }
  if (mir.hostedStartupPlan())
    for (const auto &operation : mir.hostedStartupPlan()->operations) {
      inventory.add(operation.kind);
      inventory.add(operation.failureBehavior);
    }
  if (input.embeddingPlan())
    inventory.add(Shape::EmbeddingExports);
  for (const auto &callback : mir.nativeCallbackAdapters()) {
    inventory.add(Shape::NativeCallback);
    inventory.add(callback.cThreadCallback);
  }
  for (const auto &item : input.generatedItems()) {
    inventory.add(item.identity.kind);
    inventory.add(item.sourceKind);
    if (!item.dependencies.empty())
      inventory.add(Shape::GeneratedDependencies);
  }
  // Declarations describe the source surface too, including uninstantiated
  // generics. Inventory them without demanding executable representations.
  inventory.required = false;
  inventory.executable = false;
  for (const auto &declaration : input.declarations()) {
    inventory.source = declaration.source;
    inventory.add(declaration.kind);
  }
  return plan;
}

} // namespace lang
