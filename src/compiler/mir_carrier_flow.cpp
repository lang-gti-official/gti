#include "mir_carrier_flow.h"
#include "gti/mir_effects.h"
#include "gti/optimization/dataflow.h"
#include "mir_verification_queries.h"

#include <algorithm>
#include <deque>
#include <set>
#include <tuple>
#include <type_traits>

namespace lang {
bool mirTypeMayHoldDependencies(const SemanticType &type,
                                const SemanticTypeTraits &traits) {
  return traits.containsBorrowedState || type.kind == SemanticType::Unknown ||
         type.kind == SemanticType::TypeParameter ||
         type.kind == SemanticType::Reference ||
         type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString ||
         type.kind == SemanticType::Class || type.kind == SemanticType::Enum ||
         type.kind == SemanticType::Array || type.kind == SemanticType::Lambda;
}

std::optional<std::size_t> mirRetainingParameterIndex(
    const MirBody &body, const std::vector<SemanticType> &parameterTypes,
    const std::vector<HirBindingId> &parameterBindings, MirCarrierRoot root) {
  if (root.kind != MirPlaceRootKind::Binding)
    return std::nullopt;
  std::optional<std::size_t> result;
  std::size_t offset = 0;
  std::set<HirBindingId> seen;
  for (const auto binding : parameterBindings) {
    if (!binding || !seen.insert(binding).second)
      return std::nullopt;
    const auto place = std::find_if(
        body.places.begin(), body.places.end(), [&](const auto &candidate) {
          return candidate.root == MirPlaceRootKind::Binding &&
                 candidate.binding == binding && candidate.projections.empty();
        });
    const bool pack = place != body.places.end() &&
                      place->type.kind == SemanticType::TypePack;
    if (pack && !place->type.concretePack)
      return std::nullopt;
    const auto count = pack ? place->type.arguments.size() : 1;
    if (offset > parameterTypes.size() ||
        count > parameterTypes.size() - offset)
      return std::nullopt;
    if (pack)
      for (std::size_t lane = 0; lane < count; ++lane)
        if (place->type.arguments[lane] != parameterTypes[offset + lane])
          return std::nullopt;
    if (binding == root.identity) {
      if (pack != root.packElement.has_value() ||
          (root.packElement && *root.packElement >= count))
        return std::nullopt;
      result = offset + root.packElement.value_or(0);
    }
    offset += count;
  }
  return offset == parameterTypes.size() ? result : std::nullopt;
}

namespace {
using Fact = MirCarriedLoans;
using Root = MirCarrierRoot;
using Aliases = std::optional<std::vector<Root>>;
struct AnalysisContext {
  const MirProgram *program = nullptr;
  bool includeBlockEntries = false;
  // Constructor recursion cannot certify itself as having no outward stores.
  std::set<HirConstructorInstanceId> activeConstructors;
  std::set<HirDestructorInstanceId> activeDestructors;
  // Public queries solve the reachable call graph before consuming these
  // summaries. Provisional iteration facts never escape that solver.
  std::map<HirFunctionInstanceId, MirRetainingStoreEffects> callEffects;
};
MirCarrierFlow deriveFlow(const MirBody &body, AnalysisContext &context,
                          bool symbolicSummary = false);
MirRetainingStoreEffects deriveEffects(const MirFunctionInstance &function,
                                       AnalysisContext &context);
MirRetainingStoreEffects
deriveConstructorEffects(const MirConstructorInstance &constructor,
                         AnalysisContext &context);
struct State {
  std::map<Root, Fact> roots;
  std::map<MirValueId, Fact> values;
  std::map<MirValueId, Aliases> aliases;
  // Expiration belongs to the carried value, not merely to a group ID at a
  // CFG join. Recreating a group cannot repair an older copy of its value.
  std::map<Root, Fact> expiredRoots;
  std::map<MirValueId, Fact> expiredValues;
  std::set<MirLoanId> unavailable;
};
Fact empty() { return std::vector<MirLoanId>{}; }

// An opaque whole-carrier dependency can be matched to a named field only
// when concrete layout proves that field is its sole dependency-bearing
// component. Text, unknown layouts, alternatives and multiple references do
// not provide that proof. This query never changes the represented owner.
std::optional<std::vector<SymbolId>>
soleSharedReferencePath(const MirProgram &program, const SemanticType &type) {
  std::vector<SemanticType> visiting;
  std::vector<SymbolId> path, selected;
  std::size_t count = 0;
  const auto visit = [&](const auto &self,
                         const SemanticType &current) -> bool {
    if (current.kind == SemanticType::Reference) {
      if (current.referenceAccess != AccessMode::ReadOnly ||
          current.arguments.size() != 1 || path.empty())
        return false;
      selected = path;
      return ++count == 1;
    }
    // Opaque owning storage can retain dependencies behind a pointer even
    // though its ordinary layout has no reference field.
    if (current.kind == SemanticType::RawPointer ||
        current.kind == SemanticType::SharedPointer ||
        current.kind == SemanticType::UniqueOwner)
      return false;
    if (!mirTypeMayHoldDependencies(current, {}))
      return true;
    if (current.kind != SemanticType::Class ||
        std::find(visiting.begin(), visiting.end(), current) != visiting.end())
      return false;
    const MirClassInstance *record = nullptr;
    for (const auto &candidate : program.classInstances())
      if (candidate.type == current) {
        if (record)
          return false;
        record = &candidate;
      }
    if (!record || !record->bases.empty() || !record->structuralBases.empty() ||
        record->unionLayout || record->cOpaqueHandle ||
        record->compilerCapability != CompilerCapabilityTypeKind::None)
      return false;
    visiting.push_back(current);
    for (const auto &field : record->declaredFields) {
      path.push_back(field.symbol);
      if (!field.symbol || !self(self, field.type))
        return false;
      path.pop_back();
    }
    visiting.pop_back();
    return true;
  };
  return visit(visit, type) && count == 1 ? std::optional{selected}
                                          : std::nullopt;
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
  const bool expiredRoots = mergePaths(into.expiredRoots, from.expiredRoots);
  const bool expiredValues = mergePaths(into.expiredValues, from.expiredValues);
  const auto endedSize = into.unavailable.size();
  into.unavailable.insert(from.unavailable.begin(), from.unavailable.end());
  return mergePaths(into.aliases, from.aliases) || values || roots ||
         expiredRoots || expiredValues || endedSize != into.unavailable.size();
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
  if (place->root == MirPlaceRootKind::Binding) {
    Root root{place->root, place->binding};
    if (!place->projections.empty() &&
        place->projections.front().kind == MirProjectionKind::PackElement) {
      if (!mirConcretePackElementType(body, *place))
        return std::nullopt;
      root.packElement = *place->projections.front().constantIndex;
    }
    return Storage{{root},
                   place->projections.empty() ||
                       (root.packElement && place->projections.size() == 1)};
  }
  if (place->root == MirPlaceRootKind::Temporary)
    return Storage{{{place->root, place->temporary}},
                   place->projections.empty()};
  if (place->root == MirPlaceRootKind::This)
    return Storage{{{place->root, 0}}, false};
  return std::nullopt;
}
Fact placeFact(const MirBody &body, const State &state, MirPlaceId id,
               bool expired = false) {
  const auto &roots = expired ? state.expiredRoots : state.roots;
  const auto &values = expired ? state.expiredValues : state.values;
  const auto *place = body.findPlace(id);
  if (!place)
    return std::nullopt;
  if (const auto key = storage(body, state, id)) {
    Fact held = empty();
    bool known = true;
    for (const auto &root : key->roots) {
      const auto found = roots.find(root);
      if (found == roots.end()) {
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
    const auto found = values.find(place->value);
    if (found != values.end())
      return found->second;
  }
  if (std::any_of(place->projections.begin(), place->projections.end(),
                  [](const auto &part) {
                    return part.kind == MirProjectionKind::Dereference ||
                           part.kind == MirProjectionKind::RawDereference ||
                           part.kind == MirProjectionKind::RawIndex;
                  }))
    return std::nullopt;
  return mirTypeMayHoldDependencies(place->type, place->traits) ? Fact{}
                                                                : empty();
}
// A checked scalar reference-field access uses the carrier's current
// external owners. This is access provenance, never a physical storage target
// or a claim about borrowed contents inside the referent.
Fact scalarReferenceFieldOwners(const MirBody &body, const State &state,
                                MirPlaceId id, bool expired) {
  const auto *place = body.findPlace(id);
  if (!place || place->foreignStorage ||
      place->root != MirPlaceRootKind::Binding ||
      mirTypeMayHoldDependencies(place->type, place->traits) ||
      place->projections.size() < 2 ||
      place->projections.back().kind != MirProjectionKind::Dereference ||
      !std::all_of(place->projections.begin(), place->projections.end() - 1,
                   [](const auto &part) {
                     return part.kind == MirProjectionKind::Field;
                   }))
    return std::nullopt;
  const auto &roots = expired ? state.expiredRoots : state.roots;
  const auto held = roots.find({place->root, place->binding});
  return held == roots.end() ? Fact{} : held->second;
}
Fact operandFact(const MirBody &body, const State &state,
                 const MirOperand &operand, bool expired = false) {
  const auto &roots = expired ? state.expiredRoots : state.roots;
  const auto &values = expired ? state.expiredValues : state.values;
  if (operand.kind == MirOperandKind::Constant)
    return empty();
  if (operand.kind == MirOperandKind::Loan)
    return body.findLoan(operand.loan)
               ? (expired && !state.unavailable.contains(operand.loan)
                      ? empty()
                      : Fact{std::vector<MirLoanId>{operand.loan}})
               : Fact{};
  if (operand.kind == MirOperandKind::Value) {
    const auto found = values.find(operand.value);
    Fact held = found != values.end() ? found->second : Fact{};
    // A staged borrow keeps an address, not a snapshot of the referent's
    // contents. Later stores through that address can add dependencies before
    // another call reads it. Preserve the physical capability as well.
    const auto aliases = state.aliases.find(operand.value);
    if (aliases != state.aliases.end() && aliases->second)
      for (const auto root : *aliases->second) {
        const auto current = roots.find(root);
        join(held, current == roots.end() ? Fact{} : current->second);
      }
    return held;
  }
  Fact held = placeFact(body, state, operand.place, expired);
  // A staged reference to a scalar field preserves its access dependency
  // across argument evaluation and the call. It does not need the scalar's
  // nonexistent borrowed-value contents to model that reference.
  if (!held && (operand.kind == MirOperandKind::BorrowRead ||
                operand.kind == MirOperandKind::BorrowWrite))
    held = scalarReferenceFieldOwners(body, state, operand.place, expired);
  // Copying a whole borrowed value from a shared reference formal retains
  // its contents. Physical access is observed independently by operandRead.
  // Inline text also supports the existing array-element projection rule.
  const auto *copiedPlace = body.findPlace(operand.place);
  const bool wholeBorrowedCopy =
      operand.kind == MirOperandKind::Copy && copiedPlace &&
      copiedPlace->traits.copyable &&
      copiedPlace->traits.containsBorrowedState &&
      copiedPlace->root == MirPlaceRootKind::Binding &&
      copiedPlace->projections.size() == 1 &&
      copiedPlace->projections.front().kind == MirProjectionKind::Dereference;
  const SemanticType *leaf = &operand.type;
  while (leaf->kind == SemanticType::Array && leaf->arguments.size() == 1)
    leaf = &leaf->arguments.front();
  if (held &&
      (operand.kind == MirOperandKind::Copy ||
       operand.kind == MirOperandKind::Move) &&
      (wholeBorrowedCopy || leaf->kind == SemanticType::StringView ||
       leaf->kind == SemanticType::CString)) {
    const auto external = [&](MirLoanId id) {
      const auto *shell = body.findLoan(id);
      const auto *source = shell ? body.findPlace(shell->source) : nullptr;
      if (!shell || !shell->entry || shell->kind != MirLoanKind::Parameter ||
          !shell->storedDependency || !source ||
          source->type.kind != SemanticType::Reference ||
          source->type.arguments.size() != 1)
        return id;
      const SemanticType *referent = &source->type.arguments.front();
      if (wholeBorrowedCopy && source->binding == copiedPlace->binding &&
          source->type.referenceAccess == AccessMode::ReadOnly &&
          *referent == operand.type)
        return shell->storedDependency;
      if (leaf->kind != SemanticType::StringView &&
          leaf->kind != SemanticType::CString)
        return id;
      while (referent->kind == SemanticType::Array &&
             referent->arguments.size() == 1)
        referent = &referent->arguments.front();
      return referent->kind == leaf->kind ? shell->storedDependency : id;
    };
    for (auto &id : *held)
      id = external(id);
    // External data can expire while the physical reference is still live.
    // Its abstract dependency has no binding carrier to populate expiredRoots.
    if (expired)
      if (const auto current = placeFact(body, state, operand.place))
        for (const auto id : *current) {
          const auto dependency = external(id);
          // Only the abstract reference-to-contents mapping needs this extra
          // check. Ordinary copies already carry path-sensitive expiration
          // in expiredRoots; a loan absent on a literal alternative does not
          // expire the value on the branch where that loan was produced.
          if (dependency != id && state.unavailable.contains(dependency))
            held->push_back(dependency);
        }
    std::sort(held->begin(), held->end());
    held->erase(std::unique(held->begin(), held->end()), held->end());
  }
  // A direct Copy/Move can initialize storage without an intermediate Load.
  // Its independent scalar value carries no loan, just like a scalar SSA
  // result. The physical access itself is accounted for by operandRead.
  if (operand.type.kind != SemanticType::Unknown &&
      !mirTypeMayHoldDependencies(operand.type, {}))
    return empty();
  return held;
}
Fact operandRead(const MirBody &body, const State &state,
                 const MirOperand &operand, bool expired = false) {
  const auto &roots = expired ? state.expiredRoots : state.roots;
  Fact read = operandFact(body, state, operand, expired);
  // Loading an independent scalar still reads the storage that supplies it.
  // In particular a reference field may be lowered as a typed scalar Copy
  // from a carrier projection rather than as a Loan-rooted place. Erasing
  // dependencies from the produced value must not erase this access use.
  if (operand.place && (operand.kind == MirOperandKind::Copy ||
                        operand.kind == MirOperandKind::Move)) {
    auto accessed = placeFact(body, state, operand.place, expired);
    if (!accessed)
      accessed =
          scalarReferenceFieldOwners(body, state, operand.place, expired);
    join(read, accessed);
  }
  // Accessing a referent uses its physical loan even when its value is an
  // independent scalar. Apply the same rule to arguments and receivers.
  if (const auto *place = body.findPlace(operand.place);
      place && place->root == MirPlaceRootKind::Loan)
    join(read, expired && !state.unavailable.contains(place->loan)
                   ? empty()
                   : Fact{std::vector<MirLoanId>{place->loan}});
  else if (place && place->root == MirPlaceRootKind::Binding &&
           !place->projections.empty() &&
           place->projections.front().kind == MirProjectionKind::Dereference) {
    const auto held = roots.find({place->root, place->binding});
    join(read, held == roots.end() ? Fact{} : held->second);
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
bool privateCleanupEffects(const MirDropType &recipe,
                           AnalysisContext &context) {
  const auto *program = context.program;
  if (!program || mirDropHasPolymorphicCleanup(recipe))
    return false;
  for (const auto target : mirDropDestructorTargets(recipe)) {
    const auto *destructor = program->findDestructorInstance(target);
    if (!destructor ||
        destructor->definitionKind != MirDefinitionKind::Source ||
        destructor->body.kind != MirBodyKind::Destructor ||
        !context.activeDestructors.insert(target).second)
      return false;
    const auto cleanup = deriveFlow(destructor->body, context);
    context.activeDestructors.erase(target);
    if (!cleanup.effectsComplete)
      return false;
    for (const auto &[site, targets] : cleanup.storeTargets) {
      (void)site;
      for (const auto &destination : targets) {
        if (destination.kind == MirPlaceRootKind::This)
          continue;
        if (destination.kind != MirPlaceRootKind::Binding ||
            std::any_of(destructor->body.places.begin(),
                        destructor->body.places.end(), [&](const auto &place) {
                          return place.root == MirPlaceRootKind::Binding &&
                                 place.binding == destination.identity &&
                                 place.initiallyAvailable;
                        }))
          return false;
      }
    }
  }
  return true;
}

// A missing active-cleanup recipe is not alone evidence of trivial cleanup.
// Resolve aggregate metadata and recurse through arrays before granting it.
bool passiveObject(const MirProgram &program, const SemanticType &type) {
  if (type.kind == SemanticType::Array)
    return type.arguments.size() == 1 && type.arrayLength != 0 &&
           !type.arrayLengthParameterId &&
           passiveObject(program, type.arguments.front());
  if (type.kind == SemanticType::Class) {
    for (const auto &owner : program.classInstances())
      if (owner.type == type)
        return owner.compilerCapability == CompilerCapabilityTypeKind::None &&
               !owner.requiresActiveCleanup && !owner.destructor &&
               owner.destructorStatus == SpecialMemberStatus::Generated;
    return false;
  }
  if (type.kind == SemanticType::Enum || type.kind == SemanticType::Lambda ||
      type.kind == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack)
    return false;
  return semanticTraits(type).drop == DropKind::Trivial;
}

MirCarrierFlow deriveFlow(const MirBody &body, AnalysisContext &context,
                          bool symbolicSummary) {
  const auto *program = context.program;
  MirCarrierFlow result;
  if (!body.findBlock(body.entry)) {
    result.complete = false;
    result.effectsComplete = false;
    return result;
  }
  State initial;
  // A loan ID is not a capability before its producer executes. Keep this
  // path-sensitive for direct operands; carried copies retain their own
  // validity so absence on another predecessor does not taint a valid value.
  for (const auto &loan : body.loans)
    if (!loan.entry)
      initial.unavailable.insert(loan.id);
  auto &callEffects = context.callEffects;
  for (const auto &loan : body.loans)
    if (loan.entry)
      for (const auto carrier : loan.carriers) {
        Root root{MirPlaceRootKind::Binding, carrier};
        const auto *source = body.findPlace(loan.source);
        if (source && source->root == MirPlaceRootKind::Binding &&
            source->binding == carrier && source->projections.size() == 1 &&
            source->projections.front().kind ==
                MirProjectionKind::PackElement &&
            mirConcretePackElementType(body, *source))
          root.packElement = *source->projections.front().constantIndex;
        auto [found, inserted] = initial.roots.emplace(root, empty());
        (void)inserted;
        join(found->second, Fact{std::vector<MirLoanId>{loan.id}});
        initial.expiredRoots.emplace(root, empty());
      }
  // Summary-only receiver tokens describe input contents, not a permission
  // to dereference them. Concrete program flow never takes this branch.
  if (symbolicSummary)
    for (const auto &loan : body.loans) {
      const auto *place = body.findPlace(loan.source);
      if (loan.entry && place && place->root == MirPlaceRootKind::This &&
          place->projections.empty()) {
        const Root root{MirPlaceRootKind::This, 0};
        auto [found, inserted] = initial.roots.emplace(root, empty());
        (void)inserted;
        join(found->second, Fact{std::vector<MirLoanId>{loan.id}});
        initial.expiredRoots.emplace(root, empty());
      }
    }
  const auto transfer = [&](const MirBlock &current, State state) {
    const auto *block = &current;
    const auto id = current.id;
    std::optional<State> beforeInvoke;
    const MirInstruction *invoked = nullptr;
    for (const auto &instruction : block->instructions) {
      if (instruction.tombstone)
        continue;
      const auto incompleteEffects = [&] {
        result.effectsComplete = false;
        if (std::find(result.incompleteEffectInstructions.begin(),
                      result.incompleteEffectInstructions.end(),
                      instruction.id) ==
            result.incompleteEffectInstructions.end())
          result.incompleteEffectInstructions.push_back(instruction.id);
      };
      if (block->terminator.kind == MirTerminatorKind::Invoke &&
          block->terminator.invokeInstruction == instruction.id) {
        beforeInvoke = state;
        invoked = &instruction;
      }
      Fact callReturned;
      Fact expiredCallReturned;
      bool modeledCallReturn = false;
      Fact producedInputs = empty();
      Fact expiredInputs = empty();
      Fact expiredRead = empty();
      Fact read = empty();
      for (const auto &operand : instruction.operands) {
        const Fact held = operandFact(body, state, operand);
        join(producedInputs, held);
        join(expiredInputs, operandFact(body, state, operand, true));
        join(expiredRead, operandRead(body, state, operand, true));
        join(read, operandRead(body, state, operand));
      }
      if (instruction.receiver) {
        const auto receiverRead =
            operandRead(body, state, *instruction.receiver);
        const auto expiredReceiverRead =
            operandRead(body, state, *instruction.receiver, true);
        record(result.receiverReads, instruction.id, receiverRead);
        record(result.unavailableReceiverReads, instruction.id,
               expiredReceiverRead);
        join(read, receiverRead);
        join(expiredRead, expiredReceiverRead);
      }
      // Explicit intrinsics with no storage writes or invoked code cannot
      // retain an argument in another slot. Reuse the canonical effect table,
      // not a spelling-based list of library operations. Result provenance is
      // still checked separately below, including borrowed result loans.
      const auto intrinsicEffects = effects(instruction);
      const bool nonRetainingIntrinsic =
          instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic != IntrinsicKind::None &&
          !intrinsicEffects.writesPlace &&
          !intrinsicEffects.writesUnknownMemory &&
          !intrinsicEffects.invokesUserCode &&
          !intrinsicEffects.invokesRuntime && !intrinsicEffects.allocates &&
          !intrinsicEffects.dropsValue && !intrinsicEffects.maySynchronize;
      // Unsafe address formation and pointer conversion can occur inside a
      // retaining helper. Their unsafe validity obligations do not imply a
      // hidden retaining write. Use canonical effects to distinguish these
      // computations from raw stores, calls and unknown operations; result
      // provenance and unsafe-operation validity remain independently checked.
      const bool nonRetainingUnsafeCompute =
          instruction.kind == MirInstructionKind::Compute &&
          instruction.operation != MirOperation::None &&
          instruction.operation != MirOperation::Count &&
          !intrinsicEffects.writesPlace &&
          !intrinsicEffects.writesUnknownMemory &&
          !intrinsicEffects.invokesUserCode &&
          !intrinsicEffects.invokesRuntime && !intrinsicEffects.allocates &&
          !intrinsicEffects.dropsValue && !intrinsicEffects.maySynchronize;
      // Preparing a shared-reference argument, reborrowing, and ending a
      // borrow only update checked reference bookkeeping. They can inherit
      // an unsafe marker from their producer, but do not repeat that
      // operation or write retained contents. Keep
      // value construction/movement and any effectful staging conservative.
      const bool nonRetainingReferenceBookkeeping =
          ((instruction.kind == MirInstructionKind::CallInput &&
            instruction.callInputKind == HirCallInputKind::ReadBorrow) ||
           instruction.kind == MirInstructionKind::Borrow ||
           instruction.kind == MirInstructionKind::EndBorrow) &&
          !instruction.destination && instruction.lifecycle.empty() &&
          !intrinsicEffects.writesPlace &&
          !intrinsicEffects.writesUnknownMemory &&
          !intrinsicEffects.invokesUserCode &&
          !intrinsicEffects.invokesRuntime && !intrinsicEffects.allocates &&
          !intrinsicEffects.dropsValue && !intrinsicEffects.maySynchronize;
      // Invocation-pointer signatures have no input-to-storage retention
      // relation. Program verification rejects targets that would erase one
      // at conversion (and checks lambda boundary stores independently).
      // The invocation can still mutate storage and return a checked borrow;
      // this is only its retaining-transfer contract, not a purity claim.
      const bool invocationRetentionContract =
          instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke;
      const bool nonRetainingDestruction = [&] {
        if (!program || !mir_detail::objectDestroy(*program, instruction))
          return false;
        const auto &payload =
            instruction.parameterTypes.front().arguments.front();
        const auto recipe = mirUniqueCleanupType(*program, payload);
        if (instruction.elementCleanup != recipe)
          return false;
        return recipe ? privateCleanupEffects(*recipe, context)
                      : passiveObject(*program, payload);
      }();
      const bool scalarConstruction =
          program && mir_detail::objectConstruct(*program, instruction) &&
          !instruction.constructorTarget &&
          instruction.definedFailure.empty() &&
          isPassiveScalarValueType(
              instruction.parameterTypes.front().arguments.front());
      const bool nonRetainingFailure =
          program && mir_detail::explicitFailure(*program, instruction);
      // The constant byte provider allocates fresh storage or releases raw
      // storage. Neither operation publishes checked dependencies into a
      // caller-owned slot. This is a retention fact only: their allocation,
      // failure and memory effects remain in the canonical effect table.
      const auto rawVoid = SemanticType::rawPointerTo(SemanticType::Void);
      const bool constantStorage =
          instruction.kind == MirInstructionKind::Call &&
          !instruction.functionTarget && !instruction.constructorTarget &&
          !instruction.receiver && !instruction.loan &&
          instruction.operands.size() == instruction.parameterTypes.size() &&
          std::equal(instruction.operands.begin(), instruction.operands.end(),
                     instruction.parameterTypes.begin(),
                     [](const auto &operand, const auto &type) {
                       return operand.type == type;
                     }) &&
          ((instruction.intrinsic == IntrinsicKind::ConstantAllocate &&
            instruction.info.type == rawVoid &&
            instruction.parameterTypes ==
                std::vector<SemanticType>{SemanticType::UInt64,
                                          SemanticType::UInt64}) ||
           (instruction.intrinsic == IntrinsicKind::ConstantRelease &&
            instruction.info.type == SemanticType::Void &&
            instruction.parameterTypes == std::vector<SemanticType>{rawVoid}));
      bool checkedCallEffects = nonRetainingIntrinsic ||
                                invocationRetentionContract ||
                                nonRetainingDestruction || scalarConstruction ||
                                nonRetainingFailure || constantStorage;
      const bool ownedInitialization =
          program && mir_detail::objectInitializeOwned(*program, instruction);
      const bool ownedRelocation =
          program && mir_detail::objectRelocateOwned(*program, instruction);
      if (ownedInitialization || ownedRelocation) {
        // The raw address is not a lifetime root. The explicit owner borrow
        // identifies the storage that must retain the completed value's whole
        // dependency set. New elements widen storage; they do not replace the
        // dependencies of earlier elements.
        const auto destination =
            operandStorage(body, state, instruction.operands[1]);
        const auto sourceContents = [&](bool expired) -> Fact {
          if (!ownedRelocation)
            return operandFact(body, state, instruction.operands[2], expired);
          const auto source =
              operandStorage(body, state, instruction.operands[3]);
          if (!source)
            return std::nullopt;
          const auto &roots = expired ? state.expiredRoots : state.roots;
          Fact contents = empty();
          for (const auto root : source->roots) {
            const auto found = roots.find(root);
            join(contents, found == roots.end() ? Fact{} : found->second);
          }
          return contents;
        };
        const auto held = sourceContents(false);
        const auto expired = sourceContents(true);
        checkedCallEffects = destination.has_value() && held.has_value();
        if (destination) {
          record(result.storeInputs, instruction.id, held);
          auto &targets = result.storeTargets[instruction.id];
          for (const auto root : destination->roots) {
            record(result.storeInputsByTarget[instruction.id], root, held);
            auto [current, inserted] = state.roots.emplace(root, Fact{});
            (void)inserted;
            join(current->second, held);
            auto [invalid, unused] = state.expiredRoots.emplace(root, Fact{});
            (void)unused;
            join(invalid->second, expired);
            record(result.stores, instruction.id, current->second);
            targets.push_back(root);
          }
          std::sort(targets.begin(), targets.end());
          targets.erase(std::unique(targets.begin(), targets.end()),
                        targets.end());
        }
      }
      const bool ownedConstruction =
          program &&
          instruction.intrinsic == IntrinsicKind::ObjectConstructOwned &&
          mir_detail::objectConstruct(*program, instruction);
      if (ownedConstruction) {
        std::vector<std::tuple<Storage, Fact, Fact>> writes;
        const auto appendWrite = [&](const MirOperand &destinationInput,
                                     const MirOperand *sourceInput) {
          const auto destination =
              operandStorage(body, state, destinationInput);
          if (!destination)
            return false;
          const auto held =
              sourceInput ? operandFact(body, state, *sourceInput) : empty();
          const auto expired =
              sourceInput ? operandFact(body, state, *sourceInput, true)
                          : empty();
          if (!held || !expired)
            return false;
          writes.emplace_back(*destination, held, expired);
          return true;
        };
        if (!instruction.constructorTarget) {
          // Exact generated default/copy/move shapes are checked by the
          // structural query. A completed input contributes its contents.
          checkedCallEffects = appendWrite(instruction.operands[1],
                                           instruction.operands.size() == 3
                                               ? &instruction.operands[2]
                                               : nullptr);
        } else if (const auto *constructor = program->findConstructorInstance(
                       *instruction.constructorTarget)) {
          const auto effects = deriveConstructorEffects(*constructor, context);
          checkedCallEffects =
              effects.complete && instruction.operands.size() ==
                                      constructor->parameterTypes.size() + 2;
          const auto actual = [&](Root root) -> const MirOperand * {
            if (root.kind == MirPlaceRootKind::This)
              return &instruction.operands[1];
            const auto index = mirRetainingParameterIndex(
                constructor->body, constructor->parameterTypes,
                constructor->parameterBindings, root);
            return index && *index + 2 < instruction.operands.size()
                       ? &instruction.operands[*index + 2]
                       : nullptr;
          };
          for (const auto &effect : effects.effects) {
            const auto *destination = actual(effect.destination);
            const auto *input = actual(effect.source);
            // A fresh object's self-reference is not an external supplier.
            if (!destination || !input ||
                effect.source.kind == MirPlaceRootKind::This ||
                !appendWrite(*destination, input)) {
              checkedCallEffects = false;
              break;
            }
          }
        }
        if (checkedCallEffects)
          for (const auto &[destination, held, expired] : writes) {
            record(result.storeInputs, instruction.id, held);
            auto &targets = result.storeTargets[instruction.id];
            for (const auto root : destination.roots) {
              record(result.storeInputsByTarget[instruction.id], root, held);
              auto [current, inserted] = state.roots.emplace(root, Fact{});
              (void)inserted;
              join(current->second, held);
              auto [invalid, unused] = state.expiredRoots.emplace(root, Fact{});
              (void)unused;
              join(invalid->second, expired);
              record(result.stores, instruction.id, current->second);
              targets.push_back(root);
            }
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()),
                          targets.end());
          }
      }
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
        if (callee && (effects.complete || !effects.effects.empty()) &&
            instruction.operands.size() == callee->parameterTypes.size()) {
          checkedCallEffects = effects.complete;
          const auto actual = [&](Root root) -> const MirOperand * {
            if (root.kind == MirPlaceRootKind::This)
              return instruction.receiver ? &*instruction.receiver : nullptr;
            const auto index =
                mirRetainingParameterIndex(callee->body, callee->parameterTypes,
                                           callee->parameterBindings, root);
            return index && *index < instruction.operands.size()
                       ? &instruction.operands[*index]
                       : nullptr;
          };
          if (effects.complete && effects.returnedOrigins) {
            modeledCallReturn = true;
            callReturned = empty();
            expiredCallReturned = empty();
            for (const auto origin : *effects.returnedOrigins) {
              const auto *input = actual(origin);
              join(callReturned,
                   input ? operandFact(body, state, *input) : Fact{});
              join(expiredCallReturned,
                   input ? operandFact(body, state, *input, true) : Fact{});
            }
          }
          if (const auto parameter =
                  mir_detail::copiedReferenceParameter(*callee);
              effects.complete && parameter &&
              *parameter < instruction.operands.size()) {
            const auto &input = instruction.operands[*parameter];
            const MirOperand *copyInput = &input;
            if (input.kind == MirOperandKind::Value) {
              const auto *stage = mir_detail::definitionFor(body, input.value);
              if (stage && stage->kind == MirInstructionKind::CallInput &&
                  stage->callSite == instruction.callSite &&
                  stage->callInputRole == MirCallInputRole::Argument &&
                  stage->callInputIndex == *parameter &&
                  stage->callInputKind == HirCallInputKind::ReadBorrow &&
                  stage->operands.size() == 1 &&
                  stage->operands.front().kind != MirOperandKind::Value)
                copyInput = &stage->operands.front();
            }
            // A staged reference also tracks its storage alias for mutable
            // writes. Opaque receiver storage may have unknown alias contents,
            // while the exact physical loan still carries a checked abstract
            // stored dependency. Recover that loan from its input stage rather
            // than letting the unrelated unknown alias erase its authority.
            // Owner-root contents below are still read from current state.
            const Fact held = operandFact(body, state, *copyInput);
            Fact contents = held ? empty() : Fact{};
            Fact expired = operandFact(body, state, *copyInput, true);
            const auto retain = [&](MirLoanId id) {
              const auto *loan = body.findLoan(id);
              if (!loan || loan->kind != MirLoanKind::Stored ||
                  loan->access != AccessMode::ReadOnly ||
                  loan->lifecycleBound) {
                contents.reset();
                return;
              }
              join(contents, Fact{std::vector<MirLoanId>{id}});
              if (state.unavailable.contains(id))
                join(expired, Fact{std::vector<MirLoanId>{id}});
            };
            if (held)
              for (const auto id : *held) {
                const auto *loan = body.findLoan(id);
                if (!loan) {
                  contents.reset();
                  break;
                }
                if (loan->kind == MirLoanKind::Stored) {
                  retain(id);
                } else if (loan->storedDependency) {
                  retain(loan->storedDependency);
                } else {
                  const auto *source = body.findPlace(loan->source);
                  const auto owner =
                      source && source->root == MirPlaceRootKind::Binding &&
                              source->projections.empty() &&
                              source->type.kind != SemanticType::Reference
                          ? state.roots.find({source->root, source->binding})
                          : state.roots.end();
                  if (loan->kind != MirLoanKind::CallResult ||
                      owner == state.roots.end() || !owner->second) {
                    contents.reset();
                    break;
                  }
                  for (const auto group : *owner->second)
                    retain(group);
                  const auto unavailable =
                      state.expiredRoots.find(owner->first);
                  if (unavailable != state.expiredRoots.end())
                    join(expired, unavailable->second);
                }
              }
            record(result.copiedCallResults, instruction.id, contents);
            record(result.unavailableCopiedCallResults, instruction.id,
                   expired);
            modeledCallReturn = true;
            callReturned = contents;
            expiredCallReturned = expired;
          }
          // Evaluate all actual sources before any retaining write. Aliasing
          // between actuals must not make one modeled write feed another.
          std::vector<std::tuple<Storage, Fact, Fact>> writes;
          bool mappedWrites = true;
          for (const auto &effect : effects.effects) {
            const auto *target = actual(effect.destination);
            const auto *input = actual(effect.source);
            record(result.retainingInputs[instruction.id], effect.source,
                   input ? operandFact(body, state, *input) : Fact{});
            auto destination =
                target ? operandStorage(body, state, *target) : std::nullopt;
            if (!destination || !input) {
              checkedCallEffects = false;
              mappedWrites = false;
              continue;
            }
            const Fact held = operandFact(body, state, *input);
            if (!held) {
              checkedCallEffects = false;
              mappedWrites = false;
              continue;
            }
            writes.emplace_back(*destination, held,
                                operandFact(body, state, *input, true));
          }
          // Known writes remain retention obligations even when another
          // effect is unknown. They cannot certify the call or its result.
          if (!mappedWrites)
            incompleteEffects();
          for (const auto &[destination, held, expired] : writes) {
            record(result.storeInputs, instruction.id, held);
            auto &targets = result.storeTargets[instruction.id];
            for (const auto root : destination.roots) {
              record(result.storeInputsByTarget[instruction.id], root, held);
              auto [found, inserted] = state.roots.emplace(root, Fact{});
              (void)inserted;
              join(found->second, held); // Calls always widen caller storage.
              auto [invalid, _] = state.expiredRoots.emplace(root, Fact{});
              join(invalid->second, expired);
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
        if (destination && (destination->root == MirPlaceRootKind::Loan ||
                            std::any_of(destination->projections.begin(),
                                        destination->projections.end(),
                                        [](const auto &part) {
                                          return part.kind ==
                                                 MirProjectionKind::Dereference;
                                        })))
        // A write through an alias must keep the access loan alive too.
        // Exact replacement of a direct local does not read its old value.
        {
          const MirOperand access{.kind = MirOperandKind::Copy,
                                  .place = *instruction.destination};
          join(read, operandRead(body, state, access));
          join(expiredRead, operandRead(body, state, access, true));
        }
      }
      if (instruction.destination &&
          (instruction.kind == MirInstructionKind::Drop ||
           instruction.kind == MirInstructionKind::CleanupBeforeAssign)) {
        const MirOperand access{.kind = MirOperandKind::Copy,
                                .place = *instruction.destination};
        join(read, operandRead(body, state, access));
        join(expiredRead, operandRead(body, state, access, true));
      }
      if (instruction.loan &&
          (instruction.kind == MirInstructionKind::ActivateBorrow ||
           instruction.kind == MirInstructionKind::ParkRangeCursor ||
           instruction.kind == MirInstructionKind::RestoreRangeCursor))
        join(read, operandFact(body, state,
                               {.kind = MirOperandKind::Loan,
                                .loan = *instruction.loan}));
      record(result.reads, instruction.id, read);
      record(result.unavailableReads, instruction.id, expiredRead);
      result.complete = result.complete && read.has_value();
      Fact produced = producedInputs;
      Fact expiredProduced = expiredInputs;
      // Numerical bookkeeping can read/write memory without publishing a
      // checked reference or view. Preserve its raw-memory and liveness
      // effects; only the independent retention summary can be precise.
      const auto *scalarDestination =
          instruction.destination ? body.findPlace(*instruction.destination)
                                  : nullptr;
      const bool scalarStore =
          scalarDestination &&
          isPassiveScalarValueType(scalarDestination->type) &&
          (instruction.kind == MirInstructionKind::Initialize ||
           instruction.kind == MirInstructionKind::Assign ||
           instruction.kind == MirInstructionKind::Modify) &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().type == scalarDestination->type &&
          !instruction.functionTarget && !instruction.constructorTarget &&
          !instruction.receiver && instruction.lifecycle.empty() &&
          !instruction.elementCleanup && !instruction.replacementCleanup &&
          !intrinsicEffects.invokesUserCode && !intrinsicEffects.invokesRuntime;
      const bool scalarRead = instruction.kind == MirInstructionKind::Load &&
                              isPassiveScalarValueType(instruction.info.type) &&
                              !intrinsicEffects.writesPlace &&
                              !intrinsicEffects.writesUnknownMemory &&
                              !intrinsicEffects.invokesUserCode &&
                              !intrinsicEffects.invokesRuntime;
      // Copying a checked referent can inherit its producer's unsafe-call
      // marker without calling it again. The copy reads contents but cannot
      // publish them into another storage root. Its dependency provenance and
      // permission remain separate checks; raw/foreign loads stay conservative.
      const auto *copiedPlace =
          instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Copy
              ? body.findPlace(instruction.operands.front().place)
              : nullptr;
      const bool nonRetainingCheckedLoad =
          instruction.kind == MirInstructionKind::Load && copiedPlace &&
          !copiedPlace->foreignStorage && !instruction.rawMemoryAccess &&
          !instruction.destination && !instruction.receiver &&
          instruction.lifecycle.empty() &&
          std::none_of(
              copiedPlace->projections.begin(), copiedPlace->projections.end(),
              [](const auto &projection) {
                return projection.kind == MirProjectionKind::RawIndex ||
                       projection.kind == MirProjectionKind::RawDereference;
              }) &&
          !intrinsicEffects.writesPlace &&
          !intrinsicEffects.writesUnknownMemory &&
          !intrinsicEffects.invokesUserCode &&
          !intrinsicEffects.invokesRuntime && !intrinsicEffects.allocates &&
          !intrinsicEffects.dropsValue && !intrinsicEffects.maySynchronize;
      const bool scalarMemoryAccess = scalarStore || scalarRead;
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
        if (!scalarMemoryAccess)
          incompleteEffects();
      }
      // Reconstruct each exact cleanup recipe. Source destructors must prove
      // that their executable writes stay in the dying receiver or private
      // locals; unknown and dynamic cleanup remain conservative.
      // This is a retention fact, not permission to remove cleanup or loans.
      const bool nonRetainingCleanup = [&] {
        if (!program || instruction.kind != MirInstructionKind::Drop)
          return false;
        if (instruction.lifecycle.empty())
          return false;
        for (const auto &event : instruction.lifecycle) {
          const auto *obligation = body.findDropObligation(event.source);
          if (event.kind != MirLifecycleEventKind::Drop || !obligation ||
              instruction.destination != obligation->place)
            return false;
          auto exact =
              mirUniqueCleanupType(*program, obligation->dropType.type);
          // The active-cleanup query intentionally has no recipe for a
          // passive ordinary class. Its complete recipe is just its exact
          // nominal identity, with no destructor or active subobjects.
          if (!exact && obligation->dropType.classInstance) {
            const auto *owner =
                program->findClassInstance(*obligation->dropType.classInstance);
            if (owner && owner->type == obligation->dropType.type &&
                owner->compilerCapability == CompilerCapabilityTypeKind::None &&
                !owner->requiresActiveCleanup && !owner->destructor &&
                owner->destructorStatus == SpecialMemberStatus::Generated)
              exact =
                  MirDropType{.type = owner->type, .classInstance = owner->id};
          }
          if (!exact || *exact != obligation->dropType ||
              mirDropHasPolymorphicCleanup(*exact))
            return false;
          if (!privateCleanupEffects(*exact, context))
            return false;
        }
        return true;
      }();
      const bool nonRetainingReplacement = [&] {
        if (!program ||
            instruction.kind != MirInstructionKind::CleanupBeforeAssign ||
            !instruction.destination || !instruction.replacementPair ||
            instruction.result || instruction.receiver ||
            !instruction.operands.empty() || instruction.functionTarget ||
            instruction.constructorTarget ||
            instruction.intrinsic != IntrinsicKind::None ||
            instruction.rawMemoryAccess)
          return false;
        const auto *place = body.findPlace(*instruction.destination);
        const auto exact =
            place ? mirUniqueCleanupType(*program, place->type) : std::nullopt;
        if (!exact || instruction.replacementCleanup != exact ||
            !privateCleanupEffects(*exact, context))
          return false;
        // Retention precision never supplies the success-only execution
        // contract: program verification still checks pairing and CFG order.
        for (const auto &candidateBlock : body.blocks)
          for (const auto &commit : candidateBlock.instructions)
            if (commit.id == instruction.replacementPair)
              return commit.kind == MirInstructionKind::Assign &&
                     commit.replacementPair == instruction.id &&
                     commit.destination == instruction.destination &&
                     commit.hirValue == instruction.hirValue;
        return false;
      }();
      // Construction publishes its input dependencies through the result's
      // independently checked loan. It need not invalidate the surrounding
      // retaining summary when its executable body only writes the fresh
      // receiver or private local storage. Reference-formal/global/raw writes
      // and unknown nested invocations remain unsupported here.
      const bool localConstruction = [&] {
        if (!program || instruction.kind != MirInstructionKind::Construct)
          return false;
        // Generated default construction has no input contents to publish and
        // no source body. Require both its exact producer shape and the
        // nominal policy proving that no initializer schedule was omitted.
        if (mir_detail::generatedDefaultConstruction(instruction)) {
          for (const auto &owner : program->classInstances())
            if (owner.type == instruction.info.type)
              return owner.defaultConstructor ==
                         SpecialMemberStatus::Generated &&
                     !owner.generatedDefaultRequiresSchedule;
          return false;
        }
        if (!instruction.constructorTarget)
          return false;
        const auto *constructor =
            program->findConstructorInstance(*instruction.constructorTarget);
        if (!constructor ||
            constructor->definitionKind != MirDefinitionKind::Source ||
            constructor->body.kind != MirBodyKind::Constructor ||
            !context.activeConstructors.insert(constructor->id).second)
          return false;
        const auto construction = deriveFlow(constructor->body, context);
        context.activeConstructors.erase(constructor->id);
        if (!construction.effectsComplete)
          return false;
        for (const auto &[site, targets] : construction.storeTargets) {
          (void)site;
          for (const auto &target : targets) {
            if (target.kind == MirPlaceRootKind::This)
              continue;
            if (target.kind != MirPlaceRootKind::Binding ||
                std::find(constructor->parameterBindings.begin(),
                          constructor->parameterBindings.end(),
                          target.identity) !=
                    constructor->parameterBindings.end())
              return false;
          }
        }
        return true;
      }();
      // Reading a constructor input does not make the fresh object's fields
      // retain it. Reconstruct the actual receiver publications, just as a
      // source call's result uses its checked return relation. Represented
      // borrowed constructions keep their independently verified loan below.
      if (instruction.kind == MirInstructionKind::Construct &&
          !instruction.loan) {
        // An incomplete construction summary is unknown, not evidence that
        // every input is retained. Preserve that distinction just as for a
        // call without a represented result loan.
        produced = Fact{};
        expiredProduced = Fact{};
        if (!localConstruction) {
          // Unknown effects already prevent loan shortening below.
        } else if (mir_detail::generatedDefaultConstruction(instruction)) {
          produced = empty();
          expiredProduced = empty();
        } else if (const auto *constructor = program->findConstructorInstance(
                       *instruction.constructorTarget)) {
          const auto construction =
              deriveConstructorEffects(*constructor, context);
          if (construction.complete && instruction.operands.size() ==
                                           constructor->parameterTypes.size()) {
            produced = empty();
            expiredProduced = empty();
            for (const auto &effect : construction.effects) {
              if (effect.destination.kind != MirPlaceRootKind::This)
                continue;
              const auto index = mirRetainingParameterIndex(
                  constructor->body, constructor->parameterTypes,
                  constructor->parameterBindings, effect.source);
              if (!index || *index >= instruction.operands.size()) {
                produced.reset();
                expiredProduced.reset();
                break;
              }
              join(produced,
                   operandFact(body, state, instruction.operands[*index]));
              join(
                  expiredProduced,
                  operandFact(body, state, instruction.operands[*index], true));
            }
          }
        }
      }
      // Unknown calls can mutate a receiver, reference argument or global.
      // An unsafe-call marker is a validity obligation, not an additional
      // unmodeled write when the source call has a complete retaining summary.
      // Lifecycle markers inherit that source marker but only schedule typed
      // ownership bookkeeping; actual destructor calls remain Drop/Call.
      if ((instruction.kind == MirInstructionKind::Call &&
           !checkedCallEffects) ||
          (instruction.kind == MirInstructionKind::Construct &&
           !localConstruction) ||
          (instruction.kind == MirInstructionKind::Drop &&
           !nonRetainingCleanup) ||
          (instruction.kind == MirInstructionKind::CleanupBeforeAssign &&
           !nonRetainingReplacement) ||
          instruction.kind == MirInstructionKind::Count ||
          (instruction.unsafeOperation != UnsafeOperationKind::None &&
           !nonRetainingIntrinsic && !nonRetainingUnsafeCompute &&
           !nonRetainingReferenceBookkeeping && !nonRetainingCheckedLoad &&
           !(instruction.kind == MirInstructionKind::Call &&
             checkedCallEffects) &&
           !localConstruction && !nonRetainingCleanup &&
           !nonRetainingReplacement && !scalarMemoryAccess &&
           instruction.kind != MirInstructionKind::Lifecycle &&
           !ownedInitialization && !ownedRelocation))
        incompleteEffects();
      if (instruction.kind == MirInstructionKind::Call && instruction.loan) {
        const auto *resultLoan = body.findLoan(*instruction.loan);
        const auto source =
            resultLoan && resultLoan->kind == MirLoanKind::CallResult
                ? storage(body, state, resultLoan->source)
                : std::nullopt;
        Fact contents;
        Fact expiredContents;
        if (checkedCallEffects && source) {
          contents = empty();
          expiredContents = empty();
          for (const auto &root : source->roots) {
            const auto held = state.roots.find(root);
            const auto expired = state.expiredRoots.find(root);
            join(contents, held == state.roots.end() ? Fact{} : held->second);
            join(expiredContents, expired == state.expiredRoots.end()
                                      ? Fact{}
                                      : expired->second);
          }
        }
        record(result.resultSourceContents, instruction.id, contents);
        record(result.unavailableResultSourceContents, instruction.id,
               expiredContents);
      }
      if (instruction.loan &&
          (instruction.kind == MirInstructionKind::Borrow ||
           instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct ||
           instruction.kind == MirInstructionKind::CallInput ||
           (instruction.kind == MirInstructionKind::Compute &&
            (instruction.operation == MirOperation::Closure ||
             instruction.operation == MirOperation::PayloadConstruct)))) {
        produced = body.findLoan(*instruction.loan)
                       ? Fact{std::vector<MirLoanId>{*instruction.loan}}
                       : Fact{};
        expiredProduced = produced ? empty() : Fact{};
        // A source-call result loan can name a direct caller binding without
        // a parent-loan edge. In the private symbolic summary, reconstruct
        // by-value contents through the proved callee return relation instead
        // of mistaking that fresh loan ID for a new external origin. Concrete
        // flow keeps the actual result loan and its lifetime checks intact.
        if (symbolicSummary && instruction.kind == MirInstructionKind::Call &&
            instruction.result &&
            instruction.info.type.kind != SemanticType::Reference &&
            checkedCallEffects && modeledCallReturn) {
          produced = callReturned;
          expiredProduced = expiredCallReturned;
        } else if (!symbolicSummary &&
                   instruction.kind == MirInstructionKind::Call &&
                   instruction.result &&
                   instruction.info.category == ValueCategory::Value &&
                   instruction.info.type.kind != SemanticType::Reference &&
                   checkedCallEffects && modeledCallReturn) {
          // A returned carrier's physical loan does not replace the contents
          // that its checked return summary relays. Keep both: adapters may
          // borrow container storage while copied elements still depend on
          // owners stored inside that container.
          auto retained = callReturned;
          const auto *represented = body.findLoan(*instruction.loan);
          const auto independentShared = [](const MirLoan *loan) {
            return loan && loan->kind == MirLoanKind::Stored &&
                   loan->access == AccessMode::ReadOnly && !loan->parent &&
                   !loan->storedDependency && !loan->lifecycleBound &&
                   !loan->twoPhase && !loan->escapes && loan->source &&
                   !loan->storedField && loan->conditions.empty() &&
                   !loan->referenceSelection &&
                   std::all_of(loan->additionalDependencies.begin(),
                               loan->additionalDependencies.end(),
                               [](const auto &part) {
                                 return part.source && !part.parent &&
                                        part.access == AccessMode::ReadOnly &&
                                        part.conditions.empty();
                               });
          };
          if (retained && independentShared(represented)) {
            // A fresh, independently checked result can replace an input's
            // capability for the same external owners. Field provenance may
            // change only along the exact call's verified field relay, never
            // merely because two physical source places happen to match.
            std::erase_if(*retained, [&](MirLoanId id) {
              const auto *prior = body.findLoan(id);
              if (!expiredCallReturned ||
                  std::find(expiredCallReturned->begin(),
                            expiredCallReturned->end(),
                            id) != expiredCallReturned->end())
                return false;
              if (prior && prior->kind == MirLoanKind::CallArgument &&
                  prior->access == AccessMode::ReadOnly && !prior->parent &&
                  !prior->twoPhase && !prior->lifecycleBound &&
                  prior->storedDependency &&
                  !state.unavailable.contains(prior->storedDependency))
                prior = body.findLoan(prior->storedDependency);
              if (!independentShared(prior))
                return false;
              const auto fieldsMatch = [&](const auto &from, const auto &to) {
                if (from == to)
                  return true;
                return std::any_of(
                    instruction.borrowFields.begin(),
                    instruction.borrowFields.end(), [&](const auto &field) {
                      if (field.resultFields != to ||
                          field.access != AccessMode::ReadOnly ||
                          !field.sourceConditions.empty() ||
                          !field.resultConditions.empty())
                        return false;
                      const MirOperand *input = nullptr;
                      if (field.origin == BorrowOriginKind::Receiver &&
                          instruction.receiver)
                        input = &*instruction.receiver;
                      else if (field.origin == BorrowOriginKind::Argument &&
                               field.parameter < instruction.operands.size())
                        input = &instruction.operands[field.parameter];
                      if (!input)
                        return false;
                      if (field.sourceFields != from) {
                        if (!from.empty())
                          return false;
                        const auto path =
                            soleSharedReferencePath(*program, input->type);
                        if (!path || *path != field.sourceFields)
                          return false;
                      }
                      const auto actual = operandFact(body, state, *input);
                      return actual && std::find(actual->begin(), actual->end(),
                                                 id) != actual->end();
                    });
              };
              const auto covers = [&](MirPlaceId source, const auto &fields) {
                return (source == represented->source &&
                        fieldsMatch(fields, represented->designatedFields)) ||
                       std::any_of(represented->additionalDependencies.begin(),
                                   represented->additionalDependencies.end(),
                                   [&](const auto &part) {
                                     return part.source == source &&
                                            fieldsMatch(fields,
                                                        part.designatedFields);
                                   });
              };
              return covers(prior->source, prior->designatedFields) &&
                     std::all_of(prior->additionalDependencies.begin(),
                                 prior->additionalDependencies.end(),
                                 [&](const auto &part) {
                                   return covers(part.source,
                                                 part.designatedFields);
                                 });
            });
          }
          join(produced, retained);
          join(expiredProduced, expiredCallReturned);
        }
        if (!symbolicSummary && instruction.result &&
            instruction.info.category == ValueCategory::Value &&
            instruction.info.traits.containsBorrowedState) {
          const auto *carrier = body.findLoan(*instruction.loan);
          if (carrier && carrier->kind == MirLoanKind::Stored) {
            // A stored reference to an owner can expose that owner's retained
            // values through later accessors. Its physical lifetime alone is
            // insufficient: preserve the contents present at publication.
            // This uses the checked loan source, never arbitrary call inputs.
            const auto retainSource = [&](MirPlaceId place) {
              const auto *origin = body.findPlace(place);
              const SemanticType *contents = origin ? &origin->type : nullptr;
              if (contents && contents->kind == SemanticType::Reference &&
                  contents->arguments.size() == 1)
                contents = &contents->arguments.front();
              // A reference to a scalar supplies its referent, not another
              // borrowed value stored inside it. The result's own Stored loan
              // already represents that referent; copying the parameter's
              // physical access capability would tie the result to the callee.
              if (contents && !mirTypeMayHoldDependencies(*contents, {}))
                return;
              const auto source = storage(body, state, place);
              if (!source) {
                produced.reset();
                expiredProduced.reset();
                return;
              }
              for (const auto &root : source->roots) {
                const auto held = state.roots.find(root);
                const auto expired = state.expiredRoots.find(root);
                join(produced,
                     held == state.roots.end() ? Fact{} : held->second);
                join(expiredProduced, expired == state.expiredRoots.end()
                                          ? Fact{}
                                          : expired->second);
              }
            };
            retainSource(carrier->source);
            for (const auto &dependency : carrier->additionalDependencies)
              retainSource(dependency.source);
          }
        }
        state.unavailable.erase(*instruction.loan);
      }
      // Reading an argument does not prove that the result carries its
      // dependencies. A call without a represented result loan may return
      // an independent value, a relay, or a value from another origin. Use
      // only the converged source-return relation, captured before call writes;
      // otherwise provenance stays unknown. Scalar results are cleared below.
      if (instruction.kind == MirInstructionKind::Call && !instruction.loan &&
          instruction.result &&
          mirTypeMayHoldDependencies(instruction.info.type,
                                     instruction.info.traits)) {
        produced =
            checkedCallEffects && modeledCallReturn ? callReturned : Fact{};
        expiredProduced = checkedCallEffects && modeledCallReturn
                              ? expiredCallReturned
                              : Fact{};
      }
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
          !mirTypeMayHoldDependencies(instruction.info.type,
                                      instruction.info.traits))
        produced = empty();
      if (produced && produced->empty())
        expiredProduced = empty();
      if (!produced)
        expiredProduced.reset();
      if (instruction.result) {
        state.values[*instruction.result] = produced;
        state.expiredValues[*instruction.result] = expiredProduced;
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
          if (!scalarStore)
            incompleteEffects();
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
            auto [invalid, _] = state.expiredRoots.emplace(
                root, destination->exact ? empty() : Fact{});
            if (destination->exact) {
              found->second = producedInputs;
              invalid->second = expiredInputs;
            } else {
              join(found->second, producedInputs);
              join(invalid->second, expiredInputs);
            }
            join(retained, found->second);
          }
          result.complete = result.complete && retained.has_value();
          record(result.stores, instruction.id, retained);
        }
      }
      // Ownership transfer empties the source storage after its operands and
      // SSA result have captured the carried value. Leaving those contents in
      // a moved-from local invents an observing read at its dormant cleanup.
      // Only an unconditional, whole-storage transfer permits this narrowing;
      // projected/aliased moves retain the containing owner's obligations.
      for (const auto &event : instruction.lifecycle) {
        if (event.conditional ||
            (event.kind != MirLifecycleEventKind::Move &&
             event.kind != MirLifecycleEventKind::Reparent &&
             event.kind != MirLifecycleEventKind::TransferOut))
          continue;
        const auto *source = body.findDropObligation(event.source);
        const auto from =
            source ? storage(body, state, source->place) : std::nullopt;
        if (!from || !from->exact)
          continue;
        const auto *target = body.findDropObligation(event.target);
        const auto to =
            target ? storage(body, state, target->place) : std::nullopt;
        for (const auto &root : from->roots) {
          if (to && std::find(to->roots.begin(), to->roots.end(), root) !=
                        to->roots.end())
            continue;
          state.roots[root] = empty();
          state.expiredRoots[root] = empty();
        }
      }
      if (instruction.kind == MirInstructionKind::Drop &&
          instruction.destination)
        if (const auto destination =
                storage(body, state, *instruction.destination);
            destination && destination->exact)
          for (const auto &root : destination->roots) {
            state.roots[root] = empty();
            state.expiredRoots[root] = empty();
          }
      if (instruction.kind == MirInstructionKind::EndBorrow &&
          instruction.loan) {
        const auto ended = *instruction.loan;
        state.unavailable.insert(ended);
        const auto expire = [&](const auto &held, auto &invalid) {
          for (const auto &[key, fact] : held) {
            if (!fact) {
              invalid[key].reset();
            } else if (std::find(fact->begin(), fact->end(), ended) !=
                       fact->end()) {
              auto [entry, _] = invalid.emplace(key, empty());
              join(entry->second, Fact{std::vector<MirLoanId>{ended}});
            }
          }
        };
        expire(state.roots, state.expiredRoots);
        expire(state.values, state.expiredValues);
      }
    }
    if (block->terminator.kind == MirTerminatorKind::Invoke && !beforeInvoke) {
      result.complete = false;
      result.effectsComplete = false;
    }
    Fact exitRead = empty();
    Fact expiredExit = empty();
    if (block->terminator.value) {
      join(exitRead, operandRead(body, state, *block->terminator.value));
      join(expiredExit,
           operandRead(body, state, *block->terminator.value, true));
    }
    if (block->terminator.returnLoan) {
      if (!body.findLoan(*block->terminator.returnLoan))
        exitRead.reset();
      else
        join(exitRead,
             Fact{std::vector<MirLoanId>{*block->terminator.returnLoan}});
    }
    if (block->terminator.kind == MirTerminatorKind::Return)
      result.returned[id] =
          block->terminator.value
              ? operandFact(body, state, *block->terminator.value)
              : empty();
    result.exits[id] = exitRead;
    result.unavailableExits[id] = expiredExit;
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
        mergeMap(incoming.expiredRoots, beforeInvoke->expiredRoots);
        // A failed producer cannot authorize direct uses of the new group.
        // Existing carried versions keep the validity of their own path.
        incoming.unavailable.insert(beforeInvoke->unavailable.begin(),
                                    beforeInvoke->unavailable.end());
        if (invoked->result) {
          incoming.values[*invoked->result].reset();
          incoming.expiredValues[*invoked->result].reset();
          incoming.aliases[*invoked->result].reset();
        }
      }
      // Edge parameters bind simultaneously, including loop-carried swaps.
      std::vector<Fact> argumentFacts;
      std::vector<Fact> expiredArguments;
      std::vector<Aliases> argumentAliases;
      argumentFacts.reserve(arguments.size());
      for (const auto &argument : arguments) {
        argumentFacts.push_back(operandFact(body, incoming, argument));
        expiredArguments.push_back(operandFact(body, incoming, argument, true));
        Aliases aliases;
        if (argument.kind == MirOperandKind::Value ||
            argument.kind == MirOperandKind::Loan ||
            argument.kind == MirOperandKind::BorrowRead ||
            argument.kind == MirOperandKind::BorrowWrite)
          if (const auto source = operandStorage(body, incoming, argument))
            aliases = source->roots;
        argumentAliases.push_back(std::move(aliases));
        join(result.exits[id], operandRead(body, incoming, argument));
        join(result.unavailableExits[id],
             operandRead(body, incoming, argument, true));
      }
      result.complete = result.complete && result.exits[id].has_value();
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        incoming.values[next->parameters[index].value] =
            std::move(argumentFacts[index]);
        incoming.expiredValues[next->parameters[index].value] =
            std::move(expiredArguments[index]);
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
  if (context.includeBlockEntries && solved.converged())
    for (std::size_t index = 0; index < solved.incoming.size(); ++index)
      if (solved.incoming[index])
        result.entryRoots.emplace(index + 1, solved.incoming[index]->roots);
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
MirRetainingStoreEffects deriveSourceEffects(
    const MirBody &source, const std::vector<SemanticType> &parameterTypes,
    const std::vector<HirBindingId> &parameterBindings,
    std::optional<HirClassInstanceId> ownerId, AnalysisContext &context) {
  MirRetainingStoreEffects result;
  // Derive a symbolic relation between formals, independently of whether a
  // particular source type exposes borrowed fields. Opaque storage owners
  // and ordinary by-value aggregates can carry caller-supplied contents.
  // These tokens live only in this private analysis copy. They are never
  // emitted, verified as capabilities, or returned in concrete carrier flow.
  MirBody body = source;
  const auto seed = [&](MirPlaceRootKind kind, HirBindingId binding,
                        const SemanticType &type) {
    const bool represented = std::any_of(
        body.loans.begin(), body.loans.end(), [&](const auto &loan) {
          // An existing carrier relation is authoritative even when its loan
          // is not an entry. Synthesizing a new entry would conceal an
          // unavailable/local origin and certify a false formal dependency.
          if (kind == MirPlaceRootKind::Binding)
            return std::find(loan.carriers.begin(), loan.carriers.end(),
                             binding) != loan.carriers.end();
          if (!loan.entry)
            return false;
          const auto *place = body.findPlace(loan.source);
          return place && place->root == kind && place->projections.empty();
        });
    if (represented)
      return;
    MirPlaceId source = 0;
    for (const auto &place : body.places)
      if (place.root == kind && place.projections.empty() &&
          (kind != MirPlaceRootKind::Binding || place.binding == binding)) {
        source = place.id;
        break;
      }
    if (!source) {
      source = body.places.size() + 1;
      body.places.push_back({.id = source,
                             .root = kind,
                             .binding = binding,
                             .type = type,
                             .traits = semanticTraits(type),
                             .initiallyAvailable = true});
    }
    MirLoan token;
    token.id = body.loans.size() + 1;
    token.kind = MirLoanKind::Stored;
    token.source = source;
    token.access = AccessMode::ReadOnly;
    token.entry = true;
    if (kind == MirPlaceRootKind::Binding)
      token.carriers.push_back(binding);
    body.loans.push_back(std::move(token));
  };
  if (parameterBindings.size() == parameterTypes.size())
    for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
      const auto &type = parameterTypes[index];
      if (mirTypeMayHoldDependencies(type, semanticTraits(type)))
        seed(MirPlaceRootKind::Binding, parameterBindings[index], type);
    }
  if (context.program && ownerId)
    if (const auto *owner = context.program->findClassInstance(*ownerId))
      seed(MirPlaceRootKind::This, 0, owner->type);
  const auto flow = deriveFlow(body, context, true);
  bool completeEffects = flow.effectsComplete;
  if (parameterBindings.size() != parameterTypes.size() &&
      !flow.storeInputsByTarget.empty()) {
    bool mapped = false;
    for (const auto binding : parameterBindings) {
      mapped = mapped || mirRetainingParameterIndex(
                             body, parameterTypes, parameterBindings,
                             {MirPlaceRootKind::Binding, binding})
                             .has_value();
      mapped = mapped || mirRetainingParameterIndex(
                             body, parameterTypes, parameterBindings,
                             {MirPlaceRootKind::Binding, binding, 0})
                             .has_value();
    }
    if (!mapped)
      return result;
  }
  const auto boundary = [&](Root root) {
    return root.kind == MirPlaceRootKind::This
               ? ownerId.has_value()
               : root.kind == MirPlaceRootKind::Binding &&
                     std::find(parameterBindings.begin(),
                               parameterBindings.end(),
                               root.identity) != parameterBindings.end();
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
        if (parent)
          return false;
        Root root{source->root, source->root == MirPlaceRootKind::Binding
                                    ? source->binding
                                    : 0};
        if (!source->projections.empty()) {
          if (source->projections.size() != 1 ||
              source->projections.front().kind !=
                  MirProjectionKind::PackElement ||
              !mirConcretePackElementType(body, *source))
            return false;
          root.packElement = *source->projections.front().constantIndex;
        }
        if (!boundary(root) ||
            (root.kind == MirPlaceRootKind::Binding &&
             !mirRetainingParameterIndex(body, parameterTypes,
                                         parameterBindings, root)))
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
  result.returnedOrigins = std::vector<Root>{};
  for (const auto &[block, returned] : flow.returned) {
    (void)block;
    if (!returned) {
      result.returnedOrigins.reset();
      break;
    }
    for (const auto loan : *returned)
      if (!origins(origins, loan, *result.returnedOrigins)) {
        result.returnedOrigins.reset();
        break;
      }
    if (!result.returnedOrigins)
      break;
  }
  if (result.returnedOrigins) {
    auto &returned = *result.returnedOrigins;
    std::sort(returned.begin(), returned.end());
    returned.erase(std::unique(returned.begin(), returned.end()),
                   returned.end());
  }
  for (const auto &[instruction, writes] : flow.storeInputsByTarget) {
    for (const auto &[target, input] : writes) {
      if (!boundary(target))
        continue;
      if (target.kind == MirPlaceRootKind::Binding) {
        const auto index = mirRetainingParameterIndex(
            body, parameterTypes, parameterBindings, target);
        if (!index)
          return result;
        // Replacing the callee's by-value parameter does not mutate caller
        // storage. Writes through its nested references require projected
        // provenance and deliberately remain unsupported here.
        if (parameterTypes[*index].kind != SemanticType::Reference)
          continue;
      }
      if (!input) {
        completeEffects = false;
        continue;
      }
      for (const auto id : *input) {
        std::vector<Root> owners;
        if (!origins(origins, id, owners)) {
          completeEffects = false;
          continue;
        }
        for (const auto origin : owners) {
          const MirRetainingStoreEffect effect{target, origin};
          result.effects.push_back(effect);
          result.writeEffects[instruction].push_back(effect);
        }
      }
    }
  }
  std::sort(result.effects.begin(), result.effects.end());
  result.effects.erase(
      std::unique(result.effects.begin(), result.effects.end()),
      result.effects.end());
  for (auto &[instruction, effects] : result.writeEffects) {
    (void)instruction;
    std::sort(effects.begin(), effects.end());
    effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  }
  result.complete = completeEffects;
  if (!result.complete)
    result.returnedOrigins.reset();
  return result;
}

MirRetainingStoreEffects deriveEffects(const MirFunctionInstance &function,
                                       AnalysisContext &context) {
  MirRetainingStoreEffects result;
  // The private process allocator ABI takes only scalar/raw-address inputs
  // and retains none of them. This is not a purity or memory-validity fact:
  // allocation, release, and output-status writes keep their normal effects.
  // Recheck the exact declaration before using its non-retention contract;
  // arbitrary native declarations still remain unknown.
  if (context.program &&
      function.processMemoryOperation != ProcessMemoryRuntimeOperation::None &&
      mir_detail::exactProcessMemoryRuntimeFunction(*context.program,
                                                    function)) {
    result.complete = true;
    return result;
  }
  if (function.definitionKind != MirDefinitionKind::Source ||
      function.body.kind != MirBodyKind::Function)
    return result;
  return deriveSourceEffects(
      function.body, function.parameterTypes, function.parameterBindings,
      function.staticMember ? std::nullopt : function.owner, context);
}

MirRetainingStoreEffects
deriveConstructorEffects(const MirConstructorInstance &constructor,
                         AnalysisContext &context) {
  if (constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.body.kind != MirBodyKind::Constructor ||
      !context.activeConstructors.insert(constructor.id).second)
    return {};
  auto result = deriveSourceEffects(
      constructor.body, constructor.parameterTypes,
      constructor.parameterBindings, constructor.owner, context);
  context.activeConstructors.erase(constructor.id);
  if (!result.complete)
    return result;
  // Reference fields publish through the constructor's initializer schedule,
  // not an Initialize instruction. Recover each exact escaping publication;
  // the declared summary alone is not evidence that the initializer stores it.
  const auto *owner =
      context.program ? context.program->findClassInstance(constructor.owner)
                      : nullptr;
  for (const auto &initializer : constructor.initializers) {
    if (!initializer.storesReference)
      continue;
    const auto unknown = [&]() {
      result.complete = false;
      return result;
    };
    if (!owner || initializer.kind != ConstructorInitializerTargetKind::Field ||
        !initializer.field || initializer.arguments.size() != 1 ||
        initializer.targetType.kind != SemanticType::Reference ||
        initializer.targetType.arguments.size() != 1 ||
        initializer.borrowAccess != initializer.targetType.referenceAccess)
      return unknown();
    const auto exactField = [&](const auto &field) {
      return field.symbol == initializer.field &&
             field.type == initializer.targetType;
    };
    if (std::count_if(owner->declaredFields.begin(),
                      owner->declaredFields.end(), exactField) != 1 ||
        std::count_if(constructor.initializers.begin(),
                      constructor.initializers.end(),
                      [&](const auto &candidate) {
                        return candidate.field == initializer.field;
                      }) != 1)
      return unknown();
    const MirLoan *publication = nullptr;
    for (const auto &loan : constructor.body.loans) {
      if (loan.kind != MirLoanKind::Stored || !loan.escapes ||
          loan.storedField != initializer.field)
        continue;
      if (publication || loan.access != initializer.borrowAccess ||
          !loan.additionalDependencies.empty() ||
          (loan.producedBy != initializer.arguments.front() &&
           !(loan.entry && !loan.producedBy)))
        return unknown();
      publication = &loan;
    }
    const auto sourceId =
        publication ? mir_detail::borrowSourceForPlace(constructor.body,
                                                       publication->source, 0)
                    : std::nullopt;
    const auto *source =
        sourceId ? constructor.body.findPlace(*sourceId) : nullptr;
    if (!source || source->root != MirPlaceRootKind::Binding || source->capture)
      return unknown();
    Root origin{MirPlaceRootKind::Binding, source->binding};
    std::size_t projection = 0;
    if (!source->projections.empty() &&
        source->projections.front().kind == MirProjectionKind::PackElement) {
      if (!mirConcretePackElementType(constructor.body, *source))
        return unknown();
      origin.packElement = *source->projections.front().constantIndex;
      ++projection;
    }
    // Reference-field publication names the formal's referent. Accept only
    // its single checked dereference, never arbitrary projected/raw storage.
    const bool dereferenced = projection < source->projections.size();
    if (dereferenced && (source->projections.size() != projection + 1 ||
                         source->projections[projection].kind !=
                             MirProjectionKind::Dereference))
      return unknown();
    const auto parameter =
        mirRetainingParameterIndex(constructor.body, constructor.parameterTypes,
                                   constructor.parameterBindings, origin);
    const auto &sourceType = dereferenced
                                 ? initializer.targetType.arguments.front()
                                 : initializer.targetType;
    if (!parameter ||
        constructor.parameterTypes[*parameter] != initializer.targetType ||
        source->type != sourceType)
      return unknown();
    result.effects.push_back({{MirPlaceRootKind::This, 0}, origin});
  }
  std::sort(result.effects.begin(), result.effects.end());
  result.effects.erase(
      std::unique(result.effects.begin(), result.effects.end()),
      result.effects.end());
  return result;
}

// Solve finite store and return relations over formal roots across all
// reachable static helpers. Bottom is an empty relation, not a certificate:
// callers consume it only after every dependent body reaches a fixed point.
// Unsupported bodies/calls move to unknown and propagate to every caller.
void solveCallEffects(const MirBody &body, AnalysisContext &context,
                      const MirFunctionInstance *root = nullptr) {
  if (!context.program)
    return;
  std::map<HirFunctionInstanceId, const MirFunctionInstance *> functions;
  std::map<HirFunctionInstanceId, std::set<HirFunctionInstanceId>> callers;
  std::deque<HirFunctionInstanceId> discover;
  const auto add = [&](HirFunctionInstanceId id) {
    // Previously cached entries describe a closed, already converged reachable
    // graph in this immutable program. New callers can consume them directly.
    // Entries added by this solve are also in functions and participate in its
    // fixed point before this query returns.
    if (context.callEffects.contains(id))
      return;
    const auto *function = root && root->id == id
                               ? root
                               : context.program->findFunctionInstance(id);
    functions.emplace(id, function);
    context.callEffects.emplace(
        id, MirRetainingStoreEffects{.returnedOrigins = std::vector<Root>{},
                                     .complete = function != nullptr});
    if (function)
      discover.push_back(id);
  };
  const auto scan = [&](const MirBody &candidate,
                        std::optional<HirFunctionInstanceId> caller) {
    // Constructor and cleanup bodies participate in the caller's summary.
    // Discover their helper calls too, so later changes to those helpers
    // invalidate the enclosing function instead of leaving a provisional fact.
    std::set<const MirBody *> visited;
    const auto walk = [&](const auto &self, const MirBody &nested) -> void {
      if (!visited.insert(&nested).second)
        return;
      const auto cleanup = [&](const MirDropType &recipe) {
        for (const auto target : mirDropDestructorTargets(recipe))
          if (const auto *destructor =
                  context.program->findDestructorInstance(target))
            self(self, destructor->body);
      };
      for (const auto &drop : nested.dropObligations)
        cleanup(drop.dropType);
      for (const auto &block : nested.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.dispatch == CallDispatch::Static &&
              instruction.functionTarget) {
            const auto target = *instruction.functionTarget;
            add(target);
            if (caller)
              callers[target].insert(*caller);
          }
          if (instruction.constructorTarget)
            if (const auto *constructor =
                    context.program->findConstructorInstance(
                        *instruction.constructorTarget))
              self(self, constructor->body);
          if (instruction.elementCleanup)
            cleanup(*instruction.elementCleanup);
          if (instruction.replacementCleanup)
            cleanup(*instruction.replacementCleanup);
        }
    };
    walk(walk, candidate);
  };
  if (root)
    add(root->id);
  else
    scan(body, std::nullopt);
  while (!discover.empty()) {
    const auto id = discover.front();
    discover.pop_front();
    scan(functions.at(id)->body, id);
  }

  std::deque<HirFunctionInstanceId> pending;
  std::set<HirFunctionInstanceId> queued;
  for (const auto &[id, function] : functions)
    if (function) {
      pending.push_back(id);
      queued.insert(id);
    }
  while (!pending.empty()) {
    const auto id = pending.front();
    pending.pop_front();
    queued.erase(id);
    auto &current = context.callEffects.at(id);
    auto next = deriveEffects(*functions.at(id), context);
    // Unknown remains absorbing for permission, while known obligations
    // continue growing through callers and recursive components.
    next.complete = next.complete && current.complete;
    if (next.complete) {
      if (next.returnedOrigins && current.returnedOrigins) {
        auto &returned = *next.returnedOrigins;
        returned.insert(returned.end(), current.returnedOrigins->begin(),
                        current.returnedOrigins->end());
        std::sort(returned.begin(), returned.end());
        returned.erase(std::unique(returned.begin(), returned.end()),
                       returned.end());
      } else {
        next.returnedOrigins.reset();
      }
    } else {
      next.returnedOrigins.reset();
    }
    next.effects.insert(next.effects.end(), current.effects.begin(),
                        current.effects.end());
    std::sort(next.effects.begin(), next.effects.end());
    next.effects.erase(std::unique(next.effects.begin(), next.effects.end()),
                       next.effects.end());
    if (current.complete == next.complete && current.effects == next.effects &&
        current.writeEffects == next.writeEffects &&
        current.returnedOrigins == next.returnedOrigins)
      continue;
    current = std::move(next);
    for (const auto caller : callers[id])
      if (queued.insert(caller).second)
        pending.push_back(caller);
  }
}
} // namespace

struct MirCarrierAnalysis::Impl {
  AnalysisContext context;
  explicit Impl(const MirProgram &program) : context{.program = &program} {}
};

MirCarrierAnalysis::MirCarrierAnalysis(const MirProgram &program)
    : implementation(std::make_unique<Impl>(program)) {}
MirCarrierAnalysis::~MirCarrierAnalysis() = default;

const MirProgram &MirCarrierAnalysis::program() const {
  return *implementation->context.program;
}

MirCarrierFlow MirCarrierAnalysis::flow(const MirBody &body,
                                        bool includeBlockEntries) {
  auto &context = implementation->context;
  context.includeBlockEntries = false;
  solveCallEffects(body, context);
  context.includeBlockEntries = includeBlockEntries;
  return deriveFlow(body, context);
}

MirRetainingStoreEffects
MirCarrierAnalysis::retainingStores(HirFunctionInstanceId id) {
  auto &context = implementation->context;
  context.includeBlockEntries = false;
  const auto *function = context.program->findFunctionInstance(id);
  if (!function)
    return {};
  solveCallEffects(function->body, context, function);
  return context.callEffects.at(id);
}

MirCarrierFlow deriveMirCarrierFlow(const MirBody &body,
                                    const MirProgram *program,
                                    bool includeBlockEntries) {
  AnalysisContext context{.program = program};
  solveCallEffects(body, context);
  context.includeBlockEntries = includeBlockEntries;
  return deriveFlow(body, context);
}

MirRetainingStoreEffects
deriveMirRetainingStoreEffects(const MirFunctionInstance &function,
                               const MirProgram *program) {
  AnalysisContext context{.program = program};
  if (!program)
    return deriveEffects(function, context);
  solveCallEffects(function.body, context, &function);
  return context.callEffects.at(function.id);
}
} // namespace lang
