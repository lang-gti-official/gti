#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

std::string validProgram() {
  return R"(
#include <std/atomic>

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

void testDormantImport() {
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult frontend =
      analyze("dormant-atomic-import.gti",
              "#include <std/atomic>\nint main() { return 0; }\n", single);
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "an unused atomic import should remain valid in the default "
         "single-threaded profile");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, single);
  const std::string cpp =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(lang::verifyLoweredProgram(lowered).empty() &&
             cpp.find("#include <atomic>") == std::string::npos &&
             cpp.find("class atomic_value") == std::string::npos,
         "a dormant import should cross the backend without enabling its "
         "atomic support vocabulary");
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
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  for (const std::string *cpp : {&cpp20, &cpp23}) {
    expect(cpp->find("#include <atomic>") != std::string::npos &&
               cpp->find("class atomic_value") != std::string::npos &&
               cpp->find("atomic_compare_exchange(") != std::string::npos &&
               cpp->find("std::memory_order_seq_cst") != std::string::npos,
           "both native standards should consume the verified atomic "
           "vocabulary and exact order");
  }
}

void testScalarDomain() {
  const lang::FrontendResult valid = analyze("atomic-scalar-domain.gti", R"(
#include <std/atomic>
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
    const std::string declarations = type == "choice"
                                         ? "enum class choice { one };\n"
                                     : type == "payload" ? "class payload {};\n"
                                                         : "";
    const lang::FrontendResult invalid =
        analyze("atomic-invalid-" + name + ".gti",
                "#include <std/atomic>\n" + declarations +
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
#include <std/atomic>
int main() {
  std::atomic<int32_t> value{1};
  return value.load();
}
)";
  lang::TargetInfo single = concurrentTarget();
  single.executionProfile = lang::ExecutionProfile::SingleThreaded;
  const lang::FrontendResult singleResult =
      analyze("atomic-single-profile.gti", use, single);
  expect(!singleResult.canGenerateCode() &&
             hasCode(singleResult, "GTI-S2084") &&
             hasMessage(singleResult, "concurrent execution profile") &&
             !hasCode(singleResult, "GTI-B0001"),
         "atomic use should fail before lowering in the single-threaded "
         "profile");

  const lang::FrontendResult unsupported =
      analyze("atomic-unsupported-target.gti", use, concurrentTarget(false));
  expect(
      !unsupported.canGenerateCode() && hasCode(unsupported, "GTI-S2084") &&
          hasMessage(unsupported,
                     "does not provide thread-capable concurrency support") &&
          !hasCode(unsupported, "GTI-B0001"),
      "atomic use should fail before lowering when the target lacks "
      "thread support");

  const lang::FrontendResult privateStorage =
      analyze("atomic-private-storage.gti", R"(
#include <std/atomic>
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
#include <std/atomic>
int main() {
  std::atomic<int32_t> first{1};
  std::atomic<int32_t> second = first;
  return 0;
}
)");
  expect(!copied.canGenerateCode() && !hasCode(copied, "GTI-B0001"),
         "the public atomic value should be move-only");
}

void testVerifierMutations() {
  const lang::FrontendResult frontend =
      analyze("atomic-verifier.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram relaxedLoad = frontend.mir;
  lang::MirInstruction *load =
      findAtomic(relaxedLoad, lang::IntrinsicKind::AtomicLoad);
  if (load != nullptr) {
    load->synchronization.order = lang::AtomicMemoryOrder::Relaxed;
  }
  expect(load != nullptr && !lang::verifyMirProgram(relaxedLoad).valid(),
         "MIR should reject a relaxed order forged into the SC-only slice");

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
         "MIR should reject an illegal or non-SC compare/exchange failure "
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

} // namespace

int main() {
  testDormantImport();
  testPublicPipeline();
  testScalarDomain();
  testSemanticBoundaries();
  testVerifierMutations();
  return failures == 0 ? 0 : 1;
}
