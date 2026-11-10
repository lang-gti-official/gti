#include "session.h"
#include "abi.h"
#include "checked_offsets.h"
#include "checked_ranges.h"
#include "checked_reductions.h"
#include "debug_info.h"
#include "dispatch.h"
#include "instrumentation.h"
#include "loop_invariants.h"
#include "private_atomics.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/HotColdSplitting.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace lang::native_llvm {

bool specializeBoundedRecursion(llvm::Module &module) {
  // This is a bounded first profile of call-site range specialization. These
  // operations bound every defined result without a memory, path, nowrap or
  // undef assumption. In particular, never look through freeze: freezing a
  // poison remainder may yield a value outside the remainder's domain.
  const auto maximum = [](llvm::Value *value) -> std::optional<llvm::APInt> {
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
      if (binary->getOpcode() == llvm::Instruction::URem) {
        const auto *divisor =
            llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
        if (divisor && !divisor->isZero())
          return divisor->getValue() - 1;
      }
      if (binary->getOpcode() == llvm::Instruction::And)
        for (auto &operand : binary->operands())
          if (const auto *mask =
                  llvm::dyn_cast<llvm::ConstantInt>(operand.get()))
            return mask->getValue();
    }
    if (const auto *extension = llvm::dyn_cast<llvm::ZExtInst>(value))
      return llvm::APInt::getLowBitsSet(
          value->getType()->getIntegerBitWidth(),
          extension->getSrcTy()->getIntegerBitWidth());
    return std::nullopt;
  };
  constexpr unsigned maximumClones = 8;
  constexpr unsigned maximumBodyInstructions = 192;
  constexpr unsigned maximumAddedInstructions = 1024;
  llvm::SmallVector<llvm::Function *> candidates;
  for (auto &function : module)
    if (!function.isDeclaration() && function.hasLocalLinkage() &&
        !function.isVarArg() && !function.hasPersonalityFn() &&
        !function.hasAddressTaken() && !function.hasComdat() &&
        !function.hasFnAttribute("gti.range-specialized") &&
        !function.hasFnAttribute(llvm::Attribute::OptimizeNone) &&
        !function.hasFnAttribute(llvm::Attribute::OptimizeForSize) &&
        !function.hasFnAttribute(llvm::Attribute::MinSize) &&
        !function.hasFnAttribute(llvm::Attribute::Convergent) &&
        !function.hasFnAttribute(llvm::Attribute::NoDuplicate) &&
        !function.hasFnAttribute(llvm::Attribute::ReturnsTwice) &&
        function.size() <= 32 &&
        function.getInstructionCount() <= maximumBodyInstructions)
      candidates.push_back(&function);
  unsigned clones = 0, added = 0;
  for (auto *function : candidates) {
    llvm::SmallVector<llvm::CallInst *> recursive;
    bool eligible = true, checkedArithmetic = false;
    for (auto &block : *function) {
      if (block.hasAddressTaken())
        eligible = false;
      for (auto &instruction : block) {
        if (llvm::isa<llvm::WithOverflowInst>(instruction))
          checkedArithmetic = true;
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
          if (!llvm::isa<llvm::CallInst>(call) || call->isConvergent() ||
              call->cannotDuplicate() || call->getNumOperandBundles() ||
              call->hasFnAttr(llvm::Attribute::ReturnsTwice) ||
              llvm::cast<llvm::CallInst>(call)->isMustTailCall())
            eligible = false;
          if (call->getCalledFunction() == function) {
            if (call->getFunctionType() != function->getFunctionType() ||
                call->getCallingConv() != function->getCallingConv())
              eligible = false;
            else if (auto *direct = llvm::dyn_cast<llvm::CallInst>(call))
              recursive.push_back(direct);
          }
        }
      }
    }
    if (!eligible || !checkedArithmetic || recursive.empty())
      continue;
    llvm::SmallVector<std::pair<unsigned, llvm::APInt>> bounds;
    for (auto &argument : function->args()) {
      const auto *integer =
          llvm::dyn_cast<llvm::IntegerType>(argument.getType());
      if (!integer || integer->getBitWidth() < 8 ||
          integer->getBitWidth() > 64 ||
          argument.hasAttribute(llvm::Attribute::Range))
        continue;
      std::optional<llvm::APInt> bound;
      for (auto *call : recursive) {
        const auto known = maximum(call->getArgOperand(argument.getArgNo()));
        if (!known) {
          bound.reset();
          break;
        }
        bound = bound ? llvm::APIntOps::umax(*bound, *known) : *known;
      }
      // Leave minor refinements to ordinary interprocedural propagation.
      if (bound && bound->getActiveBits() + 4 <= integer->getBitWidth()) {
        bounds.emplace_back(argument.getArgNo(), *bound);
        if (bounds.size() == 2)
          break;
      }
    }
    const auto cost = function->getInstructionCount();
    if (bounds.empty() || clones == maximumClones ||
        cost > maximumAddedInstructions - added)
      continue;
    llvm::ValueToValueMapTy mapping;
    auto *specialized = llvm::CloneFunction(function, mapping);
    specialized->setName(function->getName() + ".bounded");
    specialized->addFnAttr("gti.range-specialized");
    function->addFnAttr("gti.range-specialized");
    for (const auto &[index, bound] : bounds)
      specialized->addParamAttr(
          index, llvm::Attribute::get(
                     module.getContext(), llvm::Attribute::Range,
                     llvm::ConstantRange(llvm::APInt(bound.getBitWidth(), 0),
                                         bound + 1)));
    // A parameter range preserves poison semantics; unlike llvm.assume it
    // does not introduce a new poison-consuming operation at function entry.
    // Both the original and cloned self calls satisfy the recorded ranges.
    // Other callers keep the original unrestricted signature and failures.
    for (auto *call : recursive) {
      auto *copy = llvm::cast<llvm::CallInst>(mapping.lookup(call));
      copy->setCalledFunction(specialized);
      call->setCalledFunction(specialized);
    }
    ++clones;
    added += cost;
  }
  return clones != 0;
}

void promotePrivateScalarResults(llvm::Module &module) {
  const auto nextInstruction = [](llvm::Instruction *instruction) {
    do {
      instruction = instruction->getNextNode();
    } while (llvm::isa_and_nonnull<llvm::DbgInfoIntrinsic>(instruction));
    return instruction;
  };
  const auto previousInstruction = [](llvm::Instruction *instruction) {
    do {
      instruction = instruction->getPrevNode();
    } while (llvm::isa_and_nonnull<llvm::DbgInfoIntrinsic>(instruction));
    return instruction;
  };
  llvm::SmallVector<llvm::Function *> candidates;
  for (auto &function : module)
    if (!function.isDeclaration() && function.hasLocalLinkage() &&
        !function.isVarArg() && function.arg_size() > 0 &&
        function.getReturnType()->isIntegerTy(1) &&
        function.getAttributes().hasParamAttr(0, "gti.scalar-result"))
      candidates.push_back(&function);
  for (auto *function : candidates) {
    llvm::SmallVector<llvm::CallInst *> calls;
    bool eligible = !function->use_empty();
    for (auto &use : function->uses()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(use.getUser());
      if (!call || !call->isCallee(&use) ||
          call->getFunctionType() != function->getFunctionType() ||
          call->isMustTailCall() || call->getNumOperandBundles() != 0 ||
          call->isConvergent()) {
        eligible = false;
        break;
      }
      calls.push_back(call);
    }
    if (!eligible)
      continue;
    llvm::Type *scalar = nullptr;
    llvm::Align alignment(1);
    llvm::SmallVector<llvm::StoreInst *> stores;
    llvm::SmallVector<llvm::ReturnInst *> returns;
    auto *destination = function->getArg(0);
    // The result address never participates in the computation or escapes.
    // Each write is the final operation of a successful return, so moving it
    // to the caller cannot reorder it with another observable callee action,
    // even if the caller's destination aliases an ordinary input.
    for (auto *user : destination->users()) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
      auto *returned =
          store
              ? llvm::dyn_cast_or_null<llvm::ReturnInst>(nextInstruction(store))
              : nullptr;
      auto *status =
          returned
              ? llvm::dyn_cast<llvm::ConstantInt>(returned->getReturnValue())
              : nullptr;
      auto *type = store ? store->getValueOperand()->getType() : nullptr;
      if (!store || store->getPointerOperand() != destination ||
          store->isVolatile() || store->isAtomic() || !status ||
          !status->isOne() ||
          (!type->isIntegerTy() && !type->isFloatTy() && !type->isDoubleTy()) ||
          (type->isIntegerTy() && type->getIntegerBitWidth() > 64) ||
          (scalar && scalar != type)) {
        eligible = false;
        break;
      }
      alignment = stores.empty() ? store->getAlign()
                                 : std::min(alignment, store->getAlign());
      scalar = type;
      stores.push_back(store);
    }
    if (!eligible || stores.empty())
      continue;
    for (auto &block : *function)
      if (auto *returned =
              llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
        auto *status =
            llvm::dyn_cast<llvm::ConstantInt>(returned->getReturnValue());
        auto *store = llvm::dyn_cast_or_null<llvm::StoreInst>(
            previousInstruction(returned));
        if (!status ||
            (status->isOne() &&
             (!store || store->getPointerOperand() != destination))) {
          eligible = false;
          break;
        }
        returns.push_back(returned);
      }
    if (!eligible)
      continue;
    auto &context = module.getContext();
    auto *resultType =
        llvm::StructType::get(scalar, llvm::Type::getInt1Ty(context));
    llvm::SmallVector<llvm::Type *> parameterTypes;
    llvm::SmallVector<llvm::AttributeSet> parameterAttributes;
    for (unsigned index = 1; index < function->arg_size(); ++index) {
      parameterTypes.push_back(function->getArg(index)->getType());
      parameterAttributes.push_back(
          function->getAttributes().getParamAttrs(index));
    }
    auto *promoted = llvm::Function::Create(
        llvm::FunctionType::get(resultType, parameterTypes, false),
        function->getLinkage(), function->getName() + ".scalar", module);
    promoted->copyAttributesFrom(function);
    promoted->setAttributes(llvm::AttributeList::get(
        context, function->getAttributes().getFnAttrs(), {},
        parameterAttributes));
    promoted->copyMetadata(function, 0);
    promoted->setSubprogram(function->getSubprogram());
    for (unsigned index = 1; index < function->arg_size(); ++index) {
      auto *argument = promoted->getArg(index - 1);
      argument->takeName(function->getArg(index));
      function->getArg(index)->replaceAllUsesWith(argument);
    }
    promoted->splice(promoted->begin(), function);
    for (auto *returned : returns) {
      auto *status = llvm::cast<llvm::ConstantInt>(returned->getReturnValue());
      llvm::Value *value = llvm::PoisonValue::get(scalar);
      if (status->isOne()) {
        auto *store =
            llvm::cast<llvm::StoreInst>(previousInstruction(returned));
        value = store->getValueOperand();
        store->eraseFromParent();
      }
      llvm::IRBuilder<> builder(returned);
      llvm::Value *result = builder.CreateInsertValue(
          llvm::PoisonValue::get(resultType), value, 0);
      result = builder.CreateInsertValue(result, status, 1);
      builder.CreateRet(result);
      returned->eraseFromParent();
    }
    for (auto *call : calls) {
      llvm::SmallVector<llvm::Value *> arguments;
      llvm::SmallVector<llvm::AttributeSet> attributes;
      for (unsigned index = 1; index < call->arg_size(); ++index) {
        arguments.push_back(call->getArgOperand(index));
        attributes.push_back(call->getAttributes().getParamAttrs(index));
      }
      llvm::IRBuilder<> builder(call);
      auto *result = builder.CreateCall(promoted, arguments);
      result->setCallingConv(call->getCallingConv());
      result->setAttributes(llvm::AttributeList::get(
          context, call->getAttributes().getFnAttrs(), {}, attributes));
      result->setDebugLoc(call->getDebugLoc());
      auto *status = builder.CreateExtractValue(result, 1);
      call->replaceAllUsesWith(status);
      auto *publication =
          llvm::SplitBlockAndInsertIfThen(status, call->getNextNode(), false);
      builder.SetInsertPoint(publication);
      builder.CreateAlignedStore(builder.CreateExtractValue(result, 0),
                                 call->getArgOperand(0), alignment);
      call->eraseFromParent();
    }
    promoted->takeName(function);
    function->eraseFromParent();
  }
}

namespace {

class Diagnostics final : public llvm::DiagnosticHandler {
public:
  Diagnostics(std::vector<std::string> &messages, bool &failed)
      : messages(messages), failed(failed) {}
  bool handleDiagnostics(const llvm::DiagnosticInfo &info) override {
    std::string text;
    llvm::raw_string_ostream stream(text);
    llvm::DiagnosticPrinterRawOStream printer(stream);
    info.print(printer);
    messages.push_back(std::move(text));
    failed |= info.getSeverity() == llvm::DS_Error;
    return true;
  }

private:
  std::vector<std::string> &messages;
  bool &failed;
};

void initializeTarget() {
  static std::once_flag once;
  static bool failed = false;
  std::call_once(once, [] {
    failed = llvm::InitializeNativeTarget() ||
             llvm::InitializeNativeTargetAsmPrinter();
  });
  if (failed)
    throw std::runtime_error("LLVM native target initialization failed");
}

llvm::CodeGenOptLevel codegenLevel(LlvmCodegenOptimization level) {
  switch (level) {
  case LlvmCodegenOptimization::None:
    return llvm::CodeGenOptLevel::None;
  case LlvmCodegenOptimization::Less:
    return llvm::CodeGenOptLevel::Less;
  case LlvmCodegenOptimization::Default:
    return llvm::CodeGenOptLevel::Default;
  case LlvmCodegenOptimization::Aggressive:
    return llvm::CodeGenOptLevel::Aggressive;
  }
  throw std::invalid_argument("invalid LLVM optimization policy");
}

llvm::OptimizationLevel pipelineLevel(LlvmCodegenOptimization level) {
  switch (level) {
  case LlvmCodegenOptimization::None:
    return llvm::OptimizationLevel::O0;
  case LlvmCodegenOptimization::Less:
    return llvm::OptimizationLevel::O1;
  case LlvmCodegenOptimization::Default:
    return llvm::OptimizationLevel::O2;
  case LlvmCodegenOptimization::Aggressive:
    return llvm::OptimizationLevel::O3;
  }
  throw std::invalid_argument("invalid LLVM optimization policy");
}

InstrumentationMode diagnosticMode(LlvmInstrumentationMode mode) {
  switch (mode) {
  case LlvmInstrumentationMode::None:
    return InstrumentationMode::None;
  case LlvmInstrumentationMode::Address:
    return InstrumentationMode::Address;
  case LlvmInstrumentationMode::UndefinedMemory:
    return InstrumentationMode::UndefinedMemory;
  case LlvmInstrumentationMode::AddressAndUndefinedMemory:
    return InstrumentationMode::AddressAndUndefinedMemory;
  case LlvmInstrumentationMode::Thread:
    return InstrumentationMode::Thread;
  }
  throw std::invalid_argument("invalid LLVM instrumentation policy");
}

} // namespace

std::unique_ptr<llvm::TargetMachine> makeMachine(const TargetInfo &target,
                                                 LlvmBackendOptions options) {
  const auto host = TargetInfo::host();
  if (target.os != host.os || target.vendor != host.vendor ||
      target.arch != host.arch || target.dataLayout != host.dataLayout ||
      !((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")))
    throw BackendUnsupportedError("LLVM native emission requires a supported "
                                  "macOS arm64 or Linux x86_64 host target");
  static_cast<void>(llvmInstrumentationModeName(options.instrumentation));
  if (options.instrumentation == LlvmInstrumentationMode::Thread)
    throw BackendUnsupportedError(
        "LLVM Thread instrumentation requires verified communicating runtime "
        "and native-provider instrumentation provenance");
  initializeTarget();
  const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
#if LLVM_VERSION_MAJOR >= 21
  const auto &machineTriple = triple;
#else
  const auto machineTriple = triple.str();
#endif
  std::string error;
  const auto *registered =
      llvm::TargetRegistry::lookupTarget(machineTriple, error);
  if (!registered)
    throw std::runtime_error("LLVM target lookup failed: " + error);
  llvm::TargetOptions targetOptions;
  targetOptions.AllowFPOpFusion = llvm::FPOpFusion::Strict;
  // No fast-math flags or function attributes are supplied. LLVM 23 removed
  // older target-wide fast-math toggles in favor of those per-operation facts.
  std::unique_ptr<llvm::TargetMachine> machine(registered->createTargetMachine(
      machineTriple, "generic", "", targetOptions, llvm::Reloc::PIC_,
      std::nullopt, codegenLevel(options.optimization)));
  if (!machine)
    throw std::runtime_error("LLVM target machine construction failed");
  return machine;
}

std::string_view tuneCpu(const TargetInfo &target) {
  return target.os == "macos" && target.arch == "arm64" ? "apple-m1" : "";
}

void verifyTargetLayout(const TargetInfo &target,
                        const llvm::DataLayout &layout,
                        llvm::LLVMContext &context) {
  if (!target.supported() || !target.dataLayout.supported() ||
      layout.isLittleEndian() != target.dataLayout.littleEndian() ||
      layout.getPointerSizeInBits(0) != target.dataLayout.pointerWidthBits() ||
      layout.isNonIntegralAddressSpace(0))
    throw BackendUnsupportedError(
        "LLVM target data layout disagrees with the sealed GTI target");
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    const auto kind = static_cast<TargetScalarKind>(index);
    const auto expected = target.dataLayout.scalarLayout(kind);
    llvm::Type *storage = nullptr;
    if (kind == TargetScalarKind::Pointer)
      storage = llvm::PointerType::get(context, 0);
    else if (kind == TargetScalarKind::Float32)
      storage = llvm::Type::getFloatTy(context);
    else if (kind == TargetScalarKind::Float64)
      storage = llvm::Type::getDoubleTy(context);
    else
      storage = llvm::IntegerType::get(context, expected->sizeBytes * 8);
    if (layout.getTypeAllocSize(storage) != expected->sizeBytes ||
        layout.getABITypeAlign(storage).value() !=
            expected->abiAlignmentBytes ||
        layout.getPrefTypeAlign(storage).value() !=
            expected->preferredAlignmentBytes)
      throw BackendUnsupportedError(
          "LLVM scalar layout disagrees with GTI for " +
          std::string(targetScalarKindName(kind)));
  }
}

Session::Session(VerifiedLoweredProgram program, LlvmBackendOptions options)
    : input_(std::move(program)), options_(options),
      machine_(makeMachine(input_.program().target(), options)),
      module_("gti.native.v" + std::to_string(executionAbiVersion), context_) {
  context_.setDiagnosticHandler(
      std::make_unique<Diagnostics>(diagnostics_, diagnosticError_));
  module_.setDataLayout(machine_->createDataLayout());
#if LLVM_VERSION_MAJOR >= 21
  module_.setTargetTriple(machine_->getTargetTriple());
#else
  module_.setTargetTriple(machine_->getTargetTriple().str());
#endif
  verifyTargetLayout(input_.program().target(), dataLayout(), context_);
  module_.addModuleFlag(llvm::Module::Error, "gti.execution.abi",
                        executionAbiVersion);
  layouts_ = std::unique_ptr<Layouts>(new Layouts(*this));
  abi_ = std::unique_ptr<Abi>(new Abi(*this, *layouts_));
  static_cast<void>(diagnosticMode(options_.instrumentation));
  if (options_.debugInfo)
    debugInfo_ = std::make_unique<DebugInfo>(*this);
}

Session::~Session() = default;
const LoweredProgram &Session::program() const { return input_.program(); }
bool Session::supportsNativeAtomic(const llvm::Function &function,
                                   unsigned bits,
                                   std::uint64_t alignment) const {
  if (function.getParent() != &module_ ||
      (bits != 8 && bits != 16 && bits != 32 && bits != 64) ||
      !std::has_single_bit(alignment) || alignment < bits / 8)
    return false;
  const auto *subtarget = machine_->getSubtargetImpl(function);
  const auto *lowering = subtarget ? subtarget->getTargetLowering() : nullptr;
  return lowering && bits <= lowering->getMaxAtomicSizeInBitsSupported() &&
         bits >= lowering->getMinCmpXchgSizeInBits();
}
const llvm::DataLayout &Session::dataLayout() const {
  return module_.getDataLayout();
}

void Session::verify() const {
  if (diagnosticError_)
    throw std::runtime_error("LLVM emission diagnostic: " +
                             diagnostics_.back());
  if (module_.getDataLayout() != machine_->createDataLayout())
    throw std::logic_error(
        "LLVM module changed its planned target data layout");
#if LLVM_VERSION_MAJOR >= 21
  if (module_.getTargetTriple() != machine_->getTargetTriple())
#else
  if (module_.getTargetTriple() != machine_->getTargetTriple().str())
#endif
    throw std::logic_error("LLVM module changed its planned target triple");
  std::string error;
  llvm::raw_string_ostream stream(error);
  if (llvm::verifyModule(module_, &stream))
    throw std::logic_error("invalid LLVM module: " + error);
}

void Session::requireDefinition(llvm::Function &function) {
  if (function.getParent() != &module_)
    throw std::logic_error(
        "LLVM definition obligation belongs to another module");
  if (std::find(requiredDefinitions_.begin(), requiredDefinitions_.end(),
                &function) == requiredDefinitions_.end())
    requiredDefinitions_.push_back(&function);
}

void Session::requireSourceDefinition(HirFunctionInstanceId id,
                                      llvm::Function &function) {
  const auto *source = program().mir().findFunctionInstance(id);
  if (!source || source->definitionKind != MirDefinitionKind::Source)
    throw std::logic_error("LLVM source definition lost its sealed identity");
  requireDefinition(function);
  for (const auto &[previous, output] : sourceDefinitions_) {
    if (previous == id || output == &function) {
      if (previous != id || output != &function)
        throw std::logic_error("LLVM source definition identity is ambiguous");
      return;
    }
  }
  sourceDefinitions_.emplace_back(id, &function);
}

llvm::GlobalVariable *Session::textLiteral(std::string_view text) {
  const std::string key(text);
  if (const auto found = textLiterals_.find(key); found != textLiterals_.end())
    return found->second;
  auto *data = llvm::ConstantDataArray::getString(context_, text, true);
  auto *storage = new llvm::GlobalVariable(module_, data->getType(), true,
                                           llvm::GlobalValue::PrivateLinkage,
                                           data, "gti.text");
  // The address of a text literal is not source-observable beyond this shared
  // identity, so later constant merging may fold it with an equal constant.
  storage->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  textLiterals_.emplace(key, storage);
  return storage;
}

bool Session::hasSourceDefinition(HirFunctionInstanceId id,
                                  const llvm::Function &function) const {
  return std::any_of(sourceDefinitions_.begin(), sourceDefinitions_.end(),
                     [&](const auto &registered) {
                       return registered.first == id &&
                              registered.second == &function;
                     });
}

void reuseCheckedArithmeticResults(llvm::Module &module) {
  // Keeping the separate NSW/NUW result through LLVM's normal optimization
  // pipeline exposes induction/range facts. At instruction selection, however,
  // it can duplicate a live detector (e.g. CMN + ADD instead of ADDS). Reuse
  // the already computed wrapping lane only here, after those facts were
  // consumed. Replacing a poison result with a defined value is a valid
  // refinement; the original non-poisoning detector and all its uses remain
  // unchanged.
  using Operands = std::pair<llvm::Value *, llvm::Value *>;
  using Checks =
      llvm::DenseMap<Operands, llvm::SmallVector<llvm::IntrinsicInst *, 2>>;
  const auto family = [](unsigned opcode) -> std::optional<unsigned> {
    switch (opcode) {
    case llvm::Instruction::Add:
      return 0;
    case llvm::Instruction::Sub:
      return 1;
    case llvm::Instruction::Mul:
      return 2;
    default:
      return std::nullopt;
    }
  };
  const auto operands = [](unsigned group, llvm::Value *left,
                           llvm::Value *right) {
    if (group != 1 && std::less<llvm::Value *>{}(right, left))
      std::swap(left, right);
    return Operands{left, right};
  };
  for (auto &function : module) {
    std::array<Checks, 3> checks;
    llvm::SmallVector<llvm::BinaryOperator *, 16> results;
    llvm::DenseMap<llvm::Value *, llvm::SmallVector<llvm::ICmpInst *, 2>>
        comparisons;
    for (auto &block : function)
      for (auto &instruction : block) {
        if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction);
            binary && family(binary->getOpcode()) &&
            (binary->hasNoSignedWrap() || binary->hasNoUnsignedWrap()))
          results.push_back(binary);
        if (auto *comparison = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
          llvm::Value *tested = comparison->getOperand(0);
          if (llvm::isa<llvm::ConstantInt>(tested))
            tested = comparison->getOperand(1);
          comparisons[tested].push_back(comparison);
          if (auto *bias = llvm::dyn_cast<llvm::BinaryOperator>(tested);
              bias && bias->getOpcode() == llvm::Instruction::Add &&
              !bias->hasNoSignedWrap() && !bias->hasNoUnsignedWrap()) {
            if (llvm::isa<llvm::ConstantInt>(bias->getOperand(1)))
              comparisons[bias->getOperand(0)].push_back(comparison);
            else if (llvm::isa<llvm::ConstantInt>(bias->getOperand(0)))
              comparisons[bias->getOperand(1)].push_back(comparison);
          }
        }
        auto *check = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
        if (!check)
          continue;
        unsigned group;
        switch (check->getIntrinsicID()) {
        case llvm::Intrinsic::sadd_with_overflow:
        case llvm::Intrinsic::uadd_with_overflow:
          group = 0;
          break;
        case llvm::Intrinsic::ssub_with_overflow:
        case llvm::Intrinsic::usub_with_overflow:
          group = 1;
          break;
        case llvm::Intrinsic::smul_with_overflow:
        case llvm::Intrinsic::umul_with_overflow:
          group = 2;
          break;
        default:
          continue;
        }
        checks[group][operands(group, check->getArgOperand(0),
                               check->getArgOperand(1))]
            .push_back(check);
      }
    if (results.empty())
      continue;
    const llvm::DominatorTree dominators(function);
    llvm::DenseMap<llvm::IntrinsicInst *, llvm::Value *> lanes;
    // Range checks may replace a constant overflow intrinsic during
    // InstCombine. Reconstruct only an exactly equivalent full-domain
    // predicate; a merely sufficient range, speculative fact, or flagged bias
    // is not enough. Index by tested SSA value to avoid comparing every
    // arithmetic instruction with every condition in large functions. Recheck
    // current operands below because earlier result replacements can invalidate
    // cached keys.
    struct Recovery {
      llvm::IntrinsicInst *check = nullptr;
      llvm::ICmpInst *comparison = nullptr;
      llvm::Value *left = nullptr, *right = nullptr;
      unsigned group = 0;
      bool isSigned = false, meansSuccess = false;
    };
    // Existing detector lanes are reused wherever dominance permits. Creating
    // a detector is more restrictive: do not move arithmetic out of a rare
    // conditional region or into a frequently executed loop condition. Admit
    // only the same straight-line block, or the immediate success edge of the
    // equivalent comparison. A merged result must be guarded on that very
    // incoming edge. This is a structural frequency bound, not a target-cost
    // claim for every constant; target-specific profitability still needs
    // representative optimized measurements.
    const auto nearbyExecution = [](llvm::ICmpInst *comparison,
                                    llvm::Instruction *point,
                                    llvm::BinaryOperator *result,
                                    bool meansSuccess) {
      auto *checkedBlock = comparison->getParent();
      auto *resultBlock = result->getParent();
      const bool mergedEdge = point != result;
      if (!mergedEdge && checkedBlock == resultBlock)
        return true;
#if LLVM_VERSION_MAJOR >= 23
      const auto *branch =
          llvm::dyn_cast<llvm::CondBrInst>(checkedBlock->getTerminator());
      if (!branch)
        return false;
#else
      const auto *branch =
          llvm::dyn_cast<llvm::BranchInst>(checkedBlock->getTerminator());
      if (!branch || !branch->isConditional())
        return false;
#endif
      if (branch->getCondition() != comparison ||
          branch->getSuccessor(meansSuccess ? 0 : 1) != resultBlock ||
          branch->getSuccessor(meansSuccess ? 1 : 0) == resultBlock)
        return false;
      return mergedEdge ? point == checkedBlock->getTerminator()
                        : resultBlock->getSinglePredecessor() == checkedBlock;
    };
    const auto findRecovery =
        [&](unsigned group, llvm::Value *left, llvm::Value *right,
            llvm::Instruction *point,
            llvm::BinaryOperator *result) -> std::optional<Recovery> {
      const auto key = operands(group, left, right);
      const auto candidates = checks[group].find(key);
      if (candidates != checks[group].end())
        for (auto *check : candidates->second)
          if (operands(group, check->getArgOperand(0),
                       check->getArgOperand(1)) == key &&
              dominators.dominates(check, point))
            return Recovery{check};
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(right);
      if (!constant && group != 1) {
        constant = llvm::dyn_cast<llvm::ConstantInt>(left);
        if (constant)
          std::swap(left, right);
      }
      if (!constant || !left->getType()->isIntegerTy())
        return std::nullopt;
      const auto rangeCandidates = comparisons.find(left);
      if (rangeCandidates == comparisons.end())
        return std::nullopt;
      for (auto *comparison : rangeCandidates->second) {
        if (!dominators.dominates(comparison, point))
          continue;
        auto *bound =
            llvm::dyn_cast<llvm::ConstantInt>(comparison->getOperand(1));
        auto *tested = comparison->getOperand(0);
        auto predicate = comparison->getPredicate();
        if (!bound) {
          bound = llvm::dyn_cast<llvm::ConstantInt>(comparison->getOperand(0));
          tested = comparison->getOperand(1);
          predicate = llvm::CmpInst::getSwappedPredicate(predicate);
        }
        if (!bound || tested->getType() != left->getType())
          continue;
        auto region = llvm::ConstantRange::makeExactICmpRegion(
            predicate, bound->getValue());
        bool compoundPredicate = false;
        if (tested != left) {
          auto *bias = llvm::dyn_cast<llvm::BinaryOperator>(tested);
          if (!bias || bias->getOpcode() != llvm::Instruction::Add ||
              bias->hasNoSignedWrap() || bias->hasNoUnsignedWrap())
            continue;
          llvm::Value *base = bias->getOperand(0);
          auto *offset = llvm::dyn_cast<llvm::ConstantInt>(bias->getOperand(1));
          if (!offset) {
            offset = llvm::dyn_cast<llvm::ConstantInt>(bias->getOperand(0));
            base = bias->getOperand(1);
          }
          if (!offset || base != left)
            continue;
          region = region.subtract(offset->getValue());
          compoundPredicate = !offset->isZero();
        }
        // A bare comparison plus direct arithmetic is already a cheap target
        // idiom. Fusing it can worsen recurrence/branch scheduling despite
        // reducing instruction count (observed on AArch64 conditional counts).
        // Bound reconstruction to compound biased predicates, or to enabling
        // complete incoming-edge reuse of a merged result. Do not impose this
        // profitability boundary on reuse of an already existing intrinsic.
        if (point == result && !compoundPredicate)
          continue;
        for (bool isSigned : {true, false}) {
          if (isSigned ? !result->hasNoSignedWrap()
                       : !result->hasNoUnsignedWrap())
            continue;
          const auto valid = llvm::ConstantRange::makeExactNoWrapRegion(
              static_cast<llvm::Instruction::BinaryOps>(result->getOpcode()),
              constant->getValue(),
              isSigned ? llvm::OverflowingBinaryOperator::NoSignedWrap
                       : llvm::OverflowingBinaryOperator::NoUnsignedWrap);
          if (valid.isFullSet() || valid.isEmptySet())
            continue;
          const bool meansSuccess = region == valid;
          if (!meansSuccess && region != valid.inverse())
            continue;
          if (!nearbyExecution(comparison, point, result, meansSuccess))
            continue;
          return Recovery{nullptr, comparison, left,        right,
                          group,   isSigned,   meansSuccess};
        }
      }
      return std::nullopt;
    };
    const auto materialize = [&](const Recovery &recovery) {
      if (recovery.check)
        return recovery.check;
      const llvm::Intrinsic::ID ids[3][2] = {
          {llvm::Intrinsic::uadd_with_overflow,
           llvm::Intrinsic::sadd_with_overflow},
          {llvm::Intrinsic::usub_with_overflow,
           llvm::Intrinsic::ssub_with_overflow},
          {llvm::Intrinsic::umul_with_overflow,
           llvm::Intrinsic::smul_with_overflow}};
      llvm::IRBuilder<> builder(recovery.comparison);
      builder.SetCurrentDebugLocation(recovery.comparison->getDebugLoc());
      auto *check = llvm::cast<llvm::IntrinsicInst>(
          builder.CreateBinaryIntrinsic(ids[recovery.group][recovery.isSigned],
                                        recovery.left, recovery.right));
      auto *overflow = builder.CreateExtractValue(check, 1, "checked.overflow");
      recovery.comparison->replaceAllUsesWith(
          recovery.meansSuccess ? builder.CreateNot(overflow) : overflow);
      checks[recovery.group]
            [operands(recovery.group, recovery.left, recovery.right)]
                .push_back(check);
      return check;
    };
    const auto laneFor = [&](llvm::IntrinsicInst *check) {
      auto *&lane = lanes[check];
      if (!lane) {
        llvm::IRBuilder<> builder(check->getParent(),
                                  std::next(check->getIterator()));
        builder.SetCurrentDebugLocation(check->getDebugLoc());
        lane = builder.CreateExtractValue(check, 0, "checked.result");
      }
      return lane;
    };
    // Visit later consumers first to retain keys in ordinary arithmetic chains.
    for (auto iterator = results.rbegin(); iterator != results.rend();
         ++iterator) {
      auto *result = *iterator;
      const auto group = *family(result->getOpcode());
      if (auto recovery = findRecovery(group, result->getOperand(0),
                                       result->getOperand(1), result, result)) {
        result->replaceAllUsesWith(laneFor(materialize(*recovery)));
        result->eraseFromParent();
        continue;
      }
      // SimplifyCFG may sink equivalent checked arithmetic into a merge block.
      // Recover an edge-wise result only when every incoming edge already has
      // an identical dominating detector or exactly equivalent range guard.
      // The CFG and all source failure outcomes remain unchanged.
      auto *leftPhi = llvm::dyn_cast<llvm::PHINode>(result->getOperand(0));
      auto *rightPhi = llvm::dyn_cast<llvm::PHINode>(result->getOperand(1));
      auto *block = result->getParent();
      if ((!leftPhi || leftPhi->getParent() != block) &&
          (!rightPhi || rightPhi->getParent() != block))
        continue;
      llvm::SmallVector<std::pair<llvm::BasicBlock *, Recovery>, 4> incoming;
      bool complete = true;
      for (auto *predecessor : llvm::predecessors(block)) {
        const auto onEdge = [&](llvm::Value *value) {
          auto *phi = llvm::dyn_cast<llvm::PHINode>(value);
          return phi && phi->getParent() == block
                     ? phi->getIncomingValueForBlock(predecessor)
                     : value;
        };
        auto recovery = findRecovery(group, onEdge(result->getOperand(0)),
                                     onEdge(result->getOperand(1)),
                                     predecessor->getTerminator(), result);
        if (!recovery) {
          complete = false;
          break;
        }
        incoming.emplace_back(predecessor, *recovery);
      }
      if (!complete || incoming.empty())
        continue;
      // Materialize only after every edge has a valid plan. An incomplete
      // merge must not introduce checks or rewrite any existing guard.
      auto *merged = llvm::PHINode::Create(result->getType(), incoming.size(),
                                           "checked.merge", block->begin());
      for (const auto &[predecessor, recovery] : incoming)
        merged->addIncoming(laneFor(materialize(recovery)), predecessor);
      result->replaceAllUsesWith(merged);
      result->eraseFromParent();
    }
  }
}

BackendArtifact Session::emitObject() {
  if (emissionStarted_)
    throw std::logic_error("LLVM emission sessions are single-use");
  for (const auto *function : requiredDefinitions_) {
    if (function->isDeclaration())
      throw BackendUnsupportedError(
          "LLVM object emission requires every planned GTI body to be defined");
  }
  if (debugInfo_)
    debugInfo_->finalize();
  verify();
  emissionStarted_ = true;
  // A sealed hosted entry and no embedding plan delimit the existing private
  // whole-program executable ABI. Library/embedding compilation stays open.
  // Taking an address is conservatively a root, including callbacks/llvm.used;
  // no escape summary, external linkage promise or function attribute is
  // invented.
  const auto &mir = program().mir();
  if ((mir.hostedStartupPlan() || program().compiledScalarProducer()) &&
      !program().embeddingPlan()) {
    std::set<HirFunctionInstanceId> retained;
    for (const auto &callback : mir.nativeCallbackAdapters())
      retained.insert(callback.target);
    for (const auto &task : mir.threadTaskAdapters())
      if (task.functionTarget)
        retained.insert(*task.functionTarget);
    for (const auto &row : program().bodies()) {
      const auto *body = findMirBody(mir, row.identity.address);
      if (!body)
        continue;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.functionPointerFunctionTarget)
            retained.insert(*instruction.functionPointerFunctionTarget);
    }
    for (const auto &[id, function] : sourceDefinitions_) {
      const auto &source = *mir.findFunctionInstance(id);
      if (source.linkage != LanguageLinkage::Gti ||
          !source.externalSymbol.empty() || source.virtualMethod ||
          retained.contains(id) || source.entryKind != ProgramEntryKind::None ||
          (mir.hostedStartupPlan() && id == mir.hostedStartupPlan()->entry) ||
          function->hasAddressTaken(nullptr, false, false, false, false,
                                    false) ||
          function->hasDLLExportStorageClass() ||
          !function->hasExternalLinkage() || function->hasComdat())
        continue;
      function->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }
  const auto mode = diagnosticMode(options_.instrumentation);
  if (options_.optimization != LlvmCodegenOptimization::None)
    promotePrivateScalarResults(module_);
  if (instrumentsUndefinedMemory(mode)) {
    instrumentUndefinedMemory(module_);
    verify();
  }
  // Tuning changes only cost-model and scheduling choices. Features, and so
  // the instructions a program may execute, stay at the machine baseline:
  // Apple M1's LSE atomics measured slower than the baseline sequences for
  // GTI's uncontended scalar atomics.
  if (const auto tune = tuneCpu(program().target()); !tune.empty())
    for (auto &function : module_)
      if (!function.isDeclaration() && !function.hasFnAttribute("tune-cpu"))
        function.addFnAttr("tune-cpu", tune);
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  const auto level = pipelineLevel(options_.optimization);
  llvm::PipelineTuningOptions tuning;
  // LLVM's library defaults enable the loop vectorizer but leave SLP off.
  // Independent adjacent scalar operations need the latter even without a
  // loop. Keep its ordinary target cost model and all checked/strict semantics.
  tuning.SLPVectorization = level == llvm::OptimizationLevel::O2 ||
                            level == llvm::OptimizationLevel::O3;
  if (options_.optimization == LlvmCodegenOptimization::Aggressive)
    tuning.InlinerThreshold = 400;
  llvm::PassBuilder builder(machine_.get(), tuning);
  registerPrivateAtomicPass(builder);
  registerCheckedOffsetPass(builder);
  registerCheckedReductionPass(builder);
  registerCheckedRangePass(builder);
  registerCheckedInvariantAddPass(builder);
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  if (level == llvm::OptimizationLevel::O2 ||
      level == llvm::OptimizationLevel::O3)
    (void)promoteGuardedDispatchCalls(module_);
  auto pipeline = level == llvm::OptimizationLevel::O0
                      ? builder.buildO0DefaultPipeline(level)
                      : builder.buildPerModuleDefaultPipeline(level);
  pipeline.run(module_, modules);
  if (level == llvm::OptimizationLevel::O2 ||
      level == llvm::OptimizationLevel::O3) {
    const bool specialized = specializeBoundedRecursion(module_);
    if (specialized)
      modules.invalidate(module_, llvm::PreservedAnalyses::none());
    // Inlining exposes complete checked-failure/cleanup regions only after
    // the ordinary pipeline has simplified their ownership state. Outline
    // LLVM's profitable cold regions, then give the now-smaller hot helpers
    // one ordinary simplification/optimization round. No failure edge is
    // erased or assigned invented likelihood, and there is no retry loop.
    const auto outlined = llvm::HotColdSplittingPass().run(module_, modules);
    modules.invalidate(module_, outlined);
    const auto optimizeExposed = [&] {
      auto exposed = builder.buildModuleSimplificationPipeline(
          level, llvm::ThinOrFullLTOPhase::None);
      exposed.addPass(builder.buildModuleOptimizationPipeline(
          level, llvm::ThinOrFullLTOPhase::None));
      exposed.run(module_, modules);
    };
    if (specialized || !outlined.areAllPreserved())
      optimizeExposed();
    // The smaller helpers above can now inline and create pointer PHIs that
    // did not exist after the first pipeline. Prove those late loop invariants
    // before one final ordinary cleanup round; do not retry to a fixed point.
    if (foldInductivePointerLoops(module_)) {
      modules.invalidate(module_, llvm::PreservedAnalyses::none());
      optimizeExposed();
    }
  }
  reuseCheckedArithmeticResults(module_);
  modules.invalidate(module_, llvm::PreservedAnalyses::none());
  if (instrumentsAddress(mode))
    instrumentAddress(module_, modules);
  if (instrumentsThread(mode))
    instrumentThread(module_, modules);
  if (debugInfo_)
    debugInfo_->prepareCodegen();
  verify();
  if (options_.textualIr) {
    // The same module, stopped where target code generation would begin.
    std::string text;
    llvm::raw_string_ostream stream(text);
    module_.print(stream, nullptr);
    stream.flush();
    return {BackendArtifactKind::Source, std::move(text), ".ll"};
  }
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager passes;
  if (machine_->addPassesToEmitFile(passes, stream, nullptr,
                                    llvm::CodeGenFileType::ObjectFile))
    throw std::runtime_error("LLVM target cannot emit object files");
  passes.run(module_);
  if (diagnosticError_)
    throw std::runtime_error("LLVM object emission diagnostic: " +
                             diagnostics_.back());
  return {BackendArtifactKind::Object, std::string(bytes.begin(), bytes.end()),
          ".o"};
}

} // namespace lang::native_llvm
