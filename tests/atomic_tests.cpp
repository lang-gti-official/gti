#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "llvm_backend_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::TargetInfo concurrentTarget(bool threads = true) {
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = threads;
  return target;
}

lang::FrontendResult analyze(std::string_view name, std::string source,
                             lang::TargetInfo target = concurrentTarget()) {
  lang::FrontendOptions options;
  options.target = std::move(target);
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::string fixtureSource(std::string_view name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string validProgram() {
  return R"(
import std.atomic;

std::atomic<int32_t> program_count{0};

int main() {
  std::atomic<int32_t> value{1};
  if (value.load() != 1) { return 1; }
  value.store(2);
  if (value.exchange(3) != 2) { return 2; }
  auto exchanged = value.compare_exchange_strong(3, 4);
  if (!exchanged.exchanged() || exchanged.observed() != 3) { return 3; }
  auto rejected = value.compare_exchange_strong(3, 5);
  if (rejected.exchanged() || rejected.observed() != 4) { return 4; }

  program_count.store(value.load());
  std::atomic<bool> flag{false};
  flag.store(true);
  return flag.load() && program_count.load() == 4 ? 0 : 5;
}
)";
}

template <typename Function>
void forEachInstruction(lang::MirProgram &program, Function &&function) {
  const auto visit = [&](lang::MirBody &body) {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        function(body, instruction);
      }
    }
  };
  visit(const_cast<lang::MirBody &>(program.module()));
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances())) {
    visit(instance.body);
  }
  for (lang::MirConstructorInstance &instance :
       const_cast<std::vector<lang::MirConstructorInstance> &>(
           program.constructorInstances())) {
    visit(instance.body);
  }
  for (lang::MirDestructorInstance &instance :
       const_cast<std::vector<lang::MirDestructorInstance> &>(
           program.destructorInstances())) {
    visit(instance.body);
  }
  for (lang::MirLambdaInstance &instance :
       const_cast<std::vector<lang::MirLambdaInstance> &>(
           program.lambdaInstances())) {
    visit(instance.body);
  }
}

bool isAtomic(lang::IntrinsicKind kind) {
  return kind == lang::IntrinsicKind::AtomicLoad ||
         kind == lang::IntrinsicKind::AtomicWait ||
         kind == lang::IntrinsicKind::AtomicNotifyOne ||
         kind == lang::IntrinsicKind::AtomicNotifyAll ||
         kind == lang::IntrinsicKind::AtomicStore ||
         kind == lang::IntrinsicKind::AtomicExchange ||
         kind == lang::IntrinsicKind::AtomicCompareExchange;
}

std::size_t atomicCount(const lang::MirProgram &program,
                        lang::IntrinsicKind kind) {
  std::size_t count = 0;
  lang::MirProgram copy = program;
  forEachInstruction(copy, [&](lang::MirBody &, lang::MirInstruction &item) {
    count += item.intrinsic == kind ? 1 : 0;
  });
  return count;
}

lang::MirInstruction *findAtomic(lang::MirProgram &program,
                                 lang::IntrinsicKind kind) {
  lang::MirInstruction *found = nullptr;
  forEachInstruction(program,
                     [&](lang::MirBody &, lang::MirInstruction &instruction) {
                       if (found == nullptr && instruction.intrinsic == kind) {
                         found = &instruction;
                       }
                     });
  return found;
}

// Memory orders the verified lowered program hands every backend.
std::array<bool, static_cast<std::size_t>(lang::AtomicMemoryOrder::Count)>
loweredOrders(const lang::LoweredProgram &lowered) {
  std::array<bool, static_cast<std::size_t>(lang::AtomicMemoryOrder::Count)>
      orders{};
  lang::MirProgram copy = lowered.mir();
  forEachInstruction(copy, [&](lang::MirBody &, lang::MirInstruction &item) {
    if (!isAtomic(item.intrinsic))
      return;
    if (item.synchronization.order)
      orders[static_cast<std::size_t>(*item.synchronization.order)] = true;
    if (item.synchronization.failureOrder)
      orders[static_cast<std::size_t>(*item.synchronization.failureOrder)] =
          true;
  });
  return orders;
}

bool allOrders(const lang::LoweredProgram &lowered) {
  const auto orders = loweredOrders(lowered);
  return std::all_of(orders.begin(), orders.end(),
                     [](bool used) { return used; });
}

// The shipped backend accepts the verified program; reports why it did not.
bool emitsNative(const lang::FrontendResult &frontend,
                 lang::OptimizationLevel level,
                 const lang::TargetInfo &target) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(frontend, level, target, &error);
  if (!emitted)
    std::cerr << "native emission: " << error << '\n';
  return emitted;
}

void testDormantImport() {
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  for (const lang::TargetInfo &target : {single, concurrentTarget()}) {
    const lang::FrontendResult frontend =
        analyze("dormant-atomic-import.gti",
                "import std.atomic;\nint main() { return 0; }\n", target);
    if (!frontend.canGenerateCode()) {
      printDiagnostics(frontend);
    }
    expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
               lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                   .valid() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "an unused atomic import should remain valid in both execution "
           "profiles");
    if (!frontend.canGenerateCode()) {
      continue;
    }
    const lang::LoweredProgram lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
    lang::MirProgram loweredMir = lowered.mir();
    std::size_t atomicInstructions = 0;
    forEachInstruction(loweredMir,
                       [&](lang::MirBody &, lang::MirInstruction &item) {
                         atomicInstructions += isAtomic(item.intrinsic) ? 1 : 0;
                       });
    expect(lang::verifyLoweredProgram(lowered).empty() &&
               atomicInstructions == 0 &&
               emitsNative(frontend, lang::OptimizationLevel::O0, target),
           "a dormant import should cross the backend without enabling its "
           "atomic support vocabulary");
  }
}

void testPublicPipeline() {
  const lang::FrontendResult frontend =
      analyze("atomic-public-pipeline.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "public scalar atomics and namespace-global initialization should "
         "complete semantics, HIR, and MIR verification");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::size_t atomicInstructions = 0;
  std::size_t untrustedAtomicBodies = 0;
  lang::MirProgram inspected = frontend.mir;
  forEachInstruction(
      inspected, [&](lang::MirBody &body, lang::MirInstruction &instruction) {
        if (!isAtomic(instruction.intrinsic)) {
          return;
        }
        ++atomicInstructions;
        untrustedAtomicBodies += body.compilerTrustedSynchronization ? 0 : 1;
        const bool compareExchange =
            instruction.intrinsic == lang::IntrinsicKind::AtomicCompareExchange;
        expect(instruction.synchronization.order ==
                       lang::AtomicMemoryOrder::SequentiallyConsistent &&
                   (compareExchange
                        ? instruction.synchronization.failureOrder ==
                              lang::AtomicMemoryOrder::SequentiallyConsistent
                        : !instruction.synchronization.failureOrder),
               "each initial atomic intrinsic should retain its exact "
               "sequentially-consistent order");
      });
  expect(atomicInstructions >= 6 && untrustedAtomicBodies == 0 &&
             atomicCount(frontend.mir, lang::IntrinsicKind::AtomicLoad) >= 2 &&
             atomicCount(frontend.mir, lang::IntrinsicKind::AtomicStore) >= 2 &&
             atomicCount(frontend.mir, lang::IntrinsicKind::AtomicExchange) ==
                 1 &&
             atomicCount(frontend.mir,
                         lang::IntrinsicKind::AtomicCompareExchange) == 1,
         "the public wrappers should lower only to exact trusted atomic "
         "intrinsics");

  const lang::TargetInfo target = concurrentTarget();
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = target});
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "O3 should preserve a verified atomic program");
  for (const lang::IntrinsicKind kind :
       {lang::IntrinsicKind::AtomicLoad, lang::IntrinsicKind::AtomicStore,
        lang::IntrinsicKind::AtomicExchange,
        lang::IntrinsicKind::AtomicCompareExchange}) {
    expect(atomicCount(optimized.mir, kind) == atomicCount(frontend.mir, kind),
           "optimization must not remove or duplicate atomic barriers");
  }

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, optimized.sourceMir, optimized.mir,
                             target, optimized.report.transformations);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "atomic synchronization should cross the sealed LoweredProgram "
         "boundary");
  for (const lang::IntrinsicKind kind :
       {lang::IntrinsicKind::AtomicLoad, lang::IntrinsicKind::AtomicStore,
        lang::IntrinsicKind::AtomicExchange,
        lang::IntrinsicKind::AtomicCompareExchange}) {
    expect(atomicCount(lowered.mir(), kind) == atomicCount(frontend.mir, kind),
           "the sealed backend input retains every atomic operation");
  }
  const auto orders = loweredOrders(lowered);
  expect(orders[static_cast<std::size_t>(
             lang::AtomicMemoryOrder::SequentiallyConsistent)] &&
             std::count(orders.begin(), orders.end(), true) == 1 &&
             emitsNative(frontend, lang::OptimizationLevel::O3, target),
         "the shipped backend should consume the verified atomic vocabulary "
         "and exact order");
}

void testScalarDomain() {
  const lang::FrontendResult valid = analyze("atomic-scalar-domain.gti", R"(
import std.atomic;
int main() {
  std::atomic<bool> b{false};
  std::atomic<int8_t> i8{int8_t(1)};
  std::atomic<int16_t> i16{int16_t(2)};
  std::atomic<int32_t> i32{int32_t(3)};
  std::atomic<int64_t> i64{int64_t(4)};
  std::atomic<uint8_t> u8{uint8_t(5)};
  std::atomic<uint16_t> u16{uint16_t(6)};
  std::atomic<uint32_t> u32{uint32_t(7)};
  std::atomic<uint64_t> u64{uint64_t(8)};
  if (b.load() || i8.load() != int8_t(1) ||
      i16.load() != int16_t(2) || i32.load() != int32_t(3) ||
      i64.load() != int64_t(4) || u8.load() != uint8_t(5) ||
      u16.load() != uint16_t(6) || u32.load() != uint32_t(7) ||
      u64.load() != uint64_t(8)) {
    return 1;
  }
  return 0;
}
)");
  if (!valid.canGenerateCode()) {
    printDiagnostics(valid);
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "bool and every fixed-width integer should satisfy atomic_scalar");

  for (const auto &[name, type, initializer] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"float", "float", "float(1)"},
           {"double", "double", "double(1)"},
           {"enum", "choice", "choice::one"},
           {"class", "payload", "payload()"},
           {"borrowed", "std::string_view", "\"text\""}}) {
    const std::string declarations =
        type == "choice" ? "enum class choice { one };\n"
        : type == "payload"
            ? "class payload { public: payload() = default; };\n"
            : "";
    const lang::FrontendResult invalid =
        analyze("atomic-invalid-" + name + ".gti",
                "import std.atomic;\n" + declarations +
                    "int main() {\n  "
                    "std::atomic<" +
                    type + "> value{" + initializer + "};\n  return 0;\n}\n");
    expect(!invalid.canGenerateCode() &&
               (hasMessage(invalid, "atomic_scalar") ||
                hasMessage(invalid, "AtomicScalar") ||
                hasMessage(invalid, "constraint")) &&
               !hasCode(invalid, "GTI-B0001"),
           name + " should be rejected at the public atomic constraint");
  }
}

void testSemanticBoundaries() {
  const std::string use = R"(
import std.atomic;
int main() {
  std::atomic<int32_t> value{1};
  return value.load();
}
)";
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult singleResult =
      analyze("atomic-single-profile.gti", use, single);
  expect(singleResult.canGenerateCode(),
         "scalar atomic use is available in the single-threaded profile");

  for (auto target : {concurrentTarget(false), lang::TargetInfo::host()}) {
    target.runtimeCapabilities.threads = false;
    expect(analyze("atomic-without-thread-runtime.gti", use, target)
               .canGenerateCode(),
           "scalar atomics do not require a host thread runtime in either "
           "profile");
  }

  const lang::FrontendResult privateStorage =
      analyze("atomic-private-storage.gti", R"(
import std.atomic;
int main() {
  gti_internal::atomic_storage<int32_t> storage{1};
  return 0;
}
)");
  expect(!privateStorage.canGenerateCode() &&
             hasMessage(privateStorage, "Compiler-private") &&
             !hasCode(privateStorage, "GTI-B0001"),
         "application code should not be able to name compiler-owned atomic "
         "storage");

  const lang::FrontendResult copied = analyze("atomic-copy.gti", R"(
import std.atomic;
int main() {
  std::atomic<int32_t> first{1};
  std::atomic<int32_t> second = first;
  return 0;
}
)");
  expect(!copied.canGenerateCode() && !hasCode(copied, "GTI-B0001"),
         "the public atomic value should be move-only");
}

void testExplicitOrders() {
  const lang::FrontendResult frontend =
      analyze("atomic-orders.gti", fixtureSource("atomic_orders.gti"));
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "every public operation/order combination should retain exact valid "
         "HIR and MIR metadata");
  if (!frontend.canGenerateCode()) {
    return;
  }

  constexpr std::size_t orderCount =
      static_cast<std::size_t>(lang::AtomicMemoryOrder::Count);
  std::array<bool, orderCount> loadOrders{};
  std::array<bool, orderCount> storeOrders{};
  std::array<bool, orderCount> exchangeOrders{};
  std::array<std::array<bool, orderCount>, orderCount> compareOrders{};
  lang::MirProgram inspected = frontend.mir;
  forEachInstruction(inspected, [&](lang::MirBody &,
                                    lang::MirInstruction &instruction) {
    if (!isAtomic(instruction.intrinsic) ||
        !instruction.synchronization.order) {
      return;
    }
    const std::size_t order =
        static_cast<std::size_t>(*instruction.synchronization.order);
    if (order >= orderCount) {
      return;
    }
    switch (instruction.intrinsic) {
    case lang::IntrinsicKind::AtomicLoad:
      loadOrders[order] = true;
      break;
    case lang::IntrinsicKind::AtomicStore:
      storeOrders[order] = true;
      break;
    case lang::IntrinsicKind::AtomicExchange:
      exchangeOrders[order] = true;
      break;
    case lang::IntrinsicKind::AtomicCompareExchange:
      if (instruction.synchronization.failureOrder) {
        const std::size_t failure =
            static_cast<std::size_t>(*instruction.synchronization.failureOrder);
        if (failure < orderCount) {
          compareOrders[order][failure] = true;
        }
      }
      break;
    default:
      break;
    }
  });
  const auto has = [](const auto &orders, lang::AtomicMemoryOrder order) {
    return orders[static_cast<std::size_t>(order)];
  };
  expect(has(loadOrders, lang::AtomicMemoryOrder::Relaxed) &&
             has(loadOrders, lang::AtomicMemoryOrder::Acquire) &&
             has(loadOrders, lang::AtomicMemoryOrder::SequentiallyConsistent) &&
             !has(loadOrders, lang::AtomicMemoryOrder::Release) &&
             !has(loadOrders, lang::AtomicMemoryOrder::AcquireRelease),
         "load should expose exactly relaxed, acquire, and seq_cst");
  expect(
      has(storeOrders, lang::AtomicMemoryOrder::Relaxed) &&
          has(storeOrders, lang::AtomicMemoryOrder::Release) &&
          has(storeOrders, lang::AtomicMemoryOrder::SequentiallyConsistent) &&
          !has(storeOrders, lang::AtomicMemoryOrder::Acquire) &&
          !has(storeOrders, lang::AtomicMemoryOrder::AcquireRelease),
      "store should expose exactly relaxed, release, and seq_cst");
  expect(std::all_of(exchangeOrders.begin(), exchangeOrders.end(),
                     [](bool present) { return present; }),
         "exchange should expose every accepted memory order");
  const auto cas = [&](lang::AtomicMemoryOrder success,
                       lang::AtomicMemoryOrder failure) {
    return compareOrders[static_cast<std::size_t>(success)]
                        [static_cast<std::size_t>(failure)];
  };
  expect(
      cas(lang::AtomicMemoryOrder::Relaxed, lang::AtomicMemoryOrder::Relaxed) &&
          cas(lang::AtomicMemoryOrder::Acquire,
              lang::AtomicMemoryOrder::Relaxed) &&
          cas(lang::AtomicMemoryOrder::Acquire,
              lang::AtomicMemoryOrder::Acquire) &&
          cas(lang::AtomicMemoryOrder::Release,
              lang::AtomicMemoryOrder::Relaxed) &&
          cas(lang::AtomicMemoryOrder::AcquireRelease,
              lang::AtomicMemoryOrder::Relaxed) &&
          cas(lang::AtomicMemoryOrder::AcquireRelease,
              lang::AtomicMemoryOrder::Acquire) &&
          cas(lang::AtomicMemoryOrder::SequentiallyConsistent,
              lang::AtomicMemoryOrder::Relaxed) &&
          cas(lang::AtomicMemoryOrder::SequentiallyConsistent,
              lang::AtomicMemoryOrder::Acquire) &&
          cas(lang::AtomicMemoryOrder::SequentiallyConsistent,
              lang::AtomicMemoryOrder::SequentiallyConsistent),
      "compare/exchange should expose all nine and only statically valid "
      "success/failure pairs");
  bool exactCompareMatrix = true;
  for (std::size_t success = 0; success < orderCount; ++success) {
    for (std::size_t failure = 0; failure < orderCount; ++failure) {
      exactCompareMatrix =
          exactCompareMatrix &&
          compareOrders[success][failure] ==
              lang::validAtomicCompareExchangeFailureOrder(
                  static_cast<lang::AtomicMemoryOrder>(success),
                  static_cast<lang::AtomicMemoryOrder>(failure));
    }
  }
  expect(exactCompareMatrix,
         "compare/exchange MIR should contain exactly the legal order matrix");

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, concurrentTarget());
  expect(allOrders(lowered) &&
             emitsNative(frontend, lang::OptimizationLevel::O0,
                         concurrentTarget()),
         "the backend should lower each verified atomic order explicitly");

  const auto expectInvalidOrder = [&](const std::string &name,
                                      const std::string &expression) {
    const lang::FrontendResult invalid =
        analyze("atomic-invalid-order-" + name + ".gti",
                "import std.atomic;\nint main() {\n  std::atomic<int32_t> "
                "value{0};\n  " +
                    expression + ";\n  return 0;\n}\n");
    expect(!invalid.canGenerateCode() && !invalid.diagnostics.empty() &&
               !hasCode(invalid, "GTI-B0001"),
           name + " should fail in source overload resolution before the "
                  "backend");
  };
  for (const auto &[name, expression] :
       std::vector<std::pair<std::string, std::string>>{
           {"load-release", "value.load(std::memory_order_release())"},
           {"load-acq-rel", "value.load(std::memory_order_acq_rel())"},
           {"store-acquire", "value.store(1, std::memory_order_acquire())"},
           {"store-acq-rel", "value.store(1, std::memory_order_acq_rel())"}}) {
    expectInvalidOrder(name, expression);
  }
  constexpr std::array<std::string_view, orderCount> orderNames{
      "relaxed", "acquire", "release", "acq_rel", "seq_cst"};
  for (std::size_t success = 0; success < orderCount; ++success) {
    for (std::size_t failure = 0; failure < orderCount; ++failure) {
      if (lang::validAtomicCompareExchangeFailureOrder(
              static_cast<lang::AtomicMemoryOrder>(success),
              static_cast<lang::AtomicMemoryOrder>(failure))) {
        continue;
      }
      expectInvalidOrder(
          "cas-" + std::string(orderNames[success]) + "-" +
              std::string(orderNames[failure]),
          "value.compare_exchange_strong(0, 1, std::memory_order_" +
              std::string(orderNames[success]) + "(), std::memory_order_" +
              std::string(orderNames[failure]) + "())");
    }
  }
}

void testFetchOperations() {
  const lang::FrontendResult frontend =
      analyze("atomic-fetch.gti", fixtureSource("atomic_fetch.gti"));
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "integer fetch operations should complete exact HIR and MIR "
         "verification for every public memory order");
  if (!frontend.canGenerateCode()) {
    return;
  }

  constexpr std::size_t orderCount =
      static_cast<std::size_t>(lang::AtomicMemoryOrder::Count);
  std::array<std::array<bool, orderCount>, orderCount> compareOrders{};
  std::size_t compareExchanges = 0;
  std::size_t untrustedAtomics = 0;
  lang::MirProgram inspected = frontend.mir;
  forEachInstruction(inspected, [&](lang::MirBody &body,
                                    lang::MirInstruction &instruction) {
    if (!isAtomic(instruction.intrinsic)) {
      return;
    }
    untrustedAtomics += body.compilerTrustedSynchronization ? 0 : 1;
    if (instruction.intrinsic != lang::IntrinsicKind::AtomicCompareExchange ||
        !instruction.synchronization.order ||
        !instruction.synchronization.failureOrder) {
      return;
    }
    ++compareExchanges;
    const std::size_t success =
        static_cast<std::size_t>(*instruction.synchronization.order);
    const std::size_t failure =
        static_cast<std::size_t>(*instruction.synchronization.failureOrder);
    if (success < orderCount && failure < orderCount) {
      compareOrders[success][failure] = true;
    }
  });
  const auto cas = [&](lang::AtomicMemoryOrder success,
                       lang::AtomicMemoryOrder failure) {
    return compareOrders[static_cast<std::size_t>(success)]
                        [static_cast<std::size_t>(failure)];
  };
  expect(compareExchanges == 33 && untrustedAtomics == 0 &&
             cas(lang::AtomicMemoryOrder::Relaxed,
                 lang::AtomicMemoryOrder::Relaxed) &&
             cas(lang::AtomicMemoryOrder::Acquire,
                 lang::AtomicMemoryOrder::Acquire) &&
             cas(lang::AtomicMemoryOrder::Release,
                 lang::AtomicMemoryOrder::Relaxed) &&
             cas(lang::AtomicMemoryOrder::AcquireRelease,
                 lang::AtomicMemoryOrder::Acquire) &&
             cas(lang::AtomicMemoryOrder::SequentiallyConsistent,
                 lang::AtomicMemoryOrder::SequentiallyConsistent),
         "the five fetch families should remain trusted CAS loops with the "
         "exact requested read-modify-write order pairs");
  bool exactFetchMatrix = true;
  for (std::size_t success = 0; success < orderCount; ++success) {
    for (std::size_t failure = 0; failure < orderCount; ++failure) {
      const auto successOrder = static_cast<lang::AtomicMemoryOrder>(success);
      const auto failureOrder = static_cast<lang::AtomicMemoryOrder>(failure);
      const bool expected =
          (successOrder == lang::AtomicMemoryOrder::Relaxed &&
           failureOrder == lang::AtomicMemoryOrder::Relaxed) ||
          (successOrder == lang::AtomicMemoryOrder::Acquire &&
           failureOrder == lang::AtomicMemoryOrder::Acquire) ||
          (successOrder == lang::AtomicMemoryOrder::Release &&
           failureOrder == lang::AtomicMemoryOrder::Relaxed) ||
          (successOrder == lang::AtomicMemoryOrder::AcquireRelease &&
           failureOrder == lang::AtomicMemoryOrder::Acquire) ||
          (successOrder == lang::AtomicMemoryOrder::SequentiallyConsistent &&
           failureOrder == lang::AtomicMemoryOrder::SequentiallyConsistent);
      exactFetchMatrix =
          exactFetchMatrix && compareOrders[success][failure] == expected;
    }
  }
  expect(exactFetchMatrix,
         "fetch loops should not manufacture an unrequested order pair");

  const lang::TargetInfo target = concurrentTarget();
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = target});
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid() &&
             atomicCount(optimized.mir,
                         lang::IntrinsicKind::AtomicCompareExchange) ==
                 atomicCount(frontend.mir,
                             lang::IntrinsicKind::AtomicCompareExchange) &&
             atomicCount(optimized.mir, lang::IntrinsicKind::AtomicLoad) ==
                 atomicCount(frontend.mir, lang::IntrinsicKind::AtomicLoad),
         "O3 should preserve every fetch loop synchronization barrier");
  if (!optimized.valid()) {
    return;
  }

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, optimized.sourceMir, optimized.mir,
                             target, optimized.report.transformations);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "verified fetch loops should cross the sealed backend boundary");
  expect(atomicCount(lowered.mir(),
                     lang::IntrinsicKind::AtomicCompareExchange) != 0 &&
             allOrders(lowered) &&
             emitsNative(frontend, lang::OptimizationLevel::O3, target),
         "the shipped backend should emit the source-defined CAS loops with "
         "every verified fetch order");

  for (const std::string operation : {"fetch_add", "fetch_or"}) {
    const lang::FrontendResult invalid =
        analyze("atomic-bool-" + operation + ".gti",
                "import std.atomic;\nint main() {\n  std::atomic<bool> "
                "flag{false};\n  [[discard]] flag." +
                    operation + "(true);\n  return 0;\n}\n");
    expect(!invalid.canGenerateCode() && !invalid.diagnostics.empty() &&
               !hasCode(invalid, "GTI-B0001"),
           operation + " should reject bool through public constraints before "
                       "lowering");
  }
}

void testWaiting() {
  const std::string source = R"(
import std.atomic;
class Completion {
  std::atomic<int32_t> state{1};
public:
  void wait() {
    this.state.wait(0);
    this.state.wait(0, std::memory_order_relaxed());
    this.state.wait(0, std::memory_order_acquire());
    this.state.wait(0, std::memory_order_seq_cst());
  }
  void wake() { this.state.notify_one(); this.state.notify_all(); }
public:
  Completion() = default;
};
int main() { Completion completion{}; completion.wait(); completion.wake(); return 0; }
)";
  const auto frontend = analyze("atomic-waiting.gti", source);
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode(),
         "unrelated source wrapper can wait and notify");
  if (!frontend.canGenerateCode())
    return;
  for (const auto kind :
       {lang::IntrinsicKind::AtomicWait, lang::IntrinsicKind::AtomicNotifyOne,
        lang::IntrinsicKind::AtomicNotifyAll}) {
    expect(atomicCount(frontend.mir, kind) > 0,
           "waiting retains exact MIR intrinsic");
    auto forged = frontend.mir;
    auto *call = findAtomic(forged, kind);
    call->synchronization.kind = lang::SynchronizationOperationKind::AtomicLoad;
    expect(!lang::verifyMirProgram(forged).valid(),
           "waiting cannot be forged as a load");
    auto stripped = frontend.mir;
    findAtomic(stripped, kind)->intrinsic = lang::IntrinsicKind::None;
    expect(!lang::verifyMirProgram(stripped).valid(),
           "waiting requires trusted identity");
    auto wrongStorage = frontend.mir;
    findAtomic(wrongStorage, kind)->operands[0].kind =
        lang::MirOperandKind::Move;
    expect(!lang::verifyMirProgram(wrongStorage).valid(),
           "waiting borrows stable storage");
  }
  for (const auto order : {lang::AtomicMemoryOrder::Release,
                           lang::AtomicMemoryOrder::AcquireRelease}) {
    auto invalid = frontend.mir;
    findAtomic(invalid, lang::IntrinsicKind::AtomicWait)
        ->synchronization.order = order;
    expect(!lang::verifyMirProgram(invalid).valid(),
           "wait rejects release orders in MIR");
  }
  auto notifyOrder = frontend.mir;
  findAtomic(notifyOrder, lang::IntrinsicKind::AtomicNotifyAll)
      ->synchronization.order = lang::AtomicMemoryOrder::Acquire;
  expect(!lang::verifyMirProgram(notifyOrder).valid(),
         "notify has no ordering dimension");
  auto wrongOld = frontend.mir;
  findAtomic(wrongOld, lang::IntrinsicKind::AtomicWait)->operands[1].type =
      lang::SemanticType::Bool;
  expect(!lang::verifyMirProgram(wrongOld).valid(),
         "wait old must match scalar type");

  for (const std::string order : {"release", "acq_rel"}) {
    auto invalid =
        analyze("invalid-wait.gti",
                "import std.atomic;\nint main() { std::atomic<int32_t> "
                "a{0}; a.wait(0, std::memory_order_" +
                    order + "()); return 0; }");
    expect(!invalid.canGenerateCode() && !hasCode(invalid, "GTI-B0001"),
           "invalid wait order rejected before backend");
  }
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  expect(!analyze("wait-single.gti", source, single).canGenerateCode(),
         "waiting requires concurrent execution profile");
  expect(!analyze("wait-no-threads.gti", source, concurrentTarget(false))
              .canGenerateCode(),
         "waiting requires target threads");
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
    const auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = concurrentTarget()});
    expect(optimized.valid(), "optimized waiting program remains valid");
    for (const auto kind :
         {lang::IntrinsicKind::AtomicWait, lang::IntrinsicKind::AtomicNotifyOne,
          lang::IntrinsicKind::AtomicNotifyAll})
      expect(atomicCount(optimized.mir, kind) ==
                 atomicCount(frontend.mir, kind),
             "optimization preserves blocking and wakeups");
  }
}

void testVerifierMutations() {
  const lang::FrontendResult frontend =
      analyze("atomic-verifier.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram invalidLoad = frontend.mir;
  lang::MirInstruction *load =
      findAtomic(invalidLoad, lang::IntrinsicKind::AtomicLoad);
  if (load != nullptr) {
    load->synchronization.order = lang::AtomicMemoryOrder::Release;
  }
  expect(load != nullptr && !lang::verifyMirProgram(invalidLoad).valid(),
         "MIR should reject an operation-illegal atomic load order");

  lang::MirProgram wrongExchange = frontend.mir;
  lang::MirInstruction *exchange =
      findAtomic(wrongExchange, lang::IntrinsicKind::AtomicExchange);
  if (exchange != nullptr) {
    exchange->synchronization.kind =
        lang::SynchronizationOperationKind::AtomicStore;
  }
  expect(exchange != nullptr && !lang::verifyMirProgram(wrongExchange).valid(),
         "MIR should reject a forged atomic operation identity");

  lang::MirProgram failedStore = frontend.mir;
  lang::MirInstruction *store =
      findAtomic(failedStore, lang::IntrinsicKind::AtomicStore);
  if (store != nullptr) {
    store->synchronization.failureOrder = lang::AtomicMemoryOrder::Relaxed;
  }
  expect(store != nullptr && !lang::verifyMirProgram(failedStore).valid(),
         "MIR should reject a failure order on atomic store");

  lang::MirProgram wrongCas = frontend.mir;
  lang::MirInstruction *compareExchange =
      findAtomic(wrongCas, lang::IntrinsicKind::AtomicCompareExchange);
  if (compareExchange != nullptr) {
    compareExchange->synchronization.failureOrder =
        lang::AtomicMemoryOrder::Release;
  }
  expect(compareExchange != nullptr &&
             !lang::verifyMirProgram(wrongCas).valid(),
         "MIR should reject an operation-illegal compare/exchange failure "
         "order");

  lang::MirProgram strippedProvenance = frontend.mir;
  lang::MirInstruction *stripped =
      findAtomic(strippedProvenance, lang::IntrinsicKind::AtomicLoad);
  if (stripped != nullptr) {
    stripped->intrinsic = lang::IntrinsicKind::None;
  }
  expect(stripped != nullptr &&
             !lang::verifyMirProgram(strippedProvenance).valid(),
         "MIR should reject synchronization detached from its exact private "
         "intrinsic");
}

void testWrappingOperations() {
  auto target = lang::TargetInfo::host();
  target.runtimeCapabilities.threads = false;
  const auto frontend = analyze(
      "atomic-wrapping.gti", fixtureSource("llvm_atomic_wrapping.gti"), target);
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode(),
         "wrapping operations use exact integer storage without host threads");
  if (!frontend.canGenerateCode())
    return;
  for (auto intrinsic : {lang::IntrinsicKind::AtomicFetchAddWrapping,
                         lang::IntrinsicKind::AtomicFetchSubWrapping}) {
    for (int mutation = 0; mutation < 6; ++mutation) {
      auto invalid = frontend.mir;
      auto *operation = findAtomic(invalid, intrinsic);
      expect(operation != nullptr, "both wrapping operations reach MIR");
      if (!operation)
        continue;
      if (mutation == 0)
        operation->synchronization.order = lang::AtomicMemoryOrder::Count;
      if (mutation == 1)
        operation->parameterTypes.clear();
      if (mutation == 2)
        operation->operands[1].type = lang::SemanticType::Bool;
      if (mutation == 3)
        operation->operands[0].place = 0;
      if (mutation == 4)
        operation->result.reset();
      if (mutation == 5)
        operation->synchronization.kind =
            lang::SynchronizationOperationKind::AtomicLoad;
      expect(!lang::verifyMirProgram(invalid).valid(),
             "wrapping RMW rejects forged type/place/result/order contracts");
    }
  }
  for (const std::string body :
       {"std::atomic<bool> x{false}; x.fetch_add_wrapping(true);",
        "std::atomic<int> x{0}; x.fetch_sub_wrapping(uint64_t(1));",
        "std::atomic<int> x{0}; x.fetch_add_wrapping(1, 0);"}) {
    const auto invalid = analyze(
        "bad-wrapping.gti",
        "import std.atomic;\nint main() { " + body + " return 0; }", target);
    expect(!invalid.canGenerateCode() && !hasCode(invalid, "GTI-B0001"),
           "invalid wrapping calls reject in semantics");
  }
}

} // namespace

int main() {
  testDormantImport();
  testPublicPipeline();
  testScalarDomain();
  testSemanticBoundaries();
  testExplicitOrders();
  testFetchOperations();
  testVerifierMutations();
  testWaiting();
  testWrappingOperations();
  return failures == 0 ? 0 : 1;
}
