#include "mir_borrow_audits.h"

#include "mir_verification_queries.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace lang {
namespace {

// A storage path within one body. Raw provenance is tracked per path; a path
// never crosses a raw step (the step is resolved to the pointer's targets).
enum class StepKind : std::uint8_t {
  Field,
  Constant,
  Dynamic,
  Deref,
  Base,
  Any
};

struct Step {
  StepKind kind = StepKind::Field;
  std::uint64_t id = 0;

  friend bool operator==(const Step &, const Step &) = default;
  friend auto operator<=>(const Step &, const Step &) = default;
};

using Steps = std::vector<Step>;

struct Path {
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  std::size_t id = 0;
  Steps steps;

  friend auto operator<=>(const Path &, const Path &) = default;
};

// One possible raw target, with the checked loan the address was formed
// through (a pointer formed from a reference stays below that reference).
struct Origin {
  Path target;
  MirLoanId through = 0;

  friend auto operator<=>(const Origin &, const Origin &) = default;
};

using OriginSet = std::set<Origin>;
// Relative path inside a value -> raw targets stored there.
using ValueOrigins = std::map<Steps, OriginSet>;
// Storage slot -> raw targets stored there.
using StorageState = std::map<Path, OriginSet>;

// A raw-pointer formal's pointee is named by a Value-rooted path whose id is
// the formal's one-based index; ordinary storage paths never use that root.
constexpr MirPlaceRootKind formalRoot = MirPlaceRootKind::Value;

// What a function does with raw addresses it receives: the accesses through a
// formal's pointee, and the raw targets its result can carry (globals and
// formals' pointees). Caller loans are checked against them at each call.
struct RawSummary {
  std::set<std::tuple<std::size_t, Steps, AccessMode>> accesses;
  OriginSet returned;

  friend bool operator==(const RawSummary &, const RawSummary &) = default;
};
using RawSummaries = std::map<HirFunctionInstanceId, RawSummary>;

bool wildcard(const Step &step) {
  return step.kind == StepKind::Any || step.kind == StepKind::Dynamic;
}

bool stepsCompatible(const Step &left, const Step &right) {
  if (left.kind == StepKind::Any || right.kind == StepKind::Any)
    return true;
  const auto indexed = [](const Step &step) {
    return step.kind == StepKind::Constant || step.kind == StepKind::Dynamic;
  };
  if (indexed(left) && indexed(right))
    return left.kind == StepKind::Dynamic || right.kind == StepKind::Dynamic ||
           left.id == right.id;
  return left == right;
}

bool exact(const Path &path) {
  return std::none_of(path.steps.begin(), path.steps.end(), wildcard);
}

// Whether two storage paths can designate overlapping storage.
bool overlaps(const Path &left, const Path &right) {
  if (left.root != right.root || left.id != right.id)
    return false;
  const auto common = std::min(left.steps.size(), right.steps.size());
  for (std::size_t index = 0; index < common; ++index)
    if (!stepsCompatible(left.steps[index], right.steps[index]))
      return false;
  return true;
}

void unite(OriginSet &into, const OriginSet &from, bool &changed) {
  for (const auto &origin : from)
    changed |= into.insert(origin).second;
}

class BodyAudit {
public:
  BodyAudit(const MirProgram &program, const MirBody &body,
            MirBodyAddress address, const RawSummaries &summaries)
      : program(program), body(body), address(address), summaries(summaries) {
    for (const auto &loan : body.loans)
      for (const auto carrier : loan.carriers)
        if (!loan.entry && loan.kind != MirLoanKind::Stored &&
            loan.source != 0 && !carriedLoan.contains(carrier))
          carriedLoan.emplace(carrier, loan.id);
  }

  std::vector<MirBorrowAuditFinding> run() {
    const std::size_t blocks = body.blocks.size();
    entryStates.assign(blocks, {});
    entryLoans.assign(blocks, std::vector<char>(body.loans.size(), 0));
    reached.assign(blocks, false);
    if (body.findBlock(body.entry) == nullptr)
      return {};
    reached[body.entry - 1] = true;
    for (const auto &loan : body.loans)
      if (loan.entry)
        entryLoans[body.entry - 1][loan.id - 1] = 1;
    if (address.kind == MirBodyKind::Function)
      if (const auto *function = program.findFunctionInstance(address.owner))
        for (std::size_t index = 0; index < function->parameterBindings.size();
             ++index)
          if (index < function->parameterTypes.size() &&
              function->parameterTypes[index].kind == SemanticType::RawPointer)
            entryStates[body.entry -
                        1][{.root = MirPlaceRootKind::Binding,
                            .id = function->parameterBindings[index]}]
                .insert({.target = {.root = formalRoot, .id = index + 1}});
    constexpr int maximumRounds = 64;
    for (int round = 0; round < maximumRounds; ++round) {
      changed = false;
      for (const auto &block : body.blocks)
        if (block.reachable && reached[block.id - 1])
          runBlock(block, false);
      if (!changed)
        break;
    }
    for (const auto &block : body.blocks)
      if (block.reachable && reached[block.id - 1])
        runBlock(block, true);
    return std::move(findings);
  }

  RawSummary summary;

private:
  const MirProgram &program;
  const MirBody &body;
  MirBodyAddress address;
  const RawSummaries &summaries;
  std::map<HirBindingId, MirLoanId> carriedLoan;
  std::vector<StorageState> entryStates;
  std::vector<std::vector<char>> entryLoans;
  std::vector<bool> reached;
  std::map<MirValueId, ValueOrigins> values;
  std::vector<MirBorrowAuditFinding> findings;
  bool changed = false;

  struct Resolved {
    std::vector<Origin> paths;
    bool raw = false;
  };

  void setValue(MirValueId value, const ValueOrigins &origins) {
    if (value == 0 || origins.empty())
      return;
    auto &current = values[value];
    for (const auto &[steps, set] : origins)
      unite(current[steps], set, changed);
  }

  const ValueOrigins *valueOrigins(MirValueId value) const {
    const auto found = values.find(value);
    return found == values.end() ? nullptr : &found->second;
  }

  static OriginSet lookupRelative(const ValueOrigins &origins,
                                  const Steps &relative) {
    OriginSet result;
    for (const auto &[steps, set] : origins) {
      if (steps.size() != relative.size())
        continue;
      bool match = true;
      for (std::size_t index = 0; match && index < steps.size(); ++index)
        match = stepsCompatible(steps[index], relative[index]);
      if (match)
        result.insert(set.begin(), set.end());
    }
    return result;
  }

  // Every raw target stored at or below `slot`, keyed relative to it.
  static void collect(const StorageState &state, const Path &slot,
                      ValueOrigins &into) {
    for (const auto &[stored, set] : state) {
      if (stored.root != slot.root || stored.id != slot.id ||
          stored.steps.size() < slot.steps.size())
        continue;
      bool match = true;
      for (std::size_t index = 0; match && index < slot.steps.size(); ++index)
        match = stepsCompatible(stored.steps[index], slot.steps[index]);
      if (!match)
        continue;
      Steps relative(stored.steps.begin() +
                         static_cast<std::ptrdiff_t>(slot.steps.size()),
                     stored.steps.end());
      into[relative].insert(set.begin(), set.end());
    }
  }

  // A reference binding's referent is the source its carried loan borrows.
  void canonicalize(Origin &origin, std::size_t depth) const {
    if (origin.target.root != MirPlaceRootKind::Binding ||
        origin.target.steps.empty() ||
        origin.target.steps.front().kind != StepKind::Deref || depth > 8)
      return;
    const auto found = carriedLoan.find(origin.target.id);
    if (found == carriedLoan.end())
      return;
    const MirLoan *loan = body.findLoan(found->second);
    if (loan == nullptr)
      return;
    const auto source = resolve(loan->source, nullptr, depth + 1);
    if (source.raw || source.paths.size() != 1)
      return;
    Steps rest(origin.target.steps.begin() + 1, origin.target.steps.end());
    origin.target = source.paths.front().target;
    origin.target.steps.insert(origin.target.steps.end(), rest.begin(),
                               rest.end());
    origin.through = loan->id;
  }

  // Resolves a MIR place to the storage it designates. Raw steps follow the
  // provenance recorded in `state` (or on the root value) at this point.
  Resolved resolve(MirPlaceId id, const StorageState *state,
                   std::size_t depth = 0) const {
    Resolved result;
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr || depth > 16)
      return result;
    std::vector<Origin> base;
    // Pointer contents of the root value, until the first raw step.
    const ValueOrigins *rootValue = nullptr;
    Steps relative;
    switch (place->root) {
    case MirPlaceRootKind::Binding:
      base.push_back({.target = {.root = place->root, .id = place->binding}});
      break;
    case MirPlaceRootKind::Symbol:
      base.push_back({.target = {.root = place->root, .id = place->symbol}});
      break;
    case MirPlaceRootKind::This:
      base.push_back({.target = {.root = place->root}});
      break;
    case MirPlaceRootKind::Temporary:
      base.push_back({.target = {.root = place->root, .id = place->temporary}});
      break;
    case MirPlaceRootKind::Loan:
      if (const MirLoan *loan = body.findLoan(place->loan)) {
        auto source = resolve(loan->source, nullptr, depth + 1);
        for (auto &origin : source.paths) {
          origin.through = loan->id;
          base.push_back(origin);
        }
      }
      break;
    case MirPlaceRootKind::Value: {
      rootValue = valueOrigins(place->value);
      const MirInstruction *definition =
          mir_detail::definitionFor(body, place->value);
      if (definition != nullptr && definition->operands.size() == 1 &&
          definition->operands.front().place != 0 &&
          (definition->kind == MirInstructionKind::Load ||
           (definition->kind == MirInstructionKind::Compute &&
            definition->operation == MirOperation::PayloadExtract)))
        base =
            resolve(definition->operands.front().place, state, depth + 1).paths;
      break;
    }
    }
    for (const auto &projection : place->projections) {
      Step step;
      switch (projection.kind) {
      case MirProjectionKind::Field:
        step = {StepKind::Field, projection.field};
        break;
      case MirProjectionKind::Index:
        step = projection.constantIndex
                   ? Step{StepKind::Constant, *projection.constantIndex}
                   : Step{StepKind::Dynamic, 0};
        break;
      case MirProjectionKind::PackElement:
        step = {StepKind::Constant, projection.constantIndex.value_or(0)};
        break;
      case MirProjectionKind::Base:
        step = {StepKind::Base, projection.selection};
        break;
      case MirProjectionKind::Dereference:
        step = {StepKind::Deref, 0};
        break;
      case MirProjectionKind::RawDereference:
      case MirProjectionKind::RawIndex: {
        OriginSet targets;
        if (rootValue != nullptr && !result.raw)
          targets = lookupRelative(*rootValue, relative);
        else if (state != nullptr)
          for (const auto &slot : base) {
            ValueOrigins stored;
            collect(*state, slot.target, stored);
            if (const auto found = stored.find(Steps{}); found != stored.end())
              targets.insert(found->second.begin(), found->second.end());
          }
        base.assign(targets.begin(), targets.end());
        if (projection.kind == MirProjectionKind::RawIndex &&
            !(projection.constantIndex && *projection.constantIndex == 0))
          for (auto &origin : base)
            if (!origin.target.steps.empty() &&
                (origin.target.steps.back().kind == StepKind::Constant ||
                 origin.target.steps.back().kind == StepKind::Dynamic))
              origin.target.steps.back() = {StepKind::Dynamic, 0};
        rootValue = nullptr;
        result.raw = true;
        continue;
      }
      }
      relative.push_back(step);
      for (auto &origin : base) {
        origin.target.steps.push_back(step);
        if (step.kind == StepKind::Deref)
          canonicalize(origin, depth);
      }
    }
    result.paths = std::move(base);
    return result;
  }

  ValueOrigins operandOrigins(const MirOperand &operand,
                              const StorageState &state) const {
    ValueOrigins result;
    if (operand.kind == MirOperandKind::Value) {
      if (const auto *found = valueOrigins(operand.value))
        result = *found;
    } else if ((operand.kind == MirOperandKind::Copy ||
                operand.kind == MirOperandKind::Move) &&
               operand.place != 0) {
      for (const auto &slot : resolve(operand.place, &state).paths)
        collect(state, slot.target, result);
    }
    return result;
  }

  void store(MirPlaceId destination, const ValueOrigins &origins,
             StorageState &state) const {
    const auto resolved = resolve(destination, &state);
    if (resolved.paths.size() == 1 && exact(resolved.paths.front().target)) {
      const Path &slot = resolved.paths.front().target;
      std::erase_if(state, [&](const auto &entry) {
        const Path &stored = entry.first;
        return stored.root == slot.root && stored.id == slot.id &&
               stored.steps.size() >= slot.steps.size() &&
               std::equal(slot.steps.begin(), slot.steps.end(),
                          stored.steps.begin());
      });
    }
    for (const auto &slot : resolved.paths)
      for (const auto &[relative, set] : origins) {
        Path target = slot.target;
        target.steps.insert(target.steps.end(), relative.begin(),
                            relative.end());
        state[target].insert(set.begin(), set.end());
      }
  }

  static ValueOrigins nested(const ValueOrigins &origins) {
    ValueOrigins result;
    for (const auto &[relative, set] : origins) {
      Steps steps{{StepKind::Any, 0}};
      steps.insert(steps.end(), relative.begin(), relative.end());
      result[steps].insert(set.begin(), set.end());
    }
    return result;
  }

  bool ancestorOrSelf(MirLoanId candidate, MirLoanId loan) const {
    for (std::size_t depth = 0; loan != 0 && depth <= body.loans.size();
         ++depth) {
      if (candidate == loan)
        return true;
      const MirLoan *found = body.findLoan(loan);
      loan = found ? found->parent : 0;
    }
    return false;
  }

  void checkAccess(const MirBlock &block, const MirInstruction &instruction,
                   MirPlaceId placeId, AccessMode access,
                   const StorageState &state, const std::vector<char> &live) {
    const MirPlace *place = body.findPlace(placeId);
    if (place == nullptr ||
        std::none_of(place->projections.begin(), place->projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind ==
                                  MirProjectionKind::RawDereference ||
                              projection.kind == MirProjectionKind::RawIndex;
                     }))
      return;
    checkTargets(block, instruction, resolve(placeId, &state).paths, access,
                 live);
  }

  void checkTargets(const MirBlock &block, const MirInstruction &instruction,
                    const std::vector<Origin> &targets, AccessMode access,
                    const std::vector<char> &live) {
    std::vector<MirLoanId> conflicts;
    for (const auto &origin : targets) {
      if (origin.target.root == formalRoot) {
        summary.accesses.insert(
            {origin.target.id - 1, origin.target.steps, access});
        continue;
      }
      for (const auto &loan : body.loans) {
        if (!live[loan.id - 1] || loan.kind == MirLoanKind::Stored ||
            ancestorOrSelf(loan.id, origin.through))
          continue;
        const auto conflictsWith = [&](MirPlaceId source,
                                       AccessMode loanAccess) {
          if (source == 0 || (loanAccess == AccessMode::ReadOnly &&
                              access == AccessMode::ReadOnly))
            return false;
          const auto protectedPlace = resolve(source, nullptr);
          return !protectedPlace.raw &&
                 std::any_of(protectedPlace.paths.begin(),
                             protectedPlace.paths.end(), [&](const auto &p) {
                               return overlaps(p.target, origin.target);
                             });
        };
        bool conflict = conflictsWith(loan.source, loan.access);
        for (const auto &dependency : loan.additionalDependencies)
          conflict |= conflictsWith(dependency.source, dependency.access);
        if (conflict && std::find(conflicts.begin(), conflicts.end(),
                                  loan.id) == conflicts.end())
          conflicts.push_back(loan.id);
      }
    }
    if (conflicts.empty())
      return;
    const auto existing =
        std::find_if(findings.begin(), findings.end(), [&](const auto &found) {
          return found.block == block.id && found.instruction == instruction.id;
        });
    if (existing != findings.end()) {
      for (const auto loan : conflicts)
        if (std::find(existing->loans.begin(), existing->loans.end(), loan) ==
            existing->loans.end())
          existing->loans.push_back(loan);
      return;
    }
    findings.push_back(
        {.body = address,
         .block = block.id,
         .instruction = instruction.id,
         .hirValue = instruction.hirValue,
         .hirStatement = instruction.hirStatement,
         .callSite = instruction.callSite,
         .loans = std::move(conflicts),
         .detail = access == AccessMode::Mutable ? "write" : "read"});
  }

  void audit(const MirBlock &block, const MirInstruction &instruction,
             const StorageState &state, const std::vector<char> &live) {
    // Forming an address (`&pointer[1]`) performs no access.
    if (instruction.kind == MirInstructionKind::Compute &&
        instruction.operation == MirOperation::AddressOf)
      return;
    if (instruction.destination &&
        instruction.kind != MirInstructionKind::Drop &&
        instruction.kind != MirInstructionKind::Lifecycle)
      checkAccess(block, instruction, *instruction.destination,
                  AccessMode::Mutable, state, live);
    const auto operandAccess = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::BorrowWrite ||
          operand.kind == MirOperandKind::Move)
        return AccessMode::Mutable;
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.callInputKind == HirCallInputKind::MutableBorrow)
        return AccessMode::Mutable;
      return AccessMode::ReadOnly;
    };
    if (instruction.receiver && instruction.receiver->place != 0)
      checkAccess(block, instruction, instruction.receiver->place,
                  operandAccess(*instruction.receiver), state, live);
    for (const auto &operand : instruction.operands)
      if (operand.place != 0)
        checkAccess(block, instruction, operand.place, operandAccess(operand),
                    state, live);
    if (const RawSummary *callee = calleeSummary(instruction))
      for (const auto &[formal, steps, access] : callee->accesses) {
        if (formal >= instruction.operands.size())
          continue;
        std::vector<Origin> targets;
        for (const auto &origin : lookupRelative(
                 operandOrigins(instruction.operands[formal], state), {})) {
          Origin target = origin;
          target.target.steps.insert(target.target.steps.end(), steps.begin(),
                                     steps.end());
          targets.push_back(std::move(target));
        }
        checkTargets(block, instruction, targets, access, live);
      }
  }

  const RawSummary *calleeSummary(const MirInstruction &instruction) const {
    if (instruction.kind != MirInstructionKind::Call ||
        !instruction.functionTarget || instruction.receiver)
      return nullptr;
    const auto found = summaries.find(*instruction.functionTarget);
    return found == summaries.end() ? nullptr : &found->second;
  }

  void transfer(const MirInstruction &instruction, StorageState &state) {
    const auto first = [&]() -> ValueOrigins {
      return instruction.operands.empty()
                 ? ValueOrigins{}
                 : operandOrigins(instruction.operands.front(), state);
    };
    switch (instruction.kind) {
    case MirInstructionKind::Compute:
      if (!instruction.result)
        return;
      switch (instruction.operation) {
      case MirOperation::AddressOf: {
        if (instruction.operands.size() != 1)
          return;
        ValueOrigins origins;
        for (const auto &origin :
             resolve(instruction.operands.front().place, &state).paths)
          origins[{}].insert(origin);
        setValue(*instruction.result, origins);
        return;
      }
      case MirOperation::Convert:
      case MirOperation::Identity:
        setValue(*instruction.result, first());
        return;
      case MirOperation::PointerAdd:
      case MirOperation::PointerSubtract: {
        ValueOrigins origins = first();
        ValueOrigins widened;
        for (const auto &[relative, set] : origins)
          for (auto origin : set) {
            if (!origin.target.steps.empty() &&
                (origin.target.steps.back().kind == StepKind::Constant ||
                 origin.target.steps.back().kind == StepKind::Dynamic))
              origin.target.steps.back() = {StepKind::Dynamic, 0};
            widened[relative].insert(origin);
          }
        setValue(*instruction.result, widened);
        return;
      }
      case MirOperation::Aggregate:
      case MirOperation::PayloadConstruct:
        for (const auto &operand : instruction.operands)
          setValue(*instruction.result, nested(operandOrigins(operand, state)));
        return;
      case MirOperation::Comma:
        if (!instruction.operands.empty())
          setValue(*instruction.result,
                   operandOrigins(instruction.operands.back(), state));
        return;
      default:
        return;
      }
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Load:
      if (instruction.result) {
        if (instruction.kind == MirInstructionKind::Load ||
            (!instruction.operands.empty() &&
             instruction.operands.front().kind == MirOperandKind::Value))
          setValue(
              *instruction.result,
              instruction.kind == MirInstructionKind::Load &&
                      !instruction.operands.empty() &&
                      instruction.operands.front().place != 0
                  ? [&] {
                      ValueOrigins loaded;
                      for (const auto &slot :
                           resolve(instruction.operands.front().place, &state)
                               .paths)
                        collect(state, slot.target, loaded);
                      return loaded;
                    }()
                  : first());
      }
      return;
    case MirInstructionKind::Move:
      if (instruction.result)
        setValue(*instruction.result, first());
      if (instruction.destination)
        store(*instruction.destination, first(), state);
      return;
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
      if (instruction.destination)
        store(*instruction.destination, first(), state);
      return;
    case MirInstructionKind::Call:
      if (instruction.result)
        if (const RawSummary *callee = calleeSummary(instruction)) {
          ValueOrigins result;
          for (const auto &origin : callee->returned) {
            if (origin.target.root != formalRoot) {
              result[{}].insert(origin);
              continue;
            }
            const std::size_t formal = origin.target.id - 1;
            if (formal >= instruction.operands.size())
              continue;
            for (auto relayed : lookupRelative(
                     operandOrigins(instruction.operands[formal], state), {})) {
              relayed.target.steps.insert(relayed.target.steps.end(),
                                          origin.target.steps.begin(),
                                          origin.target.steps.end());
              result[{}].insert(relayed);
            }
          }
          setValue(*instruction.result, result);
        }
      return;
    case MirInstructionKind::Construct: {
      ValueOrigins origins;
      for (const auto &operand : instruction.operands)
        for (const auto &[relative, set] :
             nested(operandOrigins(operand, state)))
          origins[relative].insert(set.begin(), set.end());
      if (instruction.result)
        setValue(*instruction.result, origins);
      if (instruction.destination)
        store(*instruction.destination, origins, state);
      return;
    }
    default:
      return;
    }
  }

  void transferLoans(const MirInstruction &instruction,
                     std::vector<char> &live) const {
    if (!instruction.loan || *instruction.loan == 0 ||
        *instruction.loan > body.loans.size())
      return;
    if (instruction.kind == MirInstructionKind::EndBorrow)
      live[*instruction.loan - 1] = 0;
    else if (instruction.kind != MirInstructionKind::ActivateBorrow &&
             instruction.kind != MirInstructionKind::ParkRangeCursor &&
             instruction.kind != MirInstructionKind::RestoreRangeCursor)
      live[*instruction.loan - 1] = 1;
  }

  void flow(MirBlockId target, const std::vector<MirOperand> &arguments,
            const StorageState &state, const std::vector<char> &live) {
    const MirBlock *successor = body.findBlock(target);
    if (successor == nullptr)
      return;
    for (std::size_t index = 0;
         index < arguments.size() && index < successor->parameters.size();
         ++index)
      setValue(successor->parameters[index].value,
               operandOrigins(arguments[index], state));
    const std::size_t slot = target - 1;
    if (!reached[slot]) {
      reached[slot] = true;
      changed = true;
    }
    for (const auto &[path, set] : state)
      unite(entryStates[slot][path], set, changed);
    for (std::size_t index = 0; index < live.size(); ++index)
      if (live[index] && !entryLoans[slot][index]) {
        entryLoans[slot][index] = 1;
        changed = true;
      }
  }

  void runBlock(const MirBlock &block, bool report) {
    StorageState state = entryStates[block.id - 1];
    std::vector<char> live = entryLoans[block.id - 1];
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      if (report)
        audit(block, instruction, state, live);
      transfer(instruction, state);
      transferLoans(instruction, live);
    }
    const auto &terminator = block.terminator;
    if (report) {
      if (terminator.kind == MirTerminatorKind::Return && terminator.value)
        for (const auto &origin :
             lookupRelative(operandOrigins(*terminator.value, state), {}))
          if (origin.target.root == formalRoot ||
              origin.target.root == MirPlaceRootKind::Symbol)
            summary.returned.insert({.target = origin.target});
      return;
    }
    if (terminator.target != 0)
      flow(terminator.target, terminator.targetArguments, state, live);
    if (terminator.elseTarget != 0)
      flow(terminator.elseTarget, terminator.elseArguments, state, live);
    for (const auto &target : terminator.switchTargets)
      flow(target.target, target.arguments, state, live);
  }
};

bool hasRawPlace(const MirBody &body) {
  return std::any_of(
      body.places.begin(), body.places.end(), [](const MirPlace &place) {
        return std::any_of(
            place.projections.begin(), place.projections.end(),
            [](const MirPlaceProjection &projection) {
              return projection.kind == MirProjectionKind::RawDereference ||
                     projection.kind == MirProjectionKind::RawIndex;
            });
      });
}

} // namespace

std::vector<MirBorrowAuditFinding>
auditMirRawAccesses(const MirProgram &program) {
  // Function summaries are a monotone fixed point over the call graph
  // (recursion included); findings come from the final round.
  RawSummaries summaries;
  std::vector<MirBorrowAuditFinding> findings;
  constexpr int maximumRounds = 16;
  for (int round = 0; round < maximumRounds; ++round) {
    findings.clear();
    RawSummaries next;
    for (const auto address : enumerateMirBodyAddresses(program)) {
      const MirBody *body = findMirBody(program, address);
      if (body == nullptr)
        continue;
      const bool raw = hasRawPlace(*body);
      const bool calls = std::any_of(
          body->blocks.begin(), body->blocks.end(), [&](const MirBlock &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const MirInstruction &instruction) {
                  return instruction.functionTarget &&
                         summaries.contains(*instruction.functionTarget);
                });
          });
      const auto *function = address.kind == MirBodyKind::Function
                                 ? program.findFunctionInstance(address.owner)
                                 : nullptr;
      const bool formals =
          function != nullptr &&
          (function->returnType.kind == SemanticType::RawPointer ||
           std::any_of(function->parameterTypes.begin(),
                       function->parameterTypes.end(),
                       [](const SemanticType &type) {
                         return type.kind == SemanticType::RawPointer;
                       }));
      if (!raw && !calls && !formals)
        continue;
      BodyAudit audit(program, *body, address, summaries);
      auto found = audit.run();
      findings.insert(findings.end(), std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
      if (address.kind == MirBodyKind::Function &&
          (!audit.summary.accesses.empty() || !audit.summary.returned.empty()))
        next[address.owner] = std::move(audit.summary);
    }
    if (next == summaries)
      break;
    summaries = std::move(next);
  }
  return findings;
}

} // namespace lang
