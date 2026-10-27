#include "reachability.h"
#include "gti/mir_effects.h"
#include "native_c_contract.h"

#include <algorithm>
#include <stdexcept>

namespace lang::native_llvm {

bool Reachability::contains(MirBodyAddress address) const {
  return std::find(bodies.begin(), bodies.end(), address) != bodies.end();
}
bool Reachability::contains(LoweredGeneratedItemIdentity identity) const {
  return std::find(generated.begin(), generated.end(), identity) !=
         generated.end();
}

std::vector<MirBodyAddress>
dispatchSourceBodies(const LoweredProgram &program,
                     HirClassInstanceId activeClass) {
  const auto *active = program.mir().findClassInstance(activeClass);
  if (!active)
    throw std::logic_error("LLVM dispatch demand has no sealed active class");
  std::vector<MirBodyAddress> result;
  for (const auto &selection : active->virtualDispatch) {
    const auto *target = program.mir().findFunctionInstance(selection.target);
    if (!target)
      throw std::logic_error("LLVM dispatch demand lost its selected target");
    // Unsupported pure/declaration/runtime targets retain their independent
    // representation/admission gates; they never acquire fabricated bodies.
    if (target->pureVirtual ||
        target->definitionKind != MirDefinitionKind::Source)
      continue;
    const MirBodyAddress address{MirBodyKind::Function, target->id};
    const auto *body = program.findBody(address);
    if (!body || body->role != LoweredBodyRole::SourceExecutable ||
        !findMirBody(program.mir(), address))
      throw std::logic_error(
          "LLVM selected source dispatch target has no sealed executable body");
    if (std::find(result.begin(), result.end(), address) == result.end())
      result.push_back(address);
  }
  return result;
}

std::optional<HirClassInstanceId>
dynamicOwnerAllocation(const MirProgram &program,
                       const MirInstruction &instruction) {
  if (instruction.tombstone ||
      (instruction.kind != MirInstructionKind::AllocateObject &&
       instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner) ||
      instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.type.arguments.front().kind != SemanticType::Class)
    return std::nullopt;
  const MirClassInstance *selected = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == instruction.info.type.arguments.front()) {
      if (selected)
        throw std::logic_error(
            "LLVM dynamic owner allocation has ambiguous class identity");
      selected = &candidate;
    }
  if (!selected)
    throw std::logic_error("LLVM dynamic owner allocation has no sealed class");
  if (!selected->polymorphic)
    return std::nullopt;
  return selected->id;
}

Reachability reachableProgram(const LoweredProgram &program) {
  Reachability result;
  const auto &mir = program.mir();
  auto body = [&](MirBodyAddress address) {
    if (program.findBody(address) && !result.contains(address))
      result.bodies.push_back(address);
  };
  auto generated = [&](LoweredGeneratedItemIdentity identity) {
    if (!result.contains(identity))
      result.generated.push_back(identity);
  };
  const auto active = [&](HirClassInstanceId owner) {
    for (const auto target : dispatchSourceBodies(program, owner))
      body(target);
  };
  const auto virtualCall = [&](const MirInstruction &instruction) {
    const auto *selected =
        instruction.functionTarget
            ? mir.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    const auto *owner = selected && selected->owner
                            ? mir.findClassInstance(*selected->owner)
                            : nullptr;
    if (!owner)
      return false;
    std::vector<HirFunctionInstanceId> roots;
    for (const auto &row : owner->virtualDispatch)
      if (row.target == selected->id)
        roots.push_back(row.root);
    if (roots.empty())
      return false;
    // Retain every possible sealed implementation, not just the receiver's
    // statically named target. This is a conservative call-graph union, not
    // devirtualization or a proof of the receiver's dynamic class.
    for (const auto &candidate : mir.classInstances())
      for (const auto &row : candidate.virtualDispatch) {
        if (std::find(roots.begin(), roots.end(), row.root) == roots.end())
          continue;
        const auto *target = mir.findFunctionInstance(row.target);
        if (!target)
          throw std::logic_error(
              "LLVM virtual dependency lost its sealed target");
        if (!target->pureVirtual &&
            target->definitionKind == MirDefinitionKind::Source) {
          const MirBodyAddress address{MirBodyKind::Function, target->id};
          const auto *definition = program.findBody(address);
          if (!definition ||
              definition->role != LoweredBodyRole::SourceExecutable ||
              !findMirBody(mir, address))
            throw std::logic_error(
                "LLVM virtual dependency has no sealed body");
          body(address);
        }
      }
    return true;
  };
  const auto cleanup = [&](const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &recipe = *pending.back();
      pending.pop_back();
      if (recipe.destructor)
        body({MirBodyKind::Destructor, *recipe.destructor});
      // A dynamic owner's complete cleanup is demanded where that complete
      // object was allocated, below; every runtime pointee came from one.
      if (!recipe.polymorphicCleanup &&
          recipe.type.kind == SemanticType::Class && recipe.classInstance)
        active(*recipe.classInstance);
      for (const auto &child : recipe.aggregate)
        pending.push_back(&child);
    }
  };
  body({MirBodyKind::Module, 0});
  if (mir.hostedStartupPlan())
    body({MirBodyKind::HostedStartup, mir.hostedStartupPlan()->entry});
  if (program.embeddingPlan())
    for (const auto &selected : program.embeddingPlan()->exports)
      body({MirBodyKind::Function, selected.function});
  for (const auto &function : mir.functionInstances())
    if (function.linkage == LanguageLinkage::C &&
        function.definitionKind == MirDefinitionKind::Source)
      body({MirBodyKind::Function, function.id});
  for (const auto &storage : mir.threadLocalStorage()) {
    body({MirBodyKind::ThreadLocalInitializer, storage.symbol});
    cleanup(storage.cleanup);
  }

  std::size_t nextBody = 0, nextGenerated = 0;
  while (nextBody < result.bodies.size() ||
         nextGenerated < result.generated.size()) {
    while (nextBody < result.bodies.size()) {
      const auto address = result.bodies[nextBody++];
      const auto *row = program.findBody(address);
      const auto *input = findMirBody(mir, address);
      if (!row || !input)
        throw std::logic_error("LLVM dependency has no sealed body");
      if (address.kind == MirBodyKind::Constructor) {
        const auto *constructor = mir.findConstructorInstance(address.owner);
        if (!constructor)
          throw std::logic_error(
              "LLVM required constructor has no sealed instance");
        if (constructor->dispatchState)
          active(constructor->dispatchState->activeClass);
      }
      for (auto identity : row->requiredGeneratedItems)
        generated(identity);
      for (const auto &obligation : input->dropObligations)
        cleanup(obligation.dropType);
      for (const auto &block : input->blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.tombstone)
            continue;
          if (instruction.functionTarget)
            body({MirBodyKind::Function, *instruction.functionTarget});
          if (const auto owned = dynamicOwnerAllocation(mir, instruction)) {
            // The allocated class's descriptors carry its complete cleanup.
            active(*owned);
            const auto &object = *mir.findClassInstance(*owned);
            if (const auto complete = mirUniqueCleanupType(mir, object.type))
              cleanup(*complete);
            else if (object.requiresActiveCleanup)
              throw std::logic_error(
                  "LLVM dynamic owner lost its complete cleanup recipe");
          }
          if (instruction.functionPointerFunctionTarget)
            body({MirBodyKind::Function,
                  *instruction.functionPointerFunctionTarget});
          if (instruction.constructorTarget)
            body({MirBodyKind::Constructor, *instruction.constructorTarget});
          const bool generatedCopy =
              (instruction.kind == MirInstructionKind::Load &&
               instruction.operands.size() == 1 &&
               instruction.operands.front().kind == MirOperandKind::Copy) ||
              (instruction.kind == MirInstructionKind::Construct &&
               !instruction.constructorTarget &&
               instruction.constructorKind == ConstructorKind::Copy);
          const bool generatedMove =
              (instruction.kind == MirInstructionKind::Move &&
               instruction.operands.size() == 1 &&
               instruction.operands.front().kind == MirOperandKind::Move) ||
              (instruction.kind == MirInstructionKind::Construct &&
               !instruction.constructorTarget &&
               instruction.constructorKind == ConstructorKind::Move);
          if (instruction.info.type.kind == SemanticType::Class &&
              (generatedCopy || generatedMove)) {
            const MirClassInstance *object = nullptr;
            for (const auto &candidate : mir.classInstances())
              if (candidate.type == instruction.info.type) {
                if (object)
                  throw std::logic_error(
                      "LLVM transfer demand has ambiguous class identity");
                object = &candidate;
              }
            if (!object)
              throw std::logic_error(
                  "LLVM transfer demand has no sealed class");
            const auto status = generatedCopy ? object->copyConstructor
                                              : object->moveConstructor;
            if (status == SpecialMemberStatus::Generated)
              active(object->id);
            // Demand is not admission: nested, by-value, replacement and
            // uncertain source-obligation transfers retain their own gates.
          }
          if (!instruction.constructorTarget &&
              instruction.constructorKind == ConstructorKind::Ordinary &&
              instruction.operands.empty() &&
              instruction.parameterTypes.empty() &&
              instruction.info.type.kind == SemanticType::Class &&
              (instruction.kind == MirInstructionKind::Construct ||
               instruction.kind == MirInstructionKind::InitializeBase)) {
            const MirClassInstance *object = nullptr;
            for (const auto &candidate : mir.classInstances())
              if (candidate.type == instruction.info.type) {
                if (object)
                  throw std::logic_error(
                      "LLVM generated default has ambiguous class identity");
                object = &candidate;
              }
            if (!object)
              throw std::logic_error(
                  "LLVM generated default has no sealed class");
            if (object->defaultConstructor == SpecialMemberStatus::Generated &&
                !object->generatedDefaultRequiresSchedule) {
              if (instruction.kind == MirInstructionKind::InitializeBase) {
                const auto *constructor =
                    address.kind == MirBodyKind::Constructor
                        ? mir.findConstructorInstance(address.owner)
                        : nullptr;
                if (!constructor || instruction.constructorInitializer == 0 ||
                    instruction.constructorInitializer >
                        constructor->initializers.size())
                  throw std::logic_error(
                      "LLVM generated base demand lost its source stage");
                const auto &initializer =
                    constructor
                        ->initializers[instruction.constructorInitializer - 1];
                if (initializer.kind !=
                        ConstructorInitializerTargetKind::Base ||
                    initializer.base != object->id ||
                    initializer.targetType != object->type ||
                    !initializer.generatedDefault ||
                    initializer.constructorTarget ||
                    !initializer.arguments.empty())
                  throw std::logic_error(
                      "LLVM generated base demand lost its selected identity");
              }
              active(object->id);
            }
          }
          if (instruction.lambdaTarget)
            body({MirBodyKind::Lambda, *instruction.lambdaTarget});
          if (instruction.functionPointerLambdaTarget)
            body({MirBodyKind::Lambda,
                  *instruction.functionPointerLambdaTarget});
          if (instruction.bodyTarget)
            body(*instruction.bodyTarget);
          if (instruction.elementCleanup)
            cleanup(*instruction.elementCleanup);
          if (instruction.replacementCleanup)
            cleanup(*instruction.replacementCleanup);
          const bool nativeCall = nativeCIndirectCallContract(
              mir, program.target(), *input, instruction);
          const bool callback = nativeCCallbackStageContract(
              mir, program.target(), *input, instruction);
          if ((instruction.dispatch == CallDispatch::Virtual &&
               !virtualCall(instruction)) ||
              (instruction.nativeCalleeType && !nativeCall) ||
              (instruction.callableInvocation && !instruction.lambdaTarget &&
               !instruction.functionTarget && !nativeCall) ||
              (instruction.nativeCallbackAdapter && !callback) ||
              instruction.threadTaskAdapter ||
              !instruction.scopedThreadTasks.empty())
            result.conservative = true;
        }
    }
    while (nextGenerated < result.generated.size()) {
      const auto *item =
          program.findGeneratedItem(result.generated[nextGenerated++]);
      if (!item)
        throw std::logic_error("LLVM dependency has no sealed generated item");
      if (item->sourceKind == LoweredGeneratedItemSourceKind::Body)
        body(item->sourceBody);
      else
        result.conservative = true;
      for (auto dependency : item->dependencies)
        generated(dependency);
    }
  }
  // Do not omit rootless library exports or dependency forms not yet modeled.
  if (!mir.hostedStartupPlan() && !program.embeddingPlan())
    result.conservative = true;
  if (result.conservative) {
    for (const auto &row : program.bodies())
      if (row.role == LoweredBodyRole::SourceExecutable)
        body(row.identity.address);
    for (const auto &item : program.generatedItems())
      generated(item.identity);
  }
  // Diagnostic/emission order follows sealed inventory, not graph traversal.
  std::vector<MirBodyAddress> ordered;
  for (const auto &row : program.bodies())
    if (result.contains(row.identity.address))
      ordered.push_back(row.identity.address);
  result.bodies = std::move(ordered);
  return result;
}

} // namespace lang::native_llvm
