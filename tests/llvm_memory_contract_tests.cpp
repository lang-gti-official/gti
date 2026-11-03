#include "../src/compiler/llvm/memory.h"
#include "cpp_backend_test_support.h"
#include "private_stdlib_fixture.h"

#include "gti/process_memory_runtime.h"
#include <fstream>
#include <iterator>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Passes/PassBuilder.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <set>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void allocationLayoutWitness() {
  using lang::native_llvm::processMallocSatisfiesLayout;
  for (const auto specification : {"e-p:32:32", "e-p:64:64", "e-p:64:32"}) {
    const llvm::DataLayout layout(specification);
    const auto size = layout.getPointerSize(0);
    const auto alignment = layout.getPointerABIAlignment(0).value();
    expect(processMallocSatisfiesLayout(layout, 1, 1),
           "one byte needs only byte alignment");
    expect(processMallocSatisfiesLayout(layout, size, alignment) &&
               processMallocSatisfiesLayout(layout, size + 1, alignment),
           "a fitting target pointer witnesses its ABI alignment");
    expect(!processMallocSatisfiesLayout(layout, size - 1, alignment) &&
               !processMallocSatisfiesLayout(layout, size, alignment * 2),
           "neither undersized nor over-aligned requests inherit a witness");
    expect(!processMallocSatisfiesLayout(layout, 0, 1) &&
               !processMallocSatisfiesLayout(layout, size, 0) &&
               !processMallocSatisfiesLayout(layout, size, 3),
           "empty and malformed requests are not ordinary allocations");
  }
}

void providerContracts() {
  using namespace lang;
  using namespace lang::native_llvm;
  using Operation = ProcessMemoryRuntimeOperation;
  const auto frontend =
      Frontend().analyze("memory-contract.gti", "int main() { return 0; }");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input) {
    expect(false, "provider test requires a verified minimal program");
    return;
  }
  Session session(*input, {});
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {session.layouts().pointer()}, false),
      llvm::GlobalValue::InternalLinkage, "memory.contract", session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  Memory memory(session, ir, {}, {}, {});
  const auto binding = [&](Operation operation) {
    const auto *signature = processMemoryRuntimeSignature(operation);
    MirFunctionInstance target;
    target.definitionKind = MirDefinitionKind::Declaration;
    target.linkage = LanguageLinkage::C;
    target.externalSymbol = signature->symbol;
    target.returnType = signature->returnType;
    target.parameterTypes = signature->parameters;
    target.processMemoryOperation = operation;
    return target;
  };
  // No source function instance exists: synthesized owner allocation still
  // uses the canonical provider signature, not a second symbol registry.
  const auto query = binding(Operation::NativeSizeMax);
  auto *call = llvm::cast<llvm::CallBase>(memory.processCall(query, {}));
  const auto plan =
      session.abi().nativeC(query.returnType, query.parameterTypes);
  expect(call->getFunctionType() == plan.type &&
             call->getCalledFunction()->getFunctionType() == plan.type,
         "synthetic provider declaration and call share the native C planner");
  expect(!call->onlyReadsMemory() && !call->doesNotThrow() &&
             !call->hasRetAttr(llvm::Attribute::NoAlias),
         "provider representation does not invent foreign effect promises");
  auto wrong = query;
  wrong.returnType = SemanticType::Bool;
  expect(!nativeProcessMemoryBinding(input->program().mir(), wrong),
         "a sealed operation never accepts a mutated physical signature");
  const auto other = binding(input->program().mir().target().os == "windows"
                                 ? Operation::PosixPointerSize
                                 : Operation::AlignedAllocate);
  expect(!nativeProcessMemoryBinding(input->program().mir(), other),
         "the opposite platform allocation family stays rejected");
  bool rejected = false;
  try {
    (void)memory.processCall(other, {});
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "wrong-target provider emission fails before creating a call");
  auto *conflict = new llvm::GlobalVariable(
      session.module(), ir.getInt64Ty(), false,
      llvm::GlobalValue::ExternalLinkage, nullptr,
      processMemoryRuntimeSymbol(Operation::AddressIsAligned));
  rejected = false;
  try {
    const std::array<llvm::Value *, 2> args{
        llvm::ConstantPointerNull::get(session.layouts().pointer()),
        ir.getInt64(8)};
    (void)memory.processCall(binding(Operation::AddressIsAligned), args);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "conflicting provider symbols reject without a pointer bitcast");
  conflict->eraseFromParent();
  const std::array<llvm::Value *, 2> alignmentArguments{function->getArg(0),
                                                        ir.getInt64(16)};
  auto *alignment = llvm::cast<llvm::CallBase>(memory.processCall(
      binding(Operation::AddressIsAligned), alignmentArguments));
  expect(!alignment->doesNotCapture(0),
         "address-bit queries do not acquire a stronger capture promise");
  auto *first = llvm::cast<llvm::CallBase>(memory.allocateStorage(64, 16));
  auto *second = llvm::cast<llvm::CallBase>(memory.allocateStorage(64, 16));
  expect(
      first->hasRetAttr(llvm::Attribute::NoAlias) &&
          second->hasRetAttr(llvm::Attribute::NoAlias),
      "sealed allocation calls preserve the provider's fresh-result contract");
  expect(!first->onlyReadsMemory() &&
             !first->hasRetAttr(llvm::Attribute::NonNull),
         "freshness does not erase allocation effects or nullable failure");
  expect(first->doesNotThrow() ==
             (input->program().mir().target().os == "windows"),
         "only direct result-only allocation carries the removable contract");
  for (unsigned index = 0; index < first->arg_size(); ++index)
    if (first->getArgOperand(index)->getType()->isPointerTy())
      expect(first->doesNotCapture(index),
             "the sealed allocation provider cannot retain its status pointer");

  if (input->program().mir().target().os != "windows") {
    auto *ordinaryAllocation =
        llvm::cast<llvm::CallBase>(memory.allocateStorage(
            session.dataLayout().getPointerSize(0),
            session.dataLayout().getPointerABIAlignment(0).value()));
    auto *tinyAligned =
        llvm::cast<llvm::CallBase>(memory.allocateStorage(1, 2));
    expect(ordinaryAllocation->getCalledFunction()->getName() ==
                   "gti_rt_malloc_v1" &&
               ordinaryAllocation->hasRetAttr(llvm::Attribute::NoAlias) &&
               ordinaryAllocation->arg_size() == 1 &&
               !ordinaryAllocation->hasRetAttr(llvm::Attribute::NonNull) &&
               !ordinaryAllocation->onlyReadsMemory(),
           "fitting target witness selects nullable fresh ordinary allocation");
    expect(tinyAligned->getCalledFunction()->getName() ==
                   "gti_rt_posix_memalign_v1" &&
               !tinyAligned->hasFnAttr(llvm::Attribute::AllocKind),
           "status-writing allocation retains its complete output contract");
    auto *unclassified = ir.CreateCall(ordinaryAllocation->getCalledFunction(),
                                       {ir.getInt64(64)});
    expect(
        !unclassified->hasRetAttr(llvm::Attribute::NoAlias) &&
            !unclassified->hasFnAttr(llvm::Attribute::AllocKind) &&
            !unclassified->hasFnAttr(llvm::Attribute::AllocSize) &&
            !ordinaryAllocation->getCalledFunction()->hasRetAttribute(
                llvm::Attribute::NoAlias),
        "ordinary malloc-spelled foreign calls inherit no freshness promise");
  }

  // The same physical declaration can also be reached by an unclassified C
  // call. Attribute only the edge carrying the exact process-memory witness.
  llvm::SmallVector<llvm::Value *, 3> arguments;
  for (const auto &argument : first->args())
    arguments.push_back(argument.get());
  auto *ordinary = ir.CreateCall(first->getFunctionType(),
                                 first->getCalledFunction(), arguments);
  expect(!ordinary->hasRetAttr(llvm::Attribute::NoAlias) &&
             !first->getCalledFunction()->hasRetAttribute(
                 llvm::Attribute::NoAlias),
         "an ordinary foreign call does not inherit allocation facts by name");
  if (first->arg_size() == 3)
    expect(!ordinary->doesNotCapture(2) &&
               !first->getCalledFunction()->getArg(2)->hasNoCaptureAttr(),
           "ordinary calls and declarations retain unknown status capture");
  auto *derived =
      llvm::GetElementPtrInst::Create(ir.getInt8Ty(), first, {ir.getInt64(0)},
                                      "derived.alias", ir.GetInsertBlock());
  ir.CreateRetVoid();
  session.verify();

  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  auto &aliases = functions.getResult<llvm::AAManager>(*function);
  const auto location = [](llvm::Value *address) {
    return llvm::MemoryLocation(address, llvm::LocationSize::precise(8));
  };
  expect(aliases.alias(location(first), location(function->getArg(0))) ==
                 llvm::AliasResult::NoAlias &&
             aliases.alias(location(first), location(second)) ==
                 llvm::AliasResult::NoAlias,
         "LLVM can separate fresh regions from prior storage and each other");
  expect(aliases.alias(location(ordinary), location(function->getArg(0))) ==
             llvm::AliasResult::MayAlias,
         "unclassified foreign results preserve potential aliasing");
  expect(aliases.alias(location(first), location(derived)) ==
             llvm::AliasResult::MustAlias,
         "freshness does not make later copies of one pointer disjoint");

  // Prove actual optimizer use, including an unknown foreign declaration with
  // the same symbol and an escaped allocation. Metadata shape alone is not
  // sufficient evidence that only the permitted allocation pair disappears.
  std::array<llvm::Function *, 3> probes{};
  auto *observe = llvm::Function::Create(
      llvm::FunctionType::get(ir.getVoidTy(), {session.layouts().pointer()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "application_observe",
      session.module());
  for (unsigned variant = 0; variant < probes.size(); ++variant) {
    auto *probe = llvm::Function::Create(
        llvm::FunctionType::get(ir.getInt32Ty(), {}, false),
        llvm::GlobalValue::ExternalLinkage,
        "allocation_probe_" + std::to_string(variant), session.module());
    probes[variant] = probe;
    llvm::IRBuilder<> body(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    Memory allocation(session, body, {}, {}, {});
    auto *acquire =
        llvm::cast<llvm::CallInst>(allocation.allocateStorage(32, 1));
    allocation.releaseBytes(acquire);
    auto *release = llvm::cast<llvm::CallInst>(&body.GetInsertBlock()->back());
    if (variant == 1) {
      acquire->setAttributes(llvm::AttributeList{});
      release->setAttributes(llvm::AttributeList{});
    } else if (variant == 2) {
      llvm::IRBuilder<> escape(release);
      escape.CreateCall(observe, {acquire});
    }
    llvm::TargetLibraryInfoImpl libraryImpl(session.module().getTargetTriple());
    llvm::TargetLibraryInfo library(libraryImpl);
    expect(llvm::isRemovableAlloc(acquire, &library) == (variant != 1),
           "allocation removability comes from the sealed call edge");
    expect(acquire->onlyAccessesInaccessibleMemory() == (variant != 1),
           "a sealed allocator cannot mutate existing GTI-visible storage");
    expect(release->onlyAccessesInaccessibleMemOrArgMem() == (variant != 1),
           "a sealed release touches only its region and allocator state");
    expect((llvm::getFreedOperand(release, &library) == acquire) ==
               (variant != 1),
           "only sealed release edges identify their freed operand");
    body.CreateRet(body.getInt32(42));
  }
  auto pipeline =
      builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  pipeline.run(session.module(), modules);
  for (unsigned variant = 0; variant < probes.size(); ++variant) {
    unsigned calls = 0;
    for (const auto &block : *probes[variant])
      for (const auto &operation : block)
        calls += llvm::isa<llvm::CallBase>(operation);
    expect(calls == (variant == 0   ? 0U
                     : variant == 1 ? 2U
                                    : 3U),
           "only the unused sealed allocation/release pair is removed");
  }
  session.verify();
}

void sourceProviderContracts(const std::filesystem::path &root) {
  using namespace lang;
  using namespace lang::native_llvm;
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  std::ifstream fixture(root / "tests/fixtures/llvm_memory_provider.gti");
  const std::string source{std::istreambuf_iterator<char>(fixture), {}};
  expect(fixture.good() || fixture.eof(),
         "provider source fixture is readable");
  const auto frontend = gti_test::analyzePrivateMemoryFixture(
      "llvm_memory_provider.gti", source, options, root / "stdlib");
  expect(
      !frontend.sourceGraph.isCompilerTrusted(frontend.sourceGraph.entryUnit()),
      "private lease fixture grants no compiler capability trust");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "private lease provider fixture produces verified MIR");
    return;
  }
  std::vector<LoweredProgramIssue> issues;
  auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!verified) {
    expect(false,
           "private lease provider fixture produces a sealed lowered snapshot");
    return;
  }
  const auto &program = verified->program().mir();
  unsigned calls = 0;
  for (const auto &function : program.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (!target ||
            target->processMemoryOperation ==
                ProcessMemoryRuntimeOperation::None ||
            instruction.unsafeOperation !=
                UnsafeOperationKind::ForeignPointerCall)
          continue;
        ++calls;
        expect(nativeProcessMemoryCall(program, instruction),
               "source process-resource foreign pointer calls have an exact "
               "per-call witness");
        const auto reject = [&](auto mutate) {
          auto changed = instruction;
          mutate(changed);
          expect(!nativeProcessMemoryCall(program, changed),
                 "provider call mutation cannot borrow the trusted native "
                 "adapter witness");
        };
        // Unit mutations exercise the shape predicate, not accepted programs.
        reject([](auto &call) { call.functionTarget.reset(); });
        reject([](auto &call) { call.functionTarget = 0; });
        reject([](auto &call) { call.parameterTypes.clear(); });
        reject([](auto &call) { call.operands[0].type = SemanticType::Bool; });
        reject([](auto &call) {
          call.operands[0].kind = MirOperandKind::BorrowRead;
        });
        reject([](auto &call) { call.info.type = SemanticType::Bool; });
        reject([](auto &call) { call.receiver = call.operands[0]; });
        reject([](auto &call) { call.constructorTarget = 1; });
        reject([](auto &call) { call.lambdaTarget = 1; });
        reject([](auto &call) {
          call.bodyTarget = MirBodyAddress{MirBodyKind::Function, 1};
        });
        reject([](auto &call) { call.nativeCalleeType = SemanticType::Void; });
        reject([](auto &call) {
          call.unsafeOperation = UnsafeOperationKind::None;
        });
        reject([](auto &call) {
          call.definedFailure.propagation = FailurePropagationKind::DirectCall;
        });
        reject([](auto &call) {
          call.definedFailure.localOrigins.emplace_back();
        });
        reject([](auto &call) {
          call.callableInvocation = CallableInvocationCapability::Read;
        });
        reject([](auto &call) { call.dispatch = CallDispatch::Virtual; });
        reject([](auto &call) { call.info.category = ValueCategory::Place; });
        reject([](auto &call) { call.operands[0].place = 1; });
        reject([](auto &call) { call.operands[0].loan = 1; });
      }
  expect(calls >= 2,
         "source allocation and matching release are both inspected");
  const auto admission = planLlvmAdmission(*verified);
  unsigned rows = 0;
  for (const auto &use : admission.requirements())
    if (use.required && use.entry->family == "UnsafeOperationKind" &&
        use.entry->name == "ForeignPointerCall") {
      ++rows;
      expect(
          use.implementedShape &&
              use.implementedShape->name == "NativeProcessMemory",
          "canonical provider witness precedes generic foreign-call admission");
    }
  expect(rows >= 2,
         "the public allocator publishes required provider call requirements");
  // Payload and constructor prerequisites are deliberately not waived by a
  // native-call witness; full source execution is a separate mandatory gate.
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  allocationLayoutWitness();
  providerContracts();
  sourceProviderContracts(std::filesystem::path(argv[1]));
  using namespace lang;
  using Operation = ProcessMemoryRuntimeOperation;
  std::set<std::string_view> symbols;
  for (unsigned index = 1; index < static_cast<unsigned>(Operation::Count);
       ++index) {
    const auto operation = static_cast<Operation>(index);
    const auto *signature = processMemoryRuntimeSignature(operation);
    expect(signature != nullptr,
           "every provider has an explicit immutable signature");
    if (!signature)
      continue;
    expect(signature == processMemoryRuntimeSignature(operation),
           "signature views have stable lifetime");
    expect(signature->operation == operation &&
               symbols.insert(signature->symbol).second,
           "operation/symbol bindings are exact and unique");
    expect(signature->symbol == processMemoryRuntimeSymbol(operation) &&
               exactProcessMemoryRuntimeOperation(
                   signature->symbol, signature->returnType,
                   signature->parameters) == operation,
           "generation and source classification round trip through one table");
    auto wrong = signature->parameters;
    wrong.push_back(SemanticType::UInt64);
    expect(
        exactProcessMemoryRuntimeOperation(
            signature->symbol, signature->returnType, wrong) == Operation::None,
        "a plausible symbol never authorizes a different parameter signature");
    expect(exactProcessMemoryRuntimeOperation(
               signature->symbol, SemanticType::Bool, signature->parameters) ==
               Operation::None,
           "provider result transport is not guessed from a symbol");
    const bool posix = (operation >= Operation::PosixPointerSize &&
                        operation <= Operation::PosixRelease) ||
                       operation == Operation::PosixMalloc;
    const bool aligned = operation == Operation::AlignedAllocate ||
                         operation == Operation::AlignedRelease;
    expect(processMemoryRuntimeSelectedForTarget(operation, "windows") ==
                   !posix &&
               processMemoryRuntimeSelectedForTarget(operation, "linux") ==
                   !aligned &&
               processMemoryRuntimeSelectedForTarget(operation, "macos") ==
                   !aligned,
           "provider/free families follow target policy without cross-family "
           "fallback");
  }
  for (const auto operation :
       {Operation::None, Operation::Count, static_cast<Operation>(255)}) {
    expect(!processMemoryRuntimeSignature(operation) &&
               processMemoryRuntimeSymbol(operation).empty(),
           "sentinel and unknown operations do not default to an allocator");
    expect(!processMemoryRuntimeSelectedForTarget(operation, "linux"),
           "unknown providers are never selected for a target");
  }
  return failures ? 1 : 0;
}
