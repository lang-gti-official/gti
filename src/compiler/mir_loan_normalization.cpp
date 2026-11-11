#include "mir_loan_normalization.h"

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {
namespace {

// See the header: every rule here is part of the owner-reviewed gate contract.

[[nodiscard]] std::string describe(MirBodyAddress address) {
  return "body " + std::to_string(static_cast<std::size_t>(address.kind)) +
         ":" + std::to_string(address.owner);
}

// An EndBorrow may be removed only when its position states nothing but the
// end of its loan. Its own source attribution goes with it; every other field
// must hold its default value. Lowering copies a source HIR value's unsafe
// operation and ownership event onto every record attributed to that value
// (the event's authority is the value's own Move/Read record), so those two
// fields count as attribution only on an EndBorrow that names a HIR value.
void requireLoanOnlyEndBorrow(const MirInstruction &instruction,
                              MirBodyAddress address) {
  MirInstruction residue = instruction;
  residue.id = 0;
  residue.kind = MirInstructionKind::Compute;
  residue.loan.reset();
  if (residue.hirValue != 0) {
    residue.unsafeOperation = UnsafeOperationKind::None;
    residue.ownership.reset();
  }
  residue.hirValue = 0;
  residue.hirStatement = 0;
  residue.callSite = 0;
  residue.constructorInitializer = 0;
  residue.hostedStartupOperation = 0;
  if (residue == MirInstruction{})
    return;
  std::string fields;
  const auto note = [&](bool present, const char *name) {
    if (present)
      fields += std::string(fields.empty() ? "" : ", ") + name;
  };
  note(!residue.lifecycle.empty(), "lifecycle");
  note(residue.fullExpressionEnd != 0, "fullExpressionEnd");
  note(residue.earlyExitFullExpressionEnd, "earlyExitFullExpressionEnd");
  note(residue.cleanupBoundaryEnd != 0, "cleanupBoundaryEnd");
  note(!residue.operands.empty(), "operands");
  note(residue.receiver.has_value(), "receiver");
  note(residue.result.has_value(), "result");
  note(residue.destination.has_value(), "destination");
  note(residue.ownership.has_value(), "ownership");
  note(residue.unsafeOperation != UnsafeOperationKind::None, "unsafeOperation");
  note(residue.operation != MirOperation::None, "operation");
  if (fields.empty())
    fields = "another non-default field";
  throw MirLoanNormalizationError("loan normalization refuses EndBorrow i" +
                                  std::to_string(instruction.id) + " in " +
                                  describe(address) + ": it carries " + fields +
                                  ", so its position is not only a loan end");
}

class BodyRenumbering {
public:
  BodyRenumbering() = default;
  explicit BodyRenumbering(MirBodyAddress address) : address_(address) {}

  void keep(MirInstructionId old) {
    claim(old);
    renumbered_.emplace(old, renumbered_.size() + 1);
  }

  void remove(MirInstructionId old) {
    claim(old);
    removed_.insert(old);
  }

  // Zero is the "no instruction" sentinel in every remapped field (and
  // "before the terminator" in a debug point), never an allocated ID.
  [[nodiscard]] MirInstructionId map(MirInstructionId old,
                                     const char *field) const {
    if (old == 0)
      return 0;
    if (const auto found = renumbered_.find(old); found != renumbered_.end())
      return found->second;
    if (removed_.contains(old))
      throw MirLoanNormalizationError(
          std::string("loan normalization refuses ") + field + " in " +
          describe(address_) + ": it refers to removed EndBorrow i" +
          std::to_string(old));
    throw MirLoanNormalizationError(std::string("loan normalization refuses ") +
                                    field + " in " + describe(address_) +
                                    ": i" + std::to_string(old) +
                                    " names no instruction of the body");
  }

  void rewrite(MirInstructionId &id, const char *field) const {
    id = map(id, field);
  }

private:
  void claim(MirInstructionId old) {
    if (old == 0 || renumbered_.contains(old) || removed_.contains(old))
      throw MirLoanNormalizationError(
          "loan normalization refuses " + describe(address_) +
          ": instruction ID i" + std::to_string(old) +
          " is zero or not unique");
  }

  MirBodyAddress address_;
  std::unordered_map<MirInstructionId, MirInstructionId> renumbered_;
  std::unordered_set<MirInstructionId> removed_;
};

using BodyKey = std::tuple<std::size_t, std::size_t>;

[[nodiscard]] BodyKey key(MirBodyAddress address) {
  return {static_cast<std::size_t>(address.kind), address.owner};
}

BodyRenumbering normalizeBody(MirBody &body, MirBodyAddress address) {
  BodyRenumbering renumbering(address);
  for (MirBlock &block : body.blocks) {
    std::vector<MirInstruction> kept;
    kept.reserve(block.instructions.size());
    for (MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::EndBorrow &&
          !instruction.tombstone) {
        requireLoanOnlyEndBorrow(instruction, address);
        renumbering.remove(instruction.id);
        continue;
      }
      renumbering.keep(instruction.id);
      kept.push_back(std::move(instruction));
    }
    block.instructions = std::move(kept);
  }

  for (MirBlock &block : body.blocks) {
    for (MirInstruction &instruction : block.instructions) {
      renumbering.rewrite(instruction.id, "MirInstruction::id");
      renumbering.rewrite(instruction.replacementPair,
                          "MirInstruction::replacementPair");
    }
    renumbering.rewrite(block.terminator.invokeInstruction,
                        "MirTerminator::invokeInstruction");
  }
  for (MirValue &value : body.values)
    renumbering.rewrite(value.definition, "MirValue::definition");
  for (std::vector<MirValueUse> &uses : body.valueUses)
    for (MirValueUse &use : uses)
      renumbering.rewrite(use.instruction, "MirValueUse::instruction");
  for (MirFailureRecord &record : body.failureRecords)
    renumbering.rewrite(record.producerInstruction,
                        "MirFailureRecord::producerInstruction");
  for (MirRangeIteration &range : body.rangeIterations) {
    if (!range.endpointParking)
      continue;
    MirRangeEndpointParking &parking = *range.endpointParking;
    renumbering.rewrite(parking.beginCall,
                        "MirRangeEndpointParking::beginCall");
    renumbering.rewrite(parking.endCall, "MirRangeEndpointParking::endCall");
    renumbering.rewrite(parking.endReceiver,
                        "MirRangeEndpointParking::endReceiver");
    renumbering.rewrite(parking.park, "MirRangeEndpointParking::park");
    renumbering.rewrite(parking.restore, "MirRangeEndpointParking::restore");
    for (MirInstructionId &restore : parking.failureRestores)
      renumbering.rewrite(restore, "MirRangeEndpointParking::failureRestores");
  }
  if (body.sourceDebug.tables) {
    MirSourceDebugTables &tables = *body.sourceDebug.tables;
    for (MirSourceLocalEnd &end : tables.localEnds)
      renumbering.rewrite(end.point.instruction,
                          "MirSourceLocalEnd::point.instruction");
    for (MirSourceValueEvent &event : tables.valueEvents)
      renumbering.rewrite(event.point.instruction,
                          "MirSourceValueEvent::point.instruction");
  }
  for (MirLoan &loan : body.loans)
    loan.semanticLoan = 0;
  return renumbering;
}

// MirProgram exposes its program-level tables only through const accessors.
// The program here is this function's own non-const value, so writing through
// those references is well defined.
template <typename Value> [[nodiscard]] Value &owned(const Value &value) {
  return const_cast<Value &>(value);
}

} // namespace

MirProgram normalizeLoanBookkeeping(MirProgram program) {
  std::map<BodyKey, BodyRenumbering> bodies;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    MirBody *body = findMirBody(program, address);
    if (body == nullptr)
      throw MirLoanNormalizationError("loan normalization cannot resolve " +
                                      describe(address));
    bodies.insert_or_assign(key(address), normalizeBody(*body, address));
  }
  const auto bodyMap = [&](MirBodyAddress address,
                           const char *field) -> const BodyRenumbering & {
    const auto found = bodies.find(key(address));
    if (found == bodies.end())
      throw MirLoanNormalizationError(
          std::string("loan normalization refuses ") + field + ": " +
          describe(address) + " is not a body of the program");
    return found->second;
  };

  for (MirThreadTaskAdapter &adapter : owned(program.threadTaskAdapters()))
    bodyMap(adapter.sourceBody, "MirThreadTaskAdapter::spawnInstruction")
        .rewrite(adapter.spawnInstruction,
                 "MirThreadTaskAdapter::spawnInstruction");
  for (MirConstructorInstance &constructor :
       owned(program.constructorInstances())) {
    if (!constructor.dispatchState)
      continue;
    const char *field = "MirConstructorDispatchState::afterBaseInitialization";
    bodyMap({.kind = MirBodyKind::Constructor, .owner = constructor.id}, field)
        .rewrite(constructor.dispatchState->afterBaseInitialization, field);
  }
  for (MirProgramInitializationStep &step :
       owned(program.programInitializationPlan()).steps) {
    const char *field = "MirProgramInitializationStep::storageInitialization";
    bodyMap({.kind = MirBodyKind::Module}, field)
        .rewrite(step.storageInitialization, field);
  }
  if (auto &plan = owned(program.hostedStartupPlan()); plan) {
    for (MirHostedStartupOperation &operation : plan->operations) {
      const char *field = "MirHostedStartupOperation::instruction";
      bodyMap({.kind = MirBodyKind::HostedStartup, .owner = plan->entry}, field)
          .rewrite(operation.instruction, field);
    }
  }
  return program;
}

namespace {

// See the header: these are the only fields the dependency mode clears.
void clearDependencySet(std::optional<MirBorrowDependencySet> &set) {
  if (set)
    *set = MirBorrowDependencySet{};
}

void clearDependencySummary(
    std::optional<FunctionBorrowDependencySummary> &summary) {
  if (summary)
    *summary = FunctionBorrowDependencySummary{};
}

[[nodiscard]] bool isContractCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct ||
         instruction.kind == MirInstructionKind::CallBody;
}

void normalizeBodyDependencies(MirBody &body, MirBodyAddress address) {
  for (MirLoan &loan : body.loans) {
    loan.parent = 0;
    loan.storedDependency = 0;
    loan.additionalDependencies.clear();
    if (loan.referenceSelection)
      loan.referenceSelection->transferredFrom = 0;
    // A call-published loan's primary dependency is chosen from the callee
    // contract; every other loan's source is the place its own Borrow names.
    if (loan.kind == MirLoanKind::CallResult ||
        loan.kind == MirLoanKind::Stored) {
      loan.source = 0;
      loan.designatedFields.clear();
      loan.conditions.clear();
    }
  }
  for (MirValue &value : body.values)
    clearDependencySet(value.borrowDependencies);
  for (MirBlock &block : body.blocks) {
    for (MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Borrow) {
        for (MirOperand &operand : instruction.operands) {
          if (operand.kind != MirOperandKind::Loan)
            continue;
          if (operand.loan != 0 && body.findLoan(operand.loan) == nullptr)
            throw MirLoanNormalizationError(
                "dependency normalization refuses Borrow i" +
                std::to_string(instruction.id) + " in " + describe(address) +
                ": its parent operand names no loan of the body");
          operand.loan = 0;
        }
      }
      if (!isContractCall(instruction))
        continue;
      instruction.borrowOrigin = BorrowOriginKind::None;
      instruction.borrowArgument = 0;
      instruction.borrowArguments.clear();
      instruction.borrowFields.clear();
      instruction.borrowAccess = AccessMode::ReadOnly;
      instruction.borrowPlace.reset();
      instruction.restoresReferenceResult = false;
      instruction.restoredBorrowFields.clear();
    }
  }
}

} // namespace

MirProgram normalizeBorrowDependencies(MirProgram program) {
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    MirBody *body = findMirBody(program, address);
    if (body == nullptr)
      throw MirLoanNormalizationError(
          "dependency normalization cannot resolve " + describe(address));
    normalizeBodyDependencies(*body, address);
  }
  for (MirFunctionInstance &function : owned(program.functionInstances())) {
    function.explicitReturnBorrowOrigin = false;
    function.returnBorrowOrigin = BorrowOriginKind::None;
    function.returnBorrowParameter = 0;
    function.returnBorrowAccess = AccessMode::ReadOnly;
    function.returnBorrowParameters.clear();
    function.returnBorrowFields.clear();
    clearDependencySummary(function.completeReturnBorrow);
    function.returnBorrowPlace.reset();
    function.restoresReferenceResult = false;
    function.restoredBorrowFields.clear();
  }
  for (MirConstructorInstance &constructor :
       owned(program.constructorInstances())) {
    constructor.borrowOrigin = BorrowOriginKind::None;
    constructor.borrowParameter = 0;
    constructor.borrowAccess = AccessMode::ReadOnly;
    constructor.borrowParameters.clear();
    clearDependencySummary(constructor.completeBorrow);
  }
  for (MirLambdaInstance &lambda : owned(program.lambdaInstances())) {
    lambda.returnBorrowOrigin = BorrowOriginKind::None;
    lambda.returnBorrowParameter = 0;
    lambda.returnBorrowAccess = AccessMode::ReadOnly;
  }
  return program;
}

} // namespace lang
