#include "mir_carrier_flow.h"
#include "gti/optimization/dataflow.h"

#include <algorithm>

namespace lang {
namespace {
using Fact = MirCarriedLoans;
using Root = std::pair<MirPlaceRootKind, std::size_t>;
struct State {
  std::map<Root, Fact> roots;
  std::map<MirValueId, Fact> values;
};
Fact empty() { return std::vector<MirLoanId>{}; }

bool mayHoldDependencies(const SemanticType &type,
                         const SemanticTypeTraits &traits) {
  return traits.containsBorrowedState || type.kind == SemanticType::Unknown ||
         type.kind == SemanticType::TypeParameter ||
         type.kind == SemanticType::Reference ||
         type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString ||
         type.kind == SemanticType::Class || type.kind == SemanticType::Enum ||
         type.kind == SemanticType::Array || type.kind == SemanticType::Lambda;
}

bool join(Fact &into, const Fact &from) {
  if (!into)
    return false;
  if (!from) {
    into.reset();
    return true;
  }
  const auto previous = *into;
  into->insert(into->end(), from->begin(), from->end());
  std::sort(into->begin(), into->end());
  into->erase(std::unique(into->begin(), into->end()), into->end());
  return *into != previous;
}
template <class Key>
bool mergeMap(std::map<Key, Fact> &into, const std::map<Key, Fact> &from) {
  bool changed = false;
  for (const auto &[key, value] : from) {
    const auto [position, inserted] = into.emplace(key, value);
    changed = inserted || join(position->second, value) || changed;
  }
  return changed;
}
bool join(State &into, const State &from) {
  // Absence on an executable predecessor is unknown, not an empty set.
  // In particular a store on only one branch must not manufacture definite
  // provenance at the join. SSA definitions are overwritten by their own
  // instructions; block arguments have already been bound on the edge.
  const auto mergePaths = [](auto &target, const auto &incoming) {
    bool changed = false;
    for (auto &[key, value] : target)
      if (!incoming.contains(key) && value) {
        value.reset();
        changed = true;
      }
    for (const auto &[key, value] : incoming) {
      const auto [found, inserted] = target.emplace(key, Fact{});
      changed = inserted || join(found->second, value) || changed;
    }
    return changed;
  };
  const bool roots = mergePaths(into.roots, from.roots);
  return mergePaths(into.values, from.values) || roots;
}

struct Storage {
  std::vector<Root> roots;
  bool exact = false;
};
std::optional<Storage> storage(const MirBody &body, const State &state,
                               MirPlaceId id, std::size_t depth = 0) {
  const auto *place = body.findPlace(id);
  if (!place || depth > body.loans.size() || place->capture)
    return std::nullopt;
  if (place->root == MirPlaceRootKind::Loan) {
    const auto *loan = body.findLoan(place->loan);
    auto result =
        loan ? storage(body, state, loan->source, depth + 1) : std::nullopt;
    if (!result)
      return std::nullopt;
    for (const auto &dependency : loan->additionalDependencies) {
      const auto other = storage(body, state, dependency.source, depth + 1);
      if (!other)
        return std::nullopt;
      result->roots.insert(result->roots.end(), other->roots.begin(),
                           other->roots.end());
    }
    std::sort(result->roots.begin(), result->roots.end());
    result->roots.erase(std::unique(result->roots.begin(), result->roots.end()),
                        result->roots.end());
    result->exact = false; // Every candidate of an aliased store widens.
    return result;
  }
  if (place->root == MirPlaceRootKind::Binding && !place->projections.empty() &&
      place->projections.front().kind == MirProjectionKind::Dereference) {
    // A reference binding's current value selects the physical source, not
    // its declaration's historical carrier list. Multi-source aliases retain
    // every candidate; later projections cannot make this a strong update.
    const auto held = state.roots.find({place->root, place->binding});
    if (held == state.roots.end() || !held->second || held->second->empty() ||
        std::any_of(place->projections.begin() + 1, place->projections.end(),
                    [](const auto &part) {
                      return part.kind == MirProjectionKind::Dereference ||
                             part.kind == MirProjectionKind::RawDereference ||
                             part.kind == MirProjectionKind::RawIndex;
                    }))
      return std::nullopt;
    Storage result;
    for (const auto id : *held->second) {
      const auto *loan = body.findLoan(id);
      if (!loan)
        return std::nullopt;
      std::vector<MirPlaceId> sources{loan->source};
      for (const auto &dependency : loan->additionalDependencies)
        sources.push_back(dependency.source);
      for (const auto source : sources) {
        const auto candidate = storage(body, state, source, depth + 1);
        if (!candidate)
          return std::nullopt;
        result.roots.insert(result.roots.end(), candidate->roots.begin(),
                            candidate->roots.end());
      }
    }
    std::sort(result.roots.begin(), result.roots.end());
    result.roots.erase(std::unique(result.roots.begin(), result.roots.end()),
                       result.roots.end());
    return result;
  }
  if (std::any_of(place->projections.begin(), place->projections.end(),
                  [](const auto &part) {
                    return part.kind == MirProjectionKind::Dereference ||
                           part.kind == MirProjectionKind::RawDereference ||
                           part.kind == MirProjectionKind::RawIndex;
                  }))
    return std::nullopt;
  if (place->root == MirPlaceRootKind::Binding)
    return Storage{{{place->root, place->binding}}, place->projections.empty()};
  if (place->root == MirPlaceRootKind::Temporary)
    return Storage{{{place->root, place->temporary}},
                   place->projections.empty()};
  return std::nullopt;
}
Fact placeFact(const MirBody &body, const State &state, MirPlaceId id) {
  const auto *place = body.findPlace(id);
  if (!place)
    return std::nullopt;
  if (const auto key = storage(body, state, id)) {
    Fact held = empty();
    bool known = true;
    for (const auto &root : key->roots) {
      const auto found = state.roots.find(root);
      if (found == state.roots.end()) {
        known = false;
        break;
      }
      join(held, found->second);
    }
    if (known)
      return held;
    if (key->roots.size() > 1)
      return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Value) {
    const auto found = state.values.find(place->value);
    if (found != state.values.end())
      return found->second;
  }
  if (std::any_of(place->projections.begin(), place->projections.end(),
                  [](const auto &part) {
                    return part.kind == MirProjectionKind::Dereference ||
                           part.kind == MirProjectionKind::RawDereference ||
                           part.kind == MirProjectionKind::RawIndex;
                  }))
    return std::nullopt;
  return mayHoldDependencies(place->type, place->traits) ? Fact{} : empty();
}
Fact operandFact(const MirBody &body, const State &state,
                 const MirOperand &operand) {
  if (operand.kind == MirOperandKind::Constant)
    return empty();
  if (operand.kind == MirOperandKind::Loan)
    return body.findLoan(operand.loan)
               ? Fact{std::vector<MirLoanId>{operand.loan}}
               : Fact{};
  if (operand.kind == MirOperandKind::Value) {
    const auto found = state.values.find(operand.value);
    return found != state.values.end() ? found->second : Fact{};
  }
  return placeFact(body, state, operand.place);
}
Fact operandRead(const MirBody &body, const State &state,
                 const MirOperand &operand) {
  Fact read = operandFact(body, state, operand);
  // Accessing a referent uses its physical loan even when its value is an
  // independent scalar. Apply the same rule to arguments and receivers.
  if (const auto *place = body.findPlace(operand.place);
      place && place->root == MirPlaceRootKind::Loan)
    join(read, Fact{std::vector<MirLoanId>{place->loan}});
  else if (place && place->root == MirPlaceRootKind::Binding &&
           !place->projections.empty() &&
           place->projections.front().kind == MirProjectionKind::Dereference) {
    const auto held = state.roots.find({place->root, place->binding});
    join(read, held == state.roots.end() ? Fact{} : held->second);
  }
  return read;
}
void record(std::map<MirInstructionId, Fact> &facts, MirInstructionId id,
            const Fact &value) {
  const auto [found, inserted] = facts.emplace(id, value);
  if (!inserted)
    join(found->second, value);
}
} // namespace

MirCarrierFlow deriveMirCarrierFlow(const MirBody &body) {
  MirCarrierFlow result;
  if (!body.findBlock(body.entry)) {
    result.complete = false;
    return result;
  }
  State initial;
  for (const auto &loan : body.loans)
    if (loan.entry)
      for (const auto carrier : loan.carriers) {
        const Root root{MirPlaceRootKind::Binding, carrier};
        auto [found, inserted] = initial.roots.emplace(root, empty());
        (void)inserted;
        join(found->second, Fact{std::vector<MirLoanId>{loan.id}});
      }
  const auto transfer = [&](const MirBlock &current, State state) {
    const auto *block = &current;
    const auto id = current.id;
    std::optional<State> beforeInvoke;
    const MirInstruction *invoked = nullptr;
    for (const auto &instruction : block->instructions) {
      if (instruction.tombstone)
        continue;
      if (block->terminator.kind == MirTerminatorKind::Invoke &&
          block->terminator.invokeInstruction == instruction.id) {
        beforeInvoke = state;
        invoked = &instruction;
      }
      Fact producedInputs = empty();
      Fact read = empty();
      for (const auto &operand : instruction.operands) {
        const Fact held = operandFact(body, state, operand);
        join(producedInputs, held);
        join(read, operandRead(body, state, operand));
      }
      if (instruction.receiver)
        join(read, operandRead(body, state, *instruction.receiver));
      if (instruction.destination &&
          (instruction.kind == MirInstructionKind::Assign ||
           instruction.kind == MirInstructionKind::Initialize)) {
        const auto *destination = body.findPlace(*instruction.destination);
        if (destination &&
            (destination->root == MirPlaceRootKind::Loan ||
             std::any_of(destination->projections.begin(),
                         destination->projections.end(), [](const auto &part) {
                           return part.kind == MirProjectionKind::Dereference;
                         })))
          // A write through an alias must keep the access loan alive too.
          // Exact replacement of a direct local does not read its old value.
          join(read, operandRead(body, state,
                                 {.kind = MirOperandKind::Copy,
                                  .place = *instruction.destination}));
      }
      if (instruction.destination &&
          (instruction.kind == MirInstructionKind::Drop ||
           instruction.kind == MirInstructionKind::CleanupBeforeAssign))
        join(read, operandRead(body, state,
                               {.kind = MirOperandKind::Copy,
                                .place = *instruction.destination}));
      if (instruction.loan &&
          (instruction.kind == MirInstructionKind::ActivateBorrow ||
           instruction.kind == MirInstructionKind::ParkRangeCursor ||
           instruction.kind == MirInstructionKind::RestoreRangeCursor))
        join(read, operandFact(body, state,
                               {.kind = MirOperandKind::Loan,
                                .loan = *instruction.loan}));
      record(result.reads, instruction.id, read);
      result.complete = result.complete && read.has_value();
      Fact produced = producedInputs;
      if (instruction.rawMemoryAccess ||
          instruction.kind == MirInstructionKind::CallBody ||
          instruction.kind == MirInstructionKind::InitializeBase ||
          instruction.kind == MirInstructionKind::AllocateObject ||
          instruction.kind == MirInstructionKind::Modify ||
          instruction.kind == MirInstructionKind::EnsureThreadLocal ||
          instruction.kind == MirInstructionKind::DrainThreadLocals ||
          instruction.kind == MirInstructionKind::BeginThreadLocals ||
          instruction.kind == MirInstructionKind::RetireProgramStorage)
        result.complete = false;
      if (instruction.loan &&
          (instruction.kind == MirInstructionKind::Borrow ||
           instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct))
        produced = body.findLoan(*instruction.loan)
                       ? Fact{std::vector<MirLoanId>{*instruction.loan}}
                       : Fact{};
      if (instruction.kind == MirInstructionKind::Call && !instruction.loan &&
          instruction.info.traits.containsBorrowedState)
        produced.reset();
      if (!produced)
        result.complete = false;
      // A call can relay or retain dependencies through receiver/arguments.
      // Until its checked store-effect summary participates in this flow,
      // do not use a caller's partial facts to shorten any loan.
      if (instruction.kind == MirInstructionKind::Call &&
          (!read || !read->empty()))
        result.complete = false;
      // Reading a borrow to compute an integer/bool uses the loan but does
      // not make the independent scalar result carry that loan.
      if (instruction.result && !instruction.loan &&
          !mayHoldDependencies(instruction.info.type, instruction.info.traits))
        produced = empty();
      if (instruction.result)
        state.values[*instruction.result] = produced;
      if (instruction.kind == MirInstructionKind::Initialize ||
          instruction.kind == MirInstructionKind::Assign) {
        const auto destination =
            instruction.destination
                ? storage(body, state, *instruction.destination)
                : std::nullopt;
        if (!destination) {
          result.complete = false;
        } else {
          Fact retained = empty();
          for (const auto &root : destination->roots) {
            auto [found, inserted] = state.roots.emplace(
                root, destination->exact ? empty() : Fact{});
            (void)inserted;
            if (destination->exact)
              found->second = producedInputs;
            else
              join(found->second, producedInputs);
            join(retained, found->second);
          }
          result.complete = result.complete && retained.has_value();
          record(result.stores, instruction.id, retained);
        }
      }
      if (instruction.kind == MirInstructionKind::Drop &&
          instruction.destination)
        if (const auto destination =
                storage(body, state, *instruction.destination);
            destination && destination->exact)
          for (const auto &root : destination->roots)
            state.roots[root] = empty();
    }
    if (block->terminator.kind == MirTerminatorKind::Invoke && !beforeInvoke)
      result.complete = false;
    Fact exitRead = empty();
    if (block->terminator.value)
      join(exitRead, operandRead(body, state, *block->terminator.value));
    if (block->terminator.returnLoan) {
      if (!body.findLoan(*block->terminator.returnLoan))
        exitRead.reset();
      else
        join(exitRead,
             Fact{std::vector<MirLoanId>{*block->terminator.returnLoan}});
    }
    result.exits[id] = exitRead;
    result.complete = result.complete && exitRead.has_value();
    std::vector<MirForwardDataflowEdge<State>> edges;
    const auto edge = [&](MirBlockId target, std::size_t ordinal,
                          const std::vector<MirOperand> &arguments) {
      if (!target)
        return;
      const auto *next = body.findBlock(target);
      if (!next || next->parameters.size() != arguments.size()) {
        result.complete = false;
        return;
      }
      State incoming = state;
      if (block->terminator.kind == MirTerminatorKind::Invoke && ordinal == 1 &&
          beforeInvoke && invoked) {
        // A failed operation cannot narrow dependencies. Keep both the old
        // destination and every possibly retained input through failure
        // cleanup, while withholding the failed operation's SSA result.
        // Unmodelled call/raw effects still mark the entire query incomplete.
        mergeMap(incoming.roots, beforeInvoke->roots);
        if (invoked->result)
          incoming.values[*invoked->result].reset();
      }
      // Edge parameters bind simultaneously, including loop-carried swaps.
      std::vector<Fact> argumentFacts;
      argumentFacts.reserve(arguments.size());
      for (const auto &argument : arguments) {
        argumentFacts.push_back(operandFact(body, incoming, argument));
        join(result.exits[id], operandRead(body, incoming, argument));
      }
      result.complete = result.complete && result.exits[id].has_value();
      for (std::size_t index = 0; index < arguments.size(); ++index)
        incoming.values[next->parameters[index].value] =
            std::move(argumentFacts[index]);
      edges.push_back(
          {.target = target, .ordinal = ordinal, .state = std::move(incoming)});
    };
    const auto &end = block->terminator;
    switch (end.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::ContinueFailure:
      edge(end.target, 0, end.targetArguments);
      break;
    case MirTerminatorKind::Branch:
    case MirTerminatorKind::Invoke:
      edge(end.target, 0, end.targetArguments);
      edge(end.elseTarget, 1, end.elseArguments);
      break;
    case MirTerminatorKind::Switch:
      edge(end.target, 0, end.targetArguments);
      for (std::size_t index = 0; index < end.switchTargets.size(); ++index)
        edge(end.switchTargets[index].target, index + 1,
             end.switchTargets[index].arguments);
      break;
    default:
      break;
    }
    return edges;
  };
  const auto solved = runMirForwardDataflow(
      body, std::move(initial), transfer,
      [](State &into, const State &from, const MirDataflowMergeContext &) {
        return join(into, from);
      },
      {.widenAfterUpdates = 0});
  result.complete = result.complete && solved.converged();
  return result;
}
} // namespace lang
