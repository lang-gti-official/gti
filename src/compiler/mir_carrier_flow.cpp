#include "mir_carrier_flow.h"
#include "gti/optimization/dataflow.h"

#include <algorithm>
#include <type_traits>

namespace lang {
namespace {
using Fact = MirCarriedLoans;
using Root = MirCarrierRoot;
using Aliases = std::optional<std::vector<Root>>;
struct AnalysisContext {
  const MirProgram *program = nullptr;
  // An inserted incomplete entry also marks an in-progress query. A cycle
  // cannot be certified as non-retaining by assuming its own answer.
  std::map<HirFunctionInstanceId, MirRetainingStoreEffects> callEffects;
};
MirCarrierFlow deriveFlow(const MirBody &body, AnalysisContext &context);
MirRetainingStoreEffects deriveEffects(const MirFunctionInstance &function,
                                      AnalysisContext &context);
struct State {
  std::map<Root, Fact> roots;
  std::map<MirValueId, Fact> values;
  std::map<MirValueId, Aliases> aliases;
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

template <class T>
bool join(std::optional<std::vector<T>> &into,
          const std::optional<std::vector<T>> &from) {
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
      const auto [found, inserted] = target.emplace(
          key, typename std::decay_t<decltype(target)>::mapped_type{});
      changed = inserted || join(found->second, value) || changed;
    }
    return changed;
  };
  const bool roots = mergePaths(into.roots, from.roots);
  const bool values = mergePaths(into.values, from.values);
  return mergePaths(into.aliases, from.aliases) || values || roots;
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
  if (place->root == MirPlaceRootKind::This)
    return Storage{{{place->root, 0}}, false};
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
    Fact held = found != state.values.end() ? found->second : Fact{};
    // A staged borrow keeps an address, not a snapshot of the referent's
    // contents. Later stores through that address can add dependencies before
    // another call reads it. Preserve the physical capability as well.
    const auto aliases = state.aliases.find(operand.value);
    if (aliases != state.aliases.end() && aliases->second)
      for (const auto root : *aliases->second) {
        const auto current = state.roots.find(root);
        join(held, current == state.roots.end() ? Fact{} : current->second);
      }
    return held;
  }
  const Fact held = placeFact(body, state, operand.place);
  // A direct Copy/Move can initialize storage without an intermediate Load.
  // Its independent scalar value carries no loan, just like a scalar SSA
  // result. The physical access itself is accounted for by operandRead.
  if (operand.type.kind != SemanticType::Unknown &&
      !mayHoldDependencies(operand.type, {}))
    return empty();
  return held;
}
Fact operandRead(const MirBody &body, const State &state,
                 const MirOperand &operand) {
  Fact read = operandFact(body, state, operand);
  // Loading an independent scalar still reads the storage that supplies it.
  // In particular a reference field may be lowered as a typed scalar Copy
  // from a carrier projection rather than as a Loan-rooted place. Erasing
  // dependencies from the produced value must not erase this access use.
  if (operand.place && (operand.kind == MirOperandKind::Copy ||
                        operand.kind == MirOperandKind::Move))
    join(read, placeFact(body, state, operand.place));
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
std::optional<Storage> operandStorage(const MirBody &body, const State &state,
                                      const MirOperand &operand) {
  if (operand.place)
    return storage(body, state, operand.place);
  if (operand.kind == MirOperandKind::Value) {
    const auto found = state.aliases.find(operand.value);
    return found != state.aliases.end() && found->second
               ? std::optional<Storage>{{*found->second, false}}
               : std::nullopt;
  }
  if (operand.kind != MirOperandKind::Loan)
    return std::nullopt;
  const auto *loan = body.findLoan(operand.loan);
  if (!loan)
    return std::nullopt;
  auto result = storage(body, state, loan->source);
  if (!result)
    return std::nullopt;
  for (const auto &dependency : loan->additionalDependencies) {
    const auto candidate = storage(body, state, dependency.source);
    if (!candidate)
      return std::nullopt;
    result->roots.insert(result->roots.end(), candidate->roots.begin(),
                         candidate->roots.end());
  }
  std::sort(result->roots.begin(), result->roots.end());
  result->roots.erase(std::unique(result->roots.begin(), result->roots.end()),
                      result->roots.end());
  result->exact = false;
  return result;
}
template <class Key>
void record(std::map<Key, Fact> &facts, Key id, const Fact &value) {
  const auto [found, inserted] = facts.emplace(id, value);
  if (!inserted)
    join(found->second, value);
}
MirCarrierFlow deriveFlow(const MirBody &body, AnalysisContext &context) {
  const auto *program = context.program;
  MirCarrierFlow result;
  if (!body.findBlock(body.entry)) {
    result.complete = false;
    result.effectsComplete = false;
    return result;
  }
  State initial;
  auto &callEffects = context.callEffects;
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
      bool checkedCallEffects = false;
      if (instruction.kind == MirInstructionKind::Call && program &&
          instruction.dispatch == CallDispatch::Static &&
          instruction.functionTarget) {
        const auto *callee =
            program->findFunctionInstance(*instruction.functionTarget);
        auto [cached, inserted] =
            callEffects.try_emplace(*instruction.functionTarget);
        if (inserted && callee)
          cached->second = deriveEffects(*callee, context);
        const auto &effects = cached->second;
        if (callee && effects.complete &&
            instruction.operands.size() == callee->parameterBindings.size()) {
          checkedCallEffects = true;
          const auto actual = [&](Root root) -> const MirOperand * {
            if (root.kind == MirPlaceRootKind::This)
              return instruction.receiver ? &*instruction.receiver : nullptr;
            const auto found =
                std::find(callee->parameterBindings.begin(),
                          callee->parameterBindings.end(), root.identity);
            return found == callee->parameterBindings.end()
                       ? nullptr
                       : &instruction.operands[found - callee->parameterBindings
                                                           .begin()];
          };
          // Evaluate all actual sources before any retaining write. Aliasing
          // between actuals must not make one modeled write feed another.
          std::vector<std::pair<Storage, Fact>> writes;
          for (const auto &effect : effects.effects) {
            const auto *target = actual(effect.destination);
            const auto *input = actual(effect.source);
            auto destination =
                target ? operandStorage(body, state, *target) : std::nullopt;
            if (!destination || !input) {
              checkedCallEffects = false;
              break;
            }
            const Fact held = operandFact(body, state, *input);
            if (!held) {
              checkedCallEffects = false;
              break;
            }
            writes.emplace_back(*destination, held);
          }
          if (checkedCallEffects)
            for (const auto &[destination, held] : writes) {
              record(result.storeInputs, instruction.id, held);
              auto &targets = result.storeTargets[instruction.id];
              for (const auto root : destination.roots) {
                record(result.storeInputsByTarget[instruction.id], root, held);
                auto [found, inserted] = state.roots.emplace(root, Fact{});
                (void)inserted;
                join(found->second, held); // Calls always widen caller storage.
                record(result.stores, instruction.id, found->second);
                targets.push_back(root);
              }
              std::sort(targets.begin(), targets.end());
              targets.erase(std::unique(targets.begin(), targets.end()),
                            targets.end());
            }
        }
      }
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
          instruction.kind == MirInstructionKind::RetireProgramStorage) {
        result.complete = false;
        result.effectsComplete = false;
      }
      // Until interprocedural effects are available, even an apparently
      // ownerless call can mutate a receiver, reference argument or global.
      if ((instruction.kind == MirInstructionKind::Call &&
           !checkedCallEffects) ||
          instruction.kind == MirInstructionKind::Construct ||
          instruction.kind == MirInstructionKind::Drop ||
          instruction.kind == MirInstructionKind::CleanupBeforeAssign ||
          instruction.kind == MirInstructionKind::Count ||
          instruction.unsafeOperation != UnsafeOperationKind::None)
        result.effectsComplete = false;
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
      if (instruction.kind == MirInstructionKind::Call && !checkedCallEffects &&
          (!read || !read->empty()))
        result.complete = false;
      // Reading a borrow to compute an integer/bool uses the loan but does
      // not make the independent scalar result carry that loan.
      if (instruction.result && !instruction.loan &&
          !mayHoldDependencies(instruction.info.type, instruction.info.traits))
        produced = empty();
      if (instruction.result) {
        state.values[*instruction.result] = produced;
        Aliases aliases;
        if (instruction.kind == MirInstructionKind::CallInput &&
            instruction.operands.size() == 1 &&
            (instruction.callInputKind == HirCallInputKind::ReadBorrow ||
             instruction.callInputKind == HirCallInputKind::MutableBorrow ||
             instruction.callInputKind ==
                 HirCallInputKind::ReadTemporaryBorrow ||
             instruction.callInputKind ==
                 HirCallInputKind::MutableTemporaryBorrow))
          if (const auto source =
                  operandStorage(body, state, instruction.operands.front()))
            aliases = source->roots;
        state.aliases[*instruction.result] = std::move(aliases);
      }
      if (instruction.kind == MirInstructionKind::Initialize ||
          instruction.kind == MirInstructionKind::Assign) {
        const auto destination =
            instruction.destination
                ? storage(body, state, *instruction.destination)
                : std::nullopt;
        if (!destination) {
          result.complete = false;
          result.effectsComplete = false;
        } else {
          record(result.storeInputs, instruction.id, producedInputs);
          auto &targets = result.storeTargets[instruction.id];
          targets.insert(targets.end(), destination->roots.begin(),
                         destination->roots.end());
          std::sort(targets.begin(), targets.end());
          targets.erase(std::unique(targets.begin(), targets.end()),
                        targets.end());
          Fact retained = empty();
          for (const auto &root : destination->roots) {
            record(result.storeInputsByTarget[instruction.id], root,
                   producedInputs);
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
    if (block->terminator.kind == MirTerminatorKind::Invoke && !beforeInvoke) {
      result.complete = false;
      result.effectsComplete = false;
    }
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
        result.effectsComplete = false;
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
        if (invoked->result) {
          incoming.values[*invoked->result].reset();
          incoming.aliases[*invoked->result].reset();
        }
      }
      // Edge parameters bind simultaneously, including loop-carried swaps.
      std::vector<Fact> argumentFacts;
      std::vector<Aliases> argumentAliases;
      argumentFacts.reserve(arguments.size());
      for (const auto &argument : arguments) {
        argumentFacts.push_back(operandFact(body, incoming, argument));
        Aliases aliases;
        if (argument.kind == MirOperandKind::Value ||
            argument.kind == MirOperandKind::Loan ||
            argument.kind == MirOperandKind::BorrowRead ||
            argument.kind == MirOperandKind::BorrowWrite)
          if (const auto source = operandStorage(body, incoming, argument))
            aliases = source->roots;
        argumentAliases.push_back(std::move(aliases));
        join(result.exits[id], operandRead(body, incoming, argument));
      }
      result.complete = result.complete && result.exits[id].has_value();
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        incoming.values[next->parameters[index].value] =
            std::move(argumentFacts[index]);
        incoming.aliases[next->parameters[index].value] =
            std::move(argumentAliases[index]);
      }
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
  result.effectsComplete = result.effectsComplete && solved.converged();
  return result;
}

} // namespace

std::map<MirLoanId, MirConservativeRetention>
conservativeMirRetentions(const MirCarrierFlow &flow) {
  std::map<MirLoanId, MirConservativeRetention> result;
  for (const auto &[instruction, writes] : flow.storeInputsByTarget) {
    (void)instruction;
    for (const auto &[root, inputs] : writes) {
      if (!inputs)
        continue;
      for (const auto loan : *inputs) {
        auto &retained = result[loan];
        if (root.kind == MirPlaceRootKind::Binding && root.identity)
          retained.carriers.push_back(root.identity);
        else
          retained.unmappedStorage = true;
      }
    }
  }
  for (auto &[loan, retained] : result) {
    (void)loan;
    std::sort(retained.carriers.begin(), retained.carriers.end());
    retained.carriers.erase(
        std::unique(retained.carriers.begin(), retained.carriers.end()),
        retained.carriers.end());
  }
  return result;
}

namespace {
MirRetainingStoreEffects deriveEffects(const MirFunctionInstance &function,
                                      AnalysisContext &context) {
  MirRetainingStoreEffects result;
  if (function.definitionKind != MirDefinitionKind::Source ||
      function.body.kind != MirBodyKind::Function ||
      function.parameterBindings.size() != function.parameterTypes.size())
    return result;
  const auto &body = function.body;
  const auto flow = deriveFlow(body, context);
  if (!flow.effectsComplete)
    return result;
  const auto boundary = [&](Root root) {
    return root.kind == MirPlaceRootKind::This
               ? function.owner.has_value() && !function.staticMember
               : root.kind == MirPlaceRootKind::Binding &&
                     std::find(function.parameterBindings.begin(),
                               function.parameterBindings.end(),
                               root.identity) !=
                         function.parameterBindings.end();
  };
  // Reconstruct owners through explicit Loan-rooted reborrows. Following a
  // parent alone is insufficient: a loan of local carrier storage must not
  // become exportable merely because that carrier contains an input borrow.
  std::vector<MirLoanId> visiting;
  const auto origins = [&](const auto &self, MirLoanId id,
                           std::vector<Root> &result) -> bool {
    const auto *loan = body.findLoan(id);
    if (!loan || loan->storedDependency ||
        std::find(visiting.begin(), visiting.end(), id) != visiting.end())
      return false;
    visiting.push_back(id);
    const auto sourceOrigin = [&](MirPlaceId sourceId, MirLoanId parent) {
      const auto *source = body.findPlace(sourceId);
      if (!source || source->capture)
        return false;
      if (loan->entry) {
        if (parent || !source->projections.empty())
          return false;
        const Root root{
            source->root,
            source->root == MirPlaceRootKind::Binding ? source->binding : 0};
        if (!boundary(root))
          return false;
        result.push_back(root);
        return true;
      }
      if (!parent || source->root != MirPlaceRootKind::Loan ||
          source->loan != parent ||
          std::any_of(source->projections.begin(), source->projections.end(),
                      [](const auto &part) {
                        return part.kind == MirProjectionKind::Dereference ||
                               part.kind == MirProjectionKind::RawDereference ||
                               part.kind == MirProjectionKind::RawIndex;
                      }))
        return false;
      return self(self, parent, result);
    };
    bool valid = sourceOrigin(loan->source, loan->parent);
    for (const auto &dependency : loan->additionalDependencies)
      valid = sourceOrigin(dependency.source, dependency.parent) && valid;
    visiting.pop_back();
    return valid;
  };
  for (const auto &[instruction, writes] : flow.storeInputsByTarget) {
    (void)instruction;
    for (const auto &[target, input] : writes) {
      if (!boundary(target))
        continue;
      if (target.kind == MirPlaceRootKind::Binding) {
        const auto position =
            std::find(function.parameterBindings.begin(),
                      function.parameterBindings.end(), target.identity);
        const auto index = position - function.parameterBindings.begin();
        // Replacing the callee's by-value parameter does not mutate caller
        // storage. Writes through its nested references require projected
        // provenance and deliberately remain unsupported here.
        if (function.parameterTypes[index].kind != SemanticType::Reference)
          continue;
      }
      if (!input)
        return result;
      for (const auto id : *input) {
        std::vector<Root> owners;
        if (!origins(origins, id, owners))
          return result;
        for (const auto origin : owners)
          result.effects.push_back({target, origin});
      }
    }
  }
  std::sort(result.effects.begin(), result.effects.end());
  result.effects.erase(
      std::unique(result.effects.begin(), result.effects.end()),
      result.effects.end());
  result.complete = true;
  return result;
}
} // namespace

MirCarrierFlow deriveMirCarrierFlow(const MirBody &body,
                                    const MirProgram *program) {
  AnalysisContext context{.program = program};
  return deriveFlow(body, context);
}

MirRetainingStoreEffects
deriveMirRetainingStoreEffects(const MirFunctionInstance &function,
                               const MirProgram *program) {
  AnalysisContext context{.program = program};
  // Reserve the queried function too, so direct recursion is detected at
  // its first backedge rather than relying on a second nested traversal.
  if (program)
    context.callEffects.emplace(function.id, MirRetainingStoreEffects{});
  return deriveEffects(function, context);
}
} // namespace lang
