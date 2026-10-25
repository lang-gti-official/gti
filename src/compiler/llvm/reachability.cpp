#include "reachability.h"

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
  const auto cleanup = [&](const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &recipe = *pending.back();
      pending.pop_back();
      if (recipe.destructor)
        body({MirBodyKind::Destructor, *recipe.destructor});
      if (recipe.polymorphicCleanup)
        result.conservative = true;
      else if (recipe.type.kind == SemanticType::Class && recipe.classInstance)
        active(*recipe.classInstance);
      for (const auto &child : recipe.aggregate)
        pending.push_back(&child);
    }
  };
  body({MirBodyKind::Module, 0});
  if (mir.hostedStartupPlan())
    body({MirBodyKind::HostedStartup, mir.hostedStartupPlan()->entry});
  // Libraries/embedding, unknown dispatch and declaration-backed generated
  // storage require a broader root model before they can omit anything.
  result.conservative = program.embeddingPlan().has_value();
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
          if (instruction.functionPointerFunctionTarget)
            body({MirBodyKind::Function,
                  *instruction.functionPointerFunctionTarget});
          if (instruction.constructorTarget)
            body({MirBodyKind::Constructor, *instruction.constructorTarget});
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
          if (instruction.dispatch == CallDispatch::Virtual ||
              instruction.nativeCalleeType || instruction.callableInvocation ||
              instruction.nativeCallbackAdapter ||
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
  if (!mir.hostedStartupPlan())
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
