#include "gti/support.h"

#include <cstdio>
#include <cstdlib>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TimeProfiler.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GTI_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define GTI_ADDRESS_SANITIZER 1
#endif
#if defined(GTI_ADDRESS_SANITIZER)
#include <sanitizer/common_interface_defs.h>
#endif

// Same-thread stack switching. gti_run_on_native_stack(top, entry, context)
// moves the stack pointer to `top`, calls entry(context) there and returns on
// the original stack. Calls and returns stay balanced and the frame-pointer
// chain links back to the caller, so backtraces cross the switch. Exceptions
// never cross it: the entry function catches everything on the segment.
//
// Why not llvm::runOnNewStack (llvm/Support/ProgramStack.h): it exists only
// from LLVM 21, while GTI supports LLVM 18 and Linux CI builds with LLVM 20;
// distributed builds run its function on a new llvm::thread (the Homebrew
// LLVM 23 arm64 macOS library does, by disassembly), and thread-local
// compiler state must not change under a switch; and where it does switch
// stacks it uses malloc'ed memory without a guard page and, as a prebuilt
// uninstrumented library, without AddressSanitizer fiber annotations.
//
// Only the System V x86-64 and AAPCS64 calling conventions are implemented,
// for Mach-O and ELF. Elsewhere (Windows x64's Microsoft ABI, other
// architectures; none is a supported GTI host) there is no switch at all:
// recursion stays on the thread's own stack, exactly as before segments
// existed, rather than moving to a helper thread whose thread-local state
// would differ.
#if (defined(__x86_64__) || defined(__aarch64__)) &&                           \
    (defined(__APPLE__) || defined(__ELF__))
#define GTI_NATIVE_STACK_SWITCH 1
extern "C" void gti_run_on_native_stack(void *top, void (*entry)(void *),
                                        void *context) noexcept;
#if defined(__APPLE__)
#define GTI_STACK_SWITCH_PROLOGUE                                              \
  ".text\n.p2align 4\n.globl _gti_run_on_native_stack\n"                       \
  ".private_extern _gti_run_on_native_stack\n_gti_run_on_native_stack:\n"
#define GTI_STACK_SWITCH_EPILOGUE ""
#else
#define GTI_STACK_SWITCH_PROLOGUE                                              \
  ".pushsection .text\n.p2align 4\n.globl gti_run_on_native_stack\n"           \
  ".hidden gti_run_on_native_stack\n"                                          \
  ".type gti_run_on_native_stack,%function\ngti_run_on_native_stack:\n"
#define GTI_STACK_SWITCH_EPILOGUE                                              \
  ".size gti_run_on_native_stack,.-gti_run_on_native_stack\n.popsection\n"
#endif
#if defined(__x86_64__)
// System V: top in rdi, entry in rsi, context in rdx. rbx (callee-saved)
// keeps the original stack pointer across the call.
asm(GTI_STACK_SWITCH_PROLOGUE "  pushq %rbp\n"
                              "  movq %rsp, %rbp\n"
                              "  pushq %rbx\n"
                              "  movq %rsp, %rbx\n"
                              "  movq %rdi, %rsp\n"
                              "  movq %rdx, %rdi\n"
                              "  callq *%rsi\n"
                              "  movq %rbx, %rsp\n"
                              "  popq %rbx\n"
                              "  popq %rbp\n"
                              "  retq\n" GTI_STACK_SWITCH_EPILOGUE);
#else
// AAPCS64: top in x0, entry in x1, context in x2. x19 (callee-saved) keeps
// the original stack pointer across the call.
asm(GTI_STACK_SWITCH_PROLOGUE "  stp x29, x30, [sp, #-32]!\n"
                              "  mov x29, sp\n"
                              "  str x19, [sp, #16]\n"
                              "  mov x19, sp\n"
                              "  mov sp, x0\n"
                              "  mov x0, x2\n"
                              "  blr x1\n"
                              "  mov sp, x19\n"
                              "  ldr x19, [sp, #16]\n"
                              "  ldp x29, x30, [sp], #32\n"
                              "  ret\n" GTI_STACK_SWITCH_EPILOGUE);
#endif
#endif

namespace lang {

namespace {

// LLVM support failures are internal toolchain failures, never GTI source
// diagnostics. Report deterministically on stderr and abort; without an
// installed handler LLVM would abort through its own reporting instead.
void reportToolchainFatal(void *, const char *reason, bool) {
  std::fputs("GTI internal toolchain error: ", stderr);
  std::fputs(reason == nullptr ? "unknown fatal error" : reason, stderr);
  std::fputc('\n', stderr);
}

void reportToolchainBadAlloc(void *, const char *reason, bool) {
  std::fputs("GTI internal toolchain error: allocation failed: ", stderr);
  std::fputs(reason == nullptr ? "" : reason, stderr);
  std::fputc('\n', stderr);
}

[[nodiscard]] llvm::StringRef ref(std::string_view text) {
  return llvm::StringRef(text.data(), text.size());
}

#if defined(GTI_NATIVE_STACK_SWITCH)
// Stack policy. A recursion boundary needs `headroom` bytes below it: one
// nested level of staged analysis or macro expansion measured 64 KiB in an
// optimized arm64 build (160 KiB with AddressSanitizer), and the code between
// two boundaries recurses further only through syntax-bounded nesting.
// Sanitized and unoptimized builds use frames several times larger.
#if defined(GTI_ADDRESS_SANITIZER) || !defined(NDEBUG)
constexpr std::size_t stackScale = 4;
#else
constexpr std::size_t stackScale = 1;
#endif
constexpr std::size_t defaultHeadroom = stackScale * (std::size_t{1} << 20);
constexpr std::size_t defaultSegment = stackScale * (std::size_t{8} << 20);
// Kept unused at the low end of every region: the host's guard gap below a
// thread stack, signal delivery and the switch's own frames.
constexpr std::size_t regionReserve = std::size_t{64} << 10;
// Assumed stack below the first boundary on a host that cannot report its
// thread stack bounds.
constexpr std::size_t unknownThreadStack = std::size_t{256} << 10;

std::atomic<std::size_t> policyHeadroom{0};
std::atomic<std::size_t> policySegment{0};

std::size_t headroomBytes() {
  const std::size_t configured = policyHeadroom.load(std::memory_order_relaxed);
  return configured != 0 ? configured : defaultHeadroom;
}

std::size_t segmentBytes() {
  const std::size_t configured = policySegment.load(std::memory_order_relaxed);
  return configured != 0 ? configured : defaultSegment;
}

// The usable address range of the stack the thread is running on: its own
// stack, or the segment it switched to. `low` already excludes the reserve.
struct StackRegion {
  std::uintptr_t low = 0;
  std::uintptr_t high = 0;
};

thread_local StackRegion threadStack;
thread_local bool threadStackKnown = false;
thread_local StackRegion currentStack;
thread_local std::size_t segmentsEntered = 0;
// Set on entry to a segment until its first boundary check, which then
// proceeds there whatever the policy says: a boundary switches at most once.
thread_local bool segmentEntered = false;

[[nodiscard]] std::uintptr_t stackAddress() {
  return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

[[nodiscard]] StackRegion ownThreadStack(std::uintptr_t near) {
  std::uintptr_t low = 0;
  std::uintptr_t high = 0;
#if defined(__APPLE__)
  const pthread_t self = pthread_self();
  high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
  const std::size_t size = pthread_get_stacksize_np(self);
  low = high > size ? high - size : 0;
#elif defined(__linux__)
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    void *address = nullptr;
    std::size_t size = 0;
    if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
      low = reinterpret_cast<std::uintptr_t>(address);
      high = low + size;
    }
    pthread_attr_destroy(&attributes);
  }
#endif
  if (low == 0 || near <= low + regionReserve || near > high) {
    // Unknown or inconsistent bounds: assume only a small stack below here.
    return {.low = near > unknownThreadStack ? near - unknownThreadStack : 0,
            .high = UINTPTR_MAX};
  }
  return {.low = low + regionReserve, .high = high};
}

// One guard-paged mapping. The lowest page faults on overflow instead of
// silently writing into a neighbouring allocation.
struct StackSegment {
  void *base = nullptr;
  std::size_t size = 0;
};

[[nodiscard]] std::size_t pageBytes() {
  const long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<std::size_t>(page) : std::size_t{16384};
}

// Released segments are kept for the thread's next switch; a recursion that
// crosses a segment boundary repeatedly then maps memory once.
struct SegmentCache {
  std::vector<StackSegment> released;
  SegmentCache() = default;
  SegmentCache(const SegmentCache &) = delete;
  SegmentCache &operator=(const SegmentCache &) = delete;
  ~SegmentCache() {
    for (const StackSegment &segment : released)
      munmap(segment.base, segment.size);
  }
};
thread_local SegmentCache segmentCache;

[[nodiscard]] StackSegment acquireSegment(std::size_t usable) {
  const std::size_t page = pageBytes();
  const std::size_t size = (usable + page - 1) / page * page + page;
  auto &released = segmentCache.released;
  for (auto entry = released.begin(); entry != released.end(); ++entry) {
    if (entry->size == size) {
      const StackSegment reused = *entry;
      released.erase(entry);
      return reused;
    }
  }
  int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
  flags |= MAP_NORESERVE;
#endif
#if defined(MAP_STACK)
  flags |= MAP_STACK;
#endif
  void *base = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (base == MAP_FAILED)
    throw std::bad_alloc();
  if (mprotect(base, page, PROT_NONE) != 0) {
    munmap(base, size);
    throw std::bad_alloc();
  }
  return {.base = base, .size = size};
}

void releaseSegment(StackSegment segment) {
  auto &released = segmentCache.released;
  if (released.size() < 2) {
    released.push_back(segment);
    return;
  }
  munmap(segment.base, segment.size);
}

struct SegmentCall {
  void (*work)(void *) = nullptr;
  void *context = nullptr;
  StackRegion region;
  std::exception_ptr failure;
#if defined(GTI_ADDRESS_SANITIZER)
  void *callerFakeStack = nullptr;
  const void *callerBottom = nullptr;
  std::size_t callerSize = 0;
#endif
};

// Runs first on the new segment.
void enterSegment(void *raw) {
  auto &call = *static_cast<SegmentCall *>(raw);
#if defined(GTI_ADDRESS_SANITIZER)
  __sanitizer_finish_switch_fiber(nullptr, &call.callerBottom,
                                  &call.callerSize);
#endif
  const StackRegion saved = currentStack;
  currentStack = call.region;
  ++segmentsEntered;
  segmentEntered = true;
  try {
    call.work(call.context);
  } catch (...) {
    call.failure = std::current_exception();
  }
  segmentEntered = false;
  currentStack = saved;
#if defined(GTI_ADDRESS_SANITIZER)
  // The segment's frames are finished; let the sanitizer drop its fake stack.
  __sanitizer_start_switch_fiber(nullptr, call.callerBottom, call.callerSize);
#endif
}
#endif

} // namespace

#if defined(GTI_NATIVE_STACK_SWITCH)

bool nativeStackNearlyExhausted() {
  if (segmentEntered) {
    segmentEntered = false;
    return false;
  }
  const std::uintptr_t here = stackAddress();
  if (here <= currentStack.low || here > currentStack.high) {
    // First use on this thread, or control left a segment without returning
    // through it (crash recovery): the thread's own stack applies again.
    if (!threadStackKnown) {
      threadStack = ownThreadStack(here);
      threadStackKnown = true;
    }
    currentStack = threadStack;
    if (here <= currentStack.low || here > currentStack.high)
      return true;
  }
  return here - currentStack.low < headroomBytes();
}

void runOnFreshNativeStack(void (*work)(void *), void *context) {
  const StackSegment segment = acquireSegment(segmentBytes() + regionReserve);
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(segment.base);
  const std::uintptr_t top = (base + segment.size) & ~std::uintptr_t{63};
  SegmentCall call{
      .work = work,
      .context = context,
      .region = {.low = base + pageBytes() + regionReserve, .high = top}};
#if defined(GTI_ADDRESS_SANITIZER)
  __sanitizer_start_switch_fiber(&call.callerFakeStack, segment.base,
                                 segment.size);
#endif
  gti_run_on_native_stack(reinterpret_cast<void *>(top), enterSegment, &call);
#if defined(GTI_ADDRESS_SANITIZER)
  __sanitizer_finish_switch_fiber(call.callerFakeStack, nullptr, nullptr);
#endif
  releaseSegment(segment);
  if (call.failure)
    std::rethrow_exception(call.failure);
}

NativeStackPolicy setNativeStackPolicyForTesting(NativeStackPolicy policy) {
  return {.headroom = policyHeadroom.exchange(policy.headroom),
          .segment = policySegment.exchange(policy.segment)};
}

std::size_t nativeStackSegmentsEntered() { return segmentsEntered; }
#else
// No same-thread switch on this host (see above): never switch.
bool nativeStackNearlyExhausted() { return false; }

void runOnFreshNativeStack(void (*work)(void *), void *context) {
  work(context);
}

NativeStackPolicy setNativeStackPolicyForTesting(NativeStackPolicy) {
  return {};
}

std::size_t nativeStackSegmentsEntered() { return 0; }
#endif

void installCrashHandlers(std::string_view toolName) {
  // The signal printer retains the tool name for symbolization; keep a copy
  // with static storage duration.
  static std::string retainedToolName;
  retainedToolName.assign(toolName);
  llvm::install_fatal_error_handler(reportToolchainFatal);
  llvm::install_bad_alloc_error_handler(reportToolchainBadAlloc);
  llvm::sys::PrintStackTraceOnErrorSignal(retainedToolName);
  llvm::CrashRecoveryContext::Enable();
}

bool runGuarded(const std::function<void()> &work) {
  llvm::CrashRecoveryContext context;
  return context.RunSafely([&work] { work(); });
}

void beginTimeTrace(std::string_view toolName) {
  if (!llvm::timeTraceProfilerEnabled()) {
    llvm::timeTraceProfilerInitialize(/*TimeTraceGranularity=*/0,
                                      ref(toolName));
  }
}

bool endTimeTrace(const std::string &path) {
  if (!llvm::timeTraceProfilerEnabled()) {
    return false;
  }
  llvm::Error error =
      llvm::timeTraceProfilerWrite(path, /*FallbackFileName=*/"-");
  const bool failed = static_cast<bool>(error);
  llvm::consumeError(std::move(error));
  llvm::timeTraceProfilerCleanup();
  return !failed;
}

bool timeTraceAvailable() { return true; }

PhaseTimeScope::PhaseTimeScope(std::string_view name) {
  if (llvm::timeTraceProfilerEnabled()) {
    scope = new llvm::TimeTraceScope(ref(name));
  }
}

PhaseTimeScope::PhaseTimeScope(std::string_view name, std::string_view detail) {
  if (llvm::timeTraceProfilerEnabled()) {
    scope = new llvm::TimeTraceScope(ref(name), ref(detail));
  }
}

PhaseTimeScope::~PhaseTimeScope() {
  delete static_cast<llvm::TimeTraceScope *>(scope);
}

} // namespace lang
