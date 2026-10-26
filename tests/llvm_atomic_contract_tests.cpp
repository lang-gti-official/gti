#include "../src/compiler/llvm/atomics.h"
#include "../src/compiler/llvm/layout.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "cpp_backend_test_support.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

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

// Test-only C-shaped lanes for the parking oracle. These use the real atomic
// emitter and exact source-verified MIR plans; they are not GTI body wrappers,
// a production foreign-object ABI, or admission of AtomicWait/AtomicNotify.
void emitWaitLanes(const std::filesystem::path &root,
                   const std::filesystem::path &output) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_atomic_wait_lanes.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("wait lane fixture must reach verified MIR");
  }
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  if (!input)
    throw std::runtime_error("wait lane fixture must seal before emission");
  const auto admission = planLlvmAdmission(*input);
  if (!admission.admitted())
    throw std::runtime_error(admission.limitation());
  Session session(*input, {});
  const auto &program = input->program().mir();
  struct Plan {
    const MirBody *body = nullptr;
    const MirInstruction *instruction = nullptr;
  };
  std::array<Plan, 4> loads{}, stores{};
  for (const auto &owner : program.functionInstances())
    for (const auto &block : owner.body.blocks)
      for (const auto &instruction : block.instructions) {
        const bool load = instruction.intrinsic == IntrinsicKind::AtomicLoad;
        const bool store = instruction.intrinsic == IntrinsicKind::AtomicStore;
        if ((!load && !store) ||
            !nativeAtomicCall(program, owner.body, instruction))
          continue;
        if (instruction.synchronization.order !=
            (load ? AtomicMemoryOrder::Acquire : AtomicMemoryOrder::Release))
          continue;
        const auto element = nativeAtomicElement(
            program, instruction.operands[0].type.arguments[0]);
        unsigned index = 0;
        switch (element->kind) {
        case SemanticType::UInt8:
          index = 0;
          break;
        case SemanticType::UInt16:
          index = 1;
          break;
        case SemanticType::UInt32:
          index = 2;
          break;
        case SemanticType::UInt64:
          index = 3;
          break;
        default:
          continue;
        }
        (load ? loads : stores)[index] = {&owner.body, &instruction};
      }
  for (unsigned index = 0; index < loads.size(); ++index) {
    if (!loads[index].body || !stores[index].body)
      throw std::runtime_error(
          "wait lanes need all four exact load/store plans");
    const auto bits = 8U << index;
    for (const bool load : {false, true}) {
      auto *word = llvm::Type::getInt64Ty(session.context());
      auto *pointer = session.layouts().pointer();
      const std::vector<llvm::Type *> parameters =
          load ? std::vector<llvm::Type *>{pointer}
               : std::vector<llvm::Type *>{pointer, word};
      auto *type = llvm::FunctionType::get(
          load ? word : llvm::Type::getVoidTy(session.context()), parameters,
          false);
      auto *function = llvm::Function::Create(
          type, llvm::GlobalValue::ExternalLinkage,
          std::string("gti_test_llvm_") + (load ? "load" : "store") +
              std::to_string(bits),
          session.module());
      llvm::IRBuilder<> ir(
          llvm::BasicBlock::Create(session.context(), "entry", function));
      Atomics atomics(session, ir);
      const auto &plan = (load ? loads : stores)[index];
      std::vector<llvm::Value *> operands{function->getArg(0)};
      if (!load)
        operands.push_back(ir.CreateTruncOrBitCast(
            function->getArg(1),
            llvm::IntegerType::get(session.context(), bits)));
      auto *result = atomics.emit(*plan.body, *plan.instruction, operands);
      if (load)
        ir.CreateRet(ir.CreateZExtOrBitCast(result, word));
      else
        ir.CreateRetVoid();
    }
  }
  session.verify();
  std::error_code error;
  llvm::raw_fd_ostream stream(output.string(), error);
  if (error)
    throw std::runtime_error(error.message());
  session.module().print(stream, nullptr);
  stream.close();
  if (stream.has_error())
    throw std::runtime_error("could not write test-only wait lane IR");
}

void check(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
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
  unsigned storage = 0, constructors = 0, calls = 0, compares = 0, copied = 0;
  const MirBody *storeBody = nullptr, *compareBody = nullptr;
  const MirInstruction *storePlan = nullptr, *comparePlan = nullptr;
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
        if (instruction.intrinsic == IntrinsicKind::AtomicStore) {
          for (const auto &place : owner.body.places)
            if (place.type == instruction.operands[1].type) {
              for (const auto kind :
                   {MirOperandKind::Copy, MirOperandKind::Move}) {
                auto changed = instruction;
                changed.operands[1] = {
                    .kind = kind, .place = place.id, .type = place.type};
                expect(nativeAtomicCall(program, owner.body, changed),
                       "atomic.scalar-place: exact Copy/Move uses existing "
                       "materializer");
                const auto exact = changed;
                changed.operands[1].place = 0;
                expect(!nativeAtomicCall(program, owner.body, changed),
                       "atomic.near-miss: missing scalar place fails closed");
                changed = exact;
                changed.operands[1].value = 1;
                expect(!nativeAtomicCall(program, owner.body, changed),
                       "atomic.near-miss: conflicting place/SSA encoding fails "
                       "closed");
                changed = exact;
                changed.operands[1].loan = 1;
                expect(!nativeAtomicCall(program, owner.body, changed),
                       "atomic.near-miss: scalar place cannot carry a loan "
                       "encoding");
                changed = exact;
                changed.operands[1].type = SemanticType::Unknown;
                expect(
                    !nativeAtomicCall(program, owner.body, changed),
                    "atomic.near-miss: wrong scalar place type fails closed");
                ++copied;
              }
              break;
            }
          if (instruction.operands[1].type == SemanticType::Int32) {
            storeBody = &owner.body;
            storePlan = &instruction;
          }
        }
        if (instruction.intrinsic == IntrinsicKind::AtomicCompareExchange) {
          ++compares;
          reject([](auto &call) {
            call.synchronization.failureOrder = AtomicMemoryOrder::Release;
          });
          reject([](auto &call) {
            call.operands[3].kind = MirOperandKind::BorrowRead;
          });
          if (instruction.operands[1].type == SemanticType::Int32) {
            compareBody = &owner.body;
            comparePlan = &instruction;
          }
        }
      }
  expect(
      calls >= 9 * 4 && compares >= 9,
      "atomic.order-shapes: all scalar load/store/exchange/CAS paths execute");
  expect(copied >= 18,
         "atomic.scalar-place: Copy and Move for every scalar type");

  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "atomic_support_probe",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  expect(storePlan && comparePlan,
         "atomic.materialization: exact int32 plans exist");
  if (storePlan && comparePlan) {
    auto *slot = ir.CreateAlloca(ir.getInt32Ty());
    Atomics atomics(session, ir);
    const auto rejectLanes = [&](const MirBody &body,
                                 const MirInstruction &plan,
                                 std::span<llvm::Value *const> inputs) {
      const auto before = ir.GetInsertBlock()->size();
      bool rejected = false;
      try {
        static_cast<void>(atomics.emit(body, plan, inputs));
      } catch (const BackendUnsupportedError &) {
        rejected = true;
      }
      expect(rejected && ir.GetInsertBlock()->size() == before,
             "atomic.materialization: wrong scalar/address lane emits no "
             "memory operation");
    };
    const std::array<llvm::Value *, 2> pointerValue{slot, slot};
    rejectLanes(*storeBody, *storePlan, pointerValue);
    const std::array<llvm::Value *, 4> scalarAddress{
        slot, ir.getInt32(0), ir.getInt32(1), ir.getInt32(0)};
    rejectLanes(*compareBody, *comparePlan, scalarAddress);
  }
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
void checkWaiting(const std::filesystem::path &root,
                  const std::filesystem::path &output = {}) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_atomic_wait_ready.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("wait fixture must reach verified MIR");
  }
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, options.target),
      issues);
  if (!input)
    throw std::runtime_error("wait fixture must seal before representation checks");
  Session session(*input, {});
  const auto &program = input->program().mir();
  std::set<SemanticType::Kind> elements;
  std::set<AtomicMemoryOrder> orders;
  unsigned waits = 0, notifications = 0;
  for (const auto &owner : program.functionInstances())
    for (const auto &block : owner.body.blocks)
      for (const auto &instruction : block.instructions) {
        const bool wait = instruction.intrinsic == IntrinsicKind::AtomicWait;
        if (!wait && instruction.intrinsic != IntrinsicKind::AtomicNotifyOne &&
            instruction.intrinsic != IntrinsicKind::AtomicNotifyAll)
          continue;
        expect(nativeAtomicWaitingCall(program, owner.body, instruction),
               "wait.exact-plan: source uses exact storage, operands and order");
        expect(!nativeAtomicCall(program, owner.body, instruction),
               "wait.admission: scalar atomic evidence never admits blocking");
        const auto reject = [&](auto mutate) {
          auto changed = instruction;
          mutate(changed);
          expect(!nativeAtomicWaitingCall(program, owner.body, changed),
                 "wait.near-miss: malformed protocol is not representable");
        };
        reject([](auto &call) { call.operands[0].kind = MirOperandKind::Move; });
        reject([](auto &call) { call.parameterTypes.clear(); });
        reject([](auto &call) { call.result = 1; });
        reject([](auto &call) { call.borrowAccess = AccessMode::Mutable; });
        reject([](auto &call) { call.synchronization.kind = SynchronizationOperationKind::AtomicLoad; });
        if (wait) {
          ++waits;
          orders.insert(*instruction.synchronization.order);
          reject([](auto &call) { call.synchronization.order = AtomicMemoryOrder::Release; });
          reject([](auto &call) { call.synchronization.order.reset(); });
          reject([](auto &call) { call.operands[1].value = 0; });
          reject([](auto &call) { call.operands[1].type = SemanticType::Float; });
        } else {
          ++notifications;
          reject([](auto &call) { call.synchronization.order = AtomicMemoryOrder::Acquire; });
        }
        const auto layout = session.layouts().atomicStorage(
            instruction.operands[0].type.arguments[0]);
        elements.insert(layout.element.kind);
        const auto domain = constantIntegerDomain(layout.element);
        const std::string lane = layout.element == SemanticType::Bool
                                     ? "bool"
                                     : std::string(domain->signedValue ? "i" : "u") +
                                           std::to_string(session.layouts().get(layout.element).storage->getIntegerBitWidth());
        const std::string operation = wait
            ? "wait_" + std::to_string(static_cast<unsigned>(*instruction.synchronization.order))
            : instruction.intrinsic == IntrinsicKind::AtomicNotifyOne ? "one" : "all";
        const auto name = "gti_test_wait_" + lane + "_" + operation;
        if (session.module().getFunction(name))
          continue;
        std::vector<llvm::Type *> parameters{session.layouts().pointer()};
        if (wait)
          parameters.push_back(llvm::Type::getInt64Ty(session.context()));
        auto *signature = llvm::FunctionType::get(
            llvm::Type::getVoidTy(session.context()), parameters, false);
        auto *function = llvm::Function::Create(
            signature, llvm::GlobalValue::ExternalLinkage,
            name, session.module());
        llvm::IRBuilder<> ir(llvm::BasicBlock::Create(session.context(), "entry", function));
        std::vector<llvm::Value *> inputs;
        inputs.push_back(function->getArg(0));
        if (wait)
          inputs.push_back(ir.CreateTruncOrBitCast(
              function->getArg(1), session.layouts().get(layout.element).value));
        expect(Atomics(session, ir).emit(owner.body, instruction, inputs) == nullptr,
               "wait.result: no result publication or invented failure channel");
        ir.CreateRetVoid();
      }
  expect(elements.size() == 9 && orders.size() == 3 && waits >= 27 && notifications >= 18,
         "wait.domain: bool and eight integers, all load orders and both notifications");
  auto *wait = declareRuntimeService(session, RuntimeService::AtomicWait);
  auto *notify = declareRuntimeService(session, RuntimeService::AtomicNotify);
  expect(wait->arg_size() == 4 && wait->getReturnType()->isVoidTy() &&
             wait->getArg(0)->getType()->isPointerTy() &&
             wait->getArg(1)->getType()->isIntegerTy(64) &&
             wait->getArg(2)->getType()->isIntegerTy(32) &&
             wait->getArg(3)->getType()->isIntegerTy(32) &&
             notify->arg_size() == 3 && notify->getReturnType()->isVoidTy(),
         "wait.c-abi: exact versioned service signatures");
  session.verify();
  if (!output.empty()) {
    std::error_code error;
    llvm::raw_fd_ostream stream(output.string(), error);
    if (error)
      throw std::runtime_error(error.message());
    session.module().print(stream, nullptr);
    stream.close();
    if (stream.has_error())
      throw std::runtime_error("could not write test-only wait service IR");
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 &&
      !(argc == 4 && (std::string_view(argv[2]) == "--emit-wait-lanes" ||
                     std::string_view(argv[2]) == "--emit-wait-services")))
    return 2;
  try {
    if (argc == 4 && std::string_view(argv[2]) == "--emit-wait-services")
      checkWaiting(std::filesystem::path(argv[1]), std::filesystem::path(argv[3]));
    else if (argc == 4)
      emitWaitLanes(std::filesystem::path(argv[1]),
                    std::filesystem::path(argv[3]));
    else {
      check(std::filesystem::path(argv[1]));
      checkWaiting(std::filesystem::path(argv[1]));
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return failures ? 1 : 0;
}
