#include "loop_invariants.h"

#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <map>
#include <set>
#include <vector>

namespace lang::native_llvm {
namespace {
using namespace llvm;
using Edge = std::pair<const BasicBlock *, const BasicBlock *>;

bool concrete(Constant *value, unsigned depth = 0) {
  if (!value || depth > 16 || isa<UndefValue>(value) || isa<PoisonValue>(value))
    return false;
  if (isa<GlobalValue>(value) || isa<ConstantInt>(value) ||
      isa<ConstantPointerNull>(value))
    return true;
  if (auto *expression = dyn_cast<ConstantExpr>(value)) {
    for (auto &operand : expression->operands())
      if (!concrete(dyn_cast<Constant>(operand), depth + 1))
        return false;
    return true;
  }
  return false;
}

std::vector<BasicBlock *> orderFor(Loop &loop) {
  std::map<BasicBlock *, unsigned> degrees;
  unsigned instructions = 0;
  for (auto *block : loop.blocks()) {
    degrees[block] = 0;
    instructions += block->size();
    auto *terminator = block->getTerminator();
#if LLVM_VERSION_MAJOR >= 23
    if (!isa<UncondBrInst>(terminator) && !isa<CondBrInst>(terminator) &&
#else
    if (!isa<BranchInst>(terminator) &&
#endif
        !isa<SwitchInst>(terminator) && !isa<ReturnInst>(terminator) &&
        !isa<UnreachableInst>(terminator))
      return {};
  }
  if (degrees.size() > 96 || instructions > 2048)
    return {};
  for (auto *block : loop.blocks())
    for (auto *successor : successors(block))
      if (loop.contains(successor) && successor != loop.getHeader())
        ++degrees[successor];
  std::vector<BasicBlock *> ready, order;
  for (auto [block, degree] : degrees)
    if (!degree)
      ready.push_back(block);
  while (!ready.empty()) {
    auto *block = ready.back();
    ready.pop_back();
    order.push_back(block);
    for (auto *successor : successors(block))
      if (loop.contains(successor) && successor != loop.getHeader() &&
          --degrees[successor] == 0)
        ready.push_back(successor);
  }
  if (order.size() != degrees.size() || order.front() != loop.getHeader())
    return {};
  return order;
}

// Induction over loop iterations: every outside entry supplies the same
// non-poison constant. Assuming that constant for this iteration, propagate
// only exact SSA constants and loads from immutable, definitive globals. Every
// feasible backedge must supply that same constant. Unknown branch conditions
// retain both edges; opaque calls, mutable loads and unsupported value shapes
// produce no facts. The body without header backedges must be acyclic.
//
// This proves a value, not vptr immutability or a complete dispatch family.
// Side effects are neither moved nor removed here, and no path is reinstated
// after invalidation. Ordinary LLVM simplification may later remove branches
// whose conditions become constant as a consequence of the proved value.
bool prove(PHINode &root, Loop &loop, const std::vector<BasicBlock *> &order,
           Constant *initial) {
  std::map<Value *, Constant *> known{{&root, initial}};
  std::set<Edge> edges;
  const auto valueOf = [&](Value *value) -> Constant * {
    if (auto *constant = dyn_cast<Constant>(value))
      return concrete(constant) ? constant : nullptr;
    const auto found = known.find(value);
    return found == known.end() ? nullptr : found->second;
  };
  const auto &layout = root.getModule()->getDataLayout();
  for (auto *block : order) {
    bool reachable = block == loop.getHeader();
    for (auto *predecessor : predecessors(block))
      reachable |= edges.contains({predecessor, block});
    if (!reachable)
      continue;
    for (auto &instruction : *block) {
      if (&instruction == &root)
        continue;
      Constant *result = nullptr;
      if (auto *phi = dyn_cast<PHINode>(&instruction)) {
        bool seen = false, valid = block != loop.getHeader();
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          if (!edges.contains({phi->getIncomingBlock(i), block}))
            continue;
          auto *incoming = valueOf(phi->getIncomingValue(i));
          if (!incoming || (seen && incoming != result))
            valid = false;
          result = incoming;
          seen = true;
        }
        if (!seen || !valid)
          result = nullptr;
      } else if (auto *load = dyn_cast<LoadInst>(&instruction)) {
        auto *pointer = valueOf(load->getPointerOperand());
        const auto *global =
            pointer ? dyn_cast<GlobalVariable>(getUnderlyingObject(pointer))
                    : nullptr;
        if (!load->isVolatile() && !load->isAtomic() && global &&
            global->isConstant() && global->hasDefinitiveInitializer() &&
            !global->isExternallyInitialized())
          result =
              ConstantFoldLoadFromConstPtr(pointer, load->getType(), layout);
      } else if (isa<GetElementPtrInst>(instruction) ||
                 isa<CastInst>(instruction) || isa<ICmpInst>(instruction) ||
                 (isa<BinaryOperator>(instruction) &&
                  instruction.getType()->isIntegerTy()) ||
                 isa<SelectInst>(instruction)) {
        SmallVector<Constant *, 4> operands;
        bool complete = true;
        for (auto &operand : instruction.operands()) {
          auto *constant = valueOf(operand);
          complete &= constant != nullptr;
          operands.push_back(constant);
        }
        if (complete)
          result = ConstantFoldInstOperands(&instruction, operands, layout);
      }
      if (concrete(result))
        known[&instruction] = result;
    }
    auto *terminator = block->getTerminator();
    BasicBlock *selected = nullptr;
#if LLVM_VERSION_MAJOR >= 23
    if (auto *branch = dyn_cast<CondBrInst>(terminator)) {
#else
    if (auto *branch = dyn_cast<BranchInst>(terminator);
        branch && branch->isConditional()) {
#endif
      if (auto *condition =
              dyn_cast_or_null<ConstantInt>(valueOf(branch->getCondition())))
        selected = branch->getSuccessor(condition->isZero() ? 1 : 0);
    } else if (auto *branch = dyn_cast<SwitchInst>(terminator)) {
      if (auto *condition =
              dyn_cast_or_null<ConstantInt>(valueOf(branch->getCondition())))
        selected = branch->findCaseValue(condition)->getCaseSuccessor();
    }
    for (auto *successor : successors(block)) {
      if (selected && selected != successor)
        continue;
      if (successor == loop.getHeader()) {
        auto *incoming = root.getIncomingValueForBlock(block);
        if (!incoming || valueOf(incoming) != initial)
          return false;
      }
      if (loop.contains(successor))
        edges.emplace(block, successor);
    }
  }
  return true;
}

} // namespace

std::size_t foldInductivePointerLoops(llvm::Module &module) {
  using namespace llvm;
  std::size_t changed = 0;
  unsigned attempts = 0;
  for (auto &function : module) {
    if (function.isDeclaration())
      continue;
    DominatorTree dominance(function);
    LoopInfo loops(dominance);
    for (auto *loop : loops.getLoopsInPreorder()) {
      if (!loop->isInnermost())
        continue;
      auto order = orderFor(*loop);
      if (order.empty())
        continue;
      SmallVector<PHINode *, 4> roots;
      for (auto &phi : loop->getHeader()->phis())
        if (phi.getType()->isPointerTy())
          roots.push_back(&phi);
      for (auto *root : roots) {
        Constant *initial = nullptr;
        bool entry = false, valid = true;
        for (unsigned i = 0; i < root->getNumIncomingValues(); ++i) {
          if (loop->contains(root->getIncomingBlock(i)))
            continue;
          auto *constant = dyn_cast<Constant>(root->getIncomingValue(i));
          if (!concrete(constant) || (entry && initial != constant))
            valid = false;
          initial = constant;
          entry = true;
        }
        if (!entry || !valid)
          continue;
        if (++attempts > 32)
          return changed;
        if (prove(*root, *loop, order, initial)) {
          root->replaceAllUsesWith(initial);
          root->eraseFromParent();
          if (++changed == 16)
            return changed;
        }
      }
    }
  }
  return changed;
}

} // namespace lang::native_llvm
