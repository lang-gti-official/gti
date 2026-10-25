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
  const auto cleanup = [&](const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &recipe = *pending.back();
      pending.pop_back();
      if (recipe.destructor)
        body({MirBodyKind::Destructor, *recipe.destructor});
      if (recipe.polymorphicCleanup)
        result.conservative = true;
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
              instruction.nativeCalleeType ||
              (instruction.callableInvocation && !instruction.functionTarget) ||
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
