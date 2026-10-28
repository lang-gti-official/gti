#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <iostream>
#include <optional>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

int main() {
  FrontendOptions frontendOptions;
  frontendOptions.requireEntryPoint = true;
  const auto frontend = Frontend(frontendOptions).analyze("pipeline.gti", R"(
int promote(int input) { mut int local = input; return local; }
int checked(int left, int right) { return left + right; }
double strict(double left, double right, double addend) { return left * right + addend; }
double divide(double left, double right) { return left / right; }
int step(int value, int bias) { return (value * 17 + bias) % 65521; }
int drive(int seed, int rounds) {
  mut int value = seed;
  for (mut int round = 0; round < rounds; round++) { value = step(value, round % 127); }
  return value;
}
int main() {
  if (promote(7) != 7 || checked(1, 2) != 3) { return 1; }
  if (drive(1, 3) != 4932) { return 3; }
  if (strict(2.0d, 3.0d, 4.0d) != 10.0d || divide(4.0d, 2.0d) != 2.0d) { return 2; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid pipeline source");
  }
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid lowered pipeline source");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified pipeline source");
  unsigned levels = 0;
  for (auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    const LlvmBackendOptions options{.optimization = level};
    const auto admission = planLlvmAdmission(*input, options);
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    Session session(*input, options);
    lowerScalarProgram(session, admission);
    llvm::Function *promote = nullptr, *checked = nullptr, *strict = nullptr,
                   *step = nullptr, *drive = nullptr;
    std::string privateName, entryName;
    for (const auto &function : input->program().mir().functionInstances()) {
      const auto *declaration =
          input->program().findFunctionDeclaration(function.declaration);
      if (!declaration)
        continue;
      if (declaration->qualifiedName == "promote")
        promote = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "checked")
        checked = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "strict")
        strict = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "step")
        step = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "drive")
        drive = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "divide")
        privateName =
            session.abi().declareFunction(function.id)->getName().str();
      if (function.entryKind != ProgramEntryKind::None)
        entryName = session.abi().declareFunction(function.id)->getName().str();
    }
    if (!promote || !checked || !strict || !step || !drive)
      throw std::runtime_error("missing pipeline function");
    // Keep these unknown-input observation roots available after optimization.
    llvm::appendToUsed(session.module(), {promote, checked, strict, drive});
    const auto allocas = [](const llvm::Function &function) {
      unsigned count = 0;
      for (const auto &block : function)
        for (const auto &row : block)
          count += llvm::isa<llvm::AllocaInst>(row);
      return count;
    };
    if (allocas(*promote) == 0)
      throw std::runtime_error("pipeline fixture lost its source storage");
    const auto floating = [&] {
      unsigned multiply = 0, addition = 0;
      for (const auto &block : *strict)
        for (const auto &row : block) {
          if (const auto *operation =
                  llvm::dyn_cast<llvm::FPMathOperator>(&row))
            if (operation->getFastMathFlags().any())
              throw std::runtime_error("unverified fast math");
          multiply += row.getOpcode() == llvm::Instruction::FMul;
          addition += row.getOpcode() == llvm::Instruction::FAdd;
          if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
            if (call->getIntrinsicID() == llvm::Intrinsic::fma ||
                call->getIntrinsicID() == llvm::Intrinsic::fmuladd)
              throw std::runtime_error("implicit floating contraction");
        }
      if (!multiply || !addition)
        throw std::runtime_error("strict multiply/add was lost");
    };
    floating();
    session.verify();
    const auto artifact = session.emitObject();
    // Every emitted body is tuned for the target's selected processor while
    // its instruction features stay at the machine baseline. LLVM records
    // tuning knobs (fusion pairs, interleave factor, zero-cycle moves) in the
    // same feature set, so the only bits tuning may add are the tuning
    // processor's TuneImplies closure; no instruction extension, such as
    // LSE atomics, may appear.
    const auto machine = makeMachine(TargetInfo::host(), options);
    const auto &baseline = machine->getMCSubtargetInfo();
    const auto host = TargetInfo::host();
    const llvm::StringRef requestedTune(tuneCpu(host));
    const auto closure = [&](llvm::FeatureBitset bits) {
      for (bool grew = true; grew;) {
        grew = false;
        for (const auto &feature : baseline.getAllProcessorFeatures())
          if (bits[feature.Value]) {
            const auto implied = bits | feature.Implies.getAsBitset();
            grew |= implied != bits;
            bits = implied;
          }
      }
      return bits;
    };
    // LLVM may resolve the requested name through a processor alias (Apple
    // M1 tunes as the A14 core), so judge the closure of what it selected.
    const auto tuningOf = [&](llvm::StringRef processorName) {
      for (const auto &processor : baseline.getAllProcessorDescriptions())
        if (llvm::StringRef(processor.key()) == processorName)
          return closure(processor.TuneImplies.getAsBitset());
      throw std::runtime_error("unknown tuning processor " +
                               processorName.str());
    };
    std::optional<unsigned> atomics;
    for (const auto &feature : baseline.getAllProcessorFeatures())
      if (llvm::StringRef(feature.key()) == "lse")
        atomics = feature.Value;
    unsigned tuned = 0;
    for (const auto &function : session.module()) {
      if (function.isDeclaration())
        continue;
      const auto *effective = machine->getSubtargetImpl(function);
      const auto added =
          effective->getFeatureBits() & ~baseline.getFeatureBits();
      const auto attribute = function.getFnAttribute("tune-cpu");
      if ((requestedTune.empty()
               ? attribute.isValid() ||
                     effective->getTuneCPU() != baseline.getCPU()
               : !attribute.isStringAttribute() ||
                     attribute.getValueAsString() != requestedTune ||
                     effective->getTuneCPU() == baseline.getCPU()) ||
          effective->getCPU() != baseline.getCPU() ||
          (added & ~tuningOf(effective->getTuneCPU())).any() ||
          (atomics && effective->getFeatureBits()[*atomics] !=
                          baseline.getFeatureBits()[*atomics]))
        throw std::runtime_error("emitted body " + function.getName().str() +
                                 " lost its tuning or gained an instruction "
                                 "feature");
      ++tuned;
    }
    if (tuned == 0 || (host.os == "macos" && host.arch == "arm64" &&
                       (requestedTune != "apple-m1" || !atomics ||
                        baseline.getFeatureBits()[*atomics])))
      throw std::runtime_error(
          "macOS arm64 is not tuned for Apple M1 on the LSE-free baseline");
    const auto *privateFunction = session.module().getFunction(privateName);
    const auto *entryFunction = session.module().getFunction(entryName);
    if (privateName.empty() || entryName.empty() ||
        (privateFunction && !privateFunction->hasInternalLinkage()) ||
        !entryFunction || !entryFunction->hasExternalLinkage())
      throw std::runtime_error(
          "real GTI private/hosted visibility policy failed");
    if (artifact.kind != BackendArtifactKind::Object ||
        artifact.contents.empty())
      throw std::runtime_error("pipeline did not emit a real object");
    session.verify();
    floating();
    if (level == LlvmCodegenOptimization::None ? allocas(*promote) == 0
                                               : allocas(*promote) != 0)
      throw std::runtime_error("selected O0/O1/O2/O3 pipeline did not "
                               "preserve/promote storage as expected");
    bool overflow = false;
    for (const auto &block : *checked)
      for (const auto &row : block)
        if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
          overflow |=
              call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow;
    if (!overflow)
      throw std::runtime_error(
          "unknown-input checked addition lost overflow detection");
    // A checked operation originates its record through one cold helper, so
    // the record's fixed-size fill never inflates the checked body. A body
    // whose failure chain only propagates neither enters nor leaves the
    // incoming-cleanup context, which nothing inside that chain can observe.
    // Together they leave a two-check scalar helper small enough to inline
    // into its loop at O2/O3.
    const auto *originator =
        session.module().getFunction("__gti_failure_originate_v1");
    if (!originator || originator->isDeclaration() ||
        !originator->hasFnAttribute(llvm::Attribute::Cold) ||
        !originator->hasFnAttribute(llvm::Attribute::NoInline) ||
        !originator->hasInternalLinkage())
      throw std::runtime_error("failure origination is not one cold helper");
    const auto *recordType = session.abi().failureRecord();
    for (const auto &function : session.module())
      for (const auto &block : function)
        for (const auto &row : block)
          if (const auto *write = llvm::dyn_cast<llvm::StoreInst>(&row);
              write && write->getValueOperand()->getType() == recordType &&
              &function != originator)
            throw std::runtime_error("a body stores a whole failure record "
                                     "outside the cold originator");
    unsigned originations = 0, stepCalls = 0;
    for (const auto &block : *drive)
      for (const auto &row : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&row)) {
          const auto *callee = call->getCalledFunction();
          originations += callee == originator;
          stepCalls += callee == step;
          if (callee &&
              (callee->getName() == "gti_rt_failure_cleanup_enter_v1" ||
               callee->getName() == "gti_rt_failure_cleanup_leave_v1"))
            throw std::runtime_error("a propagate-only failure chain entered "
                                     "the incoming-cleanup context");
        }
    const bool inlines = level == LlvmCodegenOptimization::Default ||
                         level == LlvmCodegenOptimization::Aggressive;
    if (inlines && (stepCalls != 0 || originations == 0))
      throw std::runtime_error(
          "a two-check scalar helper was not inlined into its loop");
    Session invalid(*input, options);
    auto *malformed = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(invalid.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, "unterminated", invalid.module());
    llvm::BasicBlock::Create(invalid.context(), "entry", malformed);
    bool rejected = false;
    try {
      static_cast<void>(invalid.emitObject());
    } catch (const std::logic_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error(
          "malformed IR escaped before the selected pipeline");
    ++levels;
  }
  std::cout << levels
            << " real pipelines: private/hosted visibility, storage promotion, "
               "strict FP, checked "
               "overflow, cold failure origination, processor tuning and "
               "rejection passed\n";
}
