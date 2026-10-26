#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/memory.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/llvm_admission.h"
#include "gti/mir_effects.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace lang;
using namespace lang::native_llvm;

// Mechanical component execution, not language admission or source-failure
// attribution evidence. Public owner programs remain separate mandatory tests.
void emitProbe(Session &session, const LlvmAdmissionPlan &admission,
               std::string_view name, bool uniqueOnly, bool overflow) {
  auto &context = session.context();
  auto *type = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(context),
      overflow ? std::vector<llvm::Type *>{llvm::Type::getInt64Ty(context)}
               : std::vector<llvm::Type *>{},
      false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, session.module());
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", function));
  const auto check = [&](llvm::Value *bad, int code) {
    auto *failed = llvm::BasicBlock::Create(context, "failed", function);
    auto *next = llvm::BasicBlock::Create(context, "next", function);
    ir.CreateCondBr(bad, failed, next);
    ir.SetInsertPoint(failed);
    ir.CreateRet(ir.getInt32(code));
    ir.SetInsertPoint(next);
  };
  Failures failures(session);
  Dispatch dispatch(session, admission);
  Lifetimes lifetimes(session, failures, dispatch, {});
  Memory memory(
      session, ir,
      [&](const MirDropType &recipe) { return lifetimes.cleanup(recipe); },
      [&](llvm::Value *address, const SemanticType &selected) {
        lifetimes.moved(ir, address, selected);
      },
      [&](llvm::Value *bad, DefinedFailureOutcome outcome) {
        if (outcome.code != DefinedFailureCode::AllocationFailure)
          throw std::logic_error("unexpected mechanism guard");
        check(bad,
              outcome.detail == DefinedFailureDetail::UniqueOwner ? 91 : 92);
      });
  const auto operation = [&](IntrinsicKind intrinsic,
                             std::span<llvm::Value *const> inputs) {
    MirInstruction instruction;
    instruction.intrinsic = intrinsic;
    instruction.operands.resize(inputs.size());
    return memory.emit(instruction, inputs, nullptr, false).value;
  };
  const auto &layout = session.layouts().sharedOwner();
  if (overflow) {
    auto *block = ir.CreateAlloca(layout.block.storage);
    ir.CreateStore(
        function->getArg(0),
        ir.CreateStructGEP(layout.block.storage, block, layout.blockStrong));
    ir.CreateStore(llvm::ConstantPointerNull::get(session.layouts().pointer()),
                   ir.CreateStructGEP(layout.block.storage, block,
                                      layout.blockAllocation));
    auto *handle = ir.CreateAlloca(layout.handle.storage);
    llvm::Value *value = llvm::Constant::getNullValue(layout.handle.storage);
    value = ir.CreateInsertValue(value, block, layout.handleBlock);
    ir.CreateStore(value, handle);
    const std::array<llvm::Value *, 1> inputs{handle};
    auto *clone = operation(IntrinsicKind::SharedOwnerClone, inputs);
    const std::array<llvm::Value *, 1> cloned{clone};
    check(ir.CreateICmpNE(operation(IntrinsicKind::SharedOwnerUseCount, cloned),
                          ir.getInt64(UINT64_MAX)),
          11);
    ir.CreateRet(ir.getInt32(0));
    return;
  }
  MirInstruction allocation;
  allocation.kind = MirInstructionKind::Call;
  allocation.intrinsic = IntrinsicKind::AllocateUniqueOwner;
  allocation.info.type = SemanticType::uniqueOwnerOf(SemanticType::Int32);
  auto *address = memory.allocateObject(allocation);
  ir.CreateStore(ir.getInt32(73), address);
  auto *source = ir.CreateAlloca(session.layouts().pointer());
  ir.CreateStore(address, source);
  auto *destination = ir.CreateAlloca(session.layouts().pointer());
  ir.CreateStore(ir.CreateLoad(session.layouts().pointer(), source),
                 destination);
  lifetimes.moved(ir, source, allocation.info.type);
  check(ir.CreateIsNotNull(ir.CreateLoad(session.layouts().pointer(), source)),
        12);
  if (uniqueOnly) {
    const auto recipe =
        mirUniqueCleanupType(session.program().mir(), allocation.info.type);
    if (!recipe)
      throw std::logic_error("missing canonical scalar unique recipe");
    auto *channel = ir.CreateAlloca(session.abi().failureChannel());
    auto *status = ir.CreateCall(lifetimes.cleanup(*recipe),
                                 {destination, channel, ir.getFalse()});
    check(ir.CreateNot(status), 13);
    check(ir.CreateIsNotNull(
              ir.CreateLoad(session.layouts().pointer(), destination)),
          14);
    // Empty cleanup is idempotent and does not release the allocation twice.
    check(ir.CreateNot(ir.CreateCall(lifetimes.cleanup(*recipe),
                                     {destination, channel, ir.getFalse()})),
          15);
  } else {
    const std::array<llvm::Value *, 1> input{address};
    auto *first = operation(IntrinsicKind::SharedOwnerFromUnique, input);
    const std::array<llvm::Value *, 1> firstInput{first};
    check(ir.CreateICmpNE(
              operation(IntrinsicKind::SharedOwnerUseCount, firstInput),
              ir.getInt64(1)),
          16);
    auto *second = operation(IntrinsicKind::SharedOwnerClone, firstInput);
    const std::array<llvm::Value *, 1> secondInput{second};
    check(ir.CreateICmpNE(
              operation(IntrinsicKind::SharedOwnerUseCount, secondInput),
              ir.getInt64(2)),
          17);
    check(memory.releaseShared(first).last, 18);
    check(ir.CreateICmpNE(
              operation(IntrinsicKind::SharedOwnerUseCount, secondInput),
              ir.getInt64(1)),
          19);
    check(ir.CreateNot(operation(IntrinsicKind::SharedOwnerIsNull, firstInput)),
          20);
    const auto last = memory.releaseShared(second);
    check(ir.CreateNot(last.last), 21);
    check(ir.CreateICmpNE(last.allocation, address), 22);
    check(ir.CreateICmpNE(ir.CreateLoad(ir.getInt32Ty(), last.allocation),
                          ir.getInt32(73)),
          23);
    memory.releaseBytes(last.allocation);
    memory.releaseBytes(last.block);
    check(memory.releaseShared(second).last, 24);
  }
  ir.CreateRet(ir.getInt32(0));
}

void emitConcurrentProbes(Session &session) {
  auto &context = session.context();
  auto *pointer = session.layouts().pointer();
  const auto &layout = session.layouts().sharedOwner();
  auto *size = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), false),
      llvm::GlobalValue::ExternalLinkage, "probe_handle_size",
      session.module());
  llvm::IRBuilder<> sizeIr(llvm::BasicBlock::Create(context, "entry", size));
  sizeIr.CreateRet(sizeIr.getInt64(layout.handle.size));
  auto *init = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "probe_handle_pair",
      session.module());
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", init));
  Memory memory(
      session, ir, {}, {}, [&](llvm::Value *bad, DefinedFailureOutcome) {
        auto *failed = llvm::BasicBlock::Create(context, "failed", init);
        auto *next = llvm::BasicBlock::Create(context, "next", init);
        ir.CreateCondBr(bad, failed, next);
        ir.SetInsertPoint(failed);
        ir.CreateRet(ir.getInt32(90));
        ir.SetInsertPoint(next);
      });
  MirInstruction allocation;
  allocation.kind = MirInstructionKind::Call;
  allocation.intrinsic = IntrinsicKind::AllocateUniqueOwner;
  allocation.info.type = SemanticType::uniqueOwnerOf(SemanticType::Int32);
  auto *address = memory.allocateObject(allocation);
  MirInstruction operation;
  operation.intrinsic = IntrinsicKind::SharedOwnerFromUnique;
  operation.operands.resize(1);
  const std::array<llvm::Value *, 1> source{address};
  auto *first = memory.emit(operation, source, nullptr, false).value;
  operation.intrinsic = IntrinsicKind::SharedOwnerClone;
  const std::array<llvm::Value *, 1> input{first};
  auto *second = memory.emit(operation, input, nullptr, false).value;
  ir.CreateStore(ir.CreateLoad(layout.handle.storage, first), init->getArg(0));
  ir.CreateStore(ir.CreateLoad(layout.handle.storage, second), init->getArg(1));
  ir.CreateRet(ir.getInt32(0));

  auto *release = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {pointer},
                              false),
      llvm::GlobalValue::ExternalLinkage, "probe_handle_release",
      session.module());
  llvm::IRBuilder<> releaseIr(
      llvm::BasicBlock::Create(context, "entry", release));
  Memory releasing(session, releaseIr, {}, {}, {});
  const auto released = releasing.releaseShared(release->getArg(0));
  auto *last = llvm::BasicBlock::Create(context, "last", release);
  auto *done = llvm::BasicBlock::Create(context, "done", release);
  releaseIr.CreateCondBr(released.last, last, done);
  releaseIr.SetInsertPoint(last);
  releasing.releaseBytes(released.allocation);
  releasing.releaseBytes(released.block);
  releaseIr.CreateBr(done);
  releaseIr.SetInsertPoint(done);
  releaseIr.CreateRet(releaseIr.getInt32(0));
}

void emitArrayRangeProbe(Session &session) {
  auto &context = session.context();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false),
      llvm::GlobalValue::ExternalLinkage, "probe_array_range",
      session.module());
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", function));
  const auto array = SemanticType::arrayOf(SemanticType::Int32, 4);
  const auto layout = session.layouts().get(array);
  auto *source = ir.CreateAlloca(layout.storage);
  auto *destination = ir.CreateAlloca(layout.storage);
  ir.CreateStore(llvm::Constant::getNullValue(layout.storage), destination);
  for (unsigned index = 0; index < 4; ++index)
    ir.CreateStore(ir.getInt32((index + 1) * 11),
                   ir.CreateGEP(layout.storage, source,
                                {ir.getInt64(0), ir.getInt64(index)}));
  MirInstruction instruction;
  instruction.kind = MirInstructionKind::Call;
  instruction.intrinsic = IntrinsicKind::ObjectRelocateRange;
  instruction.unsafeOperation = UnsafeOperationKind::ObjectLifetime;
  instruction.info.type = SemanticType::Void;
  instruction.info.traits = semanticTraits(SemanticType::Void);
  instruction.parameterTypes = {SemanticType::rawPointerTo(array),
                                SemanticType::rawPointerTo(array),
                                SemanticType::UInt64};
  for (std::size_t index = 0; index < 3; ++index)
    instruction.operands.push_back({.kind = MirOperandKind::Value,
                                    .value = index + 1,
                                    .type = instruction.parameterTypes[index]});
  Memory memory(session, ir, {}, {}, {});
  const std::array<llvm::Value *, 3> inputs{destination, source,
                                            ir.getInt64(1)};
  (void)memory.emit(instruction, inputs, nullptr, false);
  llvm::Value *correct = ir.getTrue();
  for (unsigned index = 0; index < 4; ++index)
    correct = ir.CreateAnd(
        correct,
        ir.CreateICmpEQ(
            ir.CreateLoad(ir.getInt32Ty(),
                          ir.CreateGEP(layout.storage, destination,
                                       {ir.getInt64(0), ir.getInt64(index)})),
            ir.getInt32((index + 1) * 11)));
  ir.CreateRet(ir.CreateSelect(correct, ir.getInt32(0), ir.getInt32(1)));
}

const char *consumer = R"C(
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
extern int32_t probe_unique(void);
extern int32_t probe_shared(void);
extern int32_t probe_overflow(uint64_t);
extern uint64_t probe_handle_size(void);
extern int32_t probe_handle_pair(void*, void*);
extern int32_t probe_handle_release(void*);
extern int32_t probe_array_range(void);
static unsigned attempts, released, fail_at;
static void *regions[4];
static int guarded;
static _Atomic int start;
static void *allocate(uint64_t bytes, uint64_t alignment, int32_t *status) {
  ++attempts;
  if (attempts == fail_at) { *status = ENOMEM; return NULL; }
  void *address = NULL;
  if (guarded) {
#ifdef _WIN32
    address = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    address = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) address = NULL;
#endif
    *status = address ? 0 : ENOMEM;
  } else {
#ifdef _WIN32
  address = _aligned_malloc((size_t)bytes, (size_t)alignment);
  *status = address ? 0 : ENOMEM;
#else
  *status = posix_memalign(&address, (size_t)alignment, (size_t)bytes);
#endif
  }
  if (*status == 0) regions[attempts - 1] = address;
  return address;
}
static void release(void *address) {
  if (!address) return;
  for (unsigned index = 0; index < 4; ++index)
    if (regions[index] == address) { regions[index] = NULL; ++released; break; }
  if (guarded) {
#ifdef _WIN32
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, (size_t)sysconf(_SC_PAGESIZE));
#endif
  } else {
#ifdef _WIN32
    _aligned_free(address);
#else
    free(address);
#endif
  }
}
void *gti_rt_posix_memalign_v1(uint64_t bytes, uint64_t alignment, int32_t *status) {
  return allocate(bytes, alignment, status);
}
void gti_rt_posix_free_v1(void *address) { release(address); }
void *gti_rt_aligned_malloc_v1(uint64_t bytes, uint64_t alignment) {
  int32_t status; return allocate(bytes, alignment, &status);
}
void gti_rt_aligned_free_v1(void *address) { release(address); }
#ifdef _WIN32
static DWORD WINAPI worker(void *handle) {
#else
static void *worker(void *handle) {
#endif
  while (!atomic_load_explicit(&start, memory_order_acquire)) { }
  intptr_t result = probe_handle_release(handle);
#ifdef _WIN32
  return (DWORD)result;
#else
  return (void*)result;
#endif
}
int main(void) {
  if (probe_array_range()) return 10;
  if (probe_unique() || attempts != 1 || released != 1) return 1;
  attempts = released = 0;
  if (probe_shared() || attempts != 2 || released != 2) return 2;
  attempts = released = 0; fail_at = 1;
  if (probe_shared() != 91 || attempts != 1 || released != 0) return 3;
  attempts = released = 0; fail_at = 2;
  if (probe_shared() != 92 || attempts != 2 || released != 0) return 4;
  // The terminal-allocation path deliberately performs no GTI cleanup.
  // This mechanical test returns a code instead of terminating the process;
  // reclaim its retained probe allocation in the harness, not in lowering.
  release(regions[0]);
  if (probe_overflow(UINT64_MAX) != 92 || probe_overflow(UINT64_MAX - 1)) return 5;
  // Distinct handles only. Unmapping the block immediately on final release
  // makes a post-decrement non-last block read an actual inaccessible access,
  // rather than relying on malloc to overwrite or recycle a freed block.
  fail_at = 0; guarded = 1;
  for (unsigned iteration = 0; iteration < 512; ++iteration) {
    attempts = released = 0;
    size_t bytes = (size_t)probe_handle_size();
    void *first = calloc(1, bytes), *second = calloc(1, bytes);
    if (!first || !second || probe_handle_pair(first, second)) return 6;
    atomic_store_explicit(&start, 0, memory_order_relaxed);
#ifdef _WIN32
    HANDLE a = CreateThread(NULL, 0, worker, first, 0, NULL);
    HANDLE b = CreateThread(NULL, 0, worker, second, 0, NULL);
    if (!a || !b) return 7;
    atomic_store_explicit(&start, 1, memory_order_release);
    WaitForSingleObject(a, INFINITE); WaitForSingleObject(b, INFINITE);
    DWORD x, y; GetExitCodeThread(a, &x); GetExitCodeThread(b, &y);
    CloseHandle(a); CloseHandle(b);
    if (x || y) return 8;
#else
    pthread_t a, b;
    if (pthread_create(&a, NULL, worker, first) || pthread_create(&b, NULL, worker, second)) return 7;
    atomic_store_explicit(&start, 1, memory_order_release);
    void *x, *y; pthread_join(a, &x); pthread_join(b, &y);
    if (x || y) return 8;
#endif
    free(first); free(second);
    if (attempts != 2 || released != 2) return 9;
  }
  return 0;
}
)C";
} // namespace

int main() {
  namespace fs = std::filesystem;
  const auto frontend =
      Frontend().analyze("memory-mechanism.gti", "int main() { return 0; }");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    return 1;
  const auto root =
      fs::temp_directory_path() /
      ("gti-llvm-memory-mechanisms-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(root);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};
  driver::NativeToolchain toolchain;
  const auto csource = root / "consumer.c";
  if (driver::writeArtifact(csource, consumer) !=
      driver::ArtifactWriteStatus::Success)
    return 2;
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), csource, root / "consumer.o",
      driver::CStandard::C17, OptimizationLevel::O2, {}, {}));
  if (!compiled.succeeded()) {
    std::cerr << compiled.output << compiled.driverDiagnostic.value_or("")
              << '\n';
    return 2;
  }
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    const LlvmBackendOptions options{.optimization = level};
    const auto admission = planLlvmAdmission(*input, options);
    if (!admission.admitted())
      return 3;
    Session session(*input, options);
    emitProbe(session, admission, "probe_unique", true, false);
    emitProbe(session, admission, "probe_shared", false, false);
    emitProbe(session, admission, "probe_overflow", false, true);
    emitConcurrentProbes(session);
    emitArrayRangeProbe(session);
    const auto object = session.emitObject();
    const auto path = root / "probe.o";
    if (driver::writeArtifact(path, object.contents) !=
        driver::ArtifactWriteStatus::Success)
      return 3;
    driver::NativeInputs inputs;
    inputs.libraryFiles.push_back(root / "consumer.o");
    if (session.program().target().os != "windows")
      inputs.libraries.push_back("pthread");
    const auto executable = root / "probe";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), path, executable, inputs));
    if (!linked.succeeded()) {
      std::cerr << linked.output;
      return 4;
    }
    const auto result = driver::invokeProcess(
        {executable.string()},
        {.timeout = std::chrono::seconds(20), .isolateProcessGroup = true});
    if (!result.succeeded()) {
      std::cerr << "memory mechanism failure " << result.exitCode << '\n'
                << result.output;
      return 5;
    }
  }
  std::cout << "native O0-O3 memory mechanisms: move/drop, retain/release, "
               "exhaustion and overflow passed\n";
}
