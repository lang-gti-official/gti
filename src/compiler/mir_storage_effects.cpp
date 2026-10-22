#include "mir_storage_effects.h"

#include "gti/mir_effects.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <deque>
#include <map>
#include <utility>

namespace lang {
namespace {

using AddressKey = std::pair<MirBodyKind, std::size_t>;
AddressKey key(MirBodyAddress address) { return {address.kind, address.owner}; }

struct Invocation {
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  SymbolId initializerGuard = 0;
  std::vector<std::size_t> targets;
  bool unknown = false;
};

struct Node {
  MirBodyAddress address;
  const MirBody *body = nullptr;
  std::vector<MirPersistentStorageAccess> direct;
  std::vector<MirPersistentStorageAccess> summary;
  std::vector<Invocation> invocations;
  std::vector<std::size_t> callers;
  bool unknown = false;
};

bool sourceBody(const MirProgram &program, MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Function:
    return program.findFunctionInstance(address.owner)->definitionKind ==
           MirDefinitionKind::Source;
  case MirBodyKind::Constructor:
    return program.findConstructorInstance(address.owner)->definitionKind ==
           MirDefinitionKind::Source;
  case MirBodyKind::Destructor:
    return program.findDestructorInstance(address.owner)->definitionKind ==
           MirDefinitionKind::Source;
  default:
    return true;
  }
}

bool mergeAccess(std::vector<MirPersistentStorageAccess> &into,
                 MirPersistentStorageAccess access) {
  // SSA value identities belong to the callee, never to its caller. Constants
  // and field/base identities are portable; a dynamic index remains unknown.
  access.block = 0;
  access.instruction = 0;
  for (auto &projection : access.projections)
    projection.index = 0;
  const auto found =
      std::find_if(into.begin(), into.end(), [&](const auto &old) {
        return old.symbol == access.symbol &&
               old.projections == access.projections;
      });
  if (found == into.end()) {
    into.push_back(std::move(access));
    return true;
  }
  if (found->access == AccessMode::ReadOnly &&
      access.access == AccessMode::Mutable) {
    found->access = AccessMode::Mutable;
    return true;
  }
  return false;
}

bool unknownSourceInvocation(const MirBody &body,
                             const MirInstruction &instruction,
                             const MirInvocationTargets &targets) {
  if (!targets.boundary)
    return false;
  if (instruction.nativeCalleeType || instruction.nativeCallbackAdapter)
    return false; // Native invocation keeps its explicit unsafe ABI
                  // obligations.
  if (instruction.kind == MirInstructionKind::Drop) {
    for (const auto &event : instruction.lifecycle)
      if (event.kind == MirLifecycleEventKind::Drop && event.source != 0 &&
          event.source <= body.dropObligations.size() &&
          mirDropHasPolymorphicCleanup(
              body.dropObligations[event.source - 1].dropType))
        return true;
    return false;
  }
  if (instruction.kind == MirInstructionKind::CleanupBeforeAssign)
    return instruction.elementCleanup
               ? mirDropHasPolymorphicCleanup(*instruction.elementCleanup)
           : instruction.replacementCleanup
               ? mirDropHasPolymorphicCleanup(*instruction.replacementCleanup)
               : !targets.addresses.empty();
  // Generated construction without a source constructor executes no hidden
  // GTI body. Exact source constructors still enter the ordinary call graph.
  if (instruction.kind == MirInstructionKind::Construct ||
      instruction.kind == MirInstructionKind::InitializeBase)
    return false;
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic == IntrinsicKind::None &&
         instruction.synchronization.kind == SynchronizationOperationKind::None;
}

} // namespace

MirVerificationResult
verifyMirPersistentStorageEffects(const MirProgram &program) {
  std::vector<SymbolId> persistent;
  for (const auto &step : program.programInitializationPlan().steps)
    persistent.push_back(step.symbol);
  for (const auto &storage : program.threadLocalStorage())
    persistent.push_back(storage.symbol);
  std::sort(persistent.begin(), persistent.end());
  persistent.erase(std::unique(persistent.begin(), persistent.end()),
                   persistent.end());
  if (persistent.empty())
    return {};
  const auto isPersistent = [&](SymbolId symbol) {
    return std::binary_search(persistent.begin(), persistent.end(), symbol);
  };

  std::vector<Node> nodes;
  std::map<AddressKey, std::size_t> indices;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    if (!sourceBody(program, address))
      continue; // Native/unsafe obligations are not ambient GTI proofs.
    indices.emplace(key(address), nodes.size());
    nodes.push_back(
        {.address = address, .body = findMirBody(program, address)});
  }

  for (auto &node : nodes) {
    // Without a persistent symbol place there can be no canonical direct
    // access to that storage. Calls are still closed below for every body.
    if (std::any_of(node.body->places.begin(), node.body->places.end(),
                    [&](const auto &place) {
                      return place.root == MirPlaceRootKind::Symbol &&
                             isPersistent(place.symbol);
                    })) {
      std::vector<MirPersistentStorageAccess> direct;
      auto proof = verifyMirPersistentStorageLoanFlow(
          *node.body, node.address.owner, {}, &direct);
      if (!proof.valid())
        return proof;
      for (const auto &access : direct)
        if (isPersistent(access.symbol))
          mergeAccess(node.summary, access);
      node.direct = node.summary;
    }
    for (const auto &block : node.body->blocks) {
      if (!block.reachable)
        continue;
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone)
          continue;
        const auto targets =
            mirInvocationTargets(program, *node.body, instruction);
        Invocation invocation{
            .block = block.id,
            .instruction = instruction.id,
            .initializerGuard =
                instruction.kind == MirInstructionKind::EnsureThreadLocal
                    ? instruction.threadLocalStorage
                    : 0,
            .unknown =
                unknownSourceInvocation(*node.body, instruction, targets)};
        for (const auto address : targets.addresses) {
          const auto found = indices.find(key(address));
          if (found != indices.end())
            invocation.targets.push_back(found->second);
        }
        if (invocation.unknown || !invocation.targets.empty()) {
          node.unknown |= invocation.unknown;
          node.invocations.push_back(std::move(invocation));
        }
      }
    }
  }

  for (std::size_t caller = 0; caller < nodes.size(); ++caller)
    for (const auto &invocation : nodes[caller].invocations)
      for (const auto target : invocation.targets)
        nodes[target].callers.push_back(caller);

  // A finite monotone closure over structural place/access facts, including
  // recursion. Source order and effect-summary claims confer no authority.
  std::deque<std::size_t> work;
  std::vector<bool> queued(nodes.size(), true);
  for (std::size_t index = 0; index < nodes.size(); ++index)
    work.push_back(index);
  while (!work.empty()) {
    const auto target = work.front();
    work.pop_front();
    queued[target] = false;
    for (const auto caller : nodes[target].callers) {
      bool changed = false;
      if (nodes[target].unknown && !nodes[caller].unknown) {
        nodes[caller].unknown = true;
        changed = true;
      }
      // Recursive self-edges must not mutate a vector while iterating it.
      if (caller != target)
        for (const auto &access : nodes[target].summary)
          changed |= mergeAccess(nodes[caller].summary, access);
      if (changed && !queued[caller]) {
        queued[caller] = true;
        work.push_back(caller);
      }
    }
  }

  for (const auto &node : nodes) {
    if (node.body->loans.empty())
      continue;
    std::vector<MirPersistentStorageAccess> hidden;
    std::map<MirInstructionId, const Invocation *> roots;
    for (const auto &invocation : node.invocations) {
      const bool relevant =
          invocation.unknown ||
          std::any_of(invocation.targets.begin(), invocation.targets.end(),
                      [&](auto target) {
                        return nodes[target].unknown ||
                               !nodes[target].summary.empty();
                      });
      if (!relevant)
        continue;
      roots.emplace(invocation.instruction, &invocation);
      hidden.push_back(
          {.block = invocation.block, .instruction = invocation.instruction});
    }
    if (hidden.empty())
      continue;
    using QueryKey = std::pair<MirInstructionId, std::vector<SymbolId>>;
    std::map<QueryKey, std::vector<MirPersistentStorageAccess>> cache;
    const MirPersistentStorageEffectQuery query =
        [&](MirBlockId block, MirInstructionId instruction,
            std::span<const SymbolId> readySymbols) {
          std::vector<SymbolId> ready;
          for (const auto symbol : readySymbols)
            if (program.findThreadLocalStorage(symbol))
              ready.push_back(symbol);
          std::sort(ready.begin(), ready.end());
          ready.erase(std::unique(ready.begin(), ready.end()), ready.end());
          QueryKey queryKey{instruction, std::move(ready)};
          if (const auto found = cache.find(queryKey); found != cache.end())
            return found->second;
          std::vector<MirPersistentStorageAccess> accesses;
          std::vector<bool> visited(nodes.size(), false);
          std::vector<std::size_t> pending;
          bool unknown = false;
          const auto visitInvocation = [&](const Invocation &invocation) {
            if (invocation.initializerGuard != 0 &&
                std::binary_search(queryKey.second.begin(),
                                   queryKey.second.end(),
                                   invocation.initializerGuard))
              return;
            unknown |= invocation.unknown;
            for (const auto target : invocation.targets)
              if (!visited[target] &&
                  (nodes[target].unknown || !nodes[target].summary.empty())) {
                visited[target] = true;
                pending.push_back(target);
              }
          };
          const auto root = roots.find(instruction);
          if (root == roots.end() || root->second->block != block)
            return accesses;
          visitInvocation(*root->second);
          while (!pending.empty()) {
            const auto target = pending.back();
            pending.pop_back();
            for (const auto &access : nodes[target].direct)
              mergeAccess(accesses, access);
            for (const auto &invocation : nodes[target].invocations)
              visitInvocation(invocation);
          }
          if (unknown)
            for (const auto symbol : persistent)
              mergeAccess(accesses,
                          {.symbol = symbol, .access = AccessMode::Mutable});
          cache.emplace(std::move(queryKey), accesses);
          return accesses;
        };
    auto proof = verifyMirPersistentStorageLoanFlow(
        *node.body, node.address.owner, hidden, nullptr, &query);
    if (!proof.valid())
      return proof;
  }
  return {};
}

} // namespace lang
