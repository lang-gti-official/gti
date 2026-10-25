#include "../src/compiler/llvm/atomics.h"
#include "../src/compiler/llvm/layout.h"
#include "cpp_backend_test_support.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>

namespace {
using namespace lang;
using namespace lang::native_llvm;
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void check(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto target = options.target;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_atomic_values.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("atomic fixture must reach verified MIR");
  }
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target),
      issues);
  if (!input)
    throw std::runtime_error("atomic fixture must seal before admission");
  const auto plan = planLlvmAdmission(*input);
  expect(plan.admitted(), plan.limitation());
  const auto &program = input->program().mir();
  Session session(*input, {});
  unsigned storage = 0, constructors = 0, calls = 0, compares = 0;
  for (const auto &owner : program.classInstances()) {
    const auto element = nativeAtomicElement(program, owner.type);
    if (!element)
      continue;
    ++storage;
    const auto layout = session.layouts().atomicStorage(owner.type);
    const auto scalar = session.layouts().get(*element);
    expect(layout.object.size == scalar.size &&
               layout.object.alignment == scalar.alignment &&
               layout.object.storage == scalar.storage &&
               layout.object.addressOnly &&
               layout.lane->getBitWidth() == scalar.size * 8,
           "atomic.storage-domain: exact scalar access width and alignment");
    if (element->kind == SemanticType::Bool)
      expect(layout.lane->getBitWidth() == 8,
             "atomic.storage-domain: bool occupies one byte, not i1 memory");
    auto untrusted = owner.type;
    untrusted.classId = 0;
    expect(!nativeAtomicElement(program, untrusted),
           "atomic.near-miss: storage requires exact capability identity");
  }
  expect(storage == 9,
         "atomic.storage-domain: bool and all eight integer types");
  for (const auto &constructor : program.constructorInstances()) {
    const auto *owner = program.findClassInstance(constructor.owner);
    if (!owner || !nativeAtomicElement(program, owner->type))
      continue;
    ++constructors;
    expect(nativeAtomicConstructor(program, constructor).has_value(),
           "atomic.initialization: exact scalar constructor is represented");
    auto changed = constructor;
    changed.initializers.emplace_back();
    expect(!nativeAtomicConstructor(program, changed),
           "atomic.near-miss: constructor initializer cannot be erased");
    changed = constructor;
    changed.effectSummary.traits.maySynchronize = true;
    expect(!nativeAtomicConstructor(program, changed),
           "atomic.near-miss: constructor effects cannot be erased");
    changed = constructor;
    changed.body.blocks[0].instructions.emplace_back();
    expect(!nativeAtomicConstructor(program, changed),
           "atomic.near-miss: nonempty capability body stays unsupported");
  }
  expect(constructors == 9,
         "atomic.initialization: every scalar has a constructor");
  for (const auto &owner : program.functionInstances())
    for (const auto &block : owner.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!Atomics::handles(instruction.intrinsic))
          continue;
        ++calls;
        expect(nativeAtomicCall(program, owner.body, instruction),
               "atomic.order-shapes: physical source has an exact staged plan");
        const auto reject = [&](auto mutate) {
          auto changed = instruction;
          mutate(changed);
          expect(!nativeAtomicCall(program, owner.body, changed),
                 "atomic.near-miss: malformed per-call facts do not gain a "
                 "witness");
        };
        // Unit shape mutations, never accepted mutable program inputs.
        reject([](auto &call) { call.functionTarget = 1; });
        reject([](auto &call) { call.receiver = call.operands[0]; });
        reject([](auto &call) { call.operands[0].type = SemanticType::Int32; });
        reject([](auto &call) { call.operands[0].place = 0; });
        reject([](auto &call) { call.parameterTypes.clear(); });
        reject([](auto &call) {
          call.synchronization.kind = SynchronizationOperationKind::None;
        });
        reject([](auto &call) {
          call.synchronization.order = AtomicMemoryOrder::Count;
        });
        reject([](auto &call) {
          call.definedFailure.propagation = FailurePropagationKind::DirectCall;
        });
        if (instruction.intrinsic == IntrinsicKind::AtomicCompareExchange) {
          ++compares;
          reject([](auto &call) {
            call.synchronization.failureOrder = AtomicMemoryOrder::Release;
          });
          reject([](auto &call) {
            call.operands[3].kind = MirOperandKind::BorrowRead;
          });
        }
      }
  expect(
      calls >= 9 * 4 && compares >= 9,
      "atomic.order-shapes: all scalar load/store/exchange/CAS paths execute");

  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "atomic_support_probe",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  ir.CreateRetVoid();
  for (const unsigned bits : {8U, 16U, 32U, 64U})
    expect(session.supportsNativeAtomic(*function, bits, bits / 8),
           "atomic.native-support: actual target lowers the complete scalar "
           "domain");
  expect(!session.supportsNativeAtomic(*function, 64, 1) &&
             !session.supportsNativeAtomic(*function, 0, 1) &&
             !session.supportsNativeAtomic(*function, 12, 2) &&
             !session.supportsNativeAtomic(*function, 256, 32),
         "atomic.native-support: unsupported widths and alignment fail closed");
  llvm::Module foreign("foreign", session.context());
  auto *foreignFunction = llvm::Function::Create(
      function->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      "foreign_atomic_support_probe", foreign);
  expect(!session.supportsNativeAtomic(*foreignFunction, 32, 4),
         "atomic.native-support: another module cannot borrow target policy");
  session.verify();
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
    const auto artifact =
        LlvmBackend({.optimization = level}).generate(input->program());
    auto object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(artifact.contents, "atomic-contract.o"));
    if (!object)
      throw std::runtime_error(llvm::toString(object.takeError()));
    for (const auto &symbol : (*object)->symbols()) {
      auto flags = symbol.getFlags();
      auto name = symbol.getName();
      if (!flags)
        throw std::runtime_error(llvm::toString(flags.takeError()));
      if (!name)
        throw std::runtime_error(llvm::toString(name.takeError()));
      if (*flags & llvm::object::BasicSymbolRef::SF_Undefined)
        expect(
            !name->contains("__atomic_") && !name->contains("libatomic"),
            "atomic.native-support: native objects need no hidden libatomic");
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    check(std::filesystem::path(argv[1]));
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return failures ? 1 : 0;
}
