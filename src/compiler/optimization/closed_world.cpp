#include "gti/optimization/closed_world.h"

#include <algorithm>
#include <unordered_map>

namespace lang {
namespace {
enum class FactKind { Unset, Exact, Unknown };

struct Fact {
  FactKind kind = FactKind::Unset;
  SemanticType type = SemanticType::Unknown;
};

[[nodiscard]] bool join(Fact &target, const Fact &incoming) {
  if (incoming.kind == FactKind::Unset || target.kind == FactKind::Unknown)
    return false;
  if (incoming.kind == FactKind::Unknown ||
      (target.kind == FactKind::Exact && target.type != incoming.type)) {
    target = {.kind = FactKind::Unknown};
    return true;
  }
  if (target.kind == FactKind::Exact)
    return false;
  target = incoming;
  return true;
}

using ParameterKey = std::pair<HirFunctionInstanceId, std::size_t>;

// One visible call's contribution to a parameter: an exact class, unknown, or
// the current fact of a forwarded caller parameter.
struct Contribution {
  ParameterKey parameter;
  Fact fact;
  std::optional<ParameterKey> forwarded;
};

[[nodiscard]] bool classReferenceParameter(const SemanticType &type) {
  return type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
         type.arguments.front().kind == SemanticType::Class;
}

[[nodiscard]] bool concreteClass(const MirProgram &program,
                                 const SemanticType &type) {
  if (type.kind != SemanticType::Class)
    return false;
  const auto &classes = program.classInstances();
  const auto found = std::find_if(
      classes.begin(), classes.end(),
      [&](const MirClassInstance &row) { return row.type == type; });
  return found != classes.end() && !found->abstract &&
         (found->kind == ClassKind::Class || found->kind == ClassKind::Struct);
}

// A binding or temporary object, or a field of one, is a complete object or
// member subobject: its dynamic class is its static class. Dereferences, raw
// projections, base subobjects, `this`, loans and values are excluded.
[[nodiscard]] bool exactObjectPlace(const MirProgram &program,
                                    const MirPlace &place) {
  return ((place.root == MirPlaceRootKind::Binding && place.capture == 0) ||
          place.root == MirPlaceRootKind::Temporary) &&
         std::all_of(place.projections.begin(), place.projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::Field;
                     }) &&
         concreteClass(program, place.type);
}

// The reference itself is never replaced, written through a mutable borrow or
// exposed to raw memory, so every dereference names the caller's referent.
[[nodiscard]] bool stableReference(const MirBody &body, HirBindingId binding) {
  const auto bare = [&](MirPlaceId id) {
    const MirPlace *place = body.findPlace(id);
    return place != nullptr && place->root == MirPlaceRootKind::Binding &&
           place->binding == binding && place->projections.empty();
  };
  for (const MirBlock &block : body.blocks)
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      if (instruction.destination && bare(*instruction.destination) &&
          instruction.kind != MirInstructionKind::Drop &&
          instruction.kind != MirInstructionKind::Lifecycle)
        return false;
      const auto exposes = [&](const MirOperand &operand) {
        return (operand.kind == MirOperandKind::BorrowWrite ||
                operand.kind == MirOperandKind::Address) &&
               bare(operand.place);
      };
      if (std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      exposes) ||
          (instruction.receiver && exposes(*instruction.receiver)))
        return false;
    }
  return true;
}

struct Solution {
  std::vector<bool> closed;
  std::map<ParameterKey, SemanticType> exact;
  std::size_t iterations = 0;
};

class Analysis {
public:
  explicit Analysis(const MirProgram &program) : program(program) {}

  Solution run() {
    const auto &functions = program.functionInstances();
    closed.assign(functions.size(), true);
    markOpenRoots();
    const auto addresses = enumerateMirBodyAddresses(program);
    for (const MirBodyAddress address : addresses)
      if (const MirBody *body = findMirBody(program, address))
        markOpenReferences(*body);
    for (const MirFunctionInstance &function : functions)
      if (closed[function.id - 1])
        for (std::size_t index = 0; index < function.parameterTypes.size();
             ++index)
          if (classReferenceParameter(function.parameterTypes[index]))
            facts[{function.id, index}] = {};
    for (const MirBodyAddress address : addresses)
      if (const MirBody *body = findMirBody(program, address))
        collectCalls(address, *body);

    Solution result;
    // Every update moves one parameter up a three-point lattice, so the
    // solver stabilizes after at most 2 * |parameters| + 1 rounds.
    bool changed = true;
    while (changed) {
      changed = false;
      ++result.iterations;
      for (const Contribution &contribution : contributions) {
        Fact incoming = contribution.fact;
        if (contribution.forwarded) {
          const auto found = facts.find(*contribution.forwarded);
          incoming = found == facts.end() ? Fact{.kind = FactKind::Unknown}
                                          : found->second;
        }
        changed |= join(facts[contribution.parameter], incoming);
      }
    }
    result.closed = closed;
    for (const auto &[key, fact] : facts) {
      const MirFunctionInstance *function =
          program.findFunctionInstance(key.first);
      if (fact.kind == FactKind::Exact && stable(*function, key.second))
        result.exact.emplace(key, fact.type);
    }
    return result;
  }

private:
  void open(HirFunctionInstanceId id) {
    if (id != 0 && id <= closed.size())
      closed[id - 1] = false;
  }

  void markOpenRoots() {
    for (const MirFunctionInstance &function : program.functionInstances()) {
      if (function.definitionKind != MirDefinitionKind::Source ||
          function.linkage != LanguageLinkage::Gti || function.virtualMethod ||
          function.pureVirtual || function.overrideMethod ||
          !function.virtualRoots.empty() ||
          function.entryKind != ProgramEntryKind::None)
        open(function.id);
      if (function.entryArgumentAppendTarget)
        open(*function.entryArgumentAppendTarget);
      for (const MirCallableParameter &parameter :
           function.callableParameters) {
        for (const MirCallableSignature &signature : parameter.signatures)
          if (signature.functionTarget)
            open(*signature.functionTarget);
        for (const MirCallableForwarding &forwarding : parameter.forwardings)
          if (forwarding.functionTarget)
            open(*forwarding.functionTarget);
      }
    }
    for (const MirNativeCallbackAdapter &adapter :
         program.nativeCallbackAdapters())
      open(adapter.target);
    for (const MirThreadTaskAdapter &adapter : program.threadTaskAdapters())
      if (adapter.functionTarget)
        open(*adapter.functionTarget);
    if (const auto &plan = program.hostedStartupPlan()) {
      open(plan->entry);
      open(plan->appendFunction);
      open(plan->cThreadBegin);
      open(plan->cThreadEnd);
    }
    if (const auto &plan = program.embeddingPlan())
      for (const MirEmbeddingExport &row : plan->exports)
        open(row.function);
  }

  // Tombstoned instructions are inspected too: a reference is a reference
  // whether or not a later cleanup removes it.
  void markOpenReferences(const MirBody &body) {
    for (const MirBlock &block : body.blocks)
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.functionTarget && !directCall(instruction))
          open(*instruction.functionTarget);
        if (instruction.nativeCallbackAdapter)
          if (const auto *adapter = program.findNativeCallbackAdapter(
                  *instruction.nativeCallbackAdapter))
            open(adapter->target);
        if (instruction.threadTaskAdapter)
          if (const auto *adapter =
                  program.findThreadTaskAdapter(*instruction.threadTaskAdapter);
              adapter != nullptr && adapter->functionTarget)
            open(*adapter->functionTarget);
      }
  }

  [[nodiscard]] static bool directCall(const MirInstruction &instruction) {
    return instruction.kind == MirInstructionKind::Call &&
           instruction.dispatch == CallDispatch::Static &&
           instruction.intrinsic == IntrinsicKind::None &&
           !instruction.nativeCalleeType &&
           !instruction.nativeCallbackAdapter &&
           !instruction.threadTaskAdapter && !instruction.lambdaTarget &&
           !instruction.constructorTarget && !instruction.bodyTarget;
  }

  void collectCalls(MirBodyAddress address, const MirBody &body) {
    std::unordered_map<MirInstructionId, const MirInstruction *> definitions;
    for (const MirBlock &block : body.blocks)
      for (const MirInstruction &instruction : block.instructions)
        definitions.emplace(instruction.id, &instruction);
    const MirFunctionInstance *caller =
        address.kind == MirBodyKind::Function
            ? program.findFunctionInstance(address.owner)
            : nullptr;
    for (const MirBlock &block : body.blocks)
      for (const MirInstruction &call : block.instructions) {
        if (call.tombstone || !call.functionTarget || !directCall(call))
          continue;
        const MirFunctionInstance *callee =
            program.findFunctionInstance(*call.functionTarget);
        if (callee == nullptr || !closed[callee->id - 1])
          continue;
        for (std::size_t index = 0; index < callee->parameterTypes.size();
             ++index) {
          if (!classReferenceParameter(callee->parameterTypes[index]))
            continue;
          Contribution contribution{.parameter = {callee->id, index},
                                    .fact = {.kind = FactKind::Unknown}};
          if (call.operands.size() == callee->parameterTypes.size())
            classify(body, definitions, caller, call.operands[index],
                     contribution);
          contributions.push_back(std::move(contribution));
        }
      }
  }

  void classify(const MirBody &body,
                const std::unordered_map<MirInstructionId,
                                         const MirInstruction *> &definitions,
                const MirFunctionInstance *caller, const MirOperand &operand,
                Contribution &contribution) {
    const MirValue *value = operand.kind == MirOperandKind::Value
                                ? body.findValue(operand.value)
                                : nullptr;
    const auto definition = value == nullptr
                                ? definitions.end()
                                : definitions.find(value->definition);
    const MirInstruction *input =
        definition == definitions.end() ? nullptr : definition->second;
    if (input == nullptr || input->tombstone ||
        input->kind != MirInstructionKind::CallInput ||
        input->callInputRole != MirCallInputRole::Argument ||
        (input->callInputKind != HirCallInputKind::ReadBorrow &&
         input->callInputKind != HirCallInputKind::MutableBorrow) ||
        input->operands.size() != 1 ||
        (input->operands.front().kind != MirOperandKind::BorrowRead &&
         input->operands.front().kind != MirOperandKind::BorrowWrite))
      return;
    const MirPlace *place = body.findPlace(input->operands.front().place);
    if (place == nullptr)
      return;
    if (exactObjectPlace(program, *place)) {
      contribution.fact = {.kind = FactKind::Exact, .type = place->type};
      return;
    }
    if (caller == nullptr || place->root != MirPlaceRootKind::Binding ||
        place->capture != 0 || place->projections.size() != 1 ||
        place->projections.front().kind != MirProjectionKind::Dereference)
      return;
    const auto parameter =
        std::find(caller->parameterBindings.begin(),
                  caller->parameterBindings.end(), place->binding);
    const auto index = static_cast<std::size_t>(
        std::distance(caller->parameterBindings.begin(), parameter));
    if (parameter == caller->parameterBindings.end() || !stable(*caller, index))
      return;
    contribution.forwarded = ParameterKey{caller->id, index};
  }

  [[nodiscard]] bool stable(const MirFunctionInstance &function,
                            std::size_t parameter) {
    const auto key = ParameterKey{function.id, parameter};
    if (const auto found = stability.find(key); found != stability.end())
      return found->second;
    const bool result =
        parameter < function.parameterBindings.size() &&
        stableReference(function.body, function.parameterBindings[parameter]);
    stability.emplace(key, result);
    return result;
  }

  const MirProgram &program;
  std::vector<bool> closed;
  std::map<ParameterKey, Fact> facts;
  std::vector<Contribution> contributions;
  std::map<ParameterKey, bool> stability;
};
} // namespace

bool MirClosedWorldFacts::closed(HirFunctionInstanceId function) const {
  return function != 0 && function <= closed_.size() && closed_[function - 1];
}

const SemanticType *
MirClosedWorldFacts::exactParameterClass(HirFunctionInstanceId function,
                                         std::size_t parameter) const {
  const auto found = exactParameters_.find({function, parameter});
  return found == exactParameters_.end() ? nullptr : &found->second;
}

MirClosedWorldFacts analyzeMirClosedWorldFacts(const MirProgram &program) {
  Solution solution = Analysis(program).run();
  MirClosedWorldFacts result;
  result.closed_ = std::move(solution.closed);
  result.exactParameters_ = std::move(solution.exact);
  result.iterations_ = solution.iterations;
  return result;
}

} // namespace lang
