#include "gti/llvm_admission.h"

#include "../mir_verification_queries.h"
#include "closure_environment.h"
#include "constructor_field_publication.h"
#include "coverage.h"
#include "dispatch.h"
#include "embedding.h"
#include "lifetimes.h"
#include "memory_types.h"
#include "native_c_contract.h"
#include "parameter_packs.h"
#include "placement.h"
#include "reachability.h"
#include "text.h"
#include "thread_lifecycle.h"

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
  return isPassiveScalarValueType(type) && type.kind != SemanticType::Void;
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

bool arrayProjection(const MirBody &body, const MirPlace &place) {
  const auto root = std::find_if(
      body.places.begin(), body.places.end(), [&](const auto &candidate) {
        return candidate.projections.empty() && candidate.root == place.root &&
               candidate.binding == place.binding &&
               candidate.symbol == place.symbol &&
               candidate.temporary == place.temporary &&
               candidate.value == place.value && candidate.loan == place.loan;
      });
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
  };

  StaticDispatchProofs(const LlvmAdmissionPlan &plan,
                       VerifiedLoweredProgram input)
      : plan(plan), input(std::move(input)), program(plan.program().mir()) {
    for (const auto address : plan.requiredBodies()) {
      const auto &body = *findMirBody(program, address);
      native_llvm::Placement placement(body);
      auto &bodyProof = bodies[{address.kind, address.owner}];
      bodyProof.transfer = contains(body.returnType);
      if (const auto *target = function(address)) {
        for (const auto &parameter : target->parameterTypes)
          bodyProof.transfer |= contains(parameter);
        if (target->owner &&
            contains(program.findClassInstance(*target->owner)->type) &&
            target->receiverMutability == ReceiverMutability::Consuming)
          bodyProof.transfer = true;
      }
      if (address.kind == MirBodyKind::Constructor) {
        const auto &target = *program.findConstructorInstance(address.owner);
        for (const auto &parameter : target.parameterTypes)
          bodyProof.transfer |= contains(parameter);
        if (target.dispatchState)
          bodyProof.pure = pureState(target.dispatchState->activeClass);
      }
      if (address.kind == MirBodyKind::Destructor) {
        const auto &target = *program.findDestructorInstance(address.owner);
        if (contains(program.findClassInstance(target.owner)->type))
          bodyProof.observation = observesDispatch(address);
      }
      for (const auto &place : body.places)
        if (contains(place.type) &&
            (place.type.kind != SemanticType::Class || nested(place.type) ||
             place.root == MirPlaceRootKind::Symbol || place.foreignStorage))
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
          const bool fresh = freshLocal(address, body, placement, stage);
          const bool transfer = freshTransfer(body, stage);
          const bool initialization =
              freshInitialization(address, body, placement, stage);
          if (contains(stage.info.type)) {
            if ((stage.kind == MirInstructionKind::Construct && !fresh &&
                 !transfer) ||
                (stage.kind == MirInstructionKind::Move && !transfer) ||
                (stage.kind == MirInstructionKind::Load && !transfer) ||
                stage.kind == MirInstructionKind::Assign ||
                (stage.kind == MirInstructionKind::CallInput &&
                 stage.callInputKind != HirCallInputKind::ReadBorrow &&
                 stage.callInputKind != HirCallInputKind::MutableBorrow) ||
                stage.kind == MirInstructionKind::Call ||
                stage.operation == MirOperation::Aggregate ||
                (stage.kind == MirInstructionKind::Initialize &&
                 !initialization))
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
                    initialization))
                proof.transfer = true;
          const auto transferred = [&](const MirOperand &operand) {
            return contains(operand.type) &&
                   (operand.kind == MirOperandKind::Copy ||
                    operand.kind == MirOperandKind::Move);
          };
          for (const auto &operand : stage.operands)
            proof.transfer |= transferred(operand) && !transfer;
          if (stage.receiver)
            proof.transfer |= transferred(*stage.receiver);
          if (stage.replacementCleanup &&
              contains(stage.replacementCleanup->type))
            proof.transfer = true;
          if (stage.elementCleanup && contains(stage.elementCleanup->type))
            proof.transfer = true;
          if (stage.kind == MirInstructionKind::Construct ||
              stage.kind == MirInstructionKind::InitializeBase)
            if (stage.info.type.kind == SemanticType::Class &&
                contains(stage.info.type))
              proof.pure = pureState(instance(stage.info.type).id);
          if (stage.replacementCleanup)
            proof.pure |= pureCleanup(*stage.replacementCleanup);
          if (stage.kind == MirInstructionKind::Drop)
            for (const auto &event : stage.lifecycle)
              if (const auto *drop = body.findDropObligation(event.source))
                proof.observation |= cleanupObserves(drop->dropType);
        }
        auto &edge = operations[key(address, block.id, 0)];
        const auto argument = [&](const MirOperand &value) {
          edge.transfer |= contains(value.type);
        };
        if (block.terminator.value &&
            block.terminator.kind == MirTerminatorKind::Return)
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
  bool nested(const SemanticType &type) const {
    if (type.kind != SemanticType::Class)
      return contains(type);
    for (const auto &field : instance(type).declaredFields)
      if (contains(field.type))
        return true;
    return false;
  }
  bool referent(const SemanticType &type) const {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
           type.arguments.front().kind == SemanticType::Class &&
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
  std::optional<Shape> metadata(const SemanticType &type) {
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
        function.receiverMutability == ReceiverMutability::Consuming ||
        (function.returnType != SemanticType::Void &&
         !isPassiveScalarValueType(function.returnType)) ||
        std::any_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(),
                    [](const auto &type) {
                      return type == SemanticType::Void ||
                             !isPassiveScalarValueType(type);
                    }) ||
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
    if (!contains(recipe.type) || nested(recipe.type) ||
        cleanupObserves(recipe))
      return {};
    try {
      if (!metadata(recipe.type) || !planner())
        return {};
      native_llvm::Lifetimes::preflightCleanup(*session, *dispatch, recipe,
                                               replacement);
      return Shape::NativeStaticDispatchStorage;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  std::optional<Shape> borrowed(const MirBody &body,
                                const MirOperand &operand) {
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
      static_cast<void>(session->layouts().baseSubobject(source, target));
      return Shape::NativeBorrowedDispatchView;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
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
    if (!simpleLoan(loan))
      return {};
    const auto *source = body.findPlace(loan.source);
    if (!source)
      return {};
    auto type = unreference(source->type);
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

private:
  const LlvmAdmissionPlan &plan;
  VerifiedLoweredProgram input;
  const MirProgram &program;
  std::map<Key, Operation> operations;
  std::map<BodyKey, Operation> bodies;
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
  bool metadataClass(const MirClassInstance &owner,
                     std::set<HirClassInstanceId> &path) const {
    if ((owner.kind != ClassKind::Class && owner.kind != ClassKind::Struct &&
         owner.kind != ClassKind::Interface) ||
        owner.cAbiRecord || owner.cAbiLayout || owner.cOpaqueHandle ||
        owner.unionLayout ||
        owner.compilerCapability != CompilerCapabilityTypeKind::None ||
        owner.traits.containsBorrowedState || !path.insert(owner.id).second)
      return false;
    bool valid = !nested(owner.type) &&
                 std::all_of(owner.declaredFields.begin(),
                             owner.declaredFields.end(), [](const auto &field) {
                               return field.type != SemanticType::Void &&
                                      isPassiveScalarValueType(field.type);
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
  bool pureCleanup(const MirDropType &recipe) const {
    bool pure = recipe.requiresActiveCleanup && recipe.classInstance &&
                pureState(*recipe.classInstance);
    for (const auto &child : recipe.aggregate)
      pure |= pureCleanup(child);
    return pure;
  }
  bool cleanupObserves(const MirDropType &recipe) const {
    if (recipe.destructor && contains(recipe.type) &&
        observesDispatch({MirBodyKind::Destructor, *recipe.destructor}))
      return true;
    return std::any_of(
        recipe.aggregate.begin(), recipe.aggregate.end(),
        [&](const auto &child) { return cleanupObserves(child); });
  }
  bool observesDispatch(MirBodyAddress root) const {
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
  bool formal(MirBodyAddress address, HirBindingId binding) const {
    if (const auto *target = function(address))
      return std::find(target->parameterBindings.begin(),
                       target->parameterBindings.end(),
                       binding) != target->parameterBindings.end();
    if (address.kind == MirBodyKind::Constructor) {
      const auto &target = *program.findConstructorInstance(address.owner);
      return std::find(target.parameterBindings.begin(),
                       target.parameterBindings.end(),
                       binding) != target.parameterBindings.end();
    }
    return false;
  }
  bool freshLocal(MirBodyAddress address, const MirBody &body,
                  const native_llvm::Placement &placement,
                  const MirInstruction &stage) const {
    if ((address.kind != MirBodyKind::Function &&
         address.kind != MirBodyKind::Constructor &&
         address.kind != MirBodyKind::Destructor) ||
        stage.kind != MirInstructionKind::Construct || !stage.result ||
        stage.constructorKind != ConstructorKind::Ordinary ||
        stage.info.type.kind != SemanticType::Class || nested(stage.info.type))
      return false;
    const auto *target = placement.target(*stage.result);
    const auto *place =
        target && target->place ? body.findPlace(target->place) : nullptr;
    std::size_t publications = 0;
    if (target)
      for (const auto &block : body.blocks)
        for (const auto &initialization : block.instructions)
          publications +=
              !initialization.tombstone &&
              initialization.kind == MirInstructionKind::Initialize &&
              initialization.destination == target->place &&
              initialization.operands.size() == 1 &&
              initialization.operands.front().kind == MirOperandKind::Value &&
              initialization.operands.front().value == *stage.result;
    return target && !target->aggregate && !target->returned && place &&
           place->root == MirPlaceRootKind::Binding && place->binding &&
           !place->capture && publications == 1 && place->projections.empty() &&
           !place->foreignStorage && place->type == stage.info.type &&
           !formal(address, place->binding);
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
            freshLocal(address, body, placement, producer))
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
        if (stage.kind != MirInstructionKind::CallInput ||
            (stage.callInputKind != HirCallInputKind::ReadBorrow &&
             stage.callInputKind != HirCallInputKind::MutableBorrow) ||
            stage.destination || stage.preparedParameterDrop ||
            stage.retainedOwnerPlace || stage.successResultDrop ||
            stage.successResultDestination || stage.operands.size() != 1 ||
            stage.operands.front().kind == MirOperandKind::Value ||
            unreference(stage.info.type) != unreference(operand.type) ||
            unreference(stage.operands.front().type) !=
                unreference(stage.info.type))
          return nullptr;
        if (stage.loan) {
          const auto *loan = body.findLoan(*stage.loan);
          if (!loan || !simpleLoan(*loan))
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
    if (loan && !simpleLoan(*loan))
      return {};
    SemanticType source = SemanticType::Unknown;
    AccessMode access = AccessMode::ReadOnly;
    if (place) {
      if (place->foreignStorage ||
          (place->root != MirPlaceRootKind::Binding &&
           place->root != MirPlaceRootKind::This &&
           place->root != MirPlaceRootKind::Loan) ||
          std::any_of(place->projections.begin(), place->projections.end(),
                      [](const auto &projection) {
                        return projection.kind != MirProjectionKind::Base &&
                               projection.kind !=
                                   MirProjectionKind::Dereference;
                      }))
        return {};
      if (place->root == MirPlaceRootKind::Loan) {
        const auto *selected = body.findLoan(place->loan);
        if (!selected || !simpleLoan(*selected))
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
    source = unreference(source);
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
      if (address.kind == MirBodyKind::Destructor && observesDispatch(address))
        return {};
      if (address.kind == MirBodyKind::Constructor && owner->polymorphic) {
        const auto &target = *program.findConstructorInstance(address.owner);
        if (target.definitionKind != MirDefinitionKind::Source ||
            !target.dispatchState ||
            target.dispatchState->activeClass != owner->id)
          return {};
        dispatch->preflightState(owner->id);
      }
      return Shape::NativeStaticDispatchStorage;
    } catch (const BackendUnsupportedError &) {
      return {};
    }
  }
  std::optional<Shape> proveOperation(MirBodyAddress address,
                                      const MirBody &body,
                                      const native_llvm::Placement &placement,
                                      const MirInstruction &stage) {
    try {
      if (freshTransfer(body, stage)) {
        if (!metadata(stage.info.type))
          return {};
        static_cast<void>(native_llvm::Lifetimes::preflightFreshTransfer(
            *session, *dispatch, body, stage));
        return Shape::NativeFreshHierarchyTransfer;
      }
      if (freshTransferInitialization(body, stage) &&
          metadata(stage.operands.front().type))
        return Shape::NativeFreshHierarchyTransfer;
      if (stage.kind == MirInstructionKind::Construct &&
          contains(stage.info.type) &&
          freshLocal(address, body, placement, stage)) {
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
        return Shape::NativeStaticDispatchStorage;
      }
      if (stage.kind == MirInstructionKind::Initialize &&
          freshInitialization(address, body, placement, stage) &&
          metadata(stage.operands.front().type))
        return Shape::NativeStaticDispatchStorage;
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
        static_cast<void>(session->layouts().baseSubobject(
            program.findClassInstance(target->owner)->type, owner.type));
        return Shape::NativeStaticDispatchStorage;
      }
      if (stage.kind == MirInstructionKind::Drop)
        for (const auto &event : stage.lifecycle)
          if (const auto *drop = body.findDropObligation(event.source))
            if (const auto shape = cleanup(drop->dropType))
              return shape;
      if ((stage.kind == MirInstructionKind::Borrow ||
           stage.kind == MirInstructionKind::CallInput) &&
          stage.operands.size() == 1 &&
          (stage.kind != MirInstructionKind::CallInput ||
           stage.callInputKind == HirCallInputKind::ReadBorrow ||
           stage.callInputKind == HirCallInputKind::MutableBorrow)) {
        if (stage.loan) {
          const auto *loan = body.findLoan(*stage.loan);
          if (!loan || !simpleLoan(*loan))
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
        if ((borrowedUse(body, *stage.receiver) &&
             !borrowed(body, *stage.receiver)) ||
            !stage.functionTarget || stage.nativeCalleeType ||
            stage.nativeCallbackAdapter || stage.callableInvocation ||
            stage.threadTaskAdapter || !stage.scopedThreadTasks.empty())
          return {};
        const auto *target =
            program.findFunctionInstance(*stage.functionTarget);
        if (!target || target->linkage != LanguageLinkage::Gti ||
            target->receiverMutability == ReceiverMutability::Consuming ||
            (target->returnType != SemanticType::Void &&
             !isPassiveScalarValueType(target->returnType)) ||
            std::any_of(target->parameterTypes.begin(),
                        target->parameterTypes.end(), [](const auto &type) {
                          return !isPassiveScalarValueType(type) ||
                                 type == SemanticType::Void;
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
        if (isMirExactReceiverPresentation(program, stage, source)) {
          if (target->receiverMutability != ReceiverMutability::ReadOnly)
            return {};
          static_cast<void>(
              session->layouts().baseSubobject(owner.type, source));
        } else {
          static_cast<void>(
              session->layouts().baseSubobject(source, owner.type));
        }
        return Shape::NativeBorrowedDispatchView;
      }
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
        nativeTarget(nativeTarget) {}

  std::optional<MirBodyAddress> body;
  SourceSpan source;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  bool required = true;
  bool executable = false;
  const MirBody *currentBody = nullptr;

  bool nativeValue(const SemanticType &type) const {
    std::set<HirClassInstanceId> visiting;
    std::set<const MirPayloadEnum *> payloads;
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
        if (selected.arguments.size() != 1 ||
            !mir_detail::completeObjectType(program, selected.arguments[0]))
          return false;
        const auto &pointee = selected.arguments[0];
        if (pointee.kind == SemanticType::Class)
          for (const auto &owner : program.classInstances())
            if (owner.type == pointee && visiting.contains(owner.id))
              return !owner.polymorphic && !owner.abstract &&
                     owner.compilerCapability ==
                         CompilerCapabilityTypeKind::None;
        return self(self, pointee);
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
        if (!visiting.insert(owner->id).second)
          return false;
        const bool valid = self(self, owner->type.arguments[0]);
        visiting.erase(owner->id);
        return valid;
      }
      if (owner->compilerCapability != CompilerCapabilityTypeKind::None ||
          owner->polymorphic || owner->abstract || owner->cOpaqueHandle ||
          !owner->bases.empty() || !owner->structuralBases.empty() ||
          !visiting.insert(owner->id).second)
        return false;
      bool valid = true;
      for (const auto &field : owner->declaredFields)
        valid = valid && self(self, field.type);
      visiting.erase(owner->id);
      return valid;
    };
    return supported(supported, type);
  }
  std::optional<Shape> classShape(const SemanticType &type) const {
    // References previously borrowed this ordinary-storage witness while the
    // recursive referent requirement kept hierarchy execution closed. New
    // hierarchy metadata must never turn that into an operation permission.
    if (dispatchProofs.related(type))
      return {};
    const auto *leaf = &type;
    while ((leaf->kind == SemanticType::Array ||
            leaf->kind == SemanticType::Reference ||
            leaf->kind == SemanticType::RawPointer) &&
           leaf->arguments.size() == 1)
      leaf = &leaf->arguments[0];
    if (leaf->kind == SemanticType::UniqueOwner && nativeValue(type))
      return Shape::NativeUniqueOwnership;
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
    if (stage.intrinsic == IntrinsicKind::AllocateUniqueOwner)
      return native_llvm::nativeUniqueConstructionShape(program, stage) &&
             nativeValue(stage.info.type);
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
    if (stage.intrinsic == IntrinsicKind::ObjectConstruct)
      return native_llvm::nativeObjectConstructionShape(program, stage) &&
             nativeValue(stage.parameterTypes[0].arguments[0]);
    if (stage.intrinsic == IntrinsicKind::ObjectSize ||
        stage.intrinsic == IntrinsicKind::ObjectAlignment)
      return mir_detail::objectLayoutQuery(program, stage) &&
             nativeValue(*stage.objectLayoutType);
    if (stage.intrinsic == IntrinsicKind::ObjectRelocate ||
        stage.intrinsic == IntrinsicKind::ObjectRelocateRange)
      return mir_detail::objectRelocate(program, stage) &&
             nativeValue(stage.parameterTypes[0].arguments[0]);
    if (stage.intrinsic == IntrinsicKind::ObjectDestroy)
      return mir_detail::objectDestroy(program, stage) &&
             nativeValue(stage.parameterTypes[0].arguments[0]);
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
    if (type.kind != SemanticType::Class || !nativeValue(type))
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
    return nativeClass(*leaf) && nativeValue(type);
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
            !nativeValue(type.arguments[0]))
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
        if (static_cast<bool>(projection.index) ==
                projection.constantIndex.has_value() ||
            (projection.index &&
             (!index || !constantIntegerDomain(index->info.type))))
          return false;
      }
    }
    return raw && type == place.type && nativeValue(type) &&
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
      if (!primitiveRawAccess(mir, *selected)) {
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
  bool nativeAddressConversion(const MirInstruction &stage) const {
    if (!mir_detail::rawPointerConversion(stage))
      return false;
    const auto &source = stage.operands[0].type;
    const auto &target = stage.info.type;
    return (source.arguments[0] == SemanticType::Void ||
            nativeValue(source.arguments[0])) &&
           (target.arguments[0] == SemanticType::Void ||
            nativeValue(target.arguments[0]));
  }
  std::optional<Shape> representationShape(const SemanticType &type) {
    return dispatchProofs.related(type) ? dispatchProofs.metadata(type)
                                        : classShape(type);
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
        if (root.root == place.root && root.binding == place.binding &&
            root.symbol == place.symbol && root.temporary == place.temporary &&
            root.projections.empty())
          if (const auto selected = representationShape(root.type))
            return selected;
    return {};
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
    return owner &&
           (owner->kind == ClassKind::Class ||
            owner->kind == ClassKind::Struct) &&
           owner->compilerCapability == CompilerCapabilityTypeKind::None &&
           !owner->polymorphic && !owner->abstract && !owner->cOpaqueHandle &&
           !owner->cAbiRecord && !owner->cAbiLayout && !owner->unionLayout &&
           owner->bases.empty() && owner->structuralBases.empty() &&
           nativeValue(type) && classShape(type) == Shape::NativeClassStorage;
  }

  bool representedRawReferenceTypes(const MirInstruction &stage) const {
    const auto &pointer = stage.operands[0].type;
    if (!(passiveAddress(pointer) && passiveReferenceType(stage.info.type)) &&
        !ordinaryRawReferenceObject(pointer.arguments.front()))
      return false;
    const auto &witness = stage.parameterTypes[1];
    // The verifier owns origin/access; the witness need not share the raw
    // pointer's type or address. Neither branch materializes or copies it.
    return passiveReferenceType(witness) ||
           ordinaryRawReferenceObject(witness.arguments.front());
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
           unsafeCallResultProducer(body, block, stage, *loan, stage.info.type);
  }

  bool unsafeCallResultInput(const MirBody &body, const MirBlock &block,
                             const MirInstruction &stage) const {
    const bool mutableInput =
        stage.callInputKind == HirCallInputKind::MutableBorrow;
    if (stage.kind != MirInstructionKind::CallInput || !stage.result ||
        stage.callInputRole != MirCallInputRole::Argument ||
        (stage.callInputKind != HirCallInputKind::ReadBorrow &&
         !mutableInput) ||
        stage.callSite == 0 || stage.hirValue == 0 || stage.defaultArgument ||
        stage.rawMemoryAccess ||
        stage.unsafeOperation != UnsafeOperationKind::DeclaredUnsafeCall ||
        stage.intrinsic != IntrinsicKind::None ||
        stage.operation != MirOperation::None ||
        stage.info.type.kind != SemanticType::Reference ||
        stage.info.type.arguments.size() != 1 ||
        stage.info.type.referenceAccess !=
            (mutableInput ? AccessMode::Mutable : AccessMode::ReadOnly) ||
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
    if (input.kind != MirOperandKind::Loan || !input.loan || !input.place ||
        input.value || input.literal || input.type != stage.info.type)
      return false;
    const auto &referent = stage.info.type.arguments.front();
    const auto *place = body.findPlace(input.place);
    const auto *loan = body.findLoan(input.loan);
    // The explicit loan place carries the returned runtime address. Its
    // possible owner origin is never a substitute, including during call-input
    // staging.
    return place && place->root == MirPlaceRootKind::Loan &&
           place->projections.empty() && place->loan == input.loan &&
           place->type == referent && loan &&
           loan->kind == MirLoanKind::CallResult && !loan->entry &&
           !loan->escapes && loan->producedBy == stage.hirValue &&
           loan->access == place->access &&
           loan->access == stage.info.type.referenceAccess &&
           unsafeCallResultProducer(body, block, stage, *loan, referent);
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
      const auto storage = classShape(plan.targetType);
      return storage && *storage != Shape::CallableMetadata ? storage
                                                            : std::nullopt;
    }
    if (plan.transport !=
            native_llvm::UnstagedConstructorFieldPublicationTransport::
                NullUniqueOwner ||
        plan.sourceType != SemanticType::NullPtr ||
        plan.targetType.kind != SemanticType::UniqueOwner ||
        plan.targetType.arguments.size() != 1 ||
        !mir_detail::completeObjectType(program,
                                        plan.targetType.arguments.front()))
      return std::nullopt;
    const auto storage = classShape(plan.targetType);
    if (storage && *storage == Shape::NativeUniqueOwnership)
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
        add(Shape::PayloadEnum, classShape(type));
      if (type.kind == SemanticType::Class)
        for (const auto &owner : program.classInstances())
          if (owner.type == type && owner.unionLayout)
            add(Shape::NativeUnion, classShape(type));
      const bool passiveArray =
          type.kind == SemanticType::Array && passiveArrayElement(type);
      const bool arrayAddress = type.kind == SemanticType::RawPointer &&
                                type.arguments.size() == 1 &&
                                passiveArrayElement(type.arguments.front());
      const bool reference =
          type.kind == SemanticType::Reference && passiveReference(type);
      const bool address = passiveAddress(type) ||
                           type.kind == SemanticType::NullPtr ||
                           type.kind == SemanticType::CString;
      add(type.kind,
          opaqueReferent(type) ? std::optional(Shape::ErasedReference)
          : address            ? std::optional(Shape::PassiveAddressValue)
          : native_llvm::nativeCScalarFunctionValueContract(program,
                                                            nativeTarget, type)
              ? std::optional(Shape::NativeCFunctionValue)
          : native_llvm::nativeCFunctionValueContract(program, nativeTarget, type)
              ? std::optional(Shape::NativeCSignature)
          : passiveArray ? std::optional(Shape::PassiveArrayValue)
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
        add(Shape::FixedArray, passiveArray
                                   ? std::optional(Shape::PassiveArrayValue)
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

  void operand(const MirOperand &operand) {
    const auto *place =
        currentBody ? currentBody->findPlace(operand.place) : nullptr;
    const bool selectedReference =
        passiveReference(operand.type) &&
        (operand.kind == MirOperandKind::Address ||
         operand.kind == MirOperandKind::BorrowRead ||
         operand.kind == MirOperandKind::BorrowWrite ||
         operand.kind == MirOperandKind::Loan);
    const bool borrowedUse =
        currentBody && dispatchProofs.borrowedUse(*currentBody, operand);
    const auto view = currentBody
                          ? dispatchProofs.borrowed(*currentBody, operand)
                          : std::nullopt;
    add(operand.kind, selectedReference ? std::optional(Shape::PassiveReference)
                      : borrowedUse     ? view
                      : operand.kind == MirOperandKind::Address && place &&
                              arrayProjection(*currentBody, *place)
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
        add(Shape::PolymorphicClass); // Dynamic owner cleanup stays pending.
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
    }
    if (row.role != LoweredBodyRole::SourceExecutable)
      return;
    executable = true;
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
            ? std::nullopt
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
          : primitiveRawAccess(mir, place)
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
            : primitiveRawAccess(mir, place)
                ? std::optional(Shape::PrimitiveRawAccess)
            : arrayProjection(mir, place)
                ? std::optional(Shape::CheckedArrayProjection)
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
      const bool hierarchy = origin && dispatchProofs.related(origin->type);
      const auto view =
          hierarchy ? dispatchProofs.loan(mir, loan) : std::nullopt;
      add(loan.kind, hierarchy     ? view
                     : borrowShape ? borrowShape
                     : selected    ? std::optional(Shape::PassiveReference)
                     : origin      ? placeClassShape(*origin)
                                   : std::nullopt);
      add(loan.access);
      if (loan.referenceSelection)
        add(Shape::SelectedReferenceAddress, borrowShape);
      if (loan.twoPhase)
        add(Shape::TwoPhaseBorrow,
            !hierarchy && origin ? placeClassShape(*origin) : std::nullopt);
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
        const bool rawReference = representedRawReference(mir, mirBlock, stage);
        const auto rawReferenceShape =
            rawReference ? std::optional(classShape(stage.info.type)
                                             .value_or(Shape::PassiveReference))
                         : std::nullopt;
        const auto [extendedRaw, validRaw] = nativeRawStage(mir, stage);
        const bool rawClassStage = extendedRaw && validRaw;
        const bool classArithmetic = nativePointerArithmetic(stage) && validRaw;
        std::optional<Shape> storageShape =
            typedMemory ? std::optional(Shape::NativeMemoryOperation)
                        : std::nullopt;
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
        add(stage.kind,
            stage.kind == MirInstructionKind::Borrow
                ? (passiveReference(stage.info.type)
                       ? std::optional(Shape::PassiveReference)
                   : dispatchProofs.related(stage.info.type) ||
                           (!stage.operands.empty() &&
                            dispatchProofs.borrowedUse(mir,
                                                       stage.operands.front()))
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
              arrayProjection(mir, *addressPlace);
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
                  ? classShape(mirInstructionEnumType(stage))
              : nativeAddressConversion(stage) && validRaw
                  ? std::optional(Shape::NativeObjectAddress)
              : (nativeRawIndex(stage) && validRaw) || classArithmetic
                  ? std::optional(Shape::NativeClassRawAccess)
              : scalarMutation ? std::optional(Shape::InPlaceScalarMutation)
              : primitiveRawIndex(stage) || primitivePointerArithmetic(stage)
                  ? std::optional(Shape::PrimitiveRawAccess)
              : arrayAggregate ? std::optional(Shape::PassiveArrayValue)
              : stage.operation == MirOperation::Aggregate &&
                      stage.info.type.kind == SemanticType::Array
                  ? classShape(stage.info.type)
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
              callableIntrinsic ? callableIntrinsic
              : typedMemory     ? std::optional(Shape::NativeMemoryOperation)
              : nativeAddressConversion(stage) && validRaw
                  ? std::optional(Shape::NativeObjectAddress)
              : erasedReference(stage) ? std::optional(Shape::ErasedReference)
              : representedTextAddress(mir, stage)
                  ? std::optional(Shape::CountedText)
              : rawReference ? rawReferenceShape
              : stage.intrinsic == IntrinsicKind::RawPointerConversion &&
                      stage.operands.size() == 1 &&
                      passiveAddress(stage.info.type) &&
                      passiveAddress(stage.operands[0].type)
                  ? std::optional(Shape::PassiveAddressValue)
              : textQuery ? std::optional(Shape::CountedText)
              : stage.receiver &&
                      stage.receiver->type.kind == SemanticType::Array &&
                      passiveArrayElement(stage.receiver->type) &&
                      stage.operands.empty() &&
                      (stage.intrinsic == IntrinsicKind::ArraySize ||
                       stage.intrinsic == IntrinsicKind::ArrayRangeBegin)
                  ? std::optional(Shape::PassiveArrayValue)
                  : std::nullopt);
        }
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
        if (stage.threadTaskAdapter)
          add(Shape::ThreadTask);
        if (!stage.scopedThreadTasks.empty())
          add(Shape::ScopedThreadTasks);
        if (stage.preparedParameterDrop)
          add(Shape::PreparedOwnerInput,
              classShape(mir.findDropObligation(*stage.preparedParameterDrop)
                             ->dropType.type));
        if (stage.successResultDrop || stage.successResultDestination)
          add(Shape::SuccessResultPublication, classShape(stage.info.type));
        if (stage.tracksCaller || stage.callerLocation ||
            stage.sourceLocation || stage.forwardsCallerLocation)
          add(Shape::CallerLocation);
        bool rawAccess =
            rawReference || primitiveRawIndex(stage) ||
            (stage.destination &&
             primitiveRawAccess(mir, *mir.findPlace(*stage.destination)));
        for (const auto &operand : stage.operands)
          if (const auto *place = mir.findPlace(operand.place))
            rawAccess |= primitiveRawAccess(mir, *place);
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
              : rawAccess ? std::optional(Shape::PrimitiveRawAccess)
                          : std::nullopt);
        // Lifecycle marks repeat the source unsafe context but execute no
        // memory operation. The typed access/arithmetic stages are inventoried
        // independently; their pending shapes cannot be hidden by this marker.
        const auto &instructionCoverage = coverage(stage.kind);
        const bool permissionOnly =
            stage.kind == MirInstructionKind::Lifecycle ||
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
              : rawClassStage &&
                      stage.unsafeOperation == UnsafeOperationKind::RawMember &&
                      nativeRawFieldStage(mir, stage)
                  ? std::optional(Shape::NativeClassRawAccess)
              : (rawClassStage || classArithmetic) &&
                      (stage.unsafeOperation ==
                           UnsafeOperationKind::RawDereference ||
                       stage.unsafeOperation == UnsafeOperationKind::RawIndex ||
                       stage.unsafeOperation ==
                           UnsafeOperationKind::PointerArithmetic)
                  ? std::optional(Shape::NativeClassRawAccess)
              : typedMemory && stage.unsafeOperation ==
                                   UnsafeOperationKind::ObjectLifetime
                  ? std::optional(Shape::NativeMemoryOperation)
              : (ordinaryUnsafe || unsafeCallResultLoad(mir, mirBlock, stage) ||
                 unsafeCallResultInput(mir, mirBlock, stage) ||
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
                 ((primitivePointerArithmetic(stage) ||
                   (stage.kind == MirInstructionKind::Assign &&
                    passiveAddress(stage.info.type))) &&
                  stage.unsafeOperation ==
                      UnsafeOperationKind::PointerArithmetic))
                  ? std::optional(Shape::PrimitiveRawAccess)
                  : std::nullopt);
        }
        if (stage.payloadExtractionKind)
          add(*stage.payloadExtractionKind,
              classShape(mirInstructionEnumType(stage)));
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
          add(stage.synchronization.kind);
          if (stage.synchronization.order)
            add(*stage.synchronization.order);
          if (stage.synchronization.failureOrder)
            add(*stage.synchronization.failureOrder);
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
          native_llvm::uninitializedStorageRepresentation(mir, instance) &&
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
        inventory.add(Shape::PolymorphicTransfer);
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
          inventory.add(Shape::PolymorphicTransfer);
  }
  inventory.required = true;
  if (mir.hostedStartupPlan())
    for (const auto &operation : mir.hostedStartupPlan()->operations) {
      inventory.add(operation.kind,
                    native_llvm::nativeCThreadLifecycleOperation(mir, operation)
                        ? std::optional(Shape::NativeCThreadLifecycle)
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
