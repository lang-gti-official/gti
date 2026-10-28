#include "session.h"
#include "abi.h"
#include "debug_info.h"
#include "instrumentation.h"

#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <bit>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lang::native_llvm {
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
  if (mir.hostedStartupPlan() && !program().embeddingPlan()) {
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
          id == mir.hostedStartupPlan()->entry ||
          function->hasAddressTaken(nullptr, false, false, false, false,
                                    false) ||
          function->hasDLLExportStorageClass() ||
          !function->hasExternalLinkage() || function->hasComdat())
        continue;
      function->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }
  const auto mode = diagnosticMode(options_.instrumentation);
  if (instrumentsUndefinedMemory(mode)) {
    instrumentUndefinedMemory(module_);
    verify();
  }
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder(machine_.get());
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  const auto level = pipelineLevel(options_.optimization);
  auto pipeline = level == llvm::OptimizationLevel::O0
                      ? builder.buildO0DefaultPipeline(level)
                      : builder.buildPerModuleDefaultPipeline(level);
  pipeline.run(module_, modules);
  if (instrumentsAddress(mode))
    instrumentAddress(module_, modules);
  if (instrumentsThread(mode))
    instrumentThread(module_, modules);
  if (debugInfo_)
    debugInfo_->prepareCodegen();
  verify();
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
