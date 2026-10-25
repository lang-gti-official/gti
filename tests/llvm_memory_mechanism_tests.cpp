#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/memory.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
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
void emitProbe(Session &session, std::string_view name, bool uniqueOnly,
               bool overflow) {
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
  Lifetimes lifetimes(session, failures, {});
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

const char *consumer = R"C(
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif
extern int32_t probe_unique(void);
extern int32_t probe_shared(void);
extern int32_t probe_overflow(uint64_t);
static unsigned attempts, released, fail_at;
static void *regions[4];
static void *allocate(uint64_t bytes, uint64_t alignment, int32_t *status) {
  ++attempts;
  if (attempts == fail_at) { *status = ENOMEM; return NULL; }
  void *address = NULL;
#ifdef _WIN32
  address = _aligned_malloc((size_t)bytes, (size_t)alignment);
  *status = address ? 0 : ENOMEM;
#else
  *status = posix_memalign(&address, (size_t)alignment, (size_t)bytes);
#endif
  if (*status == 0) regions[attempts - 1] = address;
  return address;
}
static void release(void *address) {
  if (!address) return;
  for (unsigned index = 0; index < 4; ++index)
    if (regions[index] == address) { regions[index] = NULL; ++released; break; }
#ifdef _WIN32
  _aligned_free(address);
#else
  free(address);
#endif
}
void *gti_rt_posix_memalign_v1(uint64_t bytes, uint64_t alignment, int32_t *status) {
  return allocate(bytes, alignment, status);
}
void gti_rt_posix_free_v1(void *address) { release(address); }
void *gti_rt_aligned_malloc_v1(uint64_t bytes, uint64_t alignment) {
  int32_t status; return allocate(bytes, alignment, &status);
}
void gti_rt_aligned_free_v1(void *address) { release(address); }
int main(void) {
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
          driver::ArtifactWriteStatus::Success ||
      !toolchain
           .invoke(driver::NativeCCompileRequest(
               driver::discoverCCompiler(), csource, root / "consumer.o",
               driver::CStandard::C17, OptimizationLevel::O2, {}, {}))
           .succeeded())
    return 2;
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    Session session(*input, {.optimization = level});
    emitProbe(session, "probe_unique", true, false);
    emitProbe(session, "probe_shared", false, false);
    emitProbe(session, "probe_overflow", false, true);
    const auto object = session.emitObject();
    const auto path = root / "probe.o";
    if (driver::writeArtifact(path, object.contents) !=
        driver::ArtifactWriteStatus::Success)
      return 3;
    driver::NativeInputs inputs;
    inputs.libraryFiles.push_back(root / "consumer.o");
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
