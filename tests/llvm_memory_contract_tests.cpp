#include "../src/compiler/llvm/memory.h"
#include "cpp_backend_test_support.h"
#include "private_stdlib_fixture.h"

#include "gti/process_memory_runtime.h"
#include <fstream>
#include <iterator>

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
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
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
  ir.CreateRetVoid();
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
    const bool posix = operation >= Operation::PosixPointerSize &&
                       operation <= Operation::PosixRelease;
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
