#include "atomic_types.h"
#include "gti/llvm_admission.h"
#include "gti/mir_effects.h"

#include "../mir_verification_queries.h"
#include "closure_environment.h"
#include "constructor_field_publication.h"
#include "coverage.h"
#include "dispatch.h"
#include "embedding.h"
#include "lifetimes.h"
#include "memory_types.h"
#include "mutexes.h"
#include "native_c_contract.h"
#include "parameter_packs.h"
#include "placement.h"
#include "polymorphic_storage.h"
#include "reachability.h"
#include "text.h"
#include "thread_lifecycle.h"
#include "threads.h"

#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lang {
namespace {

using llvm_detail::coverage;
using llvm_detail::Shape;

// Representation eligibility, not source type checking: later storage families
// cannot enter the passive-array implementation through an enum-wide claim.
bool passiveArrayElement(const SemanticType &type) {
  if (type.kind == SemanticType::Array)
    return type.arguments.size() == 1 && !type.arrayLengthParameterId &&
           passiveArrayElement(type.arguments.front());
  // Pointer storage is passive regardless of its referent. Accessing that
  // referent still requires its own typed projection and unsafe witness. A C
  // string is such an inert native address; it is compared only by address.
  // So is a C function pointer: calling one is admitted separately, against
  // its C signature.
  return (isPassiveScalarValueType(type) && type.kind != SemanticType::Void) ||
         (type.kind == SemanticType::RawPointer &&
          type.arguments.size() == 1) ||
         type.kind == SemanticType::CString ||
         type.kind == SemanticType::NativeFunction;
}

// A fixed array of counted text views, or of such arrays. Each element is a
// bitwise view, but element equality and hashing compare text, so only
// storage, transport, formation and checked element selection use this; it
// is not a passive array.
bool textViewArray(const SemanticType &type) {
  return type.kind == SemanticType::Array && type.arguments.size() == 1 &&
         !type.arrayLengthParameterId &&
         (type.arguments.front().kind == SemanticType::StringView ||
          textViewArray(type.arguments.front()));
}

bool passiveAddress(const SemanticType &type) {
  if (type.kind != SemanticType::RawPointer || type.arguments.size() != 1)
    return false;
  const auto &pointee = type.arguments.front();
  return pointee.kind == SemanticType::Void || passiveArrayElement(pointee) ||
         passiveAddress(pointee);
}

bool passiveReferenceType(const SemanticType &type) {
  return passiveArrayElement(type) || passiveAddress(type) ||
         type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString ||
         (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
          (passiveArrayElement(type.arguments.front()) ||
           passiveAddress(type.arguments.front()) ||
           type.arguments.front().kind == SemanticType::StringView));
}

bool primitiveRawIndex(const MirInstruction &stage) {
  return stage.operation == MirOperation::Index && stage.operands.size() == 2 &&
         stage.operands[0].type.kind == SemanticType::RawPointer &&
         passiveAddress(stage.operands[0].type) &&
         stage.operands[0].type.arguments.front().kind != SemanticType::Void &&
         stage.info.type == stage.operands[0].type.arguments.front() &&
         constantIntegerDomain(stage.operands[1].type).has_value();
}

bool primitivePointerArithmetic(const MirInstruction &stage) {
  if (stage.operands.size() != 2)
    return false;
  const bool reversed = stage.operation == MirOperation::PointerAdd &&
                        stage.operands[1].type.kind == SemanticType::RawPointer;
  const auto &pointer = stage.operands[reversed ? 1 : 0].type;
  if (!passiveAddress(pointer) ||
      pointer.arguments.front().kind == SemanticType::Void)
    return false;
  return stage.operation == MirOperation::PointerDifference
             ? stage.operands[1].type == pointer
             : (stage.operation == MirOperation::PointerAdd ||
                stage.operation == MirOperation::PointerSubtract) &&
                   stage.info.type == pointer &&
                   constantIntegerDomain(stage.operands[reversed ? 0 : 1].type)
                       .has_value();
}

bool primitiveRawAccess(const MirBody &body, const MirPlace &place) {
  const auto *value = body.findValue(place.value);
  return place.root == MirPlaceRootKind::Value && value &&
         passiveAddress(value->info.type) &&
         value->info.type.arguments.front().kind != SemanticType::Void &&
         place.type == value->info.type.arguments.front() &&
         place.projections.size() == 1 &&
         (place.projections.front().kind == MirProjectionKind::RawIndex ||
          place.projections.front().kind == MirProjectionKind::RawDereference);
}

// The unprojected place that owns a projected place's storage. A projected
// element introduced by a destructuring declaration carries the introduced
// name in `symbol` while its storage root carries none, so requiring equal
// symbols found no root for it. `symbol` stays authoritative for a
// symbol-rooted place, where it is the storage identity itself.
bool sameStorageRoot(const MirPlace &candidate, const MirPlace &place) {
  return candidate.projections.empty() && candidate.root == place.root &&
         candidate.binding == place.binding &&
         (candidate.symbol == place.symbol ||
          (place.root != MirPlaceRootKind::Symbol && candidate.symbol == 0)) &&
         candidate.temporary == place.temporary &&
         candidate.value == place.value && candidate.loan == place.loan;
}

bool arrayProjection(const MirBody &body, const MirPlace &place) {
  const auto root = std::find_if(
      body.places.begin(), body.places.end(),
      [&](const auto &candidate) { return sameStorageRoot(candidate, place); });
  if (root == body.places.end())
    return false;
  auto type = root->type;
  const auto &array =
      type.kind == SemanticType::Reference && type.arguments.size() == 1
          ? type.arguments.front()
          : type;
  if (array.kind != SemanticType::Array || !passiveArrayElement(array))
    return false;
  for (const auto &projection : place.projections) {
    if (!((projection.kind == MirProjectionKind::Index &&
           type.kind == SemanticType::Array) ||
          (projection.kind == MirProjectionKind::Dereference &&
           type.kind == SemanticType::Reference)))
      return false;
    auto element = type.arguments.front();
    type = std::move(element);
  }
  return type == place.type;
}

// Native representation eligibility of a value type, shared by the value
// inventory and the hierarchy metadata proofs. An owner transports a handle;
// its pointee is decided by `pointee`, so hierarchy pointees use metadata.
class NativeValues final {
public:
  NativeValues(const MirProgram &program, const TargetInfo &nativeTarget,
               std::function<bool(const SemanticType &)> pointee)
      : program(program), nativeTarget(nativeTarget),
        pointee(std::move(pointee)) {}

  bool operator()(const SemanticType &type) const {
    // `visiting` and `payloads` hold the current inline containment path. An
    // owner's pointee lives behind a handle, so it starts a new path, and a
    // class or payload enum reached again beneath an owner (a node owning its
    // successor) is decided by its enclosing check rather than rejected as an
    // inline cycle.
    std::set<HirClassInstanceId> visiting;
    std::set<const MirPayloadEnum *> payloads;
    std::set<HirClassInstanceId> crossed;
    std::set<const MirPayloadEnum *> crossedPayloads;
    const auto supported = [&](const auto &self,
                               const SemanticType &selected) -> bool {
      if (passiveReferenceType(selected) || passiveAddress(selected) ||
          selected == SemanticType::Void ||
          selected == SemanticType::StringView ||
          selected == SemanticType::CString ||
          selected == SemanticType::NullPtr)
        return true;
      if (selected.kind == SemanticType::FunctionItem)
        return selected.hasFunctionItemShape();
      if (selected.kind == SemanticType::FunctionSignature)
        return true;
      if (selected.kind == SemanticType::NativeFunction)
        return native_llvm::nativeCFunctionValueContract(program, nativeTarget,
                                                        selected);
      if (selected.kind == SemanticType::Lambda) {
        const auto environment =
            native_llvm::closureEnvironmentLayout(program, selected);
        if (!environment ||
            environment->captureTypes.size() !=
                environment->captureModes.size() ||
            environment->captureTypes.size() !=
                environment->captureRequiresActiveCleanup.size())
          return false;
        return std::all_of(
            environment->captureTypes.begin(), environment->captureTypes.end(),
            [&](const auto &capture) { return self(self, capture); });
      }
      if (selected.kind == SemanticType::Reference ||
          selected.kind == SemanticType::RawPointer)
        return selected.arguments.size() == 1 &&
               selected.arguments[0].kind != SemanticType::Unknown &&
               selected.arguments[0].kind != SemanticType::TypeParameter;
      if (selected.kind == SemanticType::Enum) {
        if (const auto *payload = program.findPayloadEnum(selected)) {
          if (crossedPayloads.contains(payload))
            return true;
          if (!payloads.insert(payload).second)
            return false;
          bool valid = true;
          for (const auto &variant : payload->variants)
            for (const auto &field : variant.payloadTypes)
              valid = valid && self(self, field);
          payloads.erase(payload);
          return valid;
        }
        return std::any_of(
            program.integralEnums().begin(), program.integralEnums().end(),
            [&](const auto &entry) { return entry.type == selected; });
      }
      if (selected.kind == SemanticType::Array)
        return selected.arguments.size() == 1 &&
               !selected.arrayLengthParameterId &&
               self(self, selected.arguments[0]);
      if (selected.kind == SemanticType::TypePack && selected.concretePack)
        return std::all_of(
            selected.arguments.begin(), selected.arguments.end(),
            [&](const auto &element) { return self(self, element); });
      if (selected.kind == SemanticType::UniqueOwner) {
        // A polymorphic view need not be a complete object type: its owner
        // transports one handle whose complete object is found dynamically.
        if (selected.arguments.size() != 1 ||
            !native_llvm::uniqueOwnerPointee(program, selected.arguments[0]))
          return false;
        const auto &pointeeType = selected.arguments[0];
        if (pointeeType.kind == SemanticType::Class)
          for (const auto &owner : program.classInstances())
            if (owner.type == pointeeType && visiting.contains(owner.id))
              return !owner.polymorphic && !owner.abstract &&
                     owner.compilerCapability ==
                         CompilerCapabilityTypeKind::None;
        // An owner transports its handle, not the pointee's object bytes.
        // Exact hierarchy layout can therefore establish representation here;
        // construction, borrowing and cleanup retain independent use proofs.
        auto inlineClasses = std::exchange(visiting, {});
        auto inlinePayloads = std::exchange(payloads, {});
        const auto enclosingClasses = crossed;
        const auto enclosingPayloads = crossedPayloads;
        crossed.insert(inlineClasses.begin(), inlineClasses.end());
        crossedPayloads.insert(inlinePayloads.begin(), inlinePayloads.end());
        const bool valid = self(self, pointeeType);
        visiting = std::move(inlineClasses);
        payloads = std::move(inlinePayloads);
        crossed = enclosingClasses;
        crossedPayloads = enclosingPayloads;
        return valid || pointee(pointeeType);
      }
      if (selected.kind != SemanticType::Class)
        return false;
      const MirClassInstance *owner = nullptr;
      for (const auto &candidate : program.classInstances())
        if (candidate.type == selected) {
          if (owner)
            return false;
          owner = &candidate;
        }
      if (native_llvm::nativeAtomicElement(program, selected))
        return true;
      if (!owner)
        return false;
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::FunctionPointer)
        return mir_detail::functionPointerSignature(program, selected) !=
               nullptr;
      if (native_llvm::sharedOwnerRepresentation(program, *owner))
        return self(self,
                    SemanticType::uniqueOwnerOf(owner->type.arguments[0]));
      if (native_llvm::uninitializedStorageRepresentation(program, *owner)) {
        if (crossed.contains(owner->id))
          return true;
        if (!visiting.insert(owner->id).second)
          return false;
        const bool valid = self(self, owner->type.arguments[0]);
        visiting.erase(owner->id);
        return valid;
      }
      if (const auto handle =
              native_llvm::nativeThreadHandle(program, selected))
        // The handle stores runtime pointers only; a joined result lives in
        // its runtime box until take. Like an owner's pointee, the result is
        // a native value or hierarchy storage with exact dispatch metadata;
        // the box relocates and destroys it under independent use proofs.
        return !handle->resultBearing || self(self, handle->result) ||
               pointee(handle->result);
      if (owner->compilerCapability != CompilerCapabilityTypeKind::None ||
          owner->polymorphic || owner->abstract || owner->cOpaqueHandle ||
          !owner->bases.empty() || !owner->structuralBases.empty())
        return false;
      if (crossed.contains(owner->id))
        return true;
      if (!visiting.insert(owner->id).second)
        return false;
      bool valid = true;
      for (const auto &field : owner->declaredFields)
        valid = valid && self(self, field.type);
      visiting.erase(owner->id);
      return valid;
    };
    return supported(supported, type);
  }

private:
  const MirProgram &program;
  const TargetInfo &nativeTarget;
  std::function<bool(const SemanticType &)> pointee;
};

// Representation facts never grant an operation permission. Gather every
// transfer exclusion before creating a target session, then prove each exact
// operation independently. Inventory consumes these results only after they
// are final, so its per-witness deduplication cannot merge good and bad uses.
class StaticDispatchProofs final {
public:
  using BodyKey = std::pair<MirBodyKind, std::size_t>;
  using Key =
      std::tuple<MirBodyKind, std::size_t, MirBlockId, MirInstructionId>;
  struct Operation {
    std::optional<Shape> shape;
    bool transfer = false;
    bool pure = false;
    bool observation = false;
    // A destructor that observes its own phase under the MIR-recorded
    // lifecycle-dispatch transition.
    bool lifecycle = false;
  };

  StaticDispatchProofs(const LlvmAdmissionPlan &plan,
                       VerifiedLoweredProgram input)
      : plan(plan), input(std::move(input)), program(plan.program().mir()),
        natives(plan.program().mir(), plan.program().target(),
                [this](const SemanticType &pointee) {
                  return pointeeMetadata(pointee) || mutexPointee(pointee);
                }) {
    for (const auto address : plan.requiredBodies()) {
      const auto &body = *findMirBody(program, address);
      native_llvm::Placement placement(body);
      auto &freshTemporaries = temporaries[&body];
      auto &hiddenTemporaries = hidden[&body];
      for (const auto &block : body.blocks)
        for (const auto &stage : block.instructions) {
          if (stage.tombstone || !stage.result)
            continue;
          if (freshStorage(address, body, placement, stage) ==
              native_llvm::FreshObjectStorage::Temporary)
            freshTemporaries.insert(*stage.result);
          // A fresh object published in the hidden temporary MIR
          // materializes for it is borrowed through that place.
          if (const auto place = native_llvm::freshConstructionDestination(
                  program, address, body, placement, stage))
            if (const auto *selected = body.findPlace(*place);
                selected && selected->root == MirPlaceRootKind::Temporary)
              hiddenTemporaries.insert(*place);
        }
      auto &bodyProof = bodies[{address.kind, address.owner}];
      // A hierarchy result constructed directly in the caller's destination
      // by every return is publication, not a by-value transfer.
      bodyProof.transfer =
          contains(body.returnType) && !freshReturns(address, body, placement);
      // A by-value hierarchy parameter is an object its caller published in
      // the indirect argument storage; each caller's argument is proved there.
      // A consuming receiver is likewise the address of an object its caller
      // owns and keeps for cleanup; the body's own operations carry proofs.
      if (address.kind == MirBodyKind::Constructor) {
        const auto &target = *program.findConstructorInstance(address.owner);
        // An abstract phase state selects pure members; it is admitted only
        // when nothing running in that phase can observe it.
        if (target.dispatchState)
          bodyProof.pure = pureState(target.dispatchState->activeClass) &&
                           constructionObserves(address);
      }
      // A destructor observing its phase runs under the MIR-recorded
      // transition to its own class; only an unrecorded one stays gated.
      if (address.kind == MirBodyKind::Destructor) {
        const auto &target = *program.findDestructorInstance(address.owner);
        if (contains(program.findClassInstance(target.owner)->type) &&
            observesDispatch(address)) {
          bodyProof.lifecycle = recordedPhase(target);
          bodyProof.observation = !bodyProof.lifecycle;
        }
      }
      // A concrete parameter pack's hierarchy elements are their own class
      // places; the pack binding itself transports no object bytes.
      // A fixed array of hierarchy storage is a container of complete
      // element objects; each operation on it carries its own proof.
      for (const auto &place : body.places)
        if (contains(place.type) &&
            ((place.type.kind != SemanticType::Class &&
              !(place.type.kind == SemanticType::TypePack &&
                place.type.concretePack &&
                place.root == MirPlaceRootKind::Binding &&
                place.projections.empty()) &&
              !hierarchyContainer(place.type)) ||
             (place.root == MirPlaceRootKind::Symbol &&
              !programObject(place)) ||
             place.foreignStorage ||
             (place.type.kind != SemanticType::Class && place.capture)))
          bodyProof.transfer = true;
      for (const auto &drop : body.dropObligations) {
        bodyProof.pure |= pureCleanup(drop.dropType);
        bodyProof.observation |= cleanupObserves(drop.dropType);
      }
      for (const auto &block : body.blocks) {
        for (const auto &stage : block.instructions) {
          if (stage.tombstone)
            continue;
          auto &proof = operations[key(address, block.id, stage.id)];
          const bool fresh = freshConstruction(address, body, placement, stage);
          const bool published =
              freshStorage(address, body, placement, stage).has_value();
          const bool transfer = freshTransfer(body, stage) &&
                                publishedElement(address, body, placement,
                                                 stage);
          const bool initialization =
              freshInitialization(address, body, placement, stage);
          const bool declaration = declarationOnlyField(address, body, stage);
          const bool materialization = materializedTemporary(body, stage);
          // An assignment replacing a whole hierarchy object and its paired
          // cleanup are one proved operation, not a byte transport.
          const bool replaced =
              replacement(address, body, placement, stage) ||
              replacement(address, body, placement,
                          pairedAssignment(body, stage));
          if (contains(stage.info.type)) {
            if ((stage.kind == MirInstructionKind::Construct && !fresh &&
                 !published && !transfer) ||
                (stage.kind == MirInstructionKind::Move && !transfer &&
                 !materialization) ||
                (stage.kind == MirInstructionKind::Load && !transfer &&
                 !placeObservation(body, stage) && !materialization &&
                 !replacementCopy(address, body, placement, stage)) ||
                (stage.kind == MirInstructionKind::Assign && !replaced) ||
                (stage.kind == MirInstructionKind::CallInput &&
                 stage.callInputKind != HirCallInputKind::ReadBorrow &&
                 stage.callInputKind != HirCallInputKind::MutableBorrow &&
                 !temporaryBorrow(body, stage) &&
                 !publishedArgument(address, body, placement, stage) &&
                 !consumedReceiver(body, stage) &&
                 stage.callInputKind != HirCallInputKind::RetainedReceiver) ||
                (stage.kind == MirInstructionKind::Call &&
                 !borrowedCallResult(body, stage) && !published) ||
                (stage.operation == MirOperation::Aggregate && !fresh &&
                 !published) ||
                (stage.kind == MirInstructionKind::Initialize &&
                 !initialization && !declaration))
              proof.transfer = true;
          }
          if (stage.destination &&
              (stage.kind == MirInstructionKind::Initialize ||
               stage.kind == MirInstructionKind::Assign ||
               stage.kind == MirInstructionKind::Move ||
               stage.kind == MirInstructionKind::CallInput ||
               stage.kind == MirInstructionKind::CleanupBeforeAssign))
            if (const auto *place = body.findPlace(*stage.destination))
              if (contains(place->type) &&
                  stage.kind != MirInstructionKind::InitializeBase &&
                  !(stage.kind == MirInstructionKind::Initialize &&
                    (initialization || declaration)) &&
                  !publishedArgument(address, body, placement, stage) &&
                  !replaced)
                proof.transfer = true;
          const auto transferred = [&](const MirOperand &operand) {
            return contains(operand.type) &&
                   (operand.kind == MirOperandKind::Copy ||
                    operand.kind == MirOperandKind::Move);
          };
          // A place-category Load that only names an inline slot for its
          // operations observes that slot's address, and a hidden temporary's
          // materialization names that object in place; nothing moves.
          // Extracting a field without hierarchy storage from a payload
          // enum transports only that field, never a hierarchy object.
          const bool plainField =
              stage.operation == MirOperation::PayloadExtract &&
              !contains(stage.info.type);
          if (!placeObservation(body, stage) && !materialization && !replaced &&
              !replacementCopy(address, body, placement, stage) && !plainField)
            for (const auto &operand : stage.operands)
              proof.transfer |= transferred(operand) && !transfer;
          if (stage.receiver)
            proof.transfer |= transferred(*stage.receiver);
          if (stage.replacementCleanup &&
              contains(stage.replacementCleanup->type) && !replaced)
            proof.transfer = true;
          // A typed destruction runs the element's exact recipe in place;
          // only other element-cleanup consumers would transport the object.
          if (stage.elementCleanup && contains(stage.elementCleanup->type) &&
              stage.intrinsic != IntrinsicKind::ObjectDestroy)
            proof.transfer = true;
          // A base's phase belongs to its selected constructor's own proof;
          // a generated empty base runs no code under its state.
          if (stage.kind == MirInstructionKind::Construct)
            if (stage.info.type.kind == SemanticType::Class &&
                contains(stage.info.type))
              proof.pure = pureState(instance(stage.info.type).id);
          if (stage.replacementCleanup)
            proof.pure |= pureCleanup(*stage.replacementCleanup);
          if (stage.kind == MirInstructionKind::Drop)
            for (const auto &event : stage.lifecycle)
              if (const auto *drop = body.findDropObligation(event.source))
                proof.observation |= cleanupObserves(drop->dropType);
          // A dynamic owner selects this complete cleanup at drop time.
          if (const auto owned =
                  native_llvm::dynamicOwnerAllocation(program, stage))
            if (const auto complete = mirUniqueCleanupType(
                    program, program.findClassInstance(*owned)->type))
              proof.observation |= cleanupObserves(*complete);
        }
        auto &edge = operations[key(address, block.id, 0)];
        const auto argument = [&](const MirOperand &value) {
          edge.transfer |= contains(value.type);
        };
        if (block.terminator.value &&
            block.terminator.kind == MirTerminatorKind::Return &&
            !referenceReturn(body, block.terminator) &&
            !freshReturn(address, body, placement, *block.terminator.value))
          argument(*block.terminator.value);
        for (const auto &value : block.terminator.targetArguments)
          argument(value);
        for (const auto &value : block.terminator.elseArguments)
          argument(value);
        for (const auto &target : block.terminator.switchTargets)
          for (const auto &value : target.arguments)
            argument(value);
        for (const auto &parameter : block.parameters)
          edge.transfer |= contains(parameter.type);
      }
    }
    // All exclusions above are known before any target-machine construction.
    for (const auto address : plan.requiredBodies()) {
      auto &proof = bodies[{address.kind, address.owner}];
      proof.shape = proveBody(address);
    }
    for (const auto address : plan.requiredBodies()) {
      const auto &body = *findMirBody(program, address);
      native_llvm::Placement placement(body);
      for (const auto &block : body.blocks)
        for (const auto &stage : block.instructions)
          if (!stage.tombstone)
            operations[key(address, block.id, stage.id)].shape =
                proveOperation(address, body, placement, stage);
    }
  }

  bool contains(const SemanticType &type) const {
    try {
      return native_llvm::containsHierarchyStorage(program, type);
    } catch (const BackendUnsupportedError &) {
      // The existing inventory still owns this unsupported representation.
      // An unknown result supplies no NEW dispatch witness or transfer fact.
      return false;
    }
  }
  bool referent(const SemanticType &type) const {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
           (type.arguments.front().kind == SemanticType::Class ||
            (type.arguments.front().kind == SemanticType::Enum &&
             program.findPayloadEnum(type.arguments.front()))) &&
           contains(type.arguments.front());
  }
  bool related(const SemanticType &type) const {
    return contains(type) || referent(type) ||
           (type.kind == SemanticType::RawPointer &&
            type.arguments.size() == 1 && contains(type.arguments.front()));
  }
  const Operation *operation(MirBodyAddress body, MirBlockId block,
                             MirInstructionId instruction) const {
    const auto found = operations.find(key(body, block, instruction));
    return found == operations.end() ? nullptr : &found->second;
  }
  const Operation *body(MirBodyAddress address) const {
    const auto found = bodies.find({address.kind, address.owner});
    return found == bodies.end() ? nullptr : &found->second;
  }
  // Whether the generated typed relocation accepts this complete layout and
  // dispatch graph. The preflight publishes no helper and no declaration.
  bool generatedRelocation(const SemanticType &type) {
    if (!planner())
      return false;
    try {
      native_llvm::Lifetimes::preflightGeneratedRelocation(*session, *dispatch,
                                                           type);
      return true;
    } catch (const BackendUnsupportedError &) {
      return false;
    }
  }
  // A hierarchy object in program storage that its program initialization
  // step constructs in place: a complete object, never thread-local and, by
  // the program-storage rules, without source or owning cleanup.
  bool programStep(const MirProgramInitializationStep &step) const {
    const MirBodyAddress module{MirBodyKind::Module, 0};
    const auto *body = findMirBody(program, module);
    const auto *place = body ? body->findPlace(step.storagePlace) : nullptr;
    if (!place || place->type.kind != SemanticType::Class ||
        step.requiresActiveCleanup || step.nativeRetirement ||
        program.findThreadLocalStorage(step.symbol) ||
        step.role != ProgramInitializationStepRole::Initializer)
      return false;
    native_llvm::Placement placement(*body);
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.id == step.storageInitialization)
          return stage.destination == step.storagePlace &&
                 freshInitialization(module, *body, placement, stage);
    return false;
  }
  bool programObject(const MirPlace &place) const {
    if (place.root != MirPlaceRootKind::Symbol || !place.symbol ||
        place.capture || place.foreignStorage)
      return false;
    for (const auto &step : program.programInitializationPlan().steps)
      if (step.symbol == place.symbol)
        return programStep(step);
    return false;
  }
  std::optional<Shape> metadata(const SemanticType &type) {
    // A raw pointer to hierarchy storage is an address; its accesses keep
    // their own raw-place proofs.
    if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1 &&
        type.arguments.front().kind == SemanticType::Class &&
        contains(type.arguments.front()))
      return metadata(type.arguments.front())
                 ? std::optional(Shape::NativeBorrowedDispatchView)
                 : std::nullopt;
    if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1 &&
        type.arguments.front().kind == SemanticType::Array)
      return metadata(type.arguments.front());
    // A reference to a payload enum holding hierarchy storage is a view.
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
        type.arguments.front().kind == SemanticType::Enum)
      return metadata(type.arguments.front())
                 ? std::optional(Shape::NativeBorrowedDispatchView)
                 : std::nullopt;
    // A payload enum holds one complete object per payload field of its
    // active alternative; every alternative's storage must be represented.
    if (type.kind == SemanticType::Enum) {
      const auto *payload = program.findPayloadEnum(type);
      if (!payload || !contains(type))
        return {};
      for (const auto &variant : payload->variants)
        for (const auto &field : variant.payloadTypes) {
          if (!contains(field)) {
            if (!natives(field))
              return {};
            continue;
          }
          const auto shape = metadata(field);
          if (shape != Shape::NativeStaticDispatchStorage &&
              shape != Shape::NativeHierarchyArray &&
              shape != Shape::NativeHierarchyPayload)
            return {};
        }
      try {
        static_cast<void>(session->layouts().payload(type));
      } catch (const BackendUnsupportedError &) {
        return {};
      }
      return Shape::NativeHierarchyPayload;
    }
    // A fixed array of hierarchy storage holds complete element objects.
    if (type.kind == SemanticType::Array) {
      if (type.arguments.size() != 1 || type.arrayLengthParameterId ||
          !contains(type))
        return {};
      const auto element = metadata(type.arguments.front());
      if (element != Shape::NativeStaticDispatchStorage &&
          element != Shape::NativeHierarchyArray)
        return {};
      try {
        static_cast<void>(session->layouts().get(type));
      } catch (const BackendUnsupportedError &) {
        return {};
      }
      return Shape::NativeHierarchyArray;
    }
    const auto &selected = referent(type) ? type.arguments.front() : type;
    if (selected.kind != SemanticType::Class || !contains(selected))
      return {};
    try {
      std::set<HirClassInstanceId> path;
      if (!metadataClass(instance(selected), path) || !planner())
        return {};
      static_cast<void>(session->layouts().get(selected));
      return referent(type) || instance(selected).kind == ClassKind::Interface
                 ? Shape::NativeBorrowedDispatchView
                 : Shape::NativeStaticDispatchStorage;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  std::optional<Shape> declaration(const MirFunctionInstance &function) {
    const MirBodyAddress address{MirBodyKind::Function, function.id};
    const auto *row = plan.program().findBody(address);
    if (program.findFunctionInstance(function.id) != &function ||
        function.definitionKind != MirDefinitionKind::Declaration ||
        !function.pureVirtual || !function.virtualMethod ||
        function.staticMember || function.linkage != LanguageLinkage::Gti ||
        !function.owner ||
        std::any_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(),
                    [](const auto &type) { return type == SemanticType::Void; }) ||
        !row || row->role != LoweredBodyRole::AbiDeclaration ||
        std::find(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                  address) != plan.requiredBodies().end())
      return {};
    const auto *owner = program.findClassInstance(*function.owner);
    try {
      if (!owner || !metadata(owner->type))
        return {};
      std::optional<native_llvm::VirtualRootAbi> common;
      for (const auto &selection : owner->virtualDispatch) {
        if (selection.target != function.id)
          continue;
        const auto *root = program.findFunctionInstance(selection.root);
        if (!root || !root->owner || root->returnType != function.returnType ||
            root->parameterTypes != function.parameterTypes ||
            root->receiverMutability != function.receiverMutability ||
            std::find(function.virtualRoots.begin(),
                      function.virtualRoots.end(),
                      root->declaration) == function.virtualRoots.end())
          return {};
        const auto path =
            session->layouts().basePath(owner->type, selection.rootPath);
        if (session->layouts().instance(path.type).id != *root->owner)
          return {};
        const auto abi = dispatch->rootAbi(root->id);
        if (common && common->call != abi.call)
          return {};
        common = abi;
      }
      // A declaration supplies a typed root contract only. It is never an
      // executable source demand or permission to install a pure active state.
      return common ? std::optional(Shape::NativeBorrowedDispatchView)
                    : std::nullopt;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  std::optional<Shape> cleanup(const MirDropType &recipe,
                               bool replacement = false) {
    if (!contains(recipe.type) || cleanupObserves(recipe))
      return {};
    try {
      if (!metadata(recipe.type) || !planner())
        return {};
      native_llvm::Lifetimes::preflightCleanup(*session, *dispatch, recipe,
                                               replacement);
      return recipe.type.kind == SemanticType::Array
                 ? Shape::NativeHierarchyArray
             : recipe.type.kind == SemanticType::Enum
                 ? Shape::NativeHierarchyPayload
                 : Shape::NativeStaticDispatchStorage;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  // An owner of a polymorphic view: its complete cleanup is selected through
  // the installed descriptor, proved where each complete object is allocated.
  std::optional<Shape> dynamicOwner(const MirDropType &recipe) {
    if (!recipe.polymorphicCleanup ||
        recipe.type.kind != SemanticType::UniqueOwner ||
        recipe.type.arguments.size() != 1 ||
        recipe.type.arguments.front().kind != SemanticType::Class)
      return {};
    try {
      if (!metadata(recipe.type.arguments.front()) || !planner())
        return {};
      native_llvm::Lifetimes::preflightCleanup(*session, *dispatch, recipe);
      return Shape::NativePolymorphicOwnership;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  std::optional<Shape> borrowed(const MirBody &body,
                                const MirOperand &operand) {
    // A borrow of a payload enum holding hierarchy storage names that enum;
    // a payload is selected only by its own checked extraction.
    if ((operand.kind == MirOperandKind::BorrowRead ||
         operand.kind == MirOperandKind::BorrowWrite ||
         operand.kind == MirOperandKind::Address) &&
        unreference(operand.type).kind == SemanticType::Enum) {
      const auto &target = unreference(operand.type);
      const auto *place = body.findPlace(operand.place);
      if (!program.findPayloadEnum(target) || !place ||
          place->type != target || place->foreignStorage || place->capture ||
          !payloadPlace(body, *place))
        return {};
      const bool mut = operand.kind == MirOperandKind::BorrowWrite ||
                       (operand.type.kind == SemanticType::Reference &&
                        operand.type.referenceAccess == AccessMode::Mutable);
      if (mut && place->access != AccessMode::Mutable)
        return {};
      return metadata(target);
    }
    if (operand.kind == MirOperandKind::Value &&
        unreference(operand.type).kind == SemanticType::Enum) {
      const auto *staged = stagedBorrow(body, operand);
      return staged ? borrowed(body, staged->operands.front())
                    : std::nullopt;
    }
    // The checked address of a hierarchy array place names that storage;
    // its elements are selected by their own checked projections.
    if (operand.kind == MirOperandKind::Address &&
        operand.type.kind == SemanticType::Array) {
      const auto *place = body.findPlace(operand.place);
      if (!place || place->type != operand.type || place->foreignStorage ||
          place->capture ||
          (place->root != MirPlaceRootKind::Binding &&
           place->root != MirPlaceRootKind::This &&
           place->root != MirPlaceRootKind::Value) ||
          std::any_of(place->projections.begin(), place->projections.end(),
                      [](const auto &projection) {
                        return projection.kind != MirProjectionKind::Field &&
                               projection.kind != MirProjectionKind::Index;
                      }))
        return {};
      return metadata(operand.type);
    }
    if (operand.kind == MirOperandKind::Value) {
      const auto *staged = stagedBorrow(body, operand);
      if (!staged || !borrowed(body, staged->operands.front()))
        return {};
    }
    if (!borrowedSource(body, operand))
      return {};
    try {
      const auto source = *borrowedSource(body, operand);
      const auto &target = unreference(operand.type);
      if ((contains(source) && !metadata(source)) ||
          (contains(target) && !metadata(target)) || !planner())
        return {};
      if (!session->layouts().findBaseSubobject(source, target))
        return {};
      return Shape::NativeBorrowedDispatchView;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  // A selected place of a payload enum: a local, field, element, parameter
  // view or extracted payload, never captured or foreign storage.
  bool payloadPlace(const MirBody &body, const MirPlace &place) const {
    if ((place.root != MirPlaceRootKind::Binding &&
         place.root != MirPlaceRootKind::This &&
         place.root != MirPlaceRootKind::Value &&
         place.root != MirPlaceRootKind::Loan) ||
        std::any_of(place.projections.begin(), place.projections.end(),
                    [](const auto &projection) {
                      return projection.kind != MirProjectionKind::Field &&
                             projection.kind != MirProjectionKind::Index &&
                             projection.kind != MirProjectionKind::Dereference;
                    }))
      return false;
    if (place.root == MirPlaceRootKind::Loan) {
      const auto *loan = body.findLoan(place.loan);
      return loan && supportedLoan(body, *loan);
    }
    return place.root != MirPlaceRootKind::Value ||
           payloadView(body, place.value);
  }
  // The place-category result of a borrowing payload extraction: a view of
  // the payload of a selected enum place, which its switch arm checked.
  bool payloadView(const MirBody &body, MirValueId value) const {
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.result == value)
          return stage.kind == MirInstructionKind::Compute &&
                 stage.operation == MirOperation::PayloadExtract &&
                 stage.payloadExtractionKind ==
                     PayloadExtractionKind::BorrowValue &&
                 stage.info.category == ValueCategory::Place &&
                 stage.operands.size() == 1 && stage.operands.front().place &&
                 body.findPlace(stage.operands.front().place) &&
                 payloadPlace(body,
                              *body.findPlace(stage.operands.front().place));
    return false;
  }
  bool borrowedUse(const MirBody &body, const MirOperand &operand) const {
    if (related(operand.type))
      return true;
    if (const auto *place = body.findPlace(operand.place))
      if (related(place->type))
        return true;
    if (const auto *loan = body.findLoan(operand.loan))
      if (const auto *place = body.findPlace(loan->source))
        return related(place->type);
    if (const auto *value = body.findValue(operand.value))
      return related(value->info.type);
    return false;
  }
  std::optional<Shape> loan(const MirBody &body, const MirLoan &loan) {
    if (!supportedLoan(body, loan))
      return {};
    const auto *source = body.findPlace(loan.source);
    if (!source)
      return {};
    auto type = unreference(source->type);
    if (const auto *producer = callResult(body, loan)) {
      // A member call on a proved dispatch view selected this non-hierarchy
      // referent; the address is the callee's checked result, not a view.
      if (!related(producer->info.type))
        return producer->receiver && borrowedUse(body, *producer->receiver) &&
                       borrowed(body, *producer->receiver)
                   ? std::optional(Shape::NativeBorrowedDispatchView)
                   : std::nullopt;
      type = producer->info.type;
    }
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.loan == loan.id &&
            stage.kind == MirInstructionKind::Borrow)
          type = unreference(stage.info.type);
    return borrowed(body,
                    {.kind = MirOperandKind::Loan,
                     .loan = loan.id,
                     .type = SemanticType::referenceTo(type, loan.access)});
  }
  std::optional<Shape> returns(const MirBody &body) {
    if (!referent(body.returnType) || !metadata(body.returnType))
      return {};
    for (const auto &block : body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return)
        continue;
      if (!referenceReturn(body, block.terminator))
        return {};
      auto presentation = *block.terminator.value;
      presentation.type = body.returnType;
      if (!borrowed(body, presentation))
        return {};
    }
    return Shape::NativeBorrowedDispatchView;
  }

private:
  const LlvmAdmissionPlan &plan;
  VerifiedLoweredProgram input;
  const MirProgram &program;
  // Non-hierarchy field representations use the same rules as values.
  NativeValues natives;
  std::vector<SemanticType> pointeeVisits;
  std::map<Key, Operation> operations;
  std::map<BodyKey, Operation> bodies;
  std::map<const MirBody *, std::set<MirValueId>> temporaries;
  std::map<const MirBody *, std::set<MirPlaceId>> hidden;
  bool attempted = false;
  std::unique_ptr<native_llvm::Session> session;
  std::unique_ptr<native_llvm::Dispatch> dispatch;

  static Key key(MirBodyAddress body, MirBlockId block,
                 MirInstructionId instruction) {
    return {body.kind, body.owner, block, instruction};
  }
  static const SemanticType &unreference(const SemanticType &type) {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1
               ? type.arguments.front()
               : type;
  }
  const MirClassInstance &instance(const SemanticType &type) const {
    const MirClassInstance *result = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == type) {
        if (result)
          throw std::logic_error("LLVM dispatch type has repeated identity");
        result = &candidate;
      }
    if (!result)
      throw BackendUnsupportedError("LLVM dispatch type has no exact class");
    return *result;
  }
  const MirFunctionInstance *function(MirBodyAddress body) const {
    return body.kind == MirBodyKind::Function
               ? program.findFunctionInstance(body.owner)
               : nullptr;
  }
  bool planner() {
    if (!attempted) {
      attempted = true;
      try {
        session = std::make_unique<native_llvm::Session>(input, plan.options());
        dispatch = std::make_unique<native_llvm::Dispatch>(*session, plan);
      } catch (const BackendUnsupportedError &) {
        session.reset();
      }
    }
    return dispatch != nullptr;
  }
  // An owner pointee needs only some representable class: an owner cycle is
  // decided by the enclosing check, as for inline value representations.
  bool pointeeMetadata(const SemanticType &pointee) {
    if (std::find(pointeeVisits.begin(), pointeeVisits.end(), pointee) !=
        pointeeVisits.end())
      return true;
    pointeeVisits.push_back(pointee);
    const bool result = metadata(pointee).has_value();
    pointeeVisits.pop_back();
    return result;
  }
  // An owner's pointee may be exact mutex storage, or an inline composition
  // containing it, whose leaves are native values or hierarchy storage with
  // metadata. The owner moves only its handle; the pointee is constructed,
  // borrowed and retired in place under the inventory's own mutex proofs.
  bool mutexPointee(const SemanticType &pointee) {
    if (std::find(pointeeVisits.begin(), pointeeVisits.end(), pointee) !=
        pointeeVisits.end())
      return false;
    pointeeVisits.push_back(pointee);
    const bool result =
        native_llvm::mutexComposition(
            program, pointee, [this](const SemanticType &leaf) {
              return natives(leaf) || metadata(leaf).has_value();
            })
            .has_value();
    pointeeVisits.pop_back();
    return result;
  }
  bool metadataClass(const MirClassInstance &owner,
                     std::set<HirClassInstanceId> &path) const {
    // A compiler-bound inline slot holds at most one object whose dynamic
    // type implements its interface; payloads are proved where constructed.
    if (native_llvm::polymorphicStorageRepresentation(program, owner)) {
      const auto &interface = instance(owner.type.arguments.front());
      if (!interface.polymorphic || !path.insert(owner.id).second)
        return false;
      const bool valid = metadataClass(interface, path);
      path.erase(owner.id);
      return valid;
    }
    // Exact mutex storage inlines its payload behind the native shell. A
    // hierarchy payload keeps this exact metadata proof; any other payload
    // uses its ordinary native value representation.
    if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
      const auto payload = native_llvm::nativeMutexElement(
          program, owner.type, owner.compilerCapability);
      if (!payload || !path.insert(owner.id).second)
        return false;
      bool valid = false;
      if (contains(*payload)) {
        const auto *leaf = &*payload;
        while (leaf->kind == SemanticType::Array &&
               leaf->arguments.size() == 1 && !leaf->arrayLengthParameterId)
          leaf = &leaf->arguments.front();
        valid = leaf->kind == SemanticType::Class &&
                metadataClass(instance(*leaf), path);
      } else {
        valid = natives(*payload);
      }
      path.erase(owner.id);
      return valid;
    }
    if ((owner.kind != ClassKind::Class && owner.kind != ClassKind::Struct &&
         owner.kind != ClassKind::Interface) ||
        owner.cAbiRecord || owner.cAbiLayout || owner.cOpaqueHandle ||
        owner.unionLayout ||
        owner.compilerCapability != CompilerCapabilityTypeKind::None ||
        !path.insert(owner.id).second)
      return false;
    // Inline hierarchy fields keep this exact metadata proof; every other
    // field uses its ordinary native value representation. Borrowed state
    // is such a field: a reference or view is an address, and its loans are
    // MIR facts with no runtime state beside the dispatch metadata.
    bool valid =
        std::all_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                    [&](const auto &field) {
                      if (contains(field.type))
                        return metadataStorage(field.type, path);
                      return field.type != SemanticType::Void &&
                             natives(field.type);
                    });
    if (owner.kind == ClassKind::Interface)
      valid &= owner.declaredFields.empty() && !owner.destructor &&
               !owner.requiresActiveCleanup && !owner.requiresActiveDropState;
    const auto base = [&](const HirBaseInstance &row) {
      const auto *selected = program.findClassInstance(row.instance);
      return selected && selected->type == row.type &&
             (!row.interface || selected->kind == ClassKind::Interface) &&
             metadataClass(*selected, path);
    };
    for (const auto &row : owner.bases)
      valid &= base(row);
    for (const auto &row : owner.structuralBases)
      valid &= base(row);
    path.erase(owner.id);
    return valid;
  }
  // Inline hierarchy storage of a field: a class, a fixed array of it, or a
  // payload enum whose alternatives hold it.
  bool metadataStorage(const SemanticType &type,
                       std::set<HirClassInstanceId> &path) const {
    if (type.kind == SemanticType::Array)
      return type.arguments.size() == 1 && !type.arrayLengthParameterId &&
             metadataStorage(type.arguments.front(), path);
    if (type.kind == SemanticType::Enum) {
      const auto *payload = program.findPayloadEnum(type);
      if (!payload)
        return false;
      for (const auto &variant : payload->variants)
        for (const auto &field : variant.payloadTypes)
          if (contains(field) ? !metadataStorage(field, path)
                              : !natives(field))
            return false;
      return true;
    }
    return type.kind == SemanticType::Class &&
           metadataClass(instance(type), path);
  }
  bool pureState(HirClassInstanceId owner) const {
    const auto *selected = program.findClassInstance(owner);
    if (!selected)
      throw std::logic_error("LLVM dispatch state lost its class");
    for (const auto &row : selected->virtualDispatch)
      if (const auto *target = program.findFunctionInstance(row.target);
          target && target->pureVirtual)
        return true;
    return false;
  }
  // Cleanup reinstalls an abstract base's phase only for its destructor;
  // that phase is observable only when the destructor observes it.
  bool pureCleanup(const MirDropType &recipe) const {
    bool pure = recipe.requiresActiveCleanup && recipe.classInstance &&
                pureState(*recipe.classInstance) && recipe.destructor &&
                observesDispatch({MirBodyKind::Destructor, *recipe.destructor});
    for (const auto &child : recipe.aggregate)
      pure |= pureCleanup(child);
    return pure;
  }
  // The lifecycle-dispatch contract MIR records for a source destructor of
  // a polymorphic class: it runs under its own class.
  bool recordedPhase(const MirDestructorInstance &destructor) const {
    return destructor.definitionKind == MirDefinitionKind::Source &&
           destructor.dispatchState &&
           destructor.dispatchState->activeClass == destructor.owner;
  }
  // A destructor that may observe its phase without a recorded transition.
  bool cleanupObserves(const MirDropType &recipe) const {
    if (recipe.destructor && contains(recipe.type) &&
        observesDispatch({MirBodyKind::Destructor, *recipe.destructor}) &&
        !recordedPhase(*program.findDestructorInstance(*recipe.destructor)))
      return true;
    return std::any_of(
        recipe.aggregate.begin(), recipe.aggregate.end(),
        [&](const auto &child) { return cleanupObserves(child); });
  }
  // Whether a body uses a view of its receiver object itself (the receiver
  // place or one of its base views, never a field) as any instruction or
  // terminator input. Fields are complete objects that outlive the body. In
  // a construction phase, initializing or rolling back a base subobject runs
  // under that base's own state and does not use this phase's view.
  bool receiverViewUsed(const MirBody &body, bool construction = false) const {
    const auto view = [&](const auto &self, MirPlaceId id) -> bool {
      const auto *place = body.findPlace(id);
      if (!place ||
          std::any_of(place->projections.begin(), place->projections.end(),
                      [](const auto &projection) {
                        return projection.kind != MirProjectionKind::Base;
                      }))
        return false;
      if (place->root == MirPlaceRootKind::This)
        return true;
      if (place->root != MirPlaceRootKind::Loan)
        return false;
      const auto *loan = body.findLoan(place->loan);
      return loan && self(self, loan->source);
    };
    const auto loanView = [&](MirLoanId id) {
      const auto *loan = body.findLoan(id);
      if (!loan)
        return false;
      if (view(view, loan->source))
        return true;
      return std::any_of(loan->additionalDependencies.begin(),
                         loan->additionalDependencies.end(),
                         [&](const auto &dependency) {
                           return dependency.source &&
                                  view(view, dependency.source);
                         });
    };
    const auto input = [&](const MirOperand &operand) {
      return (operand.place && view(view, operand.place)) ||
             (operand.loan && loanView(operand.loan));
    };
    for (const auto &block : body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        const bool phaseDestination =
            construction && (stage.kind == MirInstructionKind::InitializeBase ||
                             stage.kind == MirInstructionKind::Drop);
        if (std::any_of(stage.operands.begin(), stage.operands.end(), input) ||
            (stage.receiver && input(*stage.receiver)) ||
            (stage.destination && !phaseDestination &&
             view(view, *stage.destination)) ||
            (stage.loan && loanView(*stage.loan)) ||
            (stage.retainedOwnerPlace &&
             view(view, *stage.retainedOwnerPlace)) ||
            (stage.successResultDestination &&
             view(view, *stage.successResultDestination)))
          return true;
      }
      const auto &term = block.terminator;
      if ((term.value && input(*term.value)) ||
          (term.returnLoan && loanView(*term.returnLoan)) ||
          std::any_of(term.targetArguments.begin(), term.targetArguments.end(),
                      input) ||
          std::any_of(term.elseArguments.begin(), term.elseArguments.end(),
                      input))
        return true;
      for (const auto &target : term.switchTargets)
        if (std::any_of(target.arguments.begin(), target.arguments.end(),
                        input))
          return true;
    }
    return false;
  }
  bool observesDispatch(MirBodyAddress root) const {
    // Only the destroyed object's own dispatch state changes during its
    // destruction. A class without dispatch state has nothing to observe, and
    // a source destructor that never uses a view of the object cannot hand it
    // to any code, whatever that code dispatches on other objects.
    if (root.kind == MirBodyKind::Destructor) {
      const auto *destructor = program.findDestructorInstance(root.owner);
      const auto *owner =
          destructor ? program.findClassInstance(destructor->owner) : nullptr;
      const auto *row = plan.program().findBody(root);
      const auto *body = findMirBody(program, root);
      if (owner && row && row->role == LoweredBodyRole::SourceExecutable &&
          body &&
          (!owner->polymorphic || !receiverViewUsed(*body) ||
           !derivedFrom(owner->id)))
        return false;
    }
    return reachesDispatch(root);
  }
  // Whether any class of the sealed program has this class as a direct or
  // transitive base. A class nothing derives from is always destroyed as
  // the complete object: its destructor runs under its own concrete state,
  // the state the object had throughout its lifetime.
  bool derivedFrom(HirClassInstanceId base) const {
    for (const auto &candidate : program.classInstances())
      for (const auto *rows : {&candidate.bases, &candidate.structuralBases})
        for (const auto &row : *rows)
          if (row.instance == base)
            return true;
    return false;
  }
  // Whether code running while a constructor's own phase state is installed
  // can observe it: a use of a view of the object outside base
  // initialization and rollback whose code may dispatch or escape.
  bool constructionObserves(MirBodyAddress address) const {
    const auto *row = plan.program().findBody(address);
    const auto *body = findMirBody(program, address);
    if (!row || row->role != LoweredBodyRole::SourceExecutable || !body)
      return true;
    return receiverViewUsed(*body, true) && reachesDispatch(address);
  }
  // Conservative transitive scan: any reachable dispatch, escape or
  // unresolved call.
  bool reachesDispatch(MirBodyAddress root) const {
    std::vector<MirBodyAddress> pending{root};
    std::set<BodyKey> visited;
    while (!pending.empty()) {
      const auto address = pending.back();
      pending.pop_back();
      if (!visited.emplace(address.kind, address.owner).second)
        continue;
      const auto *row = plan.program().findBody(address);
      const auto *body = findMirBody(program, address);
      if (!row || row->role != LoweredBodyRole::SourceExecutable || !body)
        return true;
      for (const auto &block : body->blocks)
        for (const auto &stage : block.instructions) {
          if (stage.tombstone)
            continue;
          if (stage.dispatch == CallDispatch::Virtual ||
              stage.nativeCalleeType || stage.nativeCallbackAdapter ||
              stage.callableInvocation || stage.lambdaTarget ||
              stage.functionPointerFunctionTarget ||
              stage.functionPointerLambdaTarget || stage.threadTaskAdapter ||
              !stage.scopedThreadTasks.empty())
            return true;
          if (stage.functionTarget) {
            const auto *target =
                program.findFunctionInstance(*stage.functionTarget);
            if (!target || target->linkage != LanguageLinkage::Gti ||
                (target->definitionKind != MirDefinitionKind::Source &&
                 stage.intrinsic == IntrinsicKind::None))
              return true;
            if (target->definitionKind == MirDefinitionKind::Source)
              pending.push_back({MirBodyKind::Function, target->id});
          }
          if (stage.constructorTarget)
            pending.push_back(
                {MirBodyKind::Constructor, *stage.constructorTarget});
          if (stage.bodyTarget)
            pending.push_back(*stage.bodyTarget);
          if (stage.kind == MirInstructionKind::Call && !stage.functionTarget &&
              !stage.constructorTarget && !stage.bodyTarget &&
              stage.intrinsic == IntrinsicKind::None)
            return true;
        }
    }
    return false;
  }
  bool declarationOnlyField(MirBodyAddress address, const MirBody &body,
                            const MirInstruction &stage) const {
    const auto *owner = address.kind == MirBodyKind::FieldInitializers
                            ? program.findClassInstance(address.owner)
                            : nullptr;
    return owner &&
           isMirDeclarationOnlyFieldInitialization(*owner, body, stage);
  }
  std::optional<native_llvm::FreshObjectStorage>
  freshStorage(MirBodyAddress address, const MirBody &body,
               const native_llvm::Placement &placement,
               const MirInstruction &stage) const {
    if (!contains(stage.info.type))
      return {};
    return native_llvm::freshObjectStorage(program, address, body, placement,
                                           stage);
  }
  bool freshReturn(MirBodyAddress address, const MirBody &body,
                   const native_llvm::Placement &placement,
                   const MirOperand &value) const {
    if (value.kind != MirOperandKind::Value || !value.value)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.result == value.value)
          return freshStorage(address, body, placement, stage) ==
                     native_llvm::FreshObjectStorage::Returned ||
                 plannedTransfer(body, stage) ==
                     native_llvm::FreshHierarchyPublication::Returned;
    return false;
  }
  // A planned element transfer relocates into a fresh aggregate that is
  // itself published in planned storage; other transfers need no aggregate.
  bool publishedElement(MirBodyAddress address, const MirBody &body,
                        const native_llvm::Placement &placement,
                        const MirInstruction &stage) const {
    if (plannedTransfer(body, stage) !=
        native_llvm::FreshHierarchyPublication::Element)
      return true;
    const auto plan =
        native_llvm::planFreshHierarchyTransfer(program, body, stage);
    return plan.publication &&
           freshStorage(address, body, placement, *plan.publication)
               .has_value();
  }
  std::optional<native_llvm::FreshHierarchyPublication>
  plannedTransfer(const MirBody &body, const MirInstruction &stage) const {
    if (!freshTransfer(body, stage))
      return {};
    return native_llvm::planFreshHierarchyTransfer(program, body, stage)
        .publishes;
  }
  // A by-value argument published in its exact prepared storage, by a fresh
  // producer or a planned move of an owned binding.
  bool publishedArgument(MirBodyAddress address, const MirBody &body,
                         const native_llvm::Placement &placement,
                         const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::CallInput ||
        stage.callInputKind != HirCallInputKind::MoveValue ||
        stage.operands.size() != 1 ||
        stage.operands.front().kind != MirOperandKind::Value)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &producer : block.instructions)
        if (!producer.tombstone &&
            producer.result == stage.operands.front().value) {
          if (freshStorage(address, body, placement, producer) ==
              native_llvm::FreshObjectStorage::Argument)
            return true;
          return plannedTransfer(body, producer) ==
                     native_llvm::FreshHierarchyPublication::Argument &&
                 native_llvm::planFreshHierarchyTransfer(program, body,
                                                         producer)
                         .publication == &stage;
        }
    return false;
  }
  // The assignment a cleanup-before-assign stage is paired with.
  const MirInstruction &pairedAssignment(const MirBody &body,
                                         const MirInstruction &stage) const {
    if (stage.kind == MirInstructionKind::CleanupBeforeAssign &&
        stage.replacementPair)
      for (const auto &block : body.blocks)
        for (const auto &candidate : block.instructions)
          if (!candidate.tombstone && candidate.id == stage.replacementPair)
            return candidate;
    return stage;
  }
  // An assignment replacing a whole object of hierarchy storage: its paired
  // cleanup ends the previous object, and the generated relocation of a
  // fresh or moved value, or the generated copy of a checked view, publishes
  // the new one. A polymorphic object must be complete: replacing a base view
  // could change the dynamic type of an enclosing object.
  bool replacement(MirBodyAddress address, const MirBody &body,
                   const native_llvm::Placement &placement,
                   const MirInstruction &stage) {
    if (stage.kind != MirInstructionKind::Assign || !stage.destination ||
        stage.operation != MirOperation::Assign ||
        stage.operands.size() != 1 || stage.receiver || stage.loan)
      return false;
    const auto *place = body.findPlace(*stage.destination);
    const auto *payload = place && place->type.kind == SemanticType::Enum
                              ? program.findPayloadEnum(place->type)
                              : nullptr;
    if (!place ||
        (place->type.kind != SemanticType::Class && !payload) ||
        place->type != stage.info.type || !contains(place->type))
      return false;
    // Structural only: this also runs while exclusions are gathered, before
    // any target session exists. The operation proof adds the metadata. A
    // payload enum has no derived type, so every enum place is complete.
    if (!payload && instance(place->type).polymorphic &&
        !native_llvm::completeObjectPlace(program, *place))
      return false;
    // An object with active cleanup is ended by its paired cleanup stage;
    // an inert object has none.
    if (stage.replacementPair) {
      const MirInstruction *cleanup = nullptr;
      for (const auto &block : body.blocks)
        for (const auto &candidate : block.instructions)
          if (!candidate.tombstone && candidate.id == stage.replacementPair)
            cleanup = &candidate;
      if (!cleanup ||
          cleanup->kind != MirInstructionKind::CleanupBeforeAssign ||
          cleanup->destination != stage.destination ||
          cleanup->replacementPair != stage.id ||
          !cleanup->replacementCleanup ||
          cleanup->replacementCleanup->type != place->type)
        return false;
    } else if (payload ? payload->requiresActiveCleanup
                       : instance(place->type).requiresActiveCleanup) {
      return false;
    }
    const auto &source = stage.operands.front();
    if (source.kind == MirOperandKind::Value) {
      for (const auto &block : body.blocks)
        for (const auto &producer : block.instructions)
          if (!producer.tombstone && producer.result == source.value)
            return copiedView(body, producer, place->type) ||
                   freshStorage(address, body, placement, producer) ==
                       native_llvm::FreshObjectStorage::Temporary ||
                   (plannedTransfer(body, producer) ==
                        native_llvm::FreshHierarchyPublication::Replacement &&
                    native_llvm::planFreshHierarchyTransfer(program, body,
                                                            producer)
                            .publication == &stage);
      return false;
    }
    return false;
  }
  // A place observation of a checked view whose only use is one
  // replacement's generated copy source.
  bool copiedView(const MirBody &body, const MirInstruction &producer,
                  const SemanticType &type) const {
    if (producer.kind != MirInstructionKind::Load || !producer.result ||
        producer.info.category != ValueCategory::Place ||
        producer.info.type != type || producer.operands.size() != 1 ||
        producer.operands.front().kind != MirOperandKind::Copy ||
        !producer.operands.front().place ||
        body.usesOf(*producer.result).size() != 1)
      return false;
    try {
      native_llvm::preflightGeneratedHierarchyCopy(program, type);
    } catch (const BackendUnsupportedError &) {
      return false;
    }
    const auto *from = body.findPlace(producer.operands.front().place);
    return from && from->type == type &&
           borrowedSource(body,
                          {.kind = MirOperandKind::Address,
                           .place = from->id,
                           .type = SemanticType::referenceTo(type)})
               .has_value();
  }
  // The place observation that one admitted replacement copies from.
  bool replacementCopy(MirBodyAddress address, const MirBody &body,
                       const native_llvm::Placement &placement,
                       const MirInstruction &stage) {
    if (stage.kind != MirInstructionKind::Load || !stage.result ||
        body.usesOf(*stage.result).size() != 1)
      return false;
    const auto &use = body.usesOf(*stage.result).front();
    const auto *block = body.findBlock(use.block);
    if (use.kind != MirValueUseKind::InstructionOperand || !block)
      return false;
    for (const auto &consumer : block->instructions)
      if (!consumer.tombstone && consumer.id == use.instruction)
        return replacement(address, body, placement, consumer) &&
               copiedView(body, stage, stage.info.type);
    return false;
  }
  // The receiver input of one consuming call: a planned move of an owned
  // binding relocated into its own value storage, or into the prepared slot
  // MIR stages for that receiver.
  bool consumedReceiver(const MirBody &body,
                        const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::CallInput ||
        stage.callInputKind != HirCallInputKind::MoveValue ||
        stage.callInputRole != MirCallInputRole::Receiver ||
        stage.operands.size() != 1 ||
        stage.operands.front().kind != MirOperandKind::Value)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &producer : block.instructions)
        if (!producer.tombstone &&
            producer.result == stage.operands.front().value) {
          const auto planned = plannedTransfer(body, producer);
          return (planned ==
                      native_llvm::FreshHierarchyPublication::Receiver ||
                  (planned ==
                       native_llvm::FreshHierarchyPublication::Argument &&
                   stage.destination)) &&
                 native_llvm::planFreshHierarchyTransfer(program, body,
                                                         producer)
                         .publication == &stage;
        }
    return false;
  }
  // The call input that staged a call's receiver value, if any.
  const MirInstruction *receiverInput(const MirBody &body,
                                      const MirOperand &receiver) const {
    if (receiver.kind != MirOperandKind::Value || !receiver.value)
      return nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.result == receiver.value)
          return stage.kind == MirInstructionKind::CallInput ? &stage
                                                             : nullptr;
    return nullptr;
  }
  bool freshReturns(MirBodyAddress address, const MirBody &body,
                    const native_llvm::Placement &placement) const {
    bool returned = false;
    for (const auto &block : body.blocks)
      if (block.terminator.kind == MirTerminatorKind::Return) {
        if (!block.terminator.value ||
            !freshReturn(address, body, placement, *block.terminator.value))
          return false;
        returned = true;
      }
    return returned;
  }
  // A Move or Load that names a branch-produced object in the hidden
  // temporary MIR materialized for it: that object in place.
  bool materializedTemporary(const MirBody &body,
                             const MirInstruction &stage) const {
    if ((stage.kind != MirInstructionKind::Move &&
         stage.kind != MirInstructionKind::Load) ||
        !stage.result || stage.operands.size() != 1)
      return false;
    const auto *place = body.findPlace(stage.operands.front().place);
    if (!place || place->root != MirPlaceRootKind::Temporary ||
        !place->projections.empty() || place->type != stage.info.type)
      return false;
    const auto materialization =
        findMirTemporaryMaterialization(body, *place, place->sourceValue);
    return materialization && materialization.producer == &stage;
  }
  // A place-category Load names its operand's address. Unused, or used only
  // as an inline slot operand, it transports nothing.
  bool placeObservation(const MirBody &body,
                        const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Load || !stage.result ||
        stage.info.category != ValueCategory::Place ||
        stage.operands.size() != 1)
      return false;
    for (const auto &use : body.usesOf(*stage.result)) {
      const auto *block = body.findBlock(use.block);
      const MirInstruction *consumer = nullptr;
      if (block)
        for (const auto &candidate : block->instructions)
          if (candidate.id == use.instruction)
            consumer = &candidate;
      if (use.kind != MirValueUseKind::InstructionOperand ||
          use.operandIndex != 0 || !consumer ||
          consumer->kind != MirInstructionKind::Call ||
          !native_llvm::PolymorphicStorage::handles(consumer->intrinsic))
        return false;
    }
    return true;
  }
  // One public operation of an exact inline polymorphic slot. Its slot
  // operand is the slot's place; a construction consumes a planned move of
  // an owned binding whose inert relocation the slot performs itself.
  std::optional<Shape> inlineSlot(const MirBody &body,
                                  const MirInstruction &stage) {
    if (stage.operands.empty() || stage.parameterTypes.empty() ||
        stage.parameterTypes.size() != stage.operands.size() ||
        stage.functionTarget || stage.receiver || !planner())
      return {};
    // Verified MIR presents the slot operand as its place-category value.
    const auto &slot = unreference(stage.parameterTypes.front());
    if (slot.kind != SemanticType::Class ||
        unreference(stage.operands.front().type) != slot)
      return {};
    const auto &owner = instance(slot);
    if (!native_llvm::polymorphicStorageRepresentation(program, owner) ||
        !metadata(slot))
      return {};
    static_cast<void>(session->layouts().polymorphicStorage(slot));
    if (stage.intrinsic == IntrinsicKind::PolymorphicStorageConstruct) {
      if (stage.operands.size() != 2 ||
          stage.operands[1].kind != MirOperandKind::Value)
        return {};
      const MirInstruction *producer = nullptr;
      for (const auto &block : body.blocks)
        for (const auto &candidate : block.instructions)
          if (!candidate.tombstone &&
              candidate.result == stage.operands[1].value)
            producer = &candidate;
      if (!producer ||
          plannedTransfer(body, *producer) !=
              native_llvm::FreshHierarchyPublication::Slot ||
          native_llvm::planFreshHierarchyTransfer(program, body, *producer)
                  .publication != &stage)
        return {};
      const auto &model = stage.operands[1].type;
      native_llvm::Lifetimes::preflightInertRelocation(*session, *dispatch,
                                                       model);
      if (!session->layouts().findBaseSubobject(model, slot.arguments.front()))
        return {};
    }
    return Shape::NativeInlinePolymorphicSlot;
  }
  // A read or mutable temporary borrow of a fresh hierarchy object's own
  // full-expression temporary: an address, never a transfer of its bytes.
  bool temporaryBorrow(const MirBody &body, const MirInstruction &stage) const {
    const bool mutableInput =
        stage.callInputKind == HirCallInputKind::MutableTemporaryBorrow;
    if (stage.kind != MirInstructionKind::CallInput ||
        (stage.callInputKind != HirCallInputKind::ReadTemporaryBorrow &&
         !mutableInput) ||
        stage.operands.size() != 1 || stage.destination ||
        stage.preparedParameterDrop || stage.retainedOwnerPlace ||
        stage.successResultDrop || stage.successResultDestination)
      return false;
    const auto &input = stage.operands.front();
    const auto *place = body.findPlace(input.place);
    const auto found = temporaries.find(&body);
    const auto materialized = hidden.find(&body);
    const bool fresh =
        place &&
        ((place->root == MirPlaceRootKind::Value &&
          found != temporaries.end() && found->second.contains(place->value)) ||
         (place->root == MirPlaceRootKind::Temporary &&
          place->projections.empty() && materialized != hidden.end() &&
          materialized->second.contains(place->id)));
    return input.kind == (mutableInput ? MirOperandKind::BorrowWrite
                                       : MirOperandKind::BorrowRead) &&
           !input.loan && !input.value && fresh &&
           unreference(stage.info.type) == place->type;
  }
  // A fixed array whose leaf elements are classes or payload enums, or a
  // payload enum: containers of complete hierarchy objects.
  bool hierarchyContainer(const SemanticType &type) const {
    if (type.kind == SemanticType::Enum)
      return program.findPayloadEnum(type) != nullptr;
    const auto *leaf = &type;
    while (leaf->kind == SemanticType::Array && leaf->arguments.size() == 1 &&
           !leaf->arrayLengthParameterId)
      leaf = &leaf->arguments.front();
    return type.kind == SemanticType::Array &&
           (leaf->kind == SemanticType::Class ||
            (leaf->kind == SemanticType::Enum &&
             program.findPayloadEnum(*leaf)));
  }
  bool freshConstruction(MirBodyAddress address, const MirBody &body,
                         const native_llvm::Placement &placement,
                         const MirInstruction &stage) const {
    return native_llvm::freshConstructionDestination(program, address, body,
                                                     placement, stage)
        .has_value();
  }
  bool freshTransfer(const MirBody &body, const MirInstruction &stage) const {
    // The dedicated row activates only the exact producer/publication proof;
    // generic opcode/storage witnesses never grant transfer permission.
    if (coverage(Shape::NativeFreshHierarchyTransfer).state !=
        LlvmCoverageState::Implemented)
      return false;
    if (!native_llvm::isFreshHierarchyTransferProducer(stage) ||
        !contains(stage.info.type))
      return false;
    try {
      static_cast<void>(
          native_llvm::planFreshHierarchyTransfer(program, body, stage));
      return true;
    } catch (const BackendUnsupportedError &) {
      return false;
    }
  }
  bool freshTransferInitialization(const MirBody &body,
                                   const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Initialize || !stage.destination ||
        stage.operands.size() != 1 ||
        stage.operands.front().kind != MirOperandKind::Value)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &producer : block.instructions)
        if (!producer.tombstone &&
            producer.result == stage.operands.front().value &&
            freshTransfer(body, producer))
          return native_llvm::planFreshHierarchyTransfer(program, body,
                                                         producer)
                     .publication == &stage;
    return false;
  }
  bool freshInitialization(MirBodyAddress address, const MirBody &body,
                           const native_llvm::Placement &placement,
                           const MirInstruction &stage) const {
    if (freshTransferInitialization(body, stage))
      return true;
    if (stage.kind != MirInstructionKind::Initialize || !stage.destination ||
        stage.operands.size() != 1 ||
        stage.operands.front().kind != MirOperandKind::Value)
      return false;
    const auto value = stage.operands.front().value;
    for (const auto &block : body.blocks)
      for (const auto &producer : block.instructions)
        if (!producer.tombstone && producer.result == value &&
            freshConstruction(address, body, placement, producer))
          return placement.target(value)->place == *stage.destination;
    return false;
  }
  static bool simpleLoan(const MirLoan &loan) {
    return !loan.referenceSelection && loan.additionalDependencies.empty() &&
           loan.conditions.empty() && !loan.storedDependency &&
           !loan.twoPhase && !loan.lifecycleBound && !loan.escapes &&
           (loan.kind == MirLoanKind::Local ||
            (loan.kind == MirLoanKind::Parameter && loan.entry));
  }
  // The accessor call of a reserved two-phase borrow store carries the
  // store's own value facts; its reserved loan carries the result's access.
  bool reservedStore(const MirBody &body, const MirInstruction &stage) const {
    const auto *loan =
        stage.kind == MirInstructionKind::Call && stage.loan && stage.result
            ? body.findLoan(*stage.loan)
            : nullptr;
    return loan && loan->twoPhase && !loan->escapes &&
           loan->kind == MirLoanKind::CallResult &&
           stage.info.category == ValueCategory::Value &&
           body.usesOf(*stage.result).empty();
  }
  bool borrowedCallResult(const MirBody &body,
                          const MirInstruction &stage) const {
    const bool reserved = reservedStore(body, stage);
    if (stage.kind != MirInstructionKind::Call || stage.tombstone ||
        !stage.result || !stage.loan ||
        (stage.info.category != ValueCategory::Place && !reserved) ||
        stage.info.type.kind == SemanticType::Unknown ||
        stage.info.type.kind == SemanticType::Void ||
        (stage.dispatch != CallDispatch::Static &&
         stage.dispatch != CallDispatch::Virtual) ||
        stage.destination ||
        stage.successResultDrop || stage.successResultDestination ||
        stage.constructorTarget || stage.lambdaTarget || stage.bodyTarget ||
        stage.nativeCalleeType || stage.nativeCallbackAdapter ||
        stage.threadTaskAdapter || stage.callableInvocation ||
        !stage.scopedThreadTasks.empty())
      return false;
    const auto *loan = body.findLoan(*stage.loan);
    const auto *value = body.findValue(*stage.result);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    if (!loan || loan->kind != MirLoanKind::CallResult || loan->entry ||
        loan->producedBy != stage.hirValue ||
        (loan->access != stage.info.access && !reserved) || !value ||
        value->definition != stage.id || !block ||
        std::none_of(
            block->instructions.begin(), block->instructions.end(),
            [&](const auto &candidate) { return &candidate == &stage; }))
      return false;
    if (stage.intrinsic != IntrinsicKind::None) {
      // Verified MIR owns the capability's exact owner, access and failure
      // contract. Its result is a checked address, never pointee object bytes.
      return !stage.functionTarget &&
             stage.dispatch == CallDispatch::Static &&
             stage.info.type.kind == SemanticType::Class &&
             (stage.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
              stage.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
              stage.intrinsic == IntrinsicKind::SharedOwnerBorrow ||
              stage.intrinsic == IntrinsicKind::PolymorphicStorageRead ||
              stage.intrinsic == IntrinsicKind::PolymorphicStorageReadMut ||
              // A guard read selects the complete payload inside the shell.
              stage.intrinsic == IntrinsicKind::MutexGuardRead ||
              stage.intrinsic == IntrinsicKind::MutexGuardReadMut ||
              ((stage.intrinsic == IntrinsicKind::RawBorrow ||
                stage.intrinsic == IntrinsicKind::RawBorrowMut) &&
               mir_detail::rawBorrowResultLoan(body, stage) == loan));
    }
    const auto *target =
        stage.functionTarget
            ? program.findFunctionInstance(*stage.functionTarget)
            : nullptr;
    // A virtual call names its sealed root member, which may be a pure
    // declaration; the selected source implementation returns the address.
    // Its dispatch plan is proved with the call stage itself.
    return target &&
           (stage.dispatch == CallDispatch::Virtual
                ? target->virtualMethod && target->owner &&
                      !target->staticMember
                : target->definitionKind == MirDefinitionKind::Source) &&
           target->linkage == LanguageLinkage::Gti &&
           target->returnType ==
               SemanticType::referenceTo(stage.info.type, loan->access);
  }
  const MirInstruction *callResult(const MirBody &body,
                                   const MirLoan &loan) const {
    if (loan.kind != MirLoanKind::CallResult)
      return nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (stage.loan == loan.id && borrowedCallResult(body, stage))
          return &stage;
    return nullptr;
  }
  bool supportedLoan(const MirBody &body, const MirLoan &loan) const {
    if (simpleLoan(loan))
      return true;
    // A borrow carrier received by reference: the parameter's stored
    // dependency only protects the external owners the carrier borrows and
    // has no representation, as for a call result.
    if (loan.kind == MirLoanKind::Parameter && loan.entry &&
        loan.storedDependency && !loan.referenceSelection &&
        loan.additionalDependencies.empty() && loan.conditions.empty() &&
        !loan.lifecycleBound && !loan.twoPhase && !loan.escapes)
      return true;
    if (loan.referenceSelection || !loan.additionalDependencies.empty() ||
        !loan.conditions.empty() || loan.lifecycleBound || loan.entry)
      return false;
    // A reserved two-phase store's address is its accessor's checked result,
    // activated before the store and ended with it.
    if (loan.twoPhase)
      return !loan.escapes && !loan.storedDependency &&
             callResult(body, loan) &&
             reservedStore(body, *callResult(body, loan));
    // A call result's address is the callee's checked result. A stored
    // dependency only protects the external owners a borrow carrier inherited
    // (such as an iterator's container) and has no representation.
    if (!loan.escapes && callResult(body, loan))
      return true;
    // A constructor publishing a reference field stores the borrowed address
    // itself; the field's Stored loan is that publication's provenance.
    if (loan.kind == MirLoanKind::Stored && loan.storedField &&
        loan.escapes && !loan.storedDependency)
      return true;
    if (loan.storedDependency)
      return false;
    // The verifier-owned read-only loan of one fresh value argument: its
    // address is the fresh temporary's own storage, ended after the call.
    if (loan.kind == MirLoanKind::CallArgument && !loan.escapes &&
        loan.access == AccessMode::ReadOnly)
      if (const auto *source = body.findPlace(loan.source);
          source && source->root == MirPlaceRootKind::Value &&
          source->projections.empty()) {
        const auto found = temporaries.find(&body);
        return found != temporaries.end() &&
               found->second.contains(source->value);
      }
    return loan.kind == MirLoanKind::Return && loan.escapes &&
           std::any_of(body.blocks.begin(), body.blocks.end(),
                       [&](const auto &block) {
                         return block.terminator.returnLoan == loan.id &&
                                referenceReturn(body, block.terminator);
                       });
  }
  bool referenceReturn(const MirBody &body, const MirTerminator &term) const {
    if (term.kind != MirTerminatorKind::Return || !term.returnLoan ||
        !term.value || term.value->kind != MirOperandKind::Loan ||
        term.value->loan != *term.returnLoan || !referent(body.returnType))
      return false;
    const auto *loan = body.findLoan(*term.returnLoan);
    if (!loan || loan->kind != MirLoanKind::Return || !loan->escapes ||
        loan->access != body.returnType.referenceAccess ||
        unreference(term.value->type).kind != SemanticType::Class)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.kind == MirInstructionKind::Borrow &&
            stage.loan == loan->id && stage.hirValue == loan->producedBy &&
            stage.info.category == ValueCategory::Place &&
            stage.info.type == unreference(term.value->type) &&
            stage.info.access == loan->access && stage.operands.size() == 1)
          return true;
    return false;
  }
  const MirInstruction *stagedBorrow(const MirBody &body,
                                     const MirOperand &operand) const {
    if (operand.kind != MirOperandKind::Value || !operand.value)
      return nullptr;
    const auto *value = body.findValue(operand.value);
    if (!value)
      return nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone || stage.result != operand.value ||
            stage.id != value->definition)
          continue;
        // A retained receiver stages its owned field or owner pointee in
        // place: the enclosing consuming receiver keeps its cleanup.
        const bool retained =
            stage.callInputKind == HirCallInputKind::RetainedReceiver &&
            stage.callInputRole == MirCallInputRole::Receiver;
        if (stage.kind != MirInstructionKind::CallInput ||
            (stage.callInputKind != HirCallInputKind::ReadBorrow &&
             stage.callInputKind != HirCallInputKind::MutableBorrow &&
             !retained && !temporaryBorrow(body, stage)) ||
            stage.destination || stage.preparedParameterDrop ||
            (stage.retainedOwnerPlace && !retained) ||
            stage.successResultDrop ||
            stage.successResultDestination || stage.operands.size() != 1 ||
            stage.operands.front().kind == MirOperandKind::Value ||
            unreference(stage.info.type) != unreference(operand.type) ||
            unreference(stage.operands.front().type) !=
                unreference(stage.info.type))
          return nullptr;
        if (stage.loan) {
          const auto *loan = body.findLoan(*stage.loan);
          if (!loan || !supportedLoan(body, *loan))
            return nullptr;
        }
        return &stage;
      }
    return nullptr;
  }
  std::optional<SemanticType> borrowedSource(const MirBody &body,
                                             const MirOperand &operand) const {
    if (const auto *staged = stagedBorrow(body, operand)) {
      if (!borrowedSource(body, staged->operands.front()))
        return {};
      return unreference(staged->info.type);
    }
    if (operand.kind != MirOperandKind::Address &&
        operand.kind != MirOperandKind::BorrowRead &&
        operand.kind != MirOperandKind::BorrowWrite &&
        operand.kind != MirOperandKind::Loan)
      return {};
    const auto &target = unreference(operand.type);
    if (target.kind != SemanticType::Class)
      return {};
    const MirPlace *place =
        operand.place ? body.findPlace(operand.place) : nullptr;
    const MirLoan *loan = operand.loan ? body.findLoan(operand.loan) : nullptr;
    if (loan && !supportedLoan(body, *loan))
      return {};
    SemanticType source = SemanticType::Unknown;
    AccessMode access = AccessMode::ReadOnly;
    const auto freshTemporary = [&](const MirPlace &selected) {
      const auto found = temporaries.find(&body);
      return selected.root == MirPlaceRootKind::Value &&
             found != temporaries.end() &&
             found->second.contains(selected.value);
    };
    // The pointee of a raw class pointer is an address the program asserts
    // under its unsafe obligation; its raw access keeps its own proof.
    const auto rawView = [&](const MirPlace &selected) {
      if (selected.root != MirPlaceRootKind::Value ||
          selected.projections.empty() ||
          selected.projections.front().kind !=
              MirProjectionKind::RawDereference)
        return false;
      const auto *value = body.findValue(selected.value);
      return value && value->info.type.kind == SemanticType::RawPointer &&
             value->info.type.arguments.size() == 1 &&
             value->info.type.arguments.front().kind == SemanticType::Class;
    };
    if (place) {
      const bool raw = rawView(*place);
      // A hidden temporary is the exact storage MIR materialized for one
      // object; its producer and drops carry their own proofs.
      const bool hidden = place->root == MirPlaceRootKind::Temporary &&
                          !place->capture;
      if (place->foreignStorage ||
          (place->root != MirPlaceRootKind::Binding &&
           place->root != MirPlaceRootKind::This &&
           place->root != MirPlaceRootKind::Loan && !freshTemporary(*place) &&
           !raw && !hidden && !programObject(*place) &&
           !(place->root == MirPlaceRootKind::Value &&
             payloadView(body, place->value))) ||
          std::any_of(place->projections.begin() + (raw ? 1 : 0),
                      place->projections.end(), [](const auto &projection) {
                        return projection.kind != MirProjectionKind::Base &&
                               projection.kind != MirProjectionKind::Field &&
                               projection.kind !=
                                   MirProjectionKind::Dereference &&
                               projection.kind !=
                                   MirProjectionKind::PackElement &&
                               projection.kind != MirProjectionKind::Index;
                      }))
        return {};
      if (place->root == MirPlaceRootKind::Loan) {
        const auto *selected = body.findLoan(place->loan);
        if (!selected || !supportedLoan(body, *selected))
          return {};
      }
      source = place->type;
      access = place->access;
    } else if (loan) {
      const auto *origin = body.findPlace(loan->source);
      if (!origin)
        return {};
      source = origin->type;
      access = loan->access;
      if (const auto *producer = callResult(body, *loan))
        source = producer->info.type;
      for (const auto &block : body.blocks)
        for (const auto &stage : block.instructions)
          if (!stage.tombstone && stage.loan == loan->id &&
              stage.kind == MirInstructionKind::Borrow)
            source = stage.info.type;
    } else {
      return {};
    }
    if (source.kind == SemanticType::Reference)
      access = source.referenceAccess;
    const bool mut = operand.kind == MirOperandKind::BorrowWrite ||
                     (operand.type.kind == SemanticType::Reference &&
                      operand.type.referenceAccess == AccessMode::Mutable);
    if (mut && access != AccessMode::Mutable)
      return {};
    // unreference names a subobject of source; copy it before assigning.
    source = SemanticType(unreference(source));
    return source.kind == SemanticType::Class &&
                   (contains(source) || contains(target))
               ? std::optional(source)
               : std::nullopt;
  }
  std::optional<Shape> proveBody(MirBodyAddress address) {
    const MirClassInstance *owner = nullptr;
    if (address.kind == MirBodyKind::Constructor)
      owner = program.findClassInstance(
          program.findConstructorInstance(address.owner)->owner);
    else if (address.kind == MirBodyKind::Destructor)
      owner = program.findClassInstance(
          program.findDestructorInstance(address.owner)->owner);
    else if (address.kind == MirBodyKind::FieldInitializers ||
             address.kind == MirBodyKind::StaticFieldInitializers)
      owner = program.findClassInstance(address.owner);
    else if (const auto *target = function(address); target && target->owner)
      owner = program.findClassInstance(*target->owner);
    if (!owner || !contains(owner->type))
      return {};
    try {
      if (!metadata(owner->type))
        return {};
      if (address.kind == MirBodyKind::Destructor && owner->polymorphic) {
        const auto &target = *program.findDestructorInstance(address.owner);
        if (observesDispatch(address) && !recordedPhase(target))
          return {};
        if (recordedPhase(target))
          dispatch->preflightPhase(target.dispatchState->activeClass);
      }
      if (address.kind == MirBodyKind::Constructor && owner->polymorphic) {
        const auto &target = *program.findConstructorInstance(address.owner);
        if (target.definitionKind != MirDefinitionKind::Source ||
            !target.dispatchState ||
            target.dispatchState->activeClass != owner->id)
          return {};
        dispatch->preflightPhase(owner->id);
      }
      return Shape::NativeStaticDispatchStorage;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  // An element published in its slot of a fresh array or payload alternative.
  static Shape elementShape(const native_llvm::Placement &placement,
                            const MirInstruction &stage) {
    const auto *target =
        stage.result ? placement.target(*stage.result) : nullptr;
    return target && target->variant ? Shape::NativeHierarchyPayload
                                     : Shape::NativeHierarchyArray;
  }
  std::optional<Shape> proveOperation(MirBodyAddress address,
                                      const MirBody &body,
                                      const native_llvm::Placement &placement,
                                      const MirInstruction &stage) {
    try {
      if (stage.kind == MirInstructionKind::Call &&
          native_llvm::PolymorphicStorage::handles(stage.intrinsic))
        return inlineSlot(body, stage);
      if (borrowedCallResult(body, stage) &&
          stage.dispatch == CallDispatch::Static &&
          metadata(stage.info.type) &&
          (!stage.receiver || !borrowedUse(body, *stage.receiver) ||
           borrowed(body, *stage.receiver)))
        return Shape::NativeBorrowedDispatchView;
      if (declarationOnlyField(address, body, stage) &&
          metadata(stage.info.type))
        return Shape::NativeStaticDispatchStorage;
      if (freshTransfer(body, stage)) {
        if (!metadata(stage.info.type))
          return {};
        const auto transfer = native_llvm::Lifetimes::preflightFreshTransfer(
            *session, *dispatch, body, stage);
        // A task the spawn relocates into its runtime box is proved with
        // that exact native thread operation and adapter.
        if (transfer.publishes == native_llvm::FreshHierarchyPublication::Task) {
          const auto &spawn = *transfer.publication;
          const auto *adapter =
              spawn.threadTaskAdapter
                  ? program.findThreadTaskAdapter(*spawn.threadTaskAdapter)
                  : nullptr;
          if (!adapter || adapter->taskType != stage.info.type ||
              !native_llvm::nativeThreadCall(program, address, body, spawn) ||
              !native_llvm::hierarchyThreadTask(program, *adapter) ||
              !generatedRelocation(stage.info.type))
            return {};
          return Shape::NativeHierarchyThreadTask;
        }
        // A payload enum moved whole, or a hierarchy field moved out of its
        // payload alternative, relocates under the payload contract.
        if (transfer.type.kind == SemanticType::Enum || transfer.sourceVariant)
          return generatedRelocation(transfer.type)
                     ? std::optional(Shape::NativeHierarchyPayload)
                     : std::nullopt;
        return Shape::NativeFreshHierarchyTransfer;
      }
      if (freshTransferInitialization(body, stage) &&
          metadata(stage.operands.front().type))
        return Shape::NativeFreshHierarchyTransfer;
      const auto storage = freshStorage(address, body, placement, stage);
      // A fresh hierarchy aggregate publishes its elements' slots in place:
      // in its final storage or, for a replacing value, in its own storage.
      if (native_llvm::isFreshHierarchyAggregate(program, stage) &&
          (storage == native_llvm::FreshObjectStorage::Local ||
           storage == native_llvm::FreshObjectStorage::Element ||
           storage == native_llvm::FreshObjectStorage::Temporary))
        return metadata(stage.info.type);
      if (stage.kind == MirInstructionKind::Construct && storage) {
        if (!metadata(stage.info.type))
          return {};
        if (!stage.constructorTarget) {
          static_cast<void>(native_llvm::Lifetimes::preflightGeneratedEmpty(
              *session, *dispatch, body, stage));
        } else {
          const auto *target =
              program.findConstructorInstance(*stage.constructorTarget);
          if (!target || target->owner != instance(stage.info.type).id ||
              target->definitionKind != MirDefinitionKind::Source ||
              !this->body({MirBodyKind::Constructor, target->id}) ||
              !this->body({MirBodyKind::Constructor, target->id})->shape)
            return {};
        }
        return *storage == native_llvm::FreshObjectStorage::Local
                   ? Shape::NativeStaticDispatchStorage
               : *storage == native_llvm::FreshObjectStorage::Element
                   ? elementShape(placement, stage)
                   : Shape::NativeFreshHierarchyPublication;
      }
      if (stage.kind == MirInstructionKind::Initialize &&
          freshInitialization(address, body, placement, stage) &&
          metadata(stage.operands.front().type))
        return stage.operands.front().type.kind == SemanticType::Array
                   ? Shape::NativeHierarchyArray
               : stage.operands.front().type.kind == SemanticType::Enum
                   ? Shape::NativeHierarchyPayload
                   : Shape::NativeStaticDispatchStorage;
      if (stage.kind == MirInstructionKind::InitializeBase &&
          address.kind == MirBodyKind::Constructor) {
        const auto *target = program.findConstructorInstance(address.owner);
        if (!target ||
            !contains(program.findClassInstance(target->owner)->type) ||
            !metadata(program.findClassInstance(target->owner)->type) ||
            (contains(stage.info.type) && !metadata(stage.info.type)))
          return {};
        if (!target || !stage.destination ||
            stage.constructorInitializer == 0 ||
            stage.constructorInitializer > target->initializers.size())
          return {};
        const auto &selected =
            target->initializers[stage.constructorInitializer - 1];
        const auto *place = body.findPlace(*stage.destination);
        const auto &owner = instance(stage.info.type);
        if (selected.kind != ConstructorInitializerTargetKind::Base ||
            selected.base != owner.id || selected.targetType != owner.type ||
            selected.constructorTarget != stage.constructorTarget || !place ||
            place->root != MirPlaceRootKind::This ||
            place->type != owner.type ||
            place->projections !=
                std::vector<MirPlaceProjection>{
                    {.kind = MirProjectionKind::Base, .selection = owner.id}})
          return {};
        if (!stage.constructorTarget)
          static_cast<void>(native_llvm::Lifetimes::preflightGeneratedEmpty(
              *session, *dispatch, body, stage, target));
        else if (const auto *base =
                     program.findConstructorInstance(*stage.constructorTarget);
                 !base || base->owner != owner.id ||
                 base->definitionKind != MirDefinitionKind::Source ||
                 (contains(owner.type) &&
                  (!this->body({MirBodyKind::Constructor, base->id}) ||
                   !this->body({MirBodyKind::Constructor, base->id})->shape)))
          return {};
        if (!session->layouts().findBaseSubobject(
                program.findClassInstance(target->owner)->type, owner.type))
          return {};
        return Shape::NativeStaticDispatchStorage;
      }
      if (stage.kind == MirInstructionKind::Call &&
          stage.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
        const auto owned = native_llvm::dynamicOwnerAllocation(program, stage);
        if (!owned)
          return {};
        const auto &object = *program.findClassInstance(*owned);
        const auto *target =
            stage.constructorTarget
                ? program.findConstructorInstance(*stage.constructorTarget)
                : nullptr;
        if (!native_llvm::nativeUniqueConstructionShape(program, stage) ||
            !metadata(object.type) || !planner())
          return {};
        if (!stage.constructorTarget &&
            stage.constructorKind == ConstructorKind::Copy) {
          // The copy source is a checked view of the exact class.
          native_llvm::preflightGeneratedHierarchyCopy(program, object.type);
          const auto &source = stage.operands.front();
          if (source.kind != MirOperandKind::Value &&
              source.kind != MirOperandKind::BorrowRead &&
              source.kind != MirOperandKind::Loan)
            return {};
        } else if (!stage.constructorTarget &&
                   stage.constructorKind == ConstructorKind::Move) {
          // The moved operand is an owned argument already published in its
          // prepared storage; the allocation relocates it.
          if (stage.operands.front().kind != MirOperandKind::Value ||
              !generatedRelocation(object.type))
            return {};
        } else if (!stage.constructorTarget)
          static_cast<void>(native_llvm::Lifetimes::preflightGeneratedEmpty(
              *session, *dispatch, body, stage));
        else if (!target || target->owner != object.id ||
                 target->definitionKind != MirDefinitionKind::Source ||
                 !this->body({MirBodyKind::Constructor, target->id}) ||
                 !this->body({MirBodyKind::Constructor, target->id})->shape)
          return {};
        // Its descriptors publish this exact complete cleanup.
        dispatch->preflightState(object.id);
        if (const auto complete = native_llvm::Lifetimes::completeCleanupRecipe(
                *session, object.id);
            complete && !cleanup(*complete))
          return {};
        return Shape::NativePolymorphicOwnership;
      }
      if (stage.kind == MirInstructionKind::Call &&
          stage.intrinsic == IntrinsicKind::UniqueOwnerUpcast) {
        if (stage.operands.size() != 1 ||
            stage.operands.front().type.kind != SemanticType::UniqueOwner ||
            stage.operands.front().type.arguments.size() != 1 ||
            stage.info.type.kind != SemanticType::UniqueOwner ||
            stage.info.type.arguments.size() != 1)
          return {};
        const auto &derived = stage.operands.front().type.arguments.front();
        const auto &base = stage.info.type.arguments.front();
        if (derived.kind != SemanticType::Class ||
            base.kind != SemanticType::Class || !metadata(derived) ||
            !metadata(base) || !instance(base).polymorphic || !planner() ||
            !session->layouts().findBaseSubobject(derived, base))
          return {};
        return Shape::NativePolymorphicOwnership;
      }
      // A shared owner upcast keeps the control block and all aliases and
      // selects the static base view of the same allocation.
      if (stage.kind == MirInstructionKind::Call &&
          stage.intrinsic == IntrinsicKind::SharedOwnerUpcast) {
        const auto *derivedOwner =
            stage.operands.size() == 1
                ? program.findClassInstance(
                      instance(stage.operands.front().type).id)
                : nullptr;
        const auto *baseOwner =
            program.findClassInstance(instance(stage.info.type).id);
        if (!derivedOwner || !baseOwner ||
            !native_llvm::sharedOwnerRepresentation(program, *derivedOwner) ||
            !native_llvm::sharedOwnerRepresentation(program, *baseOwner) ||
            stage.operands.front().kind != MirOperandKind::Value ||
            stage.parameterTypes != std::vector{stage.operands.front().type})
          return {};
        const auto &derived = stage.operands.front().type.arguments.front();
        const auto &base = stage.info.type.arguments.front();
        if (derived.kind != SemanticType::Class ||
            base.kind != SemanticType::Class || !metadata(derived) ||
            !metadata(base) || !instance(base).polymorphic || !planner() ||
            !session->layouts().findBaseSubobject(derived, base))
          return {};
        return Shape::NativePolymorphicOwnership;
      }
      if (stage.kind == MirInstructionKind::Drop)
        for (const auto &event : stage.lifecycle)
          if (const auto *drop = body.findDropObligation(event.source))
            if (const auto shape = cleanup(drop->dropType))
              return shape;
      if (consumedReceiver(body, stage))
        return metadata(stage.info.type)
                   ? std::optional(Shape::NativeConsumingHierarchyReceiver)
                   : std::nullopt;
      if (stage.kind == MirInstructionKind::Assign &&
          replacement(address, body, placement, stage))
        return metadata(stage.info.type)
                   ? std::optional(Shape::NativeHierarchyReplacement)
                   : std::nullopt;
      if (stage.kind == MirInstructionKind::CleanupBeforeAssign &&
          replacement(address, body, placement,
                      pairedAssignment(body, stage))) {
        // A complete object ends like any other cleanup.
        const auto *place = body.findPlace(*stage.destination);
        return place && cleanup(*stage.replacementCleanup,
                                place->type.kind == SemanticType::Class &&
                                    !native_llvm::completeObjectPlace(program,
                                                                      *place))
                   ? std::optional(Shape::NativeHierarchyReplacement)
                   : std::nullopt;
      }
      if (stage.kind == MirInstructionKind::CallInput &&
          stage.callInputKind == HirCallInputKind::RetainedReceiver) {
        if (!stagedBorrow(body, {.kind = MirOperandKind::Value,
                                 .value = stage.result.value_or(0),
                                 .type = stage.info.type}) ||
            !borrowed(body, stage.operands.front()))
          return {};
        return Shape::NativeConsumingHierarchyReceiver;
      }
      if ((stage.kind == MirInstructionKind::Borrow ||
           stage.kind == MirInstructionKind::CallInput) &&
          stage.operands.size() == 1 &&
          (stage.kind != MirInstructionKind::CallInput ||
           stage.callInputKind == HirCallInputKind::ReadBorrow ||
           stage.callInputKind == HirCallInputKind::MutableBorrow ||
           temporaryBorrow(body, stage))) {
        if (stage.loan) {
          const auto *loan = body.findLoan(*stage.loan);
          if (!loan || !supportedLoan(body, *loan))
            return {};
        }
        return borrowed(body, stage.operands.front());
      }
      if (stage.kind == MirInstructionKind::Call && stage.receiver &&
          (borrowedUse(body, *stage.receiver) ||
           (stage.functionTarget &&
            program.findFunctionInstance(*stage.functionTarget)->owner &&
            contains(
                program
                    .findClassInstance(
                        *program.findFunctionInstance(*stage.functionTarget)
                             ->owner)
                    ->type)))) {
        // A consuming member receives a moved owned receiver in its own
        // value storage, a retained receiver staged in place, or a fresh
        // full-expression temporary whose value obligation stays with the
        // caller. A call operator is an ordinary member of its receiver.
        const auto *input = receiverInput(body, *stage.receiver);
        const bool moved = input && consumedReceiver(body, *input);
        const bool retained =
            input && input->callInputKind == HirCallInputKind::RetainedReceiver;
        const bool temporary = input && temporaryBorrow(body, *input);
        if ((borrowedUse(body, *stage.receiver) && !moved &&
             !borrowed(body, *stage.receiver)) ||
            !stage.functionTarget || stage.nativeCalleeType ||
            stage.nativeCallbackAdapter || stage.threadTaskAdapter ||
            !stage.scopedThreadTasks.empty())
          return {};
        // The receiver and its dispatch are this proof's only obligations.
        // Arguments, results and their loans use the shared call contract
        // and are inventoried independently, including hierarchy transfer.
        const auto *target =
            program.findFunctionInstance(*stage.functionTarget);
        const bool consuming =
            target &&
            target->receiverMutability == ReceiverMutability::Consuming;
        if (!target || target->linkage != LanguageLinkage::Gti ||
            (consuming ? !(moved || retained || temporary)
                       : moved || retained) ||
            std::any_of(target->parameterTypes.begin(),
                        target->parameterTypes.end(), [](const auto &type) {
                          return type == SemanticType::Void;
                        }))
          return {};
        if (!target->owner ||
            (contains(program.findClassInstance(*target->owner)->type) &&
             !metadata(program.findClassInstance(*target->owner)->type)) ||
            !planner())
          return {};
        if (stage.dispatch == CallDispatch::Virtual) {
          static_cast<void>(dispatch->planCall(stage));
          return Shape::NativeVirtualInvocation;
        }
        if (!target->owner || target->staticMember ||
            target->definitionKind != MirDefinitionKind::Source)
          return {};
        const auto &source = unreference(stage.receiver->type);
        const auto &owner = *program.findClassInstance(*target->owner);
        // An unrelated receiver/owner pair is simply not this shape, so the
        // probe declines rather than asserting a sealed base relationship.
        if (isMirExactReceiverPresentation(program, stage, source)) {
          if (target->receiverMutability != ReceiverMutability::ReadOnly)
            return {};
          if (!session->layouts().findBaseSubobject(owner.type, source))
            return {};
        } else if (!session->layouts().findBaseSubobject(source, owner.type)) {
          return {};
        }
        return Shape::NativeBorrowedDispatchView;
      }
      // A by-value hierarchy result whose callee constructs it directly in
      // this exact final storage; the callee's own returns are proved there.
      if (stage.kind == MirInstructionKind::Call && storage &&
          metadata(stage.info.type))
        return storage == native_llvm::FreshObjectStorage::Element
                   ? elementShape(placement, stage)
                   : Shape::NativeFreshHierarchyPublication;
    } catch (const BackendUnsupportedError &) {
    }
    return {};
  }
};

// One representative witness and count per requirement/body/context. Repeated
// scalar operations do not allocate a duplicate diagnostic record per use.
class Inventory final {
public:
  Inventory(std::vector<LlvmCoverageUse> &uses, const MirProgram &program,
            StaticDispatchProofs &dispatchProofs, const TargetInfo &nativeTarget)
      : uses(uses), program(program), dispatchProofs(dispatchProofs),
        nativeTarget(nativeTarget),
        natives(program, nativeTarget, [this](const SemanticType &pointee) {
          // A mutex-bearing pointee has its own admitted representation,
          // whose native identity the allocation holds in place.
          return this->dispatchProofs.metadata(pointee).has_value() ||
                 this->ownedMutexPointee(pointee);
        }) {}

  std::optional<MirBodyAddress> body;
  SourceSpan source;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  bool required = true;
  bool executable = false;
  const MirBody *currentBody = nullptr;

  bool nativeValue(const SemanticType &type) const { return natives(type); }
  // A native value or hierarchy storage with exact dispatch metadata. Only a
  // representation fact: every operation keeps its own transfer proof.
  bool representedValue(const SemanticType &type) const {
    return nativeValue(type) || (dispatchProofs.contains(type) &&
                                 dispatchProofs.metadata(type).has_value());
  }
  // An owner's pointee may also be exact mutex storage or an inline
  // composition containing it. The owner moves only its handle; the pointee
  // is constructed, borrowed and retired in place, under the same independent
  // use proofs as any other pointee. A pointee already being decided beneath
  // an enclosing owner is decided by that enclosing check.
  bool ownedMutexPointee(const SemanticType &pointee) const {
    if (std::find(ownedMutexPointees.begin(), ownedMutexPointees.end(),
                  pointee) != ownedMutexPointees.end())
      return true;
    ownedMutexPointees.push_back(pointee);
    const bool valid = mutexShape(pointee).has_value();
    ownedMutexPointees.pop_back();
    return valid;
  }
  // A loan of exact mutex storage as a whole object: the native shell's own
  // address, which only the guard intrinsics open to the payload. A
  // hierarchy payload keeps its view proofs at those payload accesses, so
  // such a loan needs no dispatch view of its own.
  bool mutexShellLoan(const MirBody &mir, MirLoanId id) const {
    const auto *loan = mir.findLoan(id);
    const auto *origin = loan ? mir.findPlace(loan->source) : nullptr;
    if (!origin || origin->type.kind != SemanticType::Class)
      return false;
    const MirClassInstance *owner = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == origin->type) {
        if (owner)
          return false;
        owner = &candidate;
      }
    return owner &&
           owner->compilerCapability ==
               CompilerCapabilityTypeKind::MutexStorage &&
           native_llvm::nativeMutexElement(program, origin->type,
                                           owner->compilerCapability) &&
           dispatchProofs.metadata(origin->type).has_value();
  }
  // A sealed integral enum is stored exactly as its fixed-width underlying
  // integer, so raw access through a pointer to one is primitive raw access to
  // that integer's representation. A payload enum carries a tag and stays
  // outside this rule.
  bool integralEnumPointee(const SemanticType &type) const {
    const auto *sealed =
        type.kind == SemanticType::Enum ? program.findIntegralEnum(type)
                                        : nullptr;
    return sealed && !program.findPayloadEnum(type) &&
           constantIntegerDomain(sealed->underlyingType).has_value();
  }
  bool integralEnumAddress(const SemanticType &type) const {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           integralEnumPointee(type.arguments.front());
  }
  bool primitiveAddress(const SemanticType &type) const {
    return passiveAddress(type) || integralEnumAddress(type);
  }
  // A raw pointer to a counted text view. Raw access moves the whole view
  // bitwise, as it moves a primitive; the bytes it designates stay where they
  // are, so no access reaches them.
  bool textViewAddress(const SemanticType &type) const {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           type.arguments.front().kind == SemanticType::StringView;
  }
  // A raw pointer whose pointee raw access moves as one scalar value: a
  // sealed integral enum, stored as its underlying integer, or a text view.
  bool scalarAddress(const SemanticType &type) const {
    return integralEnumAddress(type) || textViewAddress(type);
  }
  bool scalarPointerArithmetic(const MirInstruction &stage) const {
    if (primitivePointerArithmetic(stage))
      return true;
    if (stage.operands.size() != 2)
      return false;
    const bool reversed =
        stage.operation == MirOperation::PointerAdd &&
        stage.operands[1].type.kind == SemanticType::RawPointer;
    const auto &pointer = stage.operands[reversed ? 1 : 0].type;
    if (!scalarAddress(pointer))
      return false;
    return stage.operation == MirOperation::PointerDifference
               ? stage.operands[1].type == pointer
               : (stage.operation == MirOperation::PointerAdd ||
                  stage.operation == MirOperation::PointerSubtract) &&
                     stage.info.type == pointer &&
                     constantIntegerDomain(
                         stage.operands[reversed ? 0 : 1].type)
                         .has_value();
  }
  bool scalarRawAccess(const MirBody &body, const MirPlace &place) const {
    if (primitiveRawAccess(body, place))
      return true;
    const auto *value = body.findValue(place.value);
    return place.root == MirPlaceRootKind::Value && value &&
           scalarAddress(value->info.type) &&
           place.type == value->info.type.arguments.front() &&
           place.projections.size() == 1 &&
           (place.projections.front().kind == MirProjectionKind::RawIndex ||
            place.projections.front().kind ==
                MirProjectionKind::RawDereference);
  }
  // An index path through a local array of passive elements or of a sealed
  // integral enum, whose elements are stored as their underlying integers.
  bool scalarArrayProjection(const MirBody &body, const MirPlace &place) const {
    if (arrayProjection(body, place))
      return true;
    const auto root = std::find_if(
        body.places.begin(), body.places.end(),
        [&](const auto &candidate) { return sameStorageRoot(candidate, place); });
    if (root == body.places.end())
      return false;
    auto type = root->type;
    const auto &array =
        type.kind == SemanticType::Reference && type.arguments.size() == 1
            ? type.arguments.front()
            : type;
    if (!integralEnumArray(array) && !textViewArray(array))
      return false;
    for (const auto &projection : place.projections) {
      if (!((projection.kind == MirProjectionKind::Index &&
             type.kind == SemanticType::Array) ||
            (projection.kind == MirProjectionKind::Dereference &&
             type.kind == SemanticType::Reference)))
        return false;
      auto element = type.arguments.front();
      type = std::move(element);
    }
    return type == place.type;
  }
  bool scalarRawIndex(const MirInstruction &stage) const {
    if (primitiveRawIndex(stage))
      return true;
    return stage.operation == MirOperation::Index &&
           stage.operands.size() == 2 &&
           scalarAddress(stage.operands[0].type) &&
           stage.info.type == stage.operands[0].type.arguments.front() &&
           constantIntegerDomain(stage.operands[1].type).has_value();
  }
  // A value whose representation relocation may transfer: an admitted native
  // value, or a mutex-bearing representation, whose native mutex identity is
  // rebuilt rather than moved.
  bool relocatableRepresentation(const SemanticType &type) const {
    return nativeValue(type) || mutexShape(type).has_value() ||
           (dispatchProofs.contains(type) &&
            dispatchProofs.metadata(type).has_value() &&
            dispatchProofs.generatedRelocation(type));
  }
  // The restoring exchanges move objects between disjoint initialized places
  // of one type without copying or running cleanup, so each is a sequence of
  // typed relocations of an admitted native value shape. The verifier proves
  // the exact places, indices and result. Admission additionally requires
  // what emission relies on: an available, failure-free relocation of an
  // admitted value shape and, when that relocation is structural, a complete
  // layout the generated relocation accepts.
  bool restoringRelocation(const MirInstruction &stage) {
    const bool indexed =
        stage.intrinsic == IntrinsicKind::RestoringIndexedSwap;
    if ((stage.intrinsic != IntrinsicKind::RestoringSwap &&
         stage.intrinsic != IntrinsicKind::RestoringExchange && !indexed) ||
        stage.kind != MirInstructionKind::Call || stage.receiver ||
        stage.functionTarget || stage.constructorTarget || stage.lambdaTarget ||
        stage.bodyTarget || stage.callableInvocation ||
        stage.parameterTypes.empty() ||
        stage.operands.size() != stage.parameterTypes.size())
      return false;
    const auto &reference = stage.parameterTypes.front();
    if (reference.kind != SemanticType::Reference ||
        reference.referenceAccess != AccessMode::Mutable ||
        reference.arguments.size() != 1)
      return false;
    auto type = reference.arguments.front();
    if (indexed) {
      if (type.kind != SemanticType::Array || type.arguments.size() != 1 ||
          !type.arrayLength || type.arrayLengthParameterId)
        return false;
      auto element = type.arguments.front();
      type = std::move(element);
    }
    const auto relocation = mirTypeRelocationKind(program, type);
    if (relocation == RelocationKind::Unavailable ||
        !mirTypeRelocationIsDefinedFailureFree(program, type) ||
        !relocatableRepresentation(type))
      return false;
    return relocation == RelocationKind::Bitwise ||
           dispatchProofs.generatedRelocation(type);
  }
  // Exactly one sealed integral enum operand read into that enum's own
  // declared underlying integer type. No conversion, widening or payload
  // alternative is admitted here.
  bool sealedIntegralUnderlyingValue(const MirInstruction &stage) const {
    if (stage.intrinsic != IntrinsicKind::EnumUnderlyingValue ||
        stage.kind != MirInstructionKind::Call || stage.functionTarget ||
        stage.receiver || stage.operands.size() != 1 ||
        stage.parameterTypes.size() != 1 ||
        stage.parameterTypes[0] != stage.operands[0].type)
      return false;
    const auto *sealed = program.findIntegralEnum(stage.operands[0].type);
    return sealed && sealed->underlyingType == stage.info.type &&
           isPassiveScalarValueType(stage.info.type);
  }
  // A fixed array's extent query never reads its receiver: emission produces
  // the sealed static extent (ArraySize) or the zero endpoint
  // (ArrayRangeBegin). The element family is therefore irrelevant to this
  // operation; the receiver only needs an admitted native representation,
  // and its operand remains independently inventoried.
  bool arrayExtentQuery(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Call ||
        (stage.intrinsic != IntrinsicKind::ArraySize &&
         stage.intrinsic != IntrinsicKind::ArrayRangeBegin) ||
        !stage.receiver || !stage.operands.empty() || stage.functionTarget ||
        stage.constructorTarget || stage.bodyTarget || stage.lambdaTarget ||
        stage.nativeCalleeType || stage.loan ||
        stage.info.type != SemanticType::UInt64)
      return false;
    const auto &array = stage.receiver->type;
    return array.kind == SemanticType::Array && array.arguments.size() == 1 &&
           !array.arrayLengthParameterId && nativeValue(array);
  }
  // A sealed integral enum is represented by exactly its declared fixed-width
  // underlying integer (layout.cpp), so a one-dimensional array of them has
  // the storage and SSA aggregate form of that integer array. Nested arrays,
  // payload enums, Bool/Char backings and every other element family stay
  // outside this predicate.
  bool integralEnumArray(const SemanticType &type) const {
    if (type.kind != SemanticType::Array || type.arguments.size() != 1 ||
        type.arrayLengthParameterId)
      return false;
    const auto &element = type.arguments.front();
    const auto *sealed = program.findIntegralEnum(element);
    return element.kind == SemanticType::Enum && sealed &&
           !program.findPayloadEnum(element) &&
           constantIntegerDomain(sealed->underlyingType).has_value() &&
           nativeValue(element);
  }
  // Complete element-wise formation only: one operand of the exact element
  // type per element, so no unwritten slot is zero-filled into a value that
  // may not name an enumerator.
  bool integralEnumArrayAggregate(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Compute ||
        stage.operation != MirOperation::Aggregate ||
        !integralEnumArray(stage.info.type) ||
        stage.operands.size() != stage.info.type.arrayLength)
      return false;
    const auto &element = stage.info.type.arguments.front();
    return std::all_of(
        stage.operands.begin(), stage.operands.end(),
        [&](const auto &operand) { return operand.type == element; });
  }
  // Formation of a text view array: element-wise from exact elements, views
  // or nested view arrays, or `{}`, whose every view is empty, as a
  // value-initialized C++ view is.
  bool textViewArrayAggregate(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Compute ||
        stage.operation != MirOperation::Aggregate ||
        !textViewArray(stage.info.type))
      return false;
    const auto &element = stage.info.type.arguments.front();
    return (stage.operands.empty() ||
            stage.operands.size() == stage.info.type.arrayLength) &&
           std::all_of(
               stage.operands.begin(), stage.operands.end(),
               [&](const auto &operand) { return operand.type == element; });
  }
  std::optional<Shape> mutexShape(const SemanticType &type) const {
    // A payload or other leaf may be hierarchy storage with exact dispatch
    // metadata; its operations keep their own hierarchy proofs.
    const auto composition = native_llvm::mutexComposition(
        program, type,
        [this](const SemanticType &leaf) { return representedValue(leaf); });
    if (!composition)
      return std::nullopt;
    return composition->closure ? Shape::NativeMutexClosure
                                : Shape::NativeMutexStorage;
  }
  std::optional<Shape> classShape(const SemanticType &type) const {
    // References previously borrowed this ordinary-storage witness while the
    // recursive referent requirement kept hierarchy execution closed. New
    // hierarchy metadata must never turn that into an operation permission.
    if (dispatchProofs.related(type))
      return {};
    if (const auto mutex = mutexShape(type))
      return mutex;
    const auto *leaf = &type;
    while ((leaf->kind == SemanticType::Array ||
            leaf->kind == SemanticType::Reference ||
            leaf->kind == SemanticType::RawPointer) &&
           leaf->arguments.size() == 1)
      leaf = &leaf->arguments[0];
    if (native_llvm::nativeAtomicElement(program, *leaf) && nativeValue(type))
      return Shape::NativeAtomicStorage;
    if (native_llvm::nativeThreadHandle(program, *leaf) && nativeValue(type))
      return Shape::NativeThreadHandle;
    if (leaf->kind == SemanticType::UniqueOwner && nativeValue(type))
      return leaf->arguments.size() == 1 && mutexShape(leaf->arguments[0])
                 ? Shape::NativeOwnedMutexStorage
                 : Shape::NativeUniqueOwnership;
    if (leaf->kind == SemanticType::Enum && program.findPayloadEnum(*leaf) &&
        nativeValue(type))
      return Shape::NativePayloadStorage;
    if ((leaf->kind == SemanticType::FunctionItem ||
         leaf->kind == SemanticType::Lambda ||
         leaf->kind == SemanticType::FunctionSignature) &&
        nativeValue(type))
      return leaf->kind == SemanticType::FunctionSignature
                 ? Shape::CallableMetadata
                 : Shape::NativeCallableStorage;
    if (leaf->kind == SemanticType::Class && nativeValue(type)) {
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == *leaf; });
      if (owner != program.classInstances().end() &&
          native_llvm::sharedOwnerRepresentation(program, *owner))
        return Shape::NativeSharedOwnership;
      if (owner != program.classInstances().end() &&
          owner->compilerCapability ==
              CompilerCapabilityTypeKind::FunctionPointer)
        return Shape::NativeCallableStorage;
      return owner != program.classInstances().end() && owner->unionLayout
                 ? Shape::NativeUnionStorage
                 : Shape::NativeClassStorage;
    }
    return {};
  }
  bool memoryOperation(const MirInstruction &stage) const {
    if (stage.intrinsic >= IntrinsicKind::SharedOwnerEmpty &&
        stage.intrinsic <= IntrinsicKind::SharedOwnerSwap) {
      const bool empty = stage.intrinsic == IntrinsicKind::SharedOwnerEmpty;
      const bool promote =
          stage.intrinsic == IntrinsicKind::SharedOwnerFromUnique;
      const bool clone = stage.intrinsic == IntrinsicKind::SharedOwnerClone;
      const bool borrow = stage.intrinsic == IntrinsicKind::SharedOwnerBorrow;
      const bool swap = stage.intrinsic == IntrinsicKind::SharedOwnerSwap;
      if (stage.kind != MirInstructionKind::Call || stage.functionTarget ||
          stage.receiver ||
          stage.operands.size() != (empty  ? 0
                                    : swap ? 2
                                           : 1) ||
          stage.parameterTypes.size() != stage.operands.size())
        return false;
      const auto *ownerType = &stage.info.type;
      if (!empty && !promote) {
        const auto &parameter = stage.parameterTypes[0];
        if (parameter.kind != SemanticType::Reference ||
            parameter.arguments.size() != 1 ||
            parameter.referenceAccess !=
                (swap ? AccessMode::Mutable : AccessMode::ReadOnly))
          return false;
        ownerType = &parameter.arguments[0];
        for (std::size_t index = 0; index < stage.operands.size(); ++index)
          if (stage.operands[index].type != parameter ||
              stage.parameterTypes[index] != parameter)
            return false;
      }
      if (classShape(*ownerType) != Shape::NativeSharedOwnership)
        return false;
      if (empty || promote || clone)
        return stage.info.type == *ownerType && stage.result &&
               (!promote ||
                stage.parameterTypes[0] ==
                    SemanticType::uniqueOwnerOf(ownerType->arguments[0]));
      if (borrow)
        return stage.loan && stage.info.category == ValueCategory::Place &&
               stage.info.access == AccessMode::ReadOnly &&
               stage.info.type == ownerType->arguments[0];
      return !stage.loan &&
             stage.info.type ==
                 (swap ? SemanticType::Void
                  : stage.intrinsic == IntrinsicKind::SharedOwnerIsNull
                      ? SemanticType::Bool
                      : SemanticType::UInt64);
    }
    if (stage.kind == MirInstructionKind::AllocateObject)
      return stage.info.type.kind == SemanticType::UniqueOwner &&
             nativeValue(stage.info.type) && stage.destination && currentBody &&
             currentBody->findDropObligation(stage.objectAllocation) &&
             currentBody->findDropObligation(stage.objectAllocation)->kind ==
                 MirDropObligationKind::ObjectAllocation;
    // A polymorphic complete object is admitted only by its dispatch proof,
    // which also proves the cleanup its descriptors will select.
    if (stage.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
      if (!native_llvm::nativeUniqueConstructionShape(program, stage) ||
          !nativeValue(stage.info.type) ||
          native_llvm::dynamicOwnerAllocation(program, stage))
        return false;
      // A hierarchy-containing pointee is relocated or field-copied, never
      // transported as object bytes.
      const auto &pointee = stage.info.type.arguments.front();
      if (stage.constructorTarget || stage.operands.empty() ||
          !dispatchProofs.contains(pointee))
        return true;
      if (stage.constructorKind == ConstructorKind::Move)
        return stage.operands.front().kind == MirOperandKind::Value &&
               dispatchProofs.generatedRelocation(pointee);
      try {
        native_llvm::preflightGeneratedHierarchyCopy(program, pointee);
        return true;
      } catch (const BackendUnsupportedError &) {
        return false;
      }
    }
    if (stage.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
        stage.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
        stage.intrinsic == IntrinsicKind::UniqueOwnerIsNull ||
        stage.intrinsic == IntrinsicKind::UniqueOwnerSwap) {
      const bool swap = stage.intrinsic == IntrinsicKind::UniqueOwnerSwap;
      const bool borrow =
          stage.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
          stage.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut;
      const bool writable =
          swap || stage.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut;
      if (stage.kind != MirInstructionKind::Call || stage.functionTarget ||
          stage.receiver || stage.parameterTypes.size() != (swap ? 2 : 1) ||
          stage.operands.size() != stage.parameterTypes.size())
        return false;
      const auto &parameter = stage.parameterTypes[0];
      if (!swap) {
        if (parameter.kind != SemanticType::UniqueOwner ||
            !nativeValue(parameter) || stage.operands[0].type != parameter)
          return false;
        return borrow
                   ? stage.loan &&
                         stage.info.category == ValueCategory::Place &&
                         stage.info.type == parameter.arguments[0] &&
                         stage.info.access == (writable ? AccessMode::Mutable
                                                        : AccessMode::ReadOnly)
                   : !stage.loan && stage.info.type == SemanticType::Bool;
      }
      if (parameter.kind != SemanticType::Reference ||
          parameter.arguments.size() != 1 ||
          parameter.referenceAccess !=
              (writable ? AccessMode::Mutable : AccessMode::ReadOnly) ||
          parameter.arguments[0].kind != SemanticType::UniqueOwner ||
          !nativeValue(parameter.arguments[0]))
        return false;
      for (std::size_t index = 0; index < stage.operands.size(); ++index)
        if (stage.parameterTypes[index] != parameter ||
            stage.operands[index].type != parameter)
          return false;
      return !stage.loan && stage.info.type == SemanticType::Void;
    }
    if (stage.intrinsic == IntrinsicKind::ObjectConstruct) {
      if (!native_llvm::nativeObjectConstructionShape(program, stage))
        return false;
      const auto &type = stage.parameterTypes[0].arguments[0];
      if (nativeValue(type))
        return true;
      // Hierarchy storage is constructed by its selected constructor, by
      // relocating a moved owned argument, or by the generated field copy.
      if (!dispatchProofs.contains(type) ||
          !dispatchProofs.metadata(type).has_value())
        return false;
      if (stage.constructorTarget)
        return true;
      if (stage.operands.size() != 2 ||
          stage.operands[1].kind != MirOperandKind::Value)
        return false;
      if (stage.constructorKind == ConstructorKind::Move)
        return dispatchProofs.generatedRelocation(type);
      if (stage.constructorKind != ConstructorKind::Copy)
        return false;
      try {
        native_llvm::preflightGeneratedHierarchyCopy(program, type);
        return true;
      } catch (const BackendUnsupportedError &) {
        return false;
      }
    }
    if (stage.intrinsic == IntrinsicKind::ObjectSize ||
        stage.intrinsic == IntrinsicKind::ObjectAlignment)
      return mir_detail::objectLayoutQuery(program, stage) &&
             (nativeValue(*stage.objectLayoutType) ||
              dispatchProofs.metadata(*stage.objectLayoutType).has_value());
    if (stage.intrinsic == IntrinsicKind::ObjectRelocate ||
        stage.intrinsic == IntrinsicKind::ObjectRelocateRange)
      return mir_detail::objectRelocate(program, stage) &&
             relocatableRepresentation(stage.parameterTypes[0].arguments[0]);
    if (stage.intrinsic == IntrinsicKind::ObjectDestroy) {
      if (!mir_detail::objectDestroy(program, stage))
        return false;
      const auto &type = stage.parameterTypes[0].arguments[0];
      if (nativeValue(type))
        return true;
      // Hierarchy storage ends through its exact proved cleanup recipe.
      return dispatchProofs.contains(type) &&
             dispatchProofs.metadata(type).has_value() &&
             (!stage.elementCleanup ||
              dispatchProofs.cleanup(*stage.elementCleanup).has_value());
    }
    if (stage.kind != MirInstructionKind::Call ||
        stage.info.type != SemanticType::Void || stage.result ||
        stage.receiver || stage.functionTarget || stage.constructorTarget ||
        stage.bodyTarget || stage.loan)
      return false;
    if (stage.intrinsic == IntrinsicKind::StorageBoundsCheck)
      return stage.parameterTypes ==
                 std::vector<SemanticType>{SemanticType::UInt64,
                                           SemanticType::UInt64} &&
             stage.operands.size() == 2 &&
             stage.operands[0].type == SemanticType::UInt64 &&
             stage.operands[1].type == SemanticType::UInt64;
    if (const auto outcome = intrinsicBooleanFailureOutcome(stage.intrinsic)) {
      const auto *site =
          stage.localFailureSites.size() == 1
              ? program.failureMetadata().findSite(stage.localFailureSites[0])
              : nullptr;
      return stage.parameterTypes ==
                 std::vector<SemanticType>{SemanticType::Bool} &&
             stage.operands.size() == 1 &&
             stage.operands[0].type == SemanticType::Bool &&
             stage.definedFailure.propagation == FailurePropagationKind::None &&
             stage.definedFailure.localOrigins.size() == 1 &&
             stage.definedFailure.localOrigins[0].outcomes ==
                 std::vector<DefinedFailureOutcome>{*outcome} &&
             site &&
             site->outcomes == std::vector<DefinedFailureOutcome>{*outcome};
    }
    return false;
  }
  const MirClassInstance *nativeClass(const SemanticType &type) const {
    if (type.kind != SemanticType::Class || !representedValue(type))
      return nullptr;
    for (const auto &owner : program.classInstances())
      if (owner.type == type &&
          owner.compilerCapability == CompilerCapabilityTypeKind::None)
        return &owner;
    return nullptr;
  }
  bool nativeClassPointee(const SemanticType &type) const {
    const auto *leaf = &type;
    while (leaf->kind == SemanticType::Array && leaf->arguments.size() == 1) {
      if (!leaf->arrayLength || leaf->arrayLengthParameterId)
        return false;
      leaf = &leaf->arguments[0];
    }
    return nativeClass(*leaf) && representedValue(type);
  }
  bool nativeRawPlace(const MirBody &mir, const MirPlace &place) const {
    const auto *root = mir.findValue(place.value);
    if (place.root != MirPlaceRootKind::Value || !root ||
        root->info.type.kind != SemanticType::RawPointer ||
        root->info.type.arguments.size() != 1 ||
        !nativeClassPointee(root->info.type.arguments[0]))
      return false;
    auto type = root->info.type;
    auto access = AccessMode::ReadOnly;
    bool raw = false;
    for (const auto &projection : place.projections) {
      if (projection.kind == MirProjectionKind::RawDereference ||
          projection.kind == MirProjectionKind::RawIndex) {
        if (type.kind != SemanticType::RawPointer ||
            type.arguments.size() != 1 ||
            type.arguments[0] == SemanticType::Void ||
            !representedValue(type.arguments[0]))
          return false;
        access = type.pointerAccess;
        auto element = type.arguments[0];
        type = std::move(element);
        raw = true;
      } else if (projection.kind == MirProjectionKind::Field) {
        const auto *owner = nativeClass(type);
        if (!owner)
          return false;
        const auto field =
            std::find_if(owner->declaredFields.begin(),
                         owner->declaredFields.end(), [&](const auto &field) {
                           return field.symbol == projection.field;
                         });
        if (field == owner->declaredFields.end())
          return false;
        if (field->access != AccessMode::Mutable)
          access = AccessMode::ReadOnly;
        type = field->type;
      } else if (projection.kind == MirProjectionKind::Index) {
        if (type.kind != SemanticType::Array || type.arguments.size() != 1)
          return false;
        auto element = type.arguments[0];
        type = std::move(element);
      } else {
        return false;
      }
      if (projection.kind == MirProjectionKind::Index ||
          projection.kind == MirProjectionKind::RawIndex) {
        const auto *index = mir.findValue(projection.index);
        // A constant-folded index legitimately retains both its evaluated
        // value and the folded constant; emission prefers the value and falls
        // back to the constant, so requiring exactly one refused a shape it
        // already lowers. An index carrying neither remains malformed.
        if ((!projection.index && !projection.constantIndex.has_value()) ||
            (projection.index &&
             (!index || !constantIntegerDomain(index->info.type))))
          return false;
      }
    }
    return raw && type == place.type && representedValue(type) &&
           (place.access != AccessMode::Mutable ||
            access == AccessMode::Mutable);
  }
  bool nativeRawIndex(const MirInstruction &stage) const {
    return stage.operation == MirOperation::Index &&
           stage.operands.size() == 2 &&
           stage.operands[0].type.kind == SemanticType::RawPointer &&
           stage.operands[0].type.arguments.size() == 1 &&
           nativeClassPointee(stage.operands[0].type.arguments[0]) &&
           stage.info.type == stage.operands[0].type.arguments[0] &&
           constantIntegerDomain(stage.operands[1].type).has_value();
  }
  std::pair<bool, bool> nativeRawStage(const MirBody &mir,
                                       const MirInstruction &stage) const {
    bool extended = nativeRawIndex(stage), valid = true;
    const auto place = [&](MirPlaceId id) {
      const auto *selected = mir.findPlace(id);
      if (!selected ||
          !std::any_of(selected->projections.begin(),
                       selected->projections.end(), [](const auto &projection) {
                         return projection.kind ==
                                    MirProjectionKind::RawDereference ||
                                projection.kind == MirProjectionKind::RawIndex;
                       }))
        return;
      if (!scalarRawAccess(mir, *selected)) {
        extended = true;
        valid &= nativeRawPlace(mir, *selected);
      }
    };
    if (stage.destination)
      place(*stage.destination);
    if (stage.receiver)
      place(stage.receiver->place);
    for (const auto &operand : stage.operands)
      place(operand.place);
    return {extended, valid};
  }
  bool nativePointerArithmetic(const MirInstruction &stage) const {
    if (stage.operands.size() != 2)
      return false;
    const bool reversed =
        stage.operation == MirOperation::PointerAdd &&
        stage.operands[1].type.kind == SemanticType::RawPointer;
    const auto &pointer = stage.operands[reversed ? 1 : 0].type;
    if (pointer.kind != SemanticType::RawPointer ||
        pointer.arguments.size() != 1 ||
        !nativeClassPointee(pointer.arguments[0]))
      return false;
    if (stage.operation == MirOperation::PointerDifference)
      return stage.operands[1].type == pointer &&
             stage.info.type == SemanticType::Int64;
    return (stage.operation == MirOperation::PointerAdd ||
            stage.operation == MirOperation::PointerSubtract) &&
           stage.info.type == pointer &&
           constantIntegerDomain(stage.operands[reversed ? 0 : 1].type)
               .has_value();
  }
  // An exact payload enum whose own storage is admitted as native payload
  // storage. Raw access through it never reads the tag or a payload field:
  // arithmetic strides by the payload layout and a raw reference transports
  // the selected address unchanged. Hierarchy-related and capability
  // payloads fail the storage witness and stay closed.
  bool nativePayloadPointee(const SemanticType &type) const {
    return type.kind == SemanticType::Enum && program.findPayloadEnum(type) &&
           nativeValue(type) && classShape(type) == Shape::NativePayloadStorage;
  }
  bool payloadPointerArithmetic(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Compute ||
        stage.operands.size() != 2 ||
        (stage.operation != MirOperation::PointerAdd &&
         stage.operation != MirOperation::PointerSubtract))
      return false;
    const bool reversed =
        stage.operation == MirOperation::PointerAdd &&
        stage.operands[1].type.kind == SemanticType::RawPointer;
    const auto &pointer = stage.operands[reversed ? 1 : 0].type;
    return pointer.kind == SemanticType::RawPointer &&
           pointer.arguments.size() == 1 &&
           nativePayloadPointee(pointer.arguments[0]) &&
           stage.info.type == pointer &&
           constantIntegerDomain(stage.operands[reversed ? 0 : 1].type)
               .has_value();
  }
  std::optional<Shape> pointerArithmeticInput(const MirBlock &block,
                                              const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::CallInput || !stage.result ||
        stage.callInputRole != MirCallInputRole::Argument ||
        stage.callInputKind != HirCallInputKind::Value || !stage.callSite ||
        !stage.hirValue || stage.defaultArgument || stage.rawMemoryAccess ||
        stage.unsafeOperation != UnsafeOperationKind::PointerArithmetic ||
        stage.operation != MirOperation::None ||
        stage.intrinsic != IntrinsicKind::None || stage.operands.size() != 1 ||
        stage.info.type.kind != SemanticType::RawPointer ||
        stage.info.category != ValueCategory::Value ||
        stage.info.access != AccessMode::ReadOnly || stage.destination ||
        stage.receiver || stage.functionTarget || stage.constructorTarget ||
        stage.bodyTarget || stage.lambdaTarget || stage.nativeCalleeType ||
        stage.ownership || !stage.lifecycle.empty() ||
        !stage.definedFailure.empty() || !stage.localFailureSites.empty())
      return std::nullopt;
    const auto &input = stage.operands.front();
    if (input.kind != MirOperandKind::Value || !input.value || input.place ||
        input.loan || input.literal ||
        input.type.kind != SemanticType::RawPointer)
      return std::nullopt;
    auto destination = stage.info.type;
    destination.pointerAccess = input.type.pointerAccess;
    if (destination != input.type ||
        (input.type.pointerAccess == AccessMode::ReadOnly &&
         stage.info.type.pointerAccess == AccessMode::Mutable))
      return std::nullopt;
    // Call-input staging transports an already evaluated pointer; it does not
    // perform another arithmetic operation or discharge another unsafe action.
    for (const auto &producer : block.instructions) {
      if (producer.id == stage.id)
        break;
      if (!producer.tombstone && producer.result == input.value &&
          producer.kind == MirInstructionKind::Compute &&
          producer.hirValue == stage.hirValue &&
          producer.info.type == input.type &&
          producer.unsafeOperation == UnsafeOperationKind::PointerArithmetic)
        // Report the shape the producer's own arithmetic was admitted under.
        // Staging transports that evaluated pointer unchanged, so a primitive
        // pointee must not be reported as class raw access.
        return nativePointerArithmetic(producer)
                   ? std::optional(Shape::NativeClassRawAccess)
               : scalarPointerArithmetic(producer)
                   ? std::optional(Shape::PrimitiveRawAccess)
               : payloadPointerArithmetic(producer)
                   ? std::optional(Shape::NativePayloadRawAccess)
                   : std::nullopt;
    }
    return std::nullopt;
  }
  bool nativeRawFieldStage(const MirBody &mir,
                           const MirInstruction &stage) const {
    if ((stage.kind != MirInstructionKind::Load &&
         stage.kind != MirInstructionKind::Assign) ||
        stage.receiver || stage.functionTarget || stage.constructorTarget ||
        stage.bodyTarget || stage.lambdaTarget || stage.nativeCalleeType)
      return false;
    const auto field = [&](MirPlaceId id) {
      const auto *place = mir.findPlace(id);
      if (!place || !nativeRawPlace(mir, *place))
        return false;
      bool dereferenced = false;
      for (const auto &projection : place->projections) {
        dereferenced |= projection.kind == MirProjectionKind::RawDereference;
        if (dereferenced && projection.kind == MirProjectionKind::Field)
          return true;
      }
      return false;
    };
    if (stage.destination && field(*stage.destination))
      return true;
    return std::any_of(
        stage.operands.begin(), stage.operands.end(),
        [&](const auto &operand) { return field(operand.place); });
  }
  // A method reached through a raw pointer is the same member access as a
  // field read through it: its receiver is staged as a borrow of the
  // dereferenced native raw place, and the call consuming that staged
  // receiver shares the access. Either stage is admitted exactly when the
  // dereferenced place is.
  bool nativeRawReceiverStage(const MirBody &mir,
                              const MirInstruction &stage) const {
    const auto staged = [&](const MirInstruction &input) {
      const bool mutableInput =
          input.callInputKind == HirCallInputKind::MutableBorrow;
      if (input.kind != MirInstructionKind::CallInput ||
          input.callInputRole != MirCallInputRole::Receiver ||
          (input.callInputKind != HirCallInputKind::ReadBorrow &&
           !mutableInput) ||
          input.operands.size() != 1 ||
          input.operands.front().kind !=
              (mutableInput ? MirOperandKind::BorrowWrite
                            : MirOperandKind::BorrowRead))
        return false;
      // The receiver is the pointee or a field or element reached from it;
      // nativeRawPlace proves the whole path, including its raw step.
      const auto *place = mir.findPlace(input.operands.front().place);
      return place && !place->projections.empty() &&
             nativeRawPlace(mir, *place);
    };
    if (stage.kind == MirInstructionKind::CallInput)
      return staged(stage);
    if (stage.kind != MirInstructionKind::Call || !stage.receiver ||
        stage.receiver->kind != MirOperandKind::Value ||
        !stage.functionTarget || stage.intrinsic != IntrinsicKind::None)
      return false;
    const auto *value = mir.findValue(stage.receiver->value);
    const auto *block = value ? mir.findBlock(value->definitionBlock) : nullptr;
    if (!block)
      return false;
    const auto input = std::find_if(
        block->instructions.begin(), block->instructions.end(),
        [&](const MirInstruction &candidate) {
          return candidate.id == value->definition &&
                 candidate.result == stage.receiver->value;
        });
    // The staged receiver carries the raw access whatever the call's own
    // marker: none for an ordinary method on a field reached through the
    // pointer, the same access for a method on the pointee itself, or the
    // method's declared unsafe permission, which is admitted separately.
    return input != block->instructions.end() && !input->tombstone &&
           input->callSite != 0 && input->callSite == stage.callSite &&
           staged(*input);
  }
  bool nativeAddressConversion(const MirInstruction &stage) const {
    if (!mir_detail::rawPointerConversion(stage))
      return false;
    const auto &source = stage.operands[0].type;
    const auto &target = stage.info.type;
    return (source.arguments[0] == SemanticType::Void ||
            representedValue(source.arguments[0])) &&
           (target.arguments[0] == SemanticType::Void ||
            representedValue(target.arguments[0]));
  }
  // Exact staged managed-thread operation whose handle, task and result
  // values all have admitted native representations.
  bool nativeThreadStage(const MirInstruction &stage) const {
    if (!body || !currentBody ||
        !native_llvm::nativeThreadCall(program, *body, *currentBody, stage))
      return false;
    for (const auto &operand : stage.operands) {
      const auto &type = operand.type.kind == SemanticType::Reference
                             ? operand.type.arguments.front()
                             : operand.type;
      if (!relocatableRepresentation(type))
        return false;
    }
    if (stage.result && !relocatableRepresentation(stage.info.type))
      return false;
    // An unscoped task enters, and a result leaves, a runtime box through the
    // one typed relocation; its generated form must accept the layout.
    const auto boxed = [&](const SemanticType &type) {
      const auto kind = mirTypeRelocationKind(program, type);
      return kind == RelocationKind::Bitwise ||
             (kind == RelocationKind::Structural &&
              dispatchProofs.generatedRelocation(type));
    };
    const auto adapter = [&](MirThreadTaskAdapterId id) {
      const auto *selected = program.findThreadTaskAdapter(id);
      return selected && relocatableRepresentation(selected->taskType) &&
             (selected->scoped || boxed(selected->taskType)) &&
             (selected->resultType == SemanticType::Void ||
              (relocatableRepresentation(selected->resultType) &&
               boxed(selected->resultType)));
    };
    if (stage.threadTaskAdapter && !adapter(*stage.threadTaskAdapter))
      return false;
    for (const auto &task : stage.scopedThreadTasks)
      if (!adapter(task.adapter))
        return false;
    return true;
  }
  std::optional<Shape> representationShape(const SemanticType &type) {
    return dispatchProofs.related(type) ? dispatchProofs.metadata(type)
                                        : classShape(type);
  }
  bool nativeRetirement(const MirHostedStartupOperation &operation) const {
    const auto &plan = program.hostedStartupPlan();
    if (!plan || operation.kind !=
                     MirHostedStartupOperationKind::RetireProgramStorage ||
        operation.failureBehavior != MirHostedStartupFailureBehavior::None ||
        operation.terminator || operation.place || operation.value ||
        operation.dropObligation || operation.failureRecord ||
        operation.cleanupBoundary)
      return false;
    const auto *startup = findMirBody(
        program, {MirBodyKind::HostedStartup, plan->entry});
    const auto *owner = startup ? startup->findBlock(operation.block) : nullptr;
    const MirInstruction *stage = nullptr;
    if (owner)
      for (const auto &candidate : owner->instructions)
        if (candidate.id == operation.instruction)
          stage = &candidate;
    if (!stage || stage->hostedStartupOperation != operation.id ||
        stage->kind != MirInstructionKind::RetireProgramStorage)
      return false;
    bool found = false;
    for (const auto &step : program.programInitializationPlan().steps)
      if (step.nativeRetirement) {
        if (!mirDropIsNativeRetirement(program, *step.nativeRetirement) ||
            !mutexShape(step.nativeRetirement->type))
          return false;
        found = true;
      }
    return found;
  }
  const MirClassInstance *bodyClass() const {
    if (!body)
      return nullptr;
    if (body->kind == MirBodyKind::FieldInitializers ||
        body->kind == MirBodyKind::StaticFieldInitializers)
      return program.findClassInstance(body->owner);
    if (body->kind == MirBodyKind::Constructor)
      return program.findClassInstance(
          program.findConstructorInstance(body->owner)->owner);
    if (body->kind == MirBodyKind::Destructor)
      return program.findClassInstance(
          program.findDestructorInstance(body->owner)->owner);
    if (body->kind == MirBodyKind::Function) {
      const auto *target = program.findFunctionInstance(body->owner);
      if (target->owner)
        return program.findClassInstance(*target->owner);
    }
    return nullptr;
  }
  std::optional<Shape> bodyClassShape() {
    if (body && body->kind == MirBodyKind::ThreadLocalInitializer)
      if (const auto *storage = program.findThreadLocalStorage(body->owner))
        if (nativeValue(storage->type))
          return Shape::NativeThreadStorage;
    if (body && body->kind == MirBodyKind::Lambda)
      if (const auto *lambda = program.findLambda(body->owner))
        return classShape(lambda->type);
    const auto *owner = bodyClass();
    if (owner && dispatchProofs.contains(owner->type)) {
      const auto *proof = body ? dispatchProofs.body(*body) : nullptr;
      return proof ? proof->shape : std::nullopt;
    }
    return owner ? classShape(owner->type) : std::nullopt;
  }
  std::optional<Shape> placeClassShape(const MirPlace &place) {
    if (const auto selected = representationShape(place.type))
      return selected;
    if (place.root == MirPlaceRootKind::This)
      return bodyClassShape();
    if (place.root == MirPlaceRootKind::Value && currentBody) {
      const auto *value = currentBody->findValue(place.value);
      if (value && value->info.category == ValueCategory::Place)
        for (const auto &block : currentBody->blocks)
          for (const auto &stage : block.instructions)
            if (!stage.tombstone && stage.result == place.value &&
                stage.operation == MirOperation::PayloadExtract)
              return classShape(mirInstructionEnumType(stage));
      return value ? representationShape(value->info.type) : std::nullopt;
    }
    if (currentBody)
      for (const auto &root : currentBody->places)
        if (sameStorageRoot(root, place)) {
          if (const auto selected = representationShape(root.type))
            return selected;
          // A concrete parameter pack has no storage of its own: each
          // constant element is an independent parameter object, so a
          // projection through one is witnessed by that element's storage.
          const auto &front = place.projections.empty()
                                  ? MirPlaceProjection{}
                                  : place.projections.front();
          if (root.type.kind == SemanticType::TypePack &&
              root.type.concretePack && !place.projections.empty() &&
              front.kind == MirProjectionKind::PackElement &&
              front.constantIndex &&
              *front.constantIndex < root.type.arguments.size() &&
              concretePackParameter(root))
            return representationShape(
                root.type.arguments[*front.constantIndex]);
        }
    if (place.root == MirPlaceRootKind::Loan && currentBody)
      if (const auto root = loanRootType(*currentBody, place.loan))
        return representationShape(*root);
    return {};
  }
  // The object a loan root designates when no unprojected place names it,
  // typed exactly as native place lowering types the loan slot: the result of
  // the borrow, call or construction producing the loan, otherwise the
  // referent of the loan's source place.
  static std::optional<SemanticType> loanRootType(const MirBody &body,
                                                  MirLoanId id) {
    const auto *loan = id ? body.findLoan(id) : nullptr;
    const auto *source = loan ? body.findPlace(loan->source) : nullptr;
    std::optional<SemanticType> type;
    if (source)
      type = source->type.kind == SemanticType::Reference &&
                     source->type.arguments.size() == 1
                 ? source->type.arguments.front()
                 : source->type;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.loan == id &&
            (stage.kind == MirInstructionKind::Call ||
             stage.kind == MirInstructionKind::CallBody ||
             stage.kind == MirInstructionKind::Construct ||
             stage.kind == MirInstructionKind::Borrow))
          type = stage.info.type;
    return type;
  }

  bool opaqueReferent(const SemanticType &type) const {
    if (type.kind != SemanticType::Class)
      return false;
    return std::any_of(program.classInstances().begin(),
                       program.classInstances().end(),
                       [&](const auto &instance) {
                         return instance.type == type &&
                                instance.compilerCapability ==
                                    CompilerCapabilityTypeKind::ErasedObject;
                       });
  }
  bool passiveNonEnumReference(const SemanticType &type) const {
    return passiveReferenceType(type) || opaqueReferent(type) ||
           (type.kind == SemanticType::Reference &&
            type.arguments.size() == 1 && opaqueReferent(type.arguments[0]));
  }
  bool representedErasureObject(const SemanticType &type) const {
    if (!nativeValue(type))
      return false;
    if (mir_detail::completeObjectType(program, type))
      return true;
    if (type.kind != SemanticType::Lambda && type.kind != SemanticType::Class)
      return false;
    return classShape(type) == Shape::NativeCallableStorage;
  }
  bool representedErasureReference(const SemanticType &type) const {
    const auto *referent = &type;
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      referent = &type.arguments[0];
    return representedErasureObject(*referent);
  }
  bool passiveReference(const SemanticType &type) const {
    if (passiveNonEnumReference(type))
      return true;
    const auto &scalar =
        type.kind == SemanticType::Reference && type.arguments.size() == 1
            ? type.arguments[0]
            : type;
    // An admitted array value is selected by address like a passive array;
    // its elements are reached only through checked array projection.
    if (textViewArray(scalar) || integralEnumArray(scalar))
      return true;
    return scalar.kind == SemanticType::Enum &&
           program.findIntegralEnum(scalar) &&
           !program.findPayloadEnum(scalar) && nativeValue(scalar);
  }
  bool erasedReference(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Call || !stage.loan ||
        stage.functionTarget || stage.operands.size() != 1 ||
        stage.parameterTypes.size() != 1 ||
        stage.parameterTypes[0].kind != SemanticType::Reference ||
        stage.parameterTypes[0].arguments.size() != 1 ||
        stage.info.category != ValueCategory::Place)
      return false;
    const bool erase = stage.intrinsic == IntrinsicKind::EraseReference ||
                       stage.intrinsic == IntrinsicKind::EraseReferenceMut;
    const bool restore = stage.intrinsic == IntrinsicKind::RestoreReference ||
                         stage.intrinsic == IntrinsicKind::RestoreReferenceMut;
    return (erase && opaqueReferent(stage.info.type) &&
            representedErasureReference(stage.parameterTypes[0])) ||
           (restore && opaqueReferent(stage.parameterTypes[0].arguments[0]) &&
            representedErasureReference(stage.info.type));
  }
  bool restoredLoanTransport(const MirBody &body,
                             const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Borrow || !stage.loan ||
        stage.operands.size() != 1 ||
        stage.operands[0].kind != MirOperandKind::Loan ||
        !representedErasureReference(stage.info.type))
      return false;
    const auto *target = body.findLoan(*stage.loan);
    const auto *source = body.findLoan(stage.operands[0].loan);
    if (!target || !source || target->access != stage.info.access ||
        source->access != target->access ||
        !(target->parent == source->id ||
          (target->kind == MirLoanKind::Return && target->escapes &&
           target->source == source->source &&
           target->producedBy == source->producedBy)))
      return false;
    const MirInstruction *producer = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &candidate : block.instructions)
        if (!candidate.tombstone &&
            candidate.kind == MirInstructionKind::Call &&
            candidate.loan == source->id) {
          if (producer)
            return false;
          producer = &candidate;
        }
    const auto *authority = mir_detail::restoredBorrowAuthority(body, *target);
    if ((target->storedDependency || source->storedDependency) &&
        authority != producer)
      return false;
    return producer && producer->kind == MirInstructionKind::Call &&
           producer->info.type == stage.info.type &&
           producer->info.access == stage.info.access &&
           mir_detail::verifiedReferenceRestoreCall(program, body, *producer);
  }

  bool ordinaryRawReferenceObject(const SemanticType &type) const {
    if (type.kind != SemanticType::Class)
      return false;
    const MirClassInstance *owner = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == type) {
        if (owner)
          return false;
        owner = &candidate;
      }
    if (owner && (owner->kind == ClassKind::Class ||
                  owner->kind == ClassKind::Struct) &&
        owner->compilerCapability == CompilerCapabilityTypeKind::None &&
        !owner->abstract && dispatchProofs.contains(type))
      // The exact referent's own hierarchy storage: the reference is that
      // unchanged address, never a base view or an object transfer.
      return dispatchProofs.metadata(type).has_value();
    return owner &&
           (owner->kind == ClassKind::Class ||
            owner->kind == ClassKind::Struct) &&
           owner->compilerCapability == CompilerCapabilityTypeKind::None &&
           !owner->polymorphic && !owner->abstract && !owner->cOpaqueHandle &&
           !owner->cAbiRecord && !owner->cAbiLayout && !owner->unionLayout &&
           owner->bases.empty() && owner->structuralBases.empty() &&
           nativeValue(type) && classShape(type) == Shape::NativeClassStorage;
  }

  // A native C record is addressed like any complete object; its fields
  // keep the C layout the record's own layout supplies.
  bool nativeRecordObject(const SemanticType &type) const {
    if (type.kind != SemanticType::Class)
      return false;
    const MirClassInstance *owner = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == type) {
        if (owner)
          return false;
        owner = &candidate;
      }
    return owner && (owner->cAbiRecord || owner->cAbiLayout) &&
           !owner->cOpaqueHandle && !owner->unionLayout &&
           owner->compilerCapability == CompilerCapabilityTypeKind::None &&
           owner->bases.empty() && owner->structuralBases.empty() &&
           nativeValue(type);
  }

  bool representedRawReferenceTypes(const MirInstruction &stage) const {
    const auto &pointer = stage.operands[0].type;
    if (!(passiveAddress(pointer) && passiveReferenceType(stage.info.type)) &&
        !ordinaryRawReferenceObject(pointer.arguments.front()) &&
        !nativeRecordObject(pointer.arguments.front()) &&
        !payloadRawReferent(stage) &&
        !(integralEnumAddress(pointer) &&
          stage.info.type == pointer.arguments.front()))
      return false;
    const auto &witness = stage.parameterTypes[1];
    // The verifier owns origin/access; the witness need not share the raw
    // pointer's type or address. Neither branch materializes or copies it.
    return passiveReferenceType(witness) ||
           ordinaryRawReferenceObject(witness.arguments.front());
  }
  // The raw-borrow call's result names exactly the pointer's payload-enum
  // pointee; the reference is that unchanged address.
  bool payloadRawReferent(const MirInstruction &stage) const {
    const auto &pointer = stage.operands[0].type;
    return pointer.kind == SemanticType::RawPointer &&
           pointer.arguments.size() == 1 &&
           nativePayloadPointee(pointer.arguments.front()) &&
           stage.info.type == pointer.arguments.front();
  }

  bool representedTextAddress(const MirBody &body,
                              const MirInstruction &stage) const {
    if (!mir_detail::textAddressResultLoan(body, stage))
      return false;
    const std::size_t witness =
        stage.intrinsic == IntrinsicKind::TextViewFromAddress ? 2 : 1;
    const auto &formal = stage.parameterTypes[witness];
    // The exact MIR query owns the address/count, ordered readonly witness
    // and loan. Native representation only proves the independent owner's
    // existing address contract; it does not read, copy or rebuild the owner.
    return passiveReferenceType(formal) ||
           ordinaryRawReferenceObject(formal.arguments.front());
  }

  const MirInstruction *earlierLiveCallResult(
      const MirBody &body, const MirBlock &block, const MirInstruction &stage,
      const MirLoan &loan, const SemanticType &referent,
      ValueCategory category = ValueCategory::Place, MirValueId returned = 0,
      bool allowReferenceInvoke = false) const {
    if (body.findBlock(block.id) != &block)
      return nullptr;
    // A source expression's caller permission is repeated on materialization.
    // Only the exact earlier call's runtime loan supplies this address; the
    // possible owner origins are not an alternative execution address.
    const MirInstruction *producer = nullptr;
    bool encountered = false;
    for (const auto &candidate : block.instructions) {
      if (&candidate == &stage) {
        encountered = true;
        break;
      }
      if (candidate.tombstone)
        continue;
      if (candidate.kind == MirInstructionKind::EndBorrow &&
          candidate.loan == loan.id)
        return nullptr;
      if (candidate.kind != MirInstructionKind::Call ||
          candidate.hirValue != stage.hirValue || candidate.loan != loan.id)
        continue;
      if (producer)
        return nullptr;
      producer = &candidate;
    }
    if (!encountered)
      return nullptr;
    if (!producer && allowReferenceInvoke && category == ValueCategory::Place &&
        !returned) {
      // A reference return carries the runtime loan instead of an SSA value.
      // Bind that loan to its unique typed producer before checking the edge.
      for (const auto &previous : body.blocks)
        for (const auto &candidate : previous.instructions) {
          if (candidate.tombstone ||
              candidate.kind != MirInstructionKind::Call ||
              candidate.hirValue != stage.hirValue || candidate.loan != loan.id)
            continue;
          if (returned || !candidate.result)
            return nullptr;
          returned = *candidate.result;
        }
    }
    if (!producer && returned &&
        ((category == ValueCategory::Value &&
          (referent == SemanticType::StringView ||
           referent == SemanticType::CString)) ||
         (allowReferenceInvoke && category == ValueCategory::Place))) {
      const auto *value = body.findValue(returned);
      const auto *previous =
          value ? body.findBlock(value->definitionBlock) : nullptr;
      if (!value || !previous || previous == &block ||
          value->sourceValue != stage.hirValue || block.failureParameter ||
          block.activeFailure || !block.parameters.empty())
        return nullptr;
      const auto &term = previous->terminator;
      if (term.kind != MirTerminatorKind::Invoke || term.target != block.id ||
          !term.elseTarget || term.elseTarget == block.id ||
          !term.failureRecord || term.invokeInstruction != value->definition ||
          !term.targetArguments.empty() || !term.elseArguments.empty() ||
          !term.successLifecycle.empty())
        return nullptr;
      for (const auto &candidate : previous->instructions) {
        if (candidate.tombstone)
          continue;
        if (producer)
          return nullptr;
        if (candidate.id == value->definition)
          producer = &candidate;
      }
      if (!producer || producer->kind != MirInstructionKind::Call ||
          producer->hirValue != stage.hirValue || producer->loan != loan.id)
        return nullptr;
    }
    return producer && producer->result &&
                   (!returned || *producer->result == returned) &&
                   producer->info.category == category &&
                   producer->info.type == referent &&
                   producer->info.access == loan.access
               ? producer
               : nullptr;
  }

  bool unsafeCallResultProducer(const MirBody &body, const MirBlock &block,
                                const MirInstruction &stage,
                                const MirLoan &loan,
                                const SemanticType &referent,
                                ValueCategory category = ValueCategory::Place,
                                MirValueId returned = 0,
                                bool allowReferenceInvoke = false) const {
    const auto *producer =
        earlierLiveCallResult(body, block, stage, loan, referent, category,
                              returned, allowReferenceInvoke);
    const auto *target =
        producer && producer->functionTarget
            ? program.findFunctionInstance(*producer->functionTarget)
            : nullptr;
    const bool exactReturn =
        target && ((category == ValueCategory::Place &&
                    target->returnType.kind == SemanticType::Reference &&
                    target->returnType.arguments.size() == 1 &&
                    target->returnType.arguments.front() == referent &&
                    target->returnType.referenceAccess == loan.access) ||
                   (category == ValueCategory::Value &&
                    (referent == SemanticType::StringView ||
                     referent == SemanticType::CString) &&
                    target->returnType == referent));
    return producer && target && exactReturn &&
           producer->unsafeOperation ==
               UnsafeOperationKind::DeclaredUnsafeCall &&
           producer->intrinsic == IntrinsicKind::None &&
           !producer->rawMemoryAccess && !producer->nativeCalleeType &&
           !producer->lambdaTarget &&
           producer->dispatch == CallDispatch::Static &&
           target->definitionKind == MirDefinitionKind::Source &&
           target->linkage == LanguageLinkage::Gti && target->requiresUnsafe;
  }

  bool unsafeCallResultLoad(const MirBody &body, const MirBlock &block,
                            const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Load || !stage.result ||
        stage.hirValue == 0 || stage.rawMemoryAccess ||
        stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall ||
        stage.intrinsic != IntrinsicKind::None ||
        stage.operation != MirOperation::None ||
        stage.info.category != ValueCategory::Value ||
        stage.info.access != AccessMode::ReadOnly ||
        stage.operands.size() != 1 || stage.destination || stage.receiver ||
        stage.loan || stage.functionTarget || stage.constructorTarget ||
        stage.lambdaTarget || stage.bodyTarget || stage.nativeCalleeType ||
        stage.nativeCallbackAdapter || stage.threadTaskAdapter ||
        stage.callableInvocation || !stage.parameterTypes.empty() ||
        !stage.lifecycle.empty() || !stage.definedFailure.empty() ||
        !stage.localFailureSites.empty())
      return false;
    const auto &input = stage.operands.front();
    if ((input.kind != MirOperandKind::Copy &&
         input.kind != MirOperandKind::BorrowRead) ||
        input.value || input.loan || input.type != stage.info.type)
      return false;
    const auto *place = body.findPlace(input.place);
    const auto *loan = place ? body.findLoan(place->loan) : nullptr;
    return place && place->root == MirPlaceRootKind::Loan &&
           place->projections.empty() && place->type == stage.info.type &&
           loan && loan->kind == MirLoanKind::CallResult && !loan->entry &&
           !loan->escapes && loan->producedBy == stage.hirValue &&
           loan->access == place->access &&
           unsafeCallResultProducer(body, block, stage, *loan, stage.info.type,
                                    ValueCategory::Place, 0, true);
  }

  // An argument stages a reference to the borrowed referent; a receiver stages
  // the referent object itself, so its staged type is the class rather than a
  // reference to it. Both transport the exact runtime address the verified
  // unsafe producer returned, and the staging itself is a pass-through.
  bool unsafeCallResultInput(const MirBody &body, const MirBlock &block,
                             const MirInstruction &stage) const {
    const bool mutableInput =
        stage.callInputKind == HirCallInputKind::MutableBorrow;
    const bool receiverStage =
        stage.callInputRole == MirCallInputRole::Receiver;
    const auto requiredAccess =
        mutableInput ? AccessMode::Mutable : AccessMode::ReadOnly;
    const bool stagedType =
        receiverStage
            ? stage.info.type.kind == SemanticType::Class
            : stage.info.type.kind == SemanticType::Reference &&
                  stage.info.type.arguments.size() == 1 &&
                  stage.info.type.referenceAccess == requiredAccess;
    if (stage.kind != MirInstructionKind::CallInput || !stage.result ||
        (stage.callInputRole != MirCallInputRole::Argument &&
         !receiverStage) ||
        (stage.callInputKind != HirCallInputKind::ReadBorrow &&
         !mutableInput) ||
        stage.callSite == 0 || stage.hirValue == 0 || stage.defaultArgument ||
        stage.rawMemoryAccess ||
        stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall ||
        stage.intrinsic != IntrinsicKind::None ||
        stage.operation != MirOperation::None || !stagedType ||
        stage.info.category != ValueCategory::Value ||
        stage.info.access != AccessMode::ReadOnly ||
        stage.operands.size() != 1 || stage.destination || stage.receiver ||
        stage.loan || stage.functionTarget || stage.constructorTarget ||
        stage.lambdaTarget || stage.bodyTarget || stage.nativeCalleeType ||
        stage.nativeCallbackAdapter || stage.threadTaskAdapter ||
        stage.callableInvocation || !stage.parameterTypes.empty() ||
        stage.preparedParameterDrop || stage.retainedOwnerPlace ||
        stage.successResultDrop || stage.successResultDestination ||
        stage.ownership || !stage.lifecycle.empty() ||
        !stage.definedFailure.empty() || !stage.localFailureSites.empty())
      return false;
    const auto &input = stage.operands.front();
    // An argument names the loan on the operand itself. A receiver borrows the
    // loan-rooted place directly, so the loan identity comes from that place
    // and the borrow kind must agree with the staged input kind.
    const bool borrowedReceiver =
        receiverStage && !input.loan &&
        input.kind == (mutableInput ? MirOperandKind::BorrowWrite
                                    : MirOperandKind::BorrowRead);
    if (!((input.kind == MirOperandKind::Loan && input.loan) ||
          borrowedReceiver) ||
        !input.place || input.value || input.literal ||
        input.type != stage.info.type)
      return false;
    const auto &referent = receiverStage ? stage.info.type
                                         : stage.info.type.arguments.front();
    const auto *place = body.findPlace(input.place);
    const auto selected = borrowedReceiver ? (place ? place->loan : 0)
                                           : input.loan;
    const auto *loan = selected ? body.findLoan(selected) : nullptr;
    // The explicit loan place carries the returned runtime address. Its
    // possible owner origin is never a substitute, including during call-input
    // staging.
    return place && place->root == MirPlaceRootKind::Loan &&
           place->projections.empty() && place->loan == selected && loan &&
           place->type == referent && loan->kind == MirLoanKind::CallResult &&
           !loan->entry && !loan->escapes &&
           loan->producedBy == stage.hirValue &&
           loan->access == place->access && loan->access == requiredAccess &&
           unsafeCallResultProducer(body, block, stage, *loan, referent,
                                    ValueCategory::Place, 0, true);
  }

  // The caller's permission is repeated on the stage that transports an
  // unsafe call's by-value result into its consumer. A call input carrying
  // the exact result of an admitted ordinary unsafe call performs no unsafe
  // operation of its own; the transport is admitted on its own terms.
  bool unsafeCallResultStaging(const MirBody &body,
                               const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::CallInput ||
        stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall ||
        stage.rawMemoryAccess || stage.intrinsic != IntrinsicKind::None ||
        stage.operands.size() != 1 ||
        stage.operands.front().kind != MirOperandKind::Value ||
        !stage.operands.front().value)
      return false;
    const auto *value = body.findValue(stage.operands.front().value);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    const MirInstruction *producer = nullptr;
    if (block)
      for (const auto &candidate : block->instructions)
        if (!candidate.tombstone && candidate.id == value->definition)
          producer = &candidate;
    const auto *target =
        producer && producer->functionTarget
            ? program.findFunctionInstance(*producer->functionTarget)
            : nullptr;
    return producer && target && producer->kind == MirInstructionKind::Call &&
           producer->result == stage.operands.front().value &&
           producer->hirValue == stage.hirValue &&
           producer->unsafeOperation ==
               UnsafeOperationKind::DeclaredUnsafeCall &&
           producer->intrinsic == IntrinsicKind::None &&
           !producer->rawMemoryAccess && !producer->nativeCalleeType &&
           !producer->lambdaTarget &&
           producer->dispatch == CallDispatch::Static &&
           producer->info.category == ValueCategory::Value &&
           target->definitionKind == MirDefinitionKind::Source &&
           target->linkage == LanguageLinkage::Gti && target->requiresUnsafe;
  }

  // A by-value result of an unsafe call that receives a further call is
  // staged as a temporary borrow of the place materializing that result. The
  // staging shares the unsafe call's HIR value, and so its unsafe operation,
  // but performs none: it transports the address of the exact object the
  // admitted unsafe call returned.
  bool unsafeCallValueReceiver(const MirBody &body,
                               const MirInstruction &stage) const {
    const bool mutableInput =
        stage.callInputKind == HirCallInputKind::MutableTemporaryBorrow;
    if (stage.kind != MirInstructionKind::CallInput || !stage.result ||
        stage.callInputRole != MirCallInputRole::Receiver ||
        (stage.callInputKind != HirCallInputKind::ReadTemporaryBorrow &&
         !mutableInput) ||
        stage.hirValue == 0 || stage.rawMemoryAccess ||
        stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall ||
        stage.intrinsic != IntrinsicKind::None ||
        stage.operands.size() != 1 || stage.destination || stage.receiver ||
        stage.loan || stage.functionTarget || stage.constructorTarget ||
        stage.lambdaTarget || stage.bodyTarget || stage.nativeCalleeType ||
        stage.callableInvocation)
      return false;
    const auto &input = stage.operands.front();
    const auto *place = body.findPlace(input.place);
    if (input.kind != (mutableInput ? MirOperandKind::BorrowWrite
                                    : MirOperandKind::BorrowRead) ||
        input.loan || input.value || !place ||
        place->root != MirPlaceRootKind::Value ||
        !place->projections.empty() || place->value == 0 ||
        place->type != stage.info.type)
      return false;
    const auto *value = body.findValue(place->value);
    const auto *block = value ? body.findBlock(value->definitionBlock) : nullptr;
    if (!block)
      return false;
    const auto producer = std::find_if(
        block->instructions.begin(), block->instructions.end(),
        [&](const MirInstruction &candidate) {
          return candidate.id == value->definition;
        });
    if (producer == block->instructions.end())
      return false;
    const auto *target =
        producer->functionTarget
            ? program.findFunctionInstance(*producer->functionTarget)
            : nullptr;
    return !producer->tombstone &&
           producer->kind == MirInstructionKind::Call &&
           producer->result == place->value &&
           producer->hirValue == stage.hirValue &&
           producer->unsafeOperation ==
               UnsafeOperationKind::DeclaredUnsafeCall &&
           producer->intrinsic == IntrinsicKind::None &&
           !producer->rawMemoryAccess && !producer->nativeCalleeType &&
           !producer->lambdaTarget &&
           producer->dispatch == CallDispatch::Static && target &&
           target->definitionKind == MirDefinitionKind::Source &&
           target->linkage == LanguageLinkage::Gti && target->requiresUnsafe &&
           target->returnType == place->type;
  }

  const MirLoan *directReturnSourceLoan(const MirBody &body,
                                        const MirBlock &block,
                                        const MirInstruction &stage) const {
    const bool text = body.returnType == SemanticType::StringView ||
                      body.returnType == SemanticType::CString;
    const bool reference = body.returnType.kind == SemanticType::Reference &&
                           body.returnType.arguments.size() == 1;
    if (!text && !reference)
      return nullptr;
    const auto &referent =
        text ? body.returnType : body.returnType.arguments.front();
    const auto category = text ? ValueCategory::Value : ValueCategory::Place;
    if (stage.kind != MirInstructionKind::Borrow || !stage.loan ||
        stage.result || stage.hirValue == 0 || stage.rawMemoryAccess ||
        stage.intrinsic != IntrinsicKind::None ||
        stage.operation != MirOperation::None ||
        stage.info.category != category || stage.info.type != referent ||
        stage.operands.size() != 1 || stage.destination || stage.receiver ||
        stage.functionTarget || stage.constructorTarget || stage.lambdaTarget ||
        stage.bodyTarget || stage.nativeCalleeType ||
        stage.nativeCallbackAdapter || stage.threadTaskAdapter ||
        stage.callableInvocation || !stage.parameterTypes.empty() ||
        !stage.lifecycle.empty() || !stage.definedFailure.empty() ||
        !stage.localFailureSites.empty())
      return nullptr;
    const auto &input = stage.operands.front();
    if (input.kind != MirOperandKind::Loan || input.place || input.value ||
        input.literal || input.type != stage.info.type)
      return nullptr;
    const auto *from = body.findLoan(input.loan);
    const auto *to = body.findLoan(*stage.loan);
    if (!from || !to ||
        from->kind != (text ? MirLoanKind::Stored : MirLoanKind::CallResult) ||
        from->entry || from->escapes || from->producedBy != stage.hirValue ||
        from->referenceSelection || to->referenceSelection ||
        to->kind != MirLoanKind::Return || to->entry || !to->escapes ||
        to->parent || to->producedBy != from->producedBy ||
        to->source != from->source || to->access != from->access ||
        stage.info.access != from->access ||
        to->storedDependency != from->storedDependency ||
        to->designatedFields != from->designatedFields ||
        to->conditions != from->conditions ||
        to->additionalDependencies != from->additionalDependencies)
      return nullptr;
    const auto &term = block.terminator;
    if (term.kind != MirTerminatorKind::Return || term.returnLoan != to->id ||
        !term.value || term.value->type != stage.info.type ||
        term.value->place || term.value->literal)
      return nullptr;
    if (!text)
      return body.returnType.referenceAccess == from->access &&
                     term.value->kind == MirOperandKind::Loan &&
                     term.value->loan == to->id && !term.value->value
                 ? from
                 : nullptr;
    // Fundamental text is a borrowed value, not an owning object or a reference
    // address. Preserve its complete dependency and exact returned SSA value.
    if (from->parent || from->lifecycleBound || from->twoPhase ||
        !from->carriers.empty() || to->lifecycleBound || to->twoPhase ||
        !to->carriers.empty() || from->access != AccessMode::ReadOnly ||
        stage.ownership || stage.preparedParameterDrop ||
        stage.successResultDrop || stage.successResultDestination)
      return nullptr;
    if (term.value->kind != MirOperandKind::Value || !term.value->value ||
        term.value->loan)
      return nullptr;
    const auto *producer = earlierLiveCallResult(
        body, block, stage, *from, referent, category, term.value->value);
    return producer && term.value->kind == MirOperandKind::Value &&
                   term.value->value == *producer->result && !term.value->loan
               ? from
               : nullptr;
  }

  bool unsafeCallResultReturn(const MirBody &body, const MirBlock &block,
                              const MirInstruction &stage) const {
    if (stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall)
      return false;
    const auto *from = directReturnSourceLoan(body, block, stage);
    return from &&
           unsafeCallResultProducer(
               body, block, stage, *from, stage.info.type, stage.info.category,
               stage.info.category == ValueCategory::Value &&
                       block.terminator.value
                   ? block.terminator.value->value
                   : 0,
               stage.info.category == ValueCategory::Place);
  }

  bool rawTextResultReturn(const MirBody &body, const MirBlock &block,
                           const MirInstruction &stage) const {
    if (stage.unsafeOperation != UnsafeOperationKind::RawDereference ||
        (stage.info.type != SemanticType::StringView &&
         stage.info.type != SemanticType::CString))
      return false;
    const auto *from = directReturnSourceLoan(body, block, stage);
    const auto *producer =
        from && block.terminator.value
            ? earlierLiveCallResult(body, block, stage, *from, stage.info.type,
                                    ValueCategory::Value,
                                    block.terminator.value->value)
            : nullptr;
    return producer && representedTextAddress(body, *producer) &&
           producer->unsafeOperation == UnsafeOperationKind::RawDereference &&
           !producer->rawMemoryAccess && !producer->functionTarget &&
           !producer->constructorTarget && !producer->lambdaTarget &&
           !producer->bodyTarget && !producer->nativeCalleeType &&
           !producer->nativeCallbackAdapter && !producer->threadTaskAdapter &&
           !producer->receiver && !producer->callableInvocation &&
           producer->dispatch == CallDispatch::Static &&
           producer->operation == MirOperation::None &&
           producer->definedFailure.empty() &&
           producer->localFailureSites.empty();
  }

  bool representedRawReference(const MirBody &body, const MirBlock &block,
                               const MirInstruction &stage) const {
    if (stage.rawMemoryAccess || stage.hirValue == 0)
      return false;
    if (stage.kind == MirInstructionKind::Call)
      return mir_detail::rawBorrowResultLoan(body, stage) &&
             representedRawReferenceTypes(stage);
    if (stage.unsafeOperation != UnsafeOperationKind::RawDereference)
      return false;
    const auto *from = directReturnSourceLoan(body, block, stage);
    const auto *producer =
        from ? earlierLiveCallResult(body, block, stage, *from, stage.info.type)
             : nullptr;
    return producer &&
           mir_detail::rawBorrowResultLoan(body, *producer) == from &&
           representedRawReferenceTypes(*producer);
  }

  std::optional<Shape>
  unstagedPublicationShape(const MirBody &mir,
                           const MirInstruction &stage) const {
    if (!body || body->kind != MirBodyKind::Constructor)
      return {};
    const auto plan = native_llvm::unstagedConstructorFieldPublication(
        program, mir, body->owner, stage);
    if (!plan.matched())
      return {};
    if (!plan.exact || plan.sourceType != stage.info.type)
      return std::nullopt;
    if (plan.transport ==
        native_llvm::UnstagedConstructorFieldPublicationTransport::ExactValue) {
      if (plan.targetType != plan.sourceType)
        return std::nullopt;
      if (isPassiveScalarValueType(plan.targetType))
        return Shape::SuccessResultPublication;
      // A counted byte view is a passive aggregate body value with an
      // implemented layout, load and store. Publishing one into a field
      // carries no ownership and borrows no owning text storage.
      if (plan.targetType == SemanticType::StringView)
        return Shape::CountedText;
      // An exact native function value is one pointer with the already
      // verified canonical signature contract; publishing it is that store.
      if (plan.targetType.kind == SemanticType::NativeFunction &&
          nativeValue(plan.targetType))
        return Shape::NativeCFunctionValue;
      // A C string or primitive address is an inert address value.
      if (plan.targetType.kind == SemanticType::CString ||
          primitiveAddress(plan.targetType))
        return Shape::PassiveAddressValue;
      // An admitted array value is published by storing the whole array.
      if (plan.targetType.kind == SemanticType::Array &&
          passiveArrayElement(plan.targetType))
        return Shape::PassiveArrayValue;
      if (integralEnumArray(plan.targetType))
        return Shape::IntegralEnumArrayValue;
      if (textViewArray(plan.targetType))
        return Shape::TextViewArrayValue;
      const auto storage = classShape(plan.targetType);
      return storage && *storage != Shape::CallableMetadata ? storage
                                                            : std::nullopt;
    }
    if (plan.transport !=
            native_llvm::UnstagedConstructorFieldPublicationTransport::
                NullPointer ||
        plan.sourceType != SemanticType::NullPtr)
      return std::nullopt;
    // A null address is inert whatever it would designate.
    if (plan.targetType.kind == SemanticType::CString ||
        plan.targetType.kind == SemanticType::RawPointer)
      return Shape::PassiveAddressValue;
    if (plan.targetType.kind != SemanticType::UniqueOwner ||
        plan.targetType.arguments.size() != 1 ||
        !native_llvm::uniqueOwnerPointee(program,
                                         plan.targetType.arguments.front()))
      return std::nullopt;
    const auto storage = classShape(plan.targetType);
    if (storage && (*storage == Shape::NativeUniqueOwnership ||
                    *storage == Shape::NativeOwnedMutexStorage))
      return storage;
    return std::nullopt;
  }

  template <class T> void add(T value, std::optional<Shape> implemented = {}) {
    const LlvmCoverageEntry &entry = coverage(value);
    const auto *shape = implemented ? &coverage(*implemented) : nullptr;
    const auto key =
        std::tuple(entry.family, entry.name, body.has_value(),
                   body ? body->kind : MirBodyKind::Module,
                   body ? body->owner : 0, required, executable, shape);
    auto [found, inserted] = indices.try_emplace(key, uses.size());
    if (inserted) {
      uses.push_back({&entry, body, source, block, instruction, 0, required,
                      executable, shape});
    }
    ++uses[found->second].occurrences;
  }

  bool concretePackParameter(const MirPlace &place) const {
    if (!body || !currentBody || place.root != MirPlaceRootKind::Binding ||
        !place.projections.empty() ||
        place.type.kind != SemanticType::TypePack || !place.type.concretePack)
      return false;
    const std::vector<HirBindingId> *bindings = nullptr;
    const std::vector<SemanticType> *parameters = nullptr;
    if (body->kind == MirBodyKind::Function) {
      const auto *owner = program.findFunctionInstance(body->owner);
      if (owner && &owner->body == currentBody) {
        bindings = &owner->parameterBindings;
        parameters = &owner->parameterTypes;
      }
    } else if (body->kind == MirBodyKind::Constructor) {
      const auto *owner = program.findConstructorInstance(body->owner);
      if (owner && &owner->body == currentBody) {
        bindings = &owner->parameterBindings;
        parameters = &owner->parameterTypes;
      }
    } else if (body->kind == MirBodyKind::Lambda) {
      const auto *owner = program.findLambda(body->owner);
      if (owner && &owner->body == currentBody) {
        bindings = &owner->parameterBindings;
        parameters = &owner->parameterTypes;
      }
    }
    if (!bindings || !parameters)
      return false;
    const auto plan = native_llvm::planConcreteParameters(
        *parameters, *bindings, *currentBody);
    return plan && std::any_of(plan->packs.begin(), plan->packs.end(),
                               [&](const auto &pack) {
                                 return pack.binding == place.binding &&
                                        pack.type == place.type;
                               });
  }

  void type(const SemanticType &root, bool concretePackMetadata = false) {
    if (concretePackMetadata) {
      if (root.kind != SemanticType::TypePack || !root.concretePack)
        throw std::logic_error(
            "LLVM concrete-pack metadata lost its verified type");
      const bool previous = executable;
      executable = false;
      add(root.kind);
      executable = previous;
      if (!root.arguments.empty())
        add(Shape::TypeArguments);
      for (const auto &argument : root.arguments)
        type(argument);
      return;
    }
    std::vector<const SemanticType *> pending{&root};
    while (!pending.empty()) {
      const SemanticType &type = *pending.back();
      pending.pop_back();
      if (program.findPayloadEnum(type))
        add(Shape::PayloadEnum, representationShape(type));
      if (type.kind == SemanticType::Class)
        for (const auto &owner : program.classInstances())
          if (owner.type == type && owner.unionLayout)
            add(Shape::NativeUnion, classShape(type));
      const bool passiveArray =
          type.kind == SemanticType::Array && passiveArrayElement(type);
      const bool enumArray = !passiveArray && integralEnumArray(type);
      const bool textArray = textViewArray(type);
      const bool arrayAddress =
          type.kind == SemanticType::RawPointer && type.arguments.size() == 1 &&
          (passiveArrayElement(type.arguments.front()) ||
           type.arguments.front().kind == SemanticType::StringView ||
           textViewArray(type.arguments.front()));
      const bool reference =
          type.kind == SemanticType::Reference && passiveReference(type);
      const bool address = primitiveAddress(type) ||
                           type.kind == SemanticType::NullPtr ||
                           type.kind == SemanticType::CString;
      add(type.kind,
          opaqueReferent(type) ? std::optional(Shape::ErasedReference)
          : address            ? std::optional(Shape::PassiveAddressValue)
          : type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
                    native_llvm::nativeCFunctionValueContract(
                        program, nativeTarget, type.arguments.front())
              ? std::optional(Shape::NativeCFunctionBorrow)
          : native_llvm::nativeCScalarFunctionValueContract(program,
                                                            nativeTarget, type)
              ? std::optional(Shape::NativeCFunctionValue)
          : native_llvm::nativeCFunctionValueContract(program, nativeTarget, type)
              ? std::optional(Shape::NativeCSignature)
          : passiveArray ? std::optional(Shape::PassiveArrayValue)
          : enumArray    ? std::optional(Shape::IntegralEnumArrayValue)
          : textArray    ? std::optional(Shape::TextViewArrayValue)
          : reference    ? std::optional(Shape::PassiveReference)
          : arrayAddress ? std::optional(Shape::CheckedArrayProjection)
                         : representationShape(type));
      if (type.nativeCScalar != NativeCScalarKind::None) {
        const auto nativeScalar =
            native_llvm::nativeCScalarValueContract(nativeTarget, type)
                ? std::optional(Shape::NativeCScalarValue)
                : std::nullopt;
        add(type.nativeCScalar, nativeScalar);
        add(Shape::NativeScalar, nativeScalar);
      }
      if (type.kind == SemanticType::Array)
        add(Shape::FixedArray,
            passiveArray ? std::optional(Shape::PassiveArrayValue)
            : enumArray  ? std::optional(Shape::IntegralEnumArrayValue)
            : textArray  ? std::optional(Shape::TextViewArrayValue)
                         : representationShape(type));
      if (!type.arguments.empty())
        add(Shape::TypeArguments);
      for (const SemanticType &argument : type.arguments) {
        if (native_llvm::nativeCOpaquePointerReferentContract(
                program, nativeTarget, type)) {
          add(argument.kind, Shape::NativeCOpaquePointerReferent);
          add(Shape::OpaqueNativeHandle, Shape::NativeCOpaquePointerReferent);
        } else {
          pending.push_back(&argument);
        }
      }
      const auto enclosing = [&](const SemanticType &argument) {
        if (argument.kind == SemanticType::TypePack && argument.concretePack)
          this->type(argument, true);
        else
          pending.push_back(&argument);
      };
      for (const SemanticType &argument : type.lambdaEnclosingClassTypes)
        enclosing(argument);
      for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes)
        enclosing(argument);
    }
  }

  void traits(const SemanticTypeTraits &traits, const SemanticType &value) {
    const auto shape = representationShape(value);
    if (traits.containsBorrowedState)
      add(Shape::BorrowedCarrier, shape);
    add(traits.ownership, shape);
    add(traits.drop, shape);
    if (traits.drop != DropKind::Trivial) {
      add(Shape::CleanupValue, shape);
    }
    if ((value.kind == SemanticType::Class && !opaqueReferent(value)) ||
        value.kind == SemanticType::Lambda) {
      add(traits.copyable  ? Shape::CopyableValue
          : traits.movable ? Shape::MoveOnlyValue
                           : Shape::ImmovableValue,
          shape);
    }
  }

  void operand(const MirOperand &operand, bool nativeFunctionBorrow = false) {
    const auto *place =
        currentBody ? currentBody->findPlace(operand.place) : nullptr;
    const bool selectedReference =
        passiveReference(operand.type) &&
        (operand.kind == MirOperandKind::Address ||
         operand.kind == MirOperandKind::BorrowRead ||
         operand.kind == MirOperandKind::BorrowWrite ||
         operand.kind == MirOperandKind::Loan);
    // A carrier's loan of exact mutex storage keeps the carrier's own
    // representation; it is not a dispatch view of the payload.
    const bool shellLoan = currentBody &&
                           operand.kind == MirOperandKind::Loan &&
                           !dispatchProofs.related(operand.type) &&
                           mutexShellLoan(*currentBody, operand.loan);
    const bool borrowedUse =
        currentBody && !shellLoan &&
        dispatchProofs.borrowedUse(*currentBody, operand);
    const auto view = currentBody
                          ? dispatchProofs.borrowed(*currentBody, operand)
                          : std::nullopt;
    add(operand.kind, nativeFunctionBorrow
                          ? std::optional(Shape::NativeCFunctionBorrow)
                      : selectedReference ? std::optional(Shape::PassiveReference)
                      : borrowedUse     ? view
                      : operand.kind == MirOperandKind::Address && place &&
                              scalarArrayProjection(*currentBody, *place)
                          ? std::optional(Shape::CheckedArrayProjection)
                          : representationShape(operand.type));
    type(operand.type);
  }

  void cleanup(const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &drop = *pending.back();
      pending.pop_back();
      type(drop.type);
      const auto shape = dispatchProofs.contains(drop.type)
                             ? dispatchProofs.cleanup(drop)
                             : classShape(drop.type);
      if (drop.fixedArray)
        add(Shape::ArrayDrop, shape);
      if (drop.destructor)
        add(Shape::UserDestructor, shape);
      if (drop.polymorphicCleanup)
        add(Shape::PolymorphicClass, dispatchProofs.dynamicOwner(drop));
      if (drop.payloadVariant)
        add(Shape::PayloadEnum, shape);
      if (!drop.aggregate.empty())
        add(Shape::FieldDrop, shape);
      if (drop.requiresActiveCleanup)
        add(Shape::CleanupValue, shape);
      for (const auto &child : drop.aggregate)
        pending.push_back(&child);
    }
  }

  void lifecycle(const MirLifecycleEvent &event) {
    add(event.kind);
    if (event.conditional) {
      const auto *drop = currentBody
                             ? currentBody->findDropObligation(
                                   event.source ? event.source : event.target)
                             : nullptr;
      add(Shape::ConditionalDrop,
          drop ? (dispatchProofs.contains(drop->dropType.type)
                      ? dispatchProofs.cleanup(drop->dropType)
                      : classShape(drop->dropType.type))
               : std::nullopt);
    }
  }

  void inspect(const LoweredBody &row, const MirBody &mir, bool needed) {
    currentBody = &mir;
    body = row.identity.address;
    source = row.identity.source;
    required = needed && row.role == LoweredBodyRole::SourceExecutable;
    executable = false;
    block = 0;
    instruction = 0;
    add(row.role);
    add(row.identity.definition);
    add(mir.kind, bodyClassShape());
    if (const auto *proof = dispatchProofs.body(*body)) {
      if (proof->transfer)
        add(Shape::PolymorphicTransfer);
      if (proof->pure)
        add(Shape::PureDispatchState);
      if (proof->observation)
        add(Shape::DestructorDispatchObservation);
      if (proof->lifecycle)
        add(Shape::DestructorDispatchObservation,
            proof->shape ? std::optional(Shape::NativeLifecycleDispatch)
                         : std::nullopt);
    }
    if (mir.kind == MirBodyKind::Constructor) {
      const auto *constructor = program.findConstructorInstance(body->owner);
      for (const auto &initializer : constructor->initializers)
        if (initializer.storesReference &&
            std::none_of(
                mir.loans.begin(), mir.loans.end(), [&](const auto &loan) {
                  return loan.kind == MirLoanKind::Stored &&
                         loan.storedField == initializer.field &&
                         (loan.entry ||
                          std::any_of(
                              mir.blocks.begin(), mir.blocks.end(),
                              [&](const auto &block) {
                                return std::any_of(
                                    block.instructions.begin(),
                                    block.instructions.end(),
                                    [&](const auto &instruction) {
                                      return !instruction.tombstone &&
                                             instruction.kind ==
                                                 MirInstructionKind::Borrow &&
                                             instruction.loan == loan.id &&
                                             instruction.operands.size() == 1;
                                    });
                              }));
                }))
          add(Shape::StoredFieldPublication);
      // A base initializer that MIR leaves unstaged has no InitializeBase in
      // this body. Its arguments are evaluated and their obligations handed
      // on, but only a backend constructing the base from the record would
      // run the base constructor, and LLVM never does: lowering the body
      // alone would leave the base unconstructed. A stateless interface base
      // has nothing to run.
      const auto *owner = program.findClassInstance(constructor->owner);
      for (std::size_t index = 0; index < constructor->initializers.size();
           ++index) {
        const auto &initializer = constructor->initializers[index];
        if (initializer.kind != ConstructorInitializerTargetKind::Base ||
            (owner && initializer.base &&
             std::any_of(owner->bases.begin(), owner->bases.end(),
                         [&](const auto &base) {
                           return base.interface &&
                                  base.instance == *initializer.base;
                         })))
          continue;
        const bool staged = std::any_of(
            mir.blocks.begin(), mir.blocks.end(), [&](const auto &block) {
              return std::any_of(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const auto &stage) {
                    return !stage.tombstone &&
                           stage.kind == MirInstructionKind::InitializeBase &&
                           stage.constructorInitializer == index + 1;
                  });
            });
        if (!staged)
          add(Shape::UnstagedBaseInitializer);
      }
    }
    if (row.role != LoweredBodyRole::SourceExecutable)
      return;
    executable = true;
    if (body && body->kind == MirBodyKind::Constructor) {
      const auto *constructor = program.findConstructorInstance(body->owner);
      const auto *owner =
          constructor ? program.findClassInstance(constructor->owner) : nullptr;
      if (owner && owner->compilerCapability ==
                       CompilerCapabilityTypeKind::AtomicStorage)
        add(Shape::AtomicInitialization,
            native_llvm::nativeAtomicConstructor(program, *constructor)
                ? std::optional(Shape::NativeAtomicStorage)
                : std::nullopt);
      if (owner && owner->compilerCapability ==
                       CompilerCapabilityTypeKind::MutexStorage)
        add(Shape::MutexInitialization,
            native_llvm::nativeMutexConstructor(program, *constructor) &&
                    mutexShape(owner->type)
                ? std::optional(dispatchProofs.contains(owner->type)
                                    ? Shape::NativeHierarchyMutexStorage
                                    : Shape::NativeMutexStorage)
                : std::nullopt);
    }
    type(mir.returnType);
    add(mir.returnType.kind == SemanticType::Reference ? Shape::BorrowReturn
        : mir.returnType.kind == SemanticType::Class ||
                mir.returnType.kind == SemanticType::Lambda
            ? Shape::OwnedReturn
            : Shape::DirectReturn,
        mir.returnType.kind == SemanticType::Reference &&
                passiveReference(mir.returnType)
            ? std::optional(Shape::PassiveReference)
        : dispatchProofs.referent(mir.returnType)
            ? dispatchProofs.returns(mir)
            : representationShape(mir.returnType));
    if (mir.hasCallerLocation)
      add(Shape::CallerLocation);
    for (const auto &value : mir.values) {
      type(value.info.type);
      traits(value.info.traits, value.info.type);
    }
    for (const auto &place : mir.places) {
      const bool rawClassPlace = nativeRawPlace(mir, place);
      add(place.root,
          rawClassPlace ? std::optional(Shape::NativeClassRawAccess)
          : scalarRawAccess(mir, place)
              ? std::optional(Shape::PrimitiveRawAccess)
          : place.root == MirPlaceRootKind::Loan && passiveReference(place.type)
              ? std::optional(Shape::PassiveReference)
              : placeClassShape(place));
      add(place.access);
      type(place.type, concretePackParameter(place));
      traits(place.traits, place.type);
      for (const auto &projection : place.projections)
        add(projection.kind,
            rawClassPlace ? std::optional(Shape::NativeClassRawAccess)
            : scalarRawAccess(mir, place)
                ? std::optional(Shape::PrimitiveRawAccess)
            : scalarArrayProjection(mir, place)
                ? std::optional(Shape::CheckedArrayProjection)
            : projection.kind == MirProjectionKind::Dereference &&
                    native_llvm::nativeCFunctionValueContract(
                        program, nativeTarget, place.type)
                ? std::optional(Shape::NativeCFunctionBorrow)
            : projection.kind == MirProjectionKind::Dereference &&
                    passiveReference(place.type)
                ? std::optional(Shape::PassiveReference)
                : placeClassShape(place));
      if (place.foreignStorage)
        add(Shape::NativeCall,
            native_llvm::nativeCStoragePlaceContract(program, nativeTarget, mir,
                                                    place)
                ? std::optional(Shape::NativeCStorage)
                : std::nullopt);
    }
    for (const auto &loan : mir.loans) {
      const auto *origin = mir.findPlace(loan.source);
      const auto nativeConditions = [&](const auto &conditions) {
        return std::all_of(
            conditions.begin(), conditions.end(), [&](const auto &condition) {
              const auto *payload =
                  program.findPayloadEnum(condition.enumeration);
              return payload && condition.variant < payload->variants.size() &&
                     nativeValue(condition.enumeration);
            });
      };
      bool nativeBorrow = origin && nativeValue(origin->type) &&
                          nativeConditions(loan.conditions);
      for (const auto &dependency : loan.additionalDependencies) {
        const auto *source = mir.findPlace(dependency.source);
        nativeBorrow &= nativeConditions(dependency.conditions) &&
                        (source ? nativeValue(source->type)
                                : !dependency.source &&
                                      !dependency.designatedFields.empty());
      }
      if (loan.referenceSelection) {
        const auto *value = mir.findValue(loan.referenceSelection->value);
        nativeBorrow &= value && nativeValue(value->info.type) &&
                        nativeConditions(loan.referenceSelection->conditions);
      }
      const auto borrowShape = nativeBorrow
                                   ? std::optional(Shape::NativeBorrowMetadata)
                                   : std::nullopt;
      const bool selected = origin && passiveReference(origin->type) &&
                            !loan.referenceSelection && loan.conditions.empty();
      // A raw pointer to hierarchy storage is a passive address: borrowing
      // the pointer's own storage is an ordinary native borrow.
      const bool hierarchy = origin && dispatchProofs.related(origin->type) &&
                             origin->type.kind != SemanticType::RawPointer;
      // An entry record that a referenced carrier stores a borrow in its
      // fields names that external referent; like other borrow metadata it
      // has no representation beside the carrier's own layout.
      const bool storedRecord =
          hierarchy && loan.kind == MirLoanKind::Stored && loan.entry &&
          !loan.storedDependency && !loan.designatedFields.empty() &&
          !loan.referenceSelection && loan.additionalDependencies.empty() &&
          loan.conditions.empty() && !loan.twoPhase &&
          dispatchProofs
              .metadata(origin->type.kind == SemanticType::Reference &&
                                origin->type.arguments.size() == 1
                            ? origin->type.arguments.front()
                            : origin->type)
              .has_value();
      const auto view =
          storedRecord ? std::optional(Shape::NativeBorrowMetadata)
          : hierarchy  ? dispatchProofs.loan(mir, loan)
                       : std::nullopt;
      add(loan.kind, hierarchy     ? (view || !mutexShellLoan(mir, loan.id)
                                          ? view
                                          : std::optional(
                                                Shape::NativeHierarchyMutexStorage))
                     : borrowShape ? borrowShape
                     : selected    ? std::optional(Shape::PassiveReference)
                     : origin      ? placeClassShape(*origin)
                                   : std::nullopt);
      add(loan.access);
      if (loan.referenceSelection)
        add(Shape::SelectedReferenceAddress, borrowShape);
      if (loan.twoPhase)
        add(Shape::TwoPhaseBorrow, hierarchy ? view
                                   : origin  ? placeClassShape(*origin)
                                             : std::nullopt);
      if (!loan.additionalDependencies.empty())
        add(Shape::MultipleBorrowOrigins, borrowShape);
      if (!loan.conditions.empty())
        add(Shape::ConditionalBorrow, borrowShape);
      for (const auto &dependency : loan.additionalDependencies) {
        add(dependency.access);
        if (!dependency.conditions.empty())
          add(Shape::ConditionalBorrow, borrowShape);
      }
    }
    for (const auto &drop : mir.dropObligations) {
      add(drop.kind, dispatchProofs.contains(drop.dropType.type)
                         ? dispatchProofs.cleanup(drop.dropType)
                         : classShape(drop.dropType.type));
      cleanup(drop.dropType);
    }
    for (const auto &boundary : mir.cleanupBoundaries)
      add(boundary.kind);
    for (const auto &record : mir.failureRecords)
      add(record.kind);

    std::map<std::pair<MirBlockId, MirInstructionId>, const SourceSpan *> spans;
    for (const auto &location : row.sourceLocations())
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
        const auto *dispatchOperation =
            dispatchProofs.operation(*body, block, instruction);
        if (dispatchOperation && dispatchOperation->transfer)
          add(Shape::PolymorphicTransfer);
        if (dispatchOperation && dispatchOperation->pure)
          add(Shape::PureDispatchState);
        if (dispatchOperation && dispatchOperation->observation)
          add(Shape::DestructorDispatchObservation);
        if (body && body->kind == MirBodyKind::Constructor &&
            mirRecordPublishedConstructorInitializer(program, mir, body->owner,
                                                     stage))
          add(Shape::UnstagedConstructorFieldPublication,
              unstagedPublicationShape(mir, stage));
        const bool typedMemory = memoryOperation(stage);
        const bool restoring = restoringRelocation(stage);
        const bool nativeFunctionBorrow =
            native_llvm::nativeCFunctionBorrowInputContract(
                program, nativeTarget, mir, stage);
        const bool rawReference = representedRawReference(mir, mirBlock, stage);
        // A payload referent is reached only through the raw-access witness;
        // its storage witness does not itself grant raw reference formation.
        const auto rawReferenceShape =
            !rawReference ? std::nullopt
            : nativePayloadPointee(stage.info.type)
                ? std::optional(Shape::NativePayloadRawAccess)
            : dispatchProofs.contains(stage.info.type)
                ? std::optional(Shape::NativeClassRawAccess)
                : std::optional(classShape(stage.info.type)
                                    .value_or(Shape::PassiveReference));
        const auto [extendedRaw, validRaw] = nativeRawStage(mir, stage);
        const bool rawClassStage = extendedRaw && validRaw;
        const bool classArithmetic = nativePointerArithmetic(stage) && validRaw;
        std::optional<Shape> storageShape =
            typedMemory || restoring
                ? std::optional(Shape::NativeMemoryOperation)
                : std::nullopt;
        const bool mutex = native_llvm::nativeMutexCall(program, mir, stage) ||
                           native_llvm::nativeMutexLockFailure(program, stage);
        if (mutex)
          storageShape = Shape::NativeMutexOperation;
        const bool thread = nativeThreadStage(stage);
        if (thread)
          storageShape = Shape::NativeThreadOperation;
        const auto *nativeCallee =
            stage.functionTarget
                ? program.findFunctionInstance(*stage.functionTarget)
                : nullptr;
        // A manual C-thread service carries the synchronization record its
        // trusted declaration implies; the runtime provides the edge.
        const bool cthread =
            nativeCallee &&
            native_llvm::nativeCThreadOperationBinding(program,
                                                       *nativeCallee) &&
            native_llvm::nativeCDirectCallContract(program, nativeTarget, mir,
                                                   stage) &&
            stage.synchronization.kind ==
                native_llvm::cThreadOperationSynchronization(
                    nativeCallee->cThreadOperation) &&
            !stage.synchronization.order && !stage.synchronization.failureOrder;
        if (stage.kind == MirInstructionKind::RetireProgramStorage &&
            body->kind == MirBodyKind::HostedStartup &&
            program.hostedStartupPlan())
          for (const auto &operation : program.hostedStartupPlan()->operations)
            if (operation.id == stage.hostedStartupOperation &&
                operation.block == block && operation.instruction == stage.id &&
                nativeRetirement(operation))
              storageShape = Shape::NativeProgramRetirement;
        if (stage.kind == MirInstructionKind::EnsureThreadLocal)
          if (const auto *storage =
                  program.findThreadLocalStorage(stage.threadLocalStorage))
            if (nativeValue(storage->type))
              storageShape = Shape::NativeThreadStorage;
        if (stage.kind == MirInstructionKind::Construct)
          storageShape = classShape(stage.info.type);
        if (stage.kind == MirInstructionKind::Drop ||
            stage.kind == MirInstructionKind::CleanupBeforeAssign)
          for (const auto &drop : mir.dropObligations)
            for (const auto &event : stage.lifecycle)
              if (event.source == drop.id)
                storageShape = classShape(drop.dropType.type);
        if (stage.kind == MirInstructionKind::CleanupBeforeAssign &&
            stage.replacementCleanup)
          storageShape = classShape(stage.replacementCleanup->type);
        if (dispatchOperation && dispatchOperation->shape)
          storageShape = dispatchOperation->shape;
        // Forwarding a carrier's loan of exact mutex storage is an ordinary
        // borrow of that carrier, not a dispatch view.
        const bool shellForward =
            stage.kind == MirInstructionKind::Borrow &&
            !dispatchProofs.related(stage.info.type) &&
            stage.operands.size() == 1 &&
            stage.operands.front().kind == MirOperandKind::Loan &&
            mutexShellLoan(mir, stage.operands.front().loan);
        add(stage.kind,
            stage.kind == MirInstructionKind::Borrow
                ? (passiveReference(stage.info.type)
                       ? std::optional(Shape::PassiveReference)
                   : !shellForward &&
                           (dispatchProofs.related(stage.info.type) ||
                            (!stage.operands.empty() &&
                             dispatchProofs.borrowedUse(
                                 mir, stage.operands.front())))
                       ? (dispatchOperation ? dispatchOperation->shape
                                            : std::nullopt)
                       : classShape(stage.info.type))
                : storageShape);
        if (stage.callInputRole) {
          if (stage.info.type == SemanticType::CString &&
              stage.operands.size() == 1 &&
              stage.operands[0].type == SemanticType::StringView)
            add(Shape::CStringLiteralInput,
                native_llvm::wholeCStringLiteral(mir, stage.operands[0])
                    ? std::optional(Shape::PassiveAddressValue)
                    : std::nullopt);
          const auto inputShape =
              dispatchProofs.related(stage.info.type) ||
                      (!stage.operands.empty() &&
                       dispatchProofs.borrowedUse(mir, stage.operands.front()))
                  ? (dispatchOperation ? dispatchOperation->shape
                                       : std::nullopt)
                  : native_llvm::nativeCScalarReceiverInputContract(
                        program, nativeTarget, mir, stage)
                      ? std::optional(Shape::NativeCScalarIndirect)
                  : native_llvm::nativeCReceiverInputContract(
                        program, nativeTarget, mir, stage)
                      ? std::optional(Shape::NativeCSignature)
                  : nativeFunctionBorrow
                      ? std::optional(Shape::NativeCFunctionBorrow)
                      : classShape(stage.info.type);
          add(*stage.callInputRole, inputShape);
          add(stage.callInputKind,
              passiveReference(stage.info.type) &&
                      (stage.callInputKind == HirCallInputKind::ReadBorrow ||
                       stage.callInputKind == HirCallInputKind::MutableBorrow)
                  ? std::optional(Shape::PassiveReference)
                  : inputShape);
        }
        if (stage.operation != MirOperation::None) {
          const auto *mutationPlace =
              stage.destination ? mir.findPlace(*stage.destination) : nullptr;
          const bool scalarMutation =
              (stage.kind == MirInstructionKind::Compute ||
               stage.kind == MirInstructionKind::Assign) &&
              mutationPlace && isPassiveScalarValueType(mutationPlace->type) &&
              stage.operands.size() == 1 &&
              isPassiveScalarValueType(stage.operands.front().type) &&
              stage.operation >= MirOperation::AddAssign &&
              stage.operation <= MirOperation::ShiftRightAssign;
          if ((stage.operation == MirOperation::Equal ||
               stage.operation == MirOperation::NotEqual) &&
              std::any_of(stage.operands.begin(), stage.operands.end(),
                          [](const auto &operand) {
                            return operand.type.kind == SemanticType::Array;
                          }))
            add(Shape::ArrayValueComparison);
          const bool arrayAggregate =
              stage.operation == MirOperation::Aggregate &&
              stage.info.type.kind == SemanticType::Array &&
              passiveArrayElement(stage.info.type);
          const auto *addressPlace =
              stage.operands.size() == 1
                  ? mir.findPlace(stage.operands[0].place)
                  : nullptr;
          const bool arrayAddress =
              stage.operation == MirOperation::AddressOf && addressPlace &&
              scalarArrayProjection(mir, *addressPlace);
          const bool referenceAddress =
              stage.operation == MirOperation::AddressOf && addressPlace &&
              passiveReference(addressPlace->type);
          const bool textIndex =
              stage.operation == MirOperation::Index &&
              stage.operands.size() == 2 &&
              stage.operands[0].type.kind == SemanticType::StringView &&
              stage.info.type == SemanticType::Char;
          add(stage.operation,
              native_llvm::nativeCCallbackStageContract(program, nativeTarget,
                                                       mir, stage)
                  ? std::optional(Shape::NativeCCallback)
              : stage.operation == MirOperation::PayloadConstruct ||
                      stage.operation == MirOperation::PayloadExtract
                  ? representationShape(mirInstructionEnumType(stage))
              : nativeAddressConversion(stage) && validRaw
                  ? std::optional(Shape::NativeObjectAddress)
              : (nativeRawIndex(stage) && validRaw) || classArithmetic
                  ? std::optional(Shape::NativeClassRawAccess)
              : scalarMutation ? std::optional(Shape::InPlaceScalarMutation)
              : scalarRawIndex(stage) || scalarPointerArithmetic(stage)
                  ? std::optional(Shape::PrimitiveRawAccess)
              : payloadPointerArithmetic(stage)
                  ? std::optional(Shape::NativePayloadRawAccess)
              : arrayAggregate ? std::optional(Shape::PassiveArrayValue)
              : integralEnumArrayAggregate(stage)
                  ? std::optional(Shape::IntegralEnumArrayValue)
              : textViewArrayAggregate(stage)
                  ? std::optional(Shape::TextViewArrayValue)
              : stage.operation == MirOperation::Aggregate &&
                      stage.info.type.kind == SemanticType::Array
                  ? (dispatchProofs.related(stage.info.type)
                         ? (dispatchOperation ? dispatchOperation->shape
                                              : std::nullopt)
                         : classShape(stage.info.type))
              : textIndex        ? std::optional(Shape::CountedText)
              : referenceAddress ? std::optional(Shape::PassiveReference)
              : arrayAddress     ? std::optional(Shape::CheckedArrayProjection)
              : stage.operation == MirOperation::Closure ||
                      stage.operation == MirOperation::FunctionItem
                  ? classShape(stage.info.type)
              : stage.operation == MirOperation::AddressOf && addressPlace
                  ? placeClassShape(*addressPlace)
                  : std::nullopt);
        }
        const bool atomic = native_llvm::nativeAtomicCall(program, mir, stage);
        const bool atomicWaiting =
            native_llvm::nativeAtomicWaitingCall(program, mir, stage);
        if (stage.intrinsic != IntrinsicKind::None) {
          const bool textQuery =
              stage.receiver &&
              stage.receiver->type.kind == SemanticType::StringView &&
              stage.operands.empty() &&
              (stage.intrinsic == IntrinsicKind::StringViewSize ||
               stage.intrinsic == IntrinsicKind::StringViewEmpty);
          std::optional<Shape> callableIntrinsic;
          if (stage.intrinsic == IntrinsicKind::FunctionPointerFromItem)
            callableIntrinsic = classShape(stage.info.type);
          else if (stage.intrinsic == IntrinsicKind::FunctionPointerInvoke &&
                   !stage.parameterTypes.empty()) {
            const auto &receiver = stage.parameterTypes.front();
            if (receiver.kind == SemanticType::Reference &&
                receiver.arguments.size() == 1)
              callableIntrinsic = classShape(receiver.arguments.front());
          }
          add(stage.intrinsic,
              atomic              ? std::optional(Shape::NativeAtomicOperation)
              : atomicWaiting
                  ? std::optional(Shape::NativeAtomicWaitingOperation)
              : mutex ? std::optional(Shape::NativeMutexOperation)
              : thread ? std::optional(Shape::NativeThreadOperation)
              : callableIntrinsic ? callableIntrinsic
              : typedMemory || restoring
                  ? std::optional(Shape::NativeMemoryOperation)
              : nativeAddressConversion(stage) && validRaw
                  ? std::optional(Shape::NativeObjectAddress)
              : erasedReference(stage) ? std::optional(Shape::ErasedReference)
              : representedTextAddress(mir, stage)
                  ? std::optional(Shape::CountedText)
              : rawReference ? rawReferenceShape
              : stage.intrinsic == IntrinsicKind::RawPointerConversion &&
                      stage.operands.size() == 1 &&
                      primitiveAddress(stage.info.type) &&
                      primitiveAddress(stage.operands[0].type)
                  ? std::optional(Shape::PassiveAddressValue)
              // A sealed integral enum already carries its underlying integer
              // representation, so reading that value is the ordinary scalar
              // shape. The sealed inventory supplies the exact underlying
              // type; a payload enum has no underlying value to read.
              : sealedIntegralUnderlyingValue(stage)
                  ? std::optional(Shape::NativeIntegralEnumValue)
              : textQuery ? std::optional(Shape::CountedText)
              : stage.receiver &&
                      stage.receiver->type.kind == SemanticType::Array &&
                      passiveArrayElement(stage.receiver->type) &&
                      stage.operands.empty() &&
                      (stage.intrinsic == IntrinsicKind::ArraySize ||
                       stage.intrinsic == IntrinsicKind::ArrayRangeBegin)
                  ? std::optional(Shape::PassiveArrayValue)
              : arrayExtentQuery(stage) ? std::optional(Shape::ArrayExtentQuery)
              : (stage.intrinsic == IntrinsicKind::UniqueOwnerUpcast ||
                 stage.intrinsic == IntrinsicKind::SharedOwnerUpcast ||
                 stage.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                 native_llvm::PolymorphicStorage::handles(stage.intrinsic)) &&
                      dispatchOperation
                  ? dispatchOperation->shape
                  : std::nullopt);
        }
        if (stage.receiver)
          operand(*stage.receiver);
        for (const auto &input : stage.operands)
          operand(input, nativeFunctionBorrow);
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
          const auto nativeIndirect =
              native_llvm::nativeCScalarIndirectCallContract(
                  program, nativeTarget, mir, stage)
                  ? std::optional(Shape::NativeCScalarIndirect)
              : native_llvm::nativeCIndirectCallContract(program, nativeTarget,
                                                        mir, stage)
                  ? std::optional(Shape::NativeCSignature)
                  : std::nullopt;
          add(Shape::NativeCall, nativeIndirect);
          add(Shape::IndirectCall, nativeIndirect);
        }
        if (stage.dispatch == CallDispatch::Virtual)
          add(Shape::VirtualCall,
              dispatchOperation ? dispatchOperation->shape : std::nullopt);
        if (stage.functionTarget || stage.constructorTarget ||
            stage.lambdaTarget)
          add(stage.dispatch,
              dispatchOperation ? dispatchOperation->shape : std::nullopt);
        if (stage.constructorTarget)
          add(stage.constructorKind,
              dispatchProofs.related(stage.info.type)
                  ? (dispatchOperation ? dispatchOperation->shape
                                       : std::nullopt)
                  : (typedMemory ? storageShape : classShape(stage.info.type)));
        if (stage.lambdaTarget || stage.functionPointerFunctionTarget ||
            stage.functionPointerLambdaTarget)
          add(Shape::IndirectCall);
        if (stage.nativeCallbackAdapter)
          add(Shape::NativeCallback,
              native_llvm::nativeCCallbackStageContract(program, nativeTarget,
                                                       mir, stage)
                  ? std::optional(Shape::NativeCCallback)
                  : std::nullopt);
        const auto taskShape = [&](MirThreadTaskAdapterId id) {
          const auto *adapter = program.findThreadTaskAdapter(id);
          return !thread || !adapter ? std::nullopt
                 : native_llvm::hierarchyThreadTask(program, *adapter)
                     ? std::optional(Shape::NativeHierarchyThreadTask)
                     : std::optional(Shape::NativeThreadTask);
        };
        if (stage.threadTaskAdapter)
          add(Shape::ThreadTask, taskShape(*stage.threadTaskAdapter));
        for (const auto &task : stage.scopedThreadTasks)
          add(Shape::ScopedThreadTasks, taskShape(task.adapter));
        if (stage.preparedParameterDrop) {
          const auto &prepared =
              mir.findDropObligation(*stage.preparedParameterDrop)->dropType;
          add(Shape::PreparedOwnerInput,
              dispatchProofs.contains(prepared.type)
                  ? dispatchProofs.cleanup(prepared)
                  : classShape(prepared.type));
        }
        if (stage.successResultDrop || stage.successResultDestination)
          add(Shape::SuccessResultPublication, classShape(stage.info.type));
        if (stage.tracksCaller || stage.callerLocation ||
            stage.sourceLocation || stage.forwardsCallerLocation)
          add(Shape::CallerLocation);
        bool rawAccess =
            rawReference || scalarRawIndex(stage) ||
            (stage.destination &&
             scalarRawAccess(mir, *mir.findPlace(*stage.destination)));
        for (const auto &operand : stage.operands)
          if (const auto *place = mir.findPlace(operand.place))
            rawAccess |= scalarRawAccess(mir, *place);
        const bool nativeStorage =
            native_llvm::nativeCStorageAccessContract(program, nativeTarget,
                                                      mir, stage) ||
            native_llvm::nativeCStorageUnionAccessContract(program, nativeTarget,
                                                           mir, stage);
        if (stage.rawMemoryAccess)
          add(Shape::RawMemory,
              nativeStorage ? std::optional(Shape::NativeCStorage)
              : extendedRaw
                  ? (validRaw ? std::optional(Shape::NativeClassRawAccess)
                              : std::nullopt)
              : nativeRawReceiverStage(mir, stage)
                  ? std::optional(Shape::NativeClassRawAccess)
              : rawAccess ? std::optional(Shape::PrimitiveRawAccess)
                          : std::nullopt);
        // Lifecycle marks repeat the source unsafe context but execute no
        // memory operation. The typed access/arithmetic stages are inventoried
        // independently; their pending shapes cannot be hidden by this marker.
        const auto &instructionCoverage = coverage(stage.kind);
        // A drop of a place not reached through raw memory destroys an
        // ordinary object; its marker repeats the statement's permission and
        // the destruction is admitted by its own drop recipe.
        const auto *dropped =
            stage.kind == MirInstructionKind::Drop && stage.destination
                ? mir.findPlace(*stage.destination)
                : nullptr;
        const bool ordinaryDrop =
            dropped && !stage.rawMemoryAccess &&
            std::none_of(dropped->projections.begin(),
                         dropped->projections.end(), [](const auto &step) {
                           return step.kind ==
                                      MirProjectionKind::RawDereference ||
                                  step.kind == MirProjectionKind::RawIndex;
                         });
        const bool permissionOnly =
            stage.kind == MirInstructionKind::Lifecycle || ordinaryDrop ||
            (instructionCoverage.state ==
                 LlvmCoverageState::VerifiedNonExecuting &&
             instructionCoverage.nonExecuting ==
                 LlvmNonExecutingRule::VerifiedMetadata);
        if (stage.unsafeOperation != UnsafeOperationKind::None &&
            !permissionOnly) {
          const auto *callee =
              stage.functionTarget
                  ? program.findFunctionInstance(*stage.functionTarget)
                  : nullptr;
          // Caller permission never turns an ordinary source function into an
          // intrinsic. Its exact body is in the required dependency closure
          // and every unsupported operation there still rejects admission.
          const bool ordinaryUnsafe =
              stage.kind == MirInstructionKind::Call &&
              stage.intrinsic == IntrinsicKind::None && callee &&
              callee->definitionKind == MirDefinitionKind::Source &&
              callee->linkage == LanguageLinkage::Gti &&
              callee->requiresUnsafe &&
              stage.unsafeOperation == UnsafeOperationKind::DeclaredUnsafeCall;
          const auto stagedPointerArithmetic =
              stage.unsafeOperation == UnsafeOperationKind::PointerArithmetic
                  ? pointerArithmeticInput(mirBlock, stage)
                  : std::nullopt;
          add(stage.unsafeOperation,
              (stage.unsafeOperation == UnsafeOperationKind::ForeignStorage ||
               stage.unsafeOperation == UnsafeOperationKind::UnionMember) &&
                      nativeStorage
                  ? std::optional(Shape::NativeCStorage)
              : stage.unsafeOperation == UnsafeOperationKind::UnionMember
                  ? classShape(stage.info.type).has_value() ? classShape(stage.info.type)
                    : [&]() -> std::optional<Shape> {
                        if (stage.destination)
                          return placeClassShape(*mir.findPlace(*stage.destination));
                        for (const auto &operand : stage.operands)
                          if (const auto *place = mir.findPlace(operand.place))
                            if (auto shape = placeClassShape(*place)) return shape;
                        return {};
                      }()
              : stage.unsafeOperation ==
                            UnsafeOperationKind::ForeignPointerCall &&
                        native_llvm::nativeCResultInputContract(
                            program, nativeTarget, mir, stage)
                  ? std::optional(Shape::NativeCResultInput)
              : stage.unsafeOperation ==
                          UnsafeOperationKind::ForeignPointerCall &&
                      native_llvm::nativeProcessMemoryCall(program, stage)
                  ? std::optional(Shape::NativeProcessMemory)
              : stage.unsafeOperation ==
                            UnsafeOperationKind::ForeignPointerCall &&
                        native_llvm::nativeCScalarIndirectCallContract(
                            program, nativeTarget, mir, stage)
                  ? std::optional(Shape::NativeCScalarIndirect)
              : stage.unsafeOperation ==
                            UnsafeOperationKind::ForeignPointerCall &&
                        callee &&
                        (native_llvm::nativeCScalarImportContract(
                             program, nativeTarget, *callee) ||
                         native_llvm::nativeCCountedInputContract(
                             program, nativeTarget, *callee) ||
                         native_llvm::nativeCScalarFunctionImportContract(
                             program, nativeTarget, *callee) ||
                         native_llvm::nativeCAggregateImportContract(
                             program, nativeTarget, *callee)) &&
                        native_llvm::nativeCDirectCallContract(
                            program, nativeTarget, mir, stage)
                  ? std::optional(
                        native_llvm::nativeCAggregateImportContract(
                            program, nativeTarget, *callee)
                            ? Shape::NativeCAggregateImport
                        : native_llvm::nativeCScalarFunctionImportContract(
                            program, nativeTarget, *callee)
                            ? Shape::NativeCFunctionImport
                        : native_llvm::nativeCCountedInputContract(
                            program, nativeTarget, *callee)
                            ? Shape::NativeCCountedInput
                            : Shape::NativeCScalarImport)
              : stage.unsafeOperation ==
                            UnsafeOperationKind::ForeignPointerCall &&
                      (native_llvm::nativeCIndirectCallContract(
                           program, nativeTarget, mir, stage) ||
                       native_llvm::nativeCDirectCallContract(
                           program, nativeTarget, mir, stage))
                  ? std::optional(Shape::NativeCSignature)
              : rawReference && stage.unsafeOperation ==
                                    UnsafeOperationKind::RawDereference
                  ? rawReferenceShape
              : stage.unsafeOperation == UnsafeOperationKind::RawMember &&
                      ((rawClassStage && nativeRawFieldStage(mir, stage)) ||
                       nativeRawReceiverStage(mir, stage))
                  ? std::optional(Shape::NativeClassRawAccess)
              : stagedPointerArithmetic ? stagedPointerArithmetic
              : (rawClassStage || classArithmetic) &&
                      (stage.unsafeOperation ==
                           UnsafeOperationKind::RawDereference ||
                       stage.unsafeOperation == UnsafeOperationKind::RawIndex ||
                       stage.unsafeOperation ==
                           UnsafeOperationKind::PointerArithmetic)
                  ? std::optional(Shape::NativeClassRawAccess)
              : payloadPointerArithmetic(stage) &&
                      stage.unsafeOperation ==
                          UnsafeOperationKind::PointerArithmetic
                  ? std::optional(Shape::NativePayloadRawAccess)
              : typedMemory && stage.unsafeOperation ==
                                   UnsafeOperationKind::ObjectLifetime
                  ? std::optional(Shape::NativeMemoryOperation)
              : (ordinaryUnsafe || unsafeCallResultLoad(mir, mirBlock, stage) ||
                 unsafeCallResultInput(mir, mirBlock, stage) ||
                 unsafeCallResultStaging(mir, stage) ||
                 unsafeCallValueReceiver(mir, stage) ||
                 unsafeCallResultReturn(mir, mirBlock, stage))
                  ? std::optional(Shape::OrdinaryUnsafeInvocation)
              : (stage.unsafeOperation ==
                     UnsafeOperationKind::ErasedReferenceRestore ||
                 stage.unsafeOperation ==
                     UnsafeOperationKind::DeclaredUnsafeCall) &&
                      (erasedReference(stage) ||
                       restoredLoanTransport(mir, stage))
                  ? std::optional(Shape::ErasedReference)
              : ((stage.unsafeOperation ==
                      UnsafeOperationKind::RawDereference &&
                  (representedTextAddress(mir, stage) ||
                   rawTextResultReturn(mir, mirBlock, stage))) ||
                 (stage.unsafeOperation ==
                      UnsafeOperationKind::DeclaredUnsafeCall &&
                  rawReference) ||
                 (rawAccess &&
                  (stage.unsafeOperation == UnsafeOperationKind::RawIndex ||
                   stage.unsafeOperation ==
                       UnsafeOperationKind::RawDereference)) ||
                 ((scalarPointerArithmetic(stage) ||
                   (stage.kind == MirInstructionKind::Assign &&
                    primitiveAddress(stage.info.type))) &&
                  stage.unsafeOperation ==
                      UnsafeOperationKind::PointerArithmetic))
                  ? std::optional(Shape::PrimitiveRawAccess)
              // The marker records the caller's declared permission for a
              // construction; the executable work is that construction's own
              // selected constructor and storage, which is admitted on its
              // own terms. Granting the construction's storage shape keeps
              // one rule instead of a second permission vocabulary.
              : stage.unsafeOperation ==
                        UnsafeOperationKind::DeclaredUnsafeConstruction &&
                    stage.kind == MirInstructionKind::Construct &&
                    stage.constructorTarget
                  ? classShape(stage.info.type)
                  : std::nullopt);
        }
        if (stage.payloadExtractionKind)
          add(*stage.payloadExtractionKind,
              representationShape(mirInstructionEnumType(stage)));
        for (const auto &argument : stage.callableArguments)
          add(argument.boundary);
        if (stage.callableBoundary)
          add(*stage.callableBoundary);
        if (stage.callableInvocation) {
          const auto *target =
              stage.functionTarget
                  ? program.findFunctionInstance(*stage.functionTarget)
                  : nullptr;
          add(*stage.callableInvocation,
              target && target->owner && !target->staticMember &&
                      !stage.lambdaTarget
                  ? classShape(program.findClassInstance(*target->owner)->type)
                  : std::nullopt);
        }
        if (stage.synchronization.kind != SynchronizationOperationKind::None) {
          const auto witness = atomic
                                   ? std::optional(Shape::NativeAtomicOperation)
                               : atomicWaiting
                                   ? std::optional(Shape::NativeAtomicWaitingOperation)
                               : mutex
                                   ? std::optional(Shape::NativeMutexOperation)
                               : thread
                                   ? std::optional(Shape::NativeThreadOperation)
                               : cthread
                                   ? std::optional(Shape::NativeCThreadOperation)
                                   : std::nullopt;
          add(stage.synchronization.kind, witness);
          if (stage.synchronization.order)
            add(*stage.synchronization.order, witness);
          if (stage.synchronization.failureOrder)
            add(*stage.synchronization.failureOrder, witness);
        }
        if (!stage.definedFailure.empty()) {
          add(Shape::CheckedFailure);
          add(stage.definedFailure.propagation, storageShape);
          for (const auto &origin : stage.definedFailure.localOrigins)
            for (const auto &outcome : origin.outcomes) {
              const bool arrayBound =
                  outcome.code == DefinedFailureCode::IndexOutOfBounds &&
                  outcome.detail == DefinedFailureDetail::FixedArray;
              const bool textBound =
                  outcome.code == DefinedFailureCode::IndexOutOfBounds &&
                  outcome.detail == DefinedFailureDetail::StringView;
              const auto shape =
                  arrayBound  ? std::optional(Shape::CheckedArrayProjection)
                  : textBound ? std::optional(Shape::CountedText)
                              : storageShape;
              add(outcome.code, shape);
              add(outcome.detail, shape);
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
      if (const auto *proof = dispatchProofs.operation(*body, block, 0);
          proof && proof->transfer)
        add(Shape::PolymorphicTransfer);
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
                         std::size_t, bool, bool, const LlvmCoverageEntry *>;
  std::vector<LlvmCoverageUse> &uses;
  const MirProgram &program;
  StaticDispatchProofs &dispatchProofs;
  const TargetInfo &nativeTarget;
  NativeValues natives;
  std::map<Key, std::size_t> indices;
  // Owner pointees whose mutex-storage representation is being decided.
  mutable std::vector<SemanticType> ownedMutexPointees;
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
        (entry.state == LlvmCoverageState::Pending &&
         (!use.implementedShape ||
          use.implementedShape->state != LlvmCoverageState::Implemented)) ||
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
  const auto closure = native_llvm::reachableProgram(input);
  plan.closure_ = closure.conservative
                      ? LlvmAdmissionClosure::AllExecutableBodies
                      : LlvmAdmissionClosure::SealedEntryDependencies;
  // Dispatch validates source demands against the complete immutable closure,
  // never the prefix visited by the inventory loop.
  for (const auto &body : input.bodies()) {
    const auto *bodyMir = findMirBody(mir, body.identity.address);
    if (bodyMir == nullptr)
      throw std::logic_error("verified LLVM input lost an inventoried body");
    if (body.role == LoweredBodyRole::SourceExecutable &&
        closure.contains(body.identity.address))
      plan.bodies.push_back(body.identity.address);
  }
  StaticDispatchProofs dispatchProofs(plan, plan.input);
  Inventory inventory(plan.uses, mir, dispatchProofs, input.target());
  inventory.add(Shape::ObjectArtifact);
  inventory.add(input.programKind(),
      input.embeddingPlan() && native_llvm::infallibleEmbeddingContract(input)
          ? std::optional(Shape::InfallibleEmbedding) : std::nullopt);
  inventory.add(mir.executionProfile());
  for (const auto &body : input.bodies())
    inventory.inspect(body, *findMirBody(mir, body.identity.address),
                      closure.contains(body.identity.address));
  inventory.body.reset();
  inventory.block = 0;
  inventory.instruction = 0;
  inventory.required = true;
  inventory.executable = true;
  // Until class/payload execution is admitted, their type requirements on
  // reachable values reject them. Dormant type inventories remain metadata.
  for (const auto &instance : mir.classInstances()) {
    inventory.required = closure.conservative;
    const auto *row = input.findClassInstance(instance.id);
    inventory.source = row == nullptr ? SourceSpan{} : row->source;
    inventory.type(instance.type);
    inventory.traits(instance.traits, instance.type);
    // This use describes ordinary storage, not the whole ClassKind family.
    // Every field, capability, body and cleanup remains independently gated.
    const bool ordinary =
        (instance.kind == ClassKind::Class ||
         instance.kind == ClassKind::Struct) &&
        !instance.cAbiLayout && !instance.cOpaqueHandle &&
        !instance.unionLayout &&
        instance.compilerCapability == CompilerCapabilityTypeKind::None;
    const auto representation = inventory.representationShape(instance.type);
    inventory.add(instance.kind,
                  ordinary || instance.kind == ClassKind::Interface
                      ? representation
                      : std::nullopt);
    if (instance.compilerCapability != CompilerCapabilityTypeKind::None)
      inventory.add(
          instance.compilerCapability,
          native_llvm::nativeAtomicElement(mir, instance.type)
              ? std::optional(Shape::NativeAtomicStorage)
          : native_llvm::uninitializedStorageRepresentation(mir, instance) &&
                  inventory.nativeValue(instance.type)
              ? std::optional(Shape::NativeMemoryOperation)
          : native_llvm::sharedOwnerRepresentation(mir, instance) &&
                  inventory.nativeValue(instance.type)
              ? std::optional(Shape::NativeSharedOwnership)
              : inventory.classShape(instance.type));
    if (!instance.declaredFields.empty())
      inventory.add(Shape::ClassFields, representation);
    for (const auto &field : instance.declaredFields) {
      inventory.type(field.type);
      if (dispatchProofs.contains(field.type))
        // Storage metadata does not transfer an object. Executable producers,
        // publications and replacements require their own operation proofs.
        inventory.add(Shape::ClassFields,
                      inventory.representationShape(field.type));
    }
    if (!instance.bases.empty() || !instance.structuralBases.empty())
      inventory.add(Shape::ClassBases, representation);
    if (instance.polymorphic)
      inventory.add(Shape::PolymorphicClass, representation);
    if (instance.cAbiLayout)
      inventory.add(Shape::NativeRecord, inventory.classShape(instance.type));
    if (instance.unionLayout)
      inventory.add(Shape::NativeUnion, inventory.classShape(instance.type));
    if (instance.cOpaqueHandle)
      inventory.add(Shape::OpaqueNativeHandle);
  }
  for (const auto &function : mir.functionInstances()) {
    inventory.body = MirBodyAddress{MirBodyKind::Function, function.id};
    inventory.required = closure.contains(*inventory.body);
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    const auto nativeBinding =
        native_llvm::nativeProcessMemoryBinding(mir, function)
            ? std::optional(Shape::NativeProcessMemory)
        : native_llvm::nativeCThreadLifecycleBinding(mir, function)
            ? std::optional(Shape::NativeCThreadLifecycle)
        : native_llvm::nativeCThreadOperationBinding(mir, function)
            ? std::optional(Shape::NativeCThreadOperation)
            : std::nullopt;
    const auto nativeImport =
        nativeBinding ? nativeBinding
        : native_llvm::nativeCScalarImportContract(mir, input.target(),
                                                   function)
            ? std::optional(Shape::NativeCScalarImport)
        : native_llvm::nativeCCountedInputContract(mir, input.target(),
                                                   function)
            ? std::optional(Shape::NativeCCountedInput)
        : native_llvm::nativeCScalarFunctionImportContract(mir, input.target(),
                                                           function)
            ? std::optional(Shape::NativeCFunctionImport)
        : native_llvm::nativeCAggregateImportContract(mir, input.target(),
                                                      function)
            ? std::optional(Shape::NativeCAggregateImport)
        : native_llvm::nativeCDeclarationContract(mir, input.target(), function)
            ? std::optional(Shape::NativeCSignature)
            : std::nullopt;
    const auto declaration = dispatchProofs.declaration(function);
    if (declaration && nativeImport)
      throw BackendUnsupportedError("LLVM native import conflicts "
                                    "with a GTI declaration witness");
    const auto declarationShape = declaration ? declaration : nativeImport;
    inventory.executable = !declaration;
    inventory.add(function.definitionKind, declarationShape);
    inventory.add(function.linkage, nativeImport);
    inventory.add(function.entryKind);
    if (function.owner)
      inventory.add(function.receiverMutability,
                    inventory.representationShape(
                        mir.findClassInstance(*function.owner)->type));
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
      inventory.add(function.cThreadOperation, nativeBinding);
    if (function.processMemoryOperation != ProcessMemoryRuntimeOperation::None)
      inventory.add(function.processMemoryOperation, nativeBinding);
    if (function.definitionKind == MirDefinitionKind::RuntimeBinding)
      inventory.add(Shape::RuntimeBinding);
    if (function.definitionKind == MirDefinitionKind::Declaration)
      inventory.add(Shape::LibraryDeclaration, declarationShape);
    if (function.linkage == LanguageLinkage::C)
      inventory.add(Shape::NativeCall, nativeImport);
    if (function.mayRaiseDefinedFailure)
      inventory.add(Shape::FailureReturn);
    if (function.receiverMutability != ReceiverMutability::ReadOnly)
      inventory.add(Shape::MutableReceiver,
                    function.owner
                        ? inventory.representationShape(
                              mir.findClassInstance(*function.owner)->type)
                        : std::nullopt);
  }
  // ABI-only requirement metadata must not change later executable inventories.
  inventory.executable = true;
  inventory.body.reset();
  inventory.required = true;
  inventory.executable = true;
  for (const auto &storage : mir.foreignStorageDeclarations()) {
    inventory.type(storage.type);
    inventory.add(
        Shape::NativeCall,
        native_llvm::nativeCStorageImportContract(mir, input.target(), storage)
            ? std::optional(Shape::NativeCStorage)
            : std::nullopt);
  }
  for (const auto &payload : mir.payloadEnums()) {
    inventory.required = closure.conservative;
    inventory.type(payload.type);
    inventory.add(Shape::PayloadEnum);
    for (const auto &variant : payload.variants)
      for (const auto &type : variant.payloadTypes)
        inventory.type(type);
  }
  for (const auto &storage : mir.threadLocalStorage()) {
    inventory.required = true;
    inventory.body =
        MirBodyAddress{MirBodyKind::ThreadLocalInitializer, storage.symbol};
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    inventory.type(storage.type);
    if (dispatchProofs.contains(storage.type))
      inventory.add(Shape::PolymorphicTransfer);
    inventory.add(storage.dataInitializer ? Shape::ThreadLocalPassive
                                          : Shape::ThreadLocalDynamic,
                  inventory.nativeValue(storage.type)
                      ? std::optional(Shape::NativeThreadStorage)
                      : std::nullopt);
    if (storage.requiresActiveCleanup)
      inventory.cleanup(storage.cleanup);
  }
  inventory.body.reset();
  inventory.required = true;
  inventory.source = {};
  for (const auto &step : mir.programInitializationPlan().steps) {
    // Unused passive data is not executable initialization. Reachable symbol
    // accesses and exports still require an admitted storage representation.
    inventory.required =
        closure.conservative ||
        step.role == ProgramInitializationStepRole::Initializer;
    if (inventory.required)
      inventory.add(Shape::ProgramInitialization);
    inventory.add(step.dataInitialization);
    inventory.add(step.storageKind);
    inventory.add(step.role);
    if (const auto *module = findMirBody(mir, {MirBodyKind::Module, 0}))
      if (const auto *place = module->findPlace(step.storagePlace))
        if (dispatchProofs.contains(place->type))
          inventory.add(Shape::PolymorphicTransfer,
                        dispatchProofs.programStep(step) &&
                                dispatchProofs.metadata(place->type) ==
                                    Shape::NativeStaticDispatchStorage
                            ? std::optional(Shape::NativeProgramHierarchyStorage)
                            : std::nullopt);
  }
  inventory.required = true;
  if (mir.hostedStartupPlan())
    for (const auto &operation : mir.hostedStartupPlan()->operations) {
      inventory.add(operation.kind,
                    native_llvm::nativeCThreadLifecycleOperation(mir, operation)
                        ? std::optional(Shape::NativeCThreadLifecycle)
                    : inventory.nativeRetirement(operation)
                        ? std::optional(Shape::NativeProgramRetirement)
                        : std::nullopt);
      inventory.add(operation.failureBehavior);
    }
  if (input.embeddingPlan())
    inventory.add(Shape::EmbeddingExports,
        native_llvm::infallibleEmbeddingContract(input)
            ? std::optional(Shape::InfallibleEmbedding) : std::nullopt);
  for (const auto &callback : mir.nativeCallbackAdapters()) {
    inventory.required = closure.contains(LoweredGeneratedItemIdentity{
        .kind = LoweredGeneratedItemKind::NativeInteropAdapter,
        .owner = callback.id});
    const auto shape =
        native_llvm::nativeCCallbackContract(mir, input.target(), callback)
            ? std::optional(Shape::NativeCCallback)
            : std::nullopt;
    inventory.add(Shape::NativeCallback, shape);
    inventory.add(callback.cThreadCallback, shape);
    inventory.add(callback.failurePolicy, shape);
    inventory.type(callback.type);
  }
  for (const auto &item : input.generatedItems()) {
    inventory.required = closure.contains(item.identity);
    std::optional<Shape> shape;
    if (const auto *callback =
            std::get_if<LoweredNativeCallbackItem>(&item.payload))
      if (item.identity.kind == LoweredGeneratedItemKind::NativeInteropAdapter &&
          item.identity.owner == callback->adapter.id &&
          item.sourceBody ==
              MirBodyAddress{MirBodyKind::Function, callback->adapter.target} &&
          native_llvm::nativeCCallbackContract(mir, input.target(),
                                             callback->adapter))
        shape = Shape::NativeCCallback;
    if (const auto *task =
            std::get_if<LoweredThreadTaskAdapterItem>(&item.payload))
      if (item.identity.kind == LoweredGeneratedItemKind::ThreadTaskAdapter &&
          item.identity.owner == task->adapter.id &&
          item.sourceBody == task->adapter.sourceBody &&
          native_llvm::nativeThreadTaskAdapter(mir, task->adapter) &&
          inventory.relocatableRepresentation(task->adapter.taskType) &&
          (task->adapter.resultType == SemanticType::Void ||
           inventory.relocatableRepresentation(task->adapter.resultType)))
        shape = native_llvm::hierarchyThreadTask(mir, task->adapter)
                    ? Shape::NativeHierarchyThreadTask
                    : Shape::NativeThreadTask;
    if (item.identity.kind == LoweredGeneratedItemKind::ThreadLocalStorage)
      if (const auto *storage = mir.findThreadLocalStorage(item.identity.owner))
        if (inventory.nativeValue(storage->type))
          shape = Shape::NativeThreadStorage;
    if (const auto *cleanup =
            std::get_if<LoweredLifecycleCleanupItem>(&item.payload))
      if (const auto *owner = mir.findClassInstance(cleanup->classInstance)) {
        shape = inventory.classShape(owner->type);
        if (dispatchProofs.contains(owner->type))
          if (const auto *body = dispatchProofs.body(
                  {MirBodyKind::Destructor, cleanup->destructorInstance});
              body && body->shape &&
              (cleanup->form == LoweredLifecycleCleanupForm::OrdinaryClass ||
               cleanup->form ==
                   LoweredLifecycleCleanupForm::ConcreteSpecialization))
            shape = body->shape;
      }
    if (const auto *adapter =
            std::get_if<LoweredConcreteInstanceAdapterItem>(&item.payload))
      if (adapter->ownerClassInstance)
        if (const auto *owner =
                mir.findClassInstance(adapter->ownerClassInstance))
          shape = dispatchProofs.contains(owner->type)
                      ? (dispatchProofs.body(adapter->body)
                             ? dispatchProofs.body(adapter->body)->shape
                             : std::nullopt)
                      : inventory.classShape(owner->type);
    inventory.add(item.identity.kind, shape);
    inventory.add(item.sourceKind, shape);
    if (!item.dependencies.empty())
      inventory.add(Shape::GeneratedDependencies, shape);
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
