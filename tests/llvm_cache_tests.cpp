#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/llvm_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {
using namespace lang;
using namespace lang::driver;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "cannot read fixture artifact");
  return {std::istreambuf_iterator<char>(input), {}};
}

void write(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  require(static_cast<bool>(output), "cannot write fixture artifact");
}

struct TemporaryDirectory {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("gti-llvm-cache-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(root); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

struct Policy {
  BackendKind backend = BackendKind::Llvm;
  OptimizationLevel optimization = OptimizationLevel::O3;
  TargetInfo target = TargetInfo::host();
  RawPointerAuditMode audit = RawPointerAuditMode::Off;
  bool debug = false;
  LlvmInstrumentationMode instrumentation = LlvmInstrumentationMode::None;
  bool keep = false;
  bool cache = true;
  NativeInputs native;
  std::string compiler = "c++";
  std::string compilerIdentity = "llvm-cache-test-v1";
  std::string modelIdentity = "llvm-cache-project-v1";
};

void checkBuild(const ExecutableBuildResult &result) {
  if (!result.succeeded()) {
    std::cerr << "build status=" << static_cast<int>(result.status)
              << " cache=" << static_cast<int>(result.cache.status)
              << " detail=" << result.cache.detail.value_or("none")
              << " driver=" << result.driverDiagnostic.value_or("none") << '\n';
    if (result.nativeProcess)
      std::cerr << result.nativeProcess->output << '\n';
    for (const auto &diagnostic : result.compilation.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  require(result.succeeded(), "real cache fixture build failed");
}

void runNativeRuntimeChecks(const std::string &compiler) {
  std::string error;
  const auto backend = queryLlvmBackendIdentity(TargetInfo::host(), {}, error);
  require(backend.has_value(),
          "runtime gate needs real producer toolchain facts");
  std::set<std::string> identities;
  for (const auto mode : {LlvmInstrumentationMode::Address,
                          LlvmInstrumentationMode::UndefinedMemory,
                          LlvmInstrumentationMode::AddressAndUndefinedMemory,
                          LlvmInstrumentationMode::Thread}) {
    const auto plan = resolveLlvmNativeRuntime(compiler, *backend, mode, error);
    if (!plan)
      throw std::runtime_error(error);
    require(
        plan->mode == llvmInstrumentationModeName(mode) &&
            plan->compilerVersion == backend->llvmVersion &&
            !plan->runtimeFiles.empty() && !plan->implementationFiles.empty() &&
            plan->identity.size() == 64,
        "runtime plan must bind actual compiler-rt files and producer version");
    require(identities.insert(plan->identity).second,
            "different selected diagnostic modes must not share native runtime "
            "identity");
    for (const auto &path : plan->runtimeFiles)
      require(std::filesystem::is_regular_file(path),
              "resolved compiler-rt must be a real selected file");
    NativeInputs inputs;
    inputs.instrumentation = *plan;
    const NativeToolchain native;
    const auto link = native.command(
        NativeLinkRequest(compiler, "existing.o", "program", inputs));
    bool combinedRejected = false;
    try {
      static_cast<void>(native.command(NativeCompileRequest(
          compiler, "generated.cpp", "program", CppStandard::Cpp23,
          OptimizationLevel::O3, inputs)));
    } catch (const std::invalid_argument &) {
      combinedRejected = true;
    }
    require(combinedRejected, "runtime-only link flags must not silently widen "
                              "a combined source compile");
    require(
        std::search(link.begin(), link.end(), plan->linkArguments.begin(),
                    plan->linkArguments.end()) != link.end(),
        "native object link must preserve the exact ordered diagnostic plan");
    if (mode == LlvmInstrumentationMode::Address) {
      const auto repeated =
          resolveLlvmNativeRuntime(compiler, *backend, mode, error);
      require(repeated && repeated->identity == plan->identity,
              "same native runtime facts must have stable identity across "
              "fresh probes");
    }
    if (mode == LlvmInstrumentationMode::UndefinedMemory) {
      require(
          std::find(
              plan->compileArguments.begin(), plan->compileArguments.end(),
              "-fsanitize=null,alignment") != plan->compileArguments.end() &&
              std::find(plan->linkArguments.begin(), plan->linkArguments.end(),
                        "-fsanitize=undefined") != plan->linkArguments.end(),
          "bounded generated-memory policy must remain distinct from runtime "
          "linkage");
    }
    std::cout << "Native runtime identity mode=" << plan->mode
              << " LLVM/Clang=" << plan->compilerVersion
              << " files=" << plan->runtimeFiles.size()
              << " identity=" << plan->identity << '\n';
  }
  std::cout
      << "Real native runtime/tool identity controls passed; GTI Session mode "
         "admission remains a separate gate\n";
}

void runGeneratedMemoryChecks(const std::filesystem::path &self,
                              const std::string &compiler,
                              const std::string &cCompiler,
                              const std::filesystem::path &root) {
  require(!std::filesystem::exists(root),
          "instrumented evidence directory must be new");
  std::filesystem::create_directories(root);
  const auto toolchain = discoverToolchainLayout(self.c_str());
  const auto provider = root / "provider.c";
  write(provider, R"c(
#include <stdlib.h>
#include <string.h>
static int released;
int *memory_probe(int mode) {
  if (mode == 2) return 0;
  if (mode == 3) {
    unsigned char *bytes = malloc(sizeof(int) + 1);
    int value = 17;
    if (!bytes) abort();
    memcpy(bytes + 1, &value, sizeof(value));
    return (int *)(bytes + 1);
  }
  int *value = malloc(sizeof(int));
  if (!value) abort();
  *value = 17;
  if (mode == 1) free(value);
  return value;
}
void release_probe(int *value) { free(value); ++released; }
int release_count(void) { return released; }
)c");
  const auto source = root / "main.gti";
  const auto program = [&](unsigned mode, bool valid) {
    return std::string("extern \"C\" { int* memory_probe(int mode); void "
                       "release_probe(int* value); int release_count(); }\n") +
           "int main() { unsafe { int* pointer = memory_probe(" +
           std::to_string(mode) + "); int value = *pointer; " +
           (valid ? "release_probe(pointer); return value == 17 && "
                    "release_count() == 1 ? 0 : 1;"
                  : "return value;") +
           " } }\n";
  };
  NativeInputs native;
  native.cSources = {provider};
  native.cStandard = CStandard::C17;
  const auto managedRoot = root / "build/gti";
  std::filesystem::create_directories(managedRoot);
  const auto output = managedRoot / "program";
  const auto object = managedRoot / "program.o";
  const ManagedOutputPolicy managed{.trustedRoot = root,
                                    .outputRoot = managedRoot};
  const BuildCachePolicy cache{.root = managedRoot / "cache",
                               .sourceRoot = root,
                               .compilerIdentity = "generated-memory-probe",
                               .projectModelIdentity =
                                   "typed-native-inputs-v1"};
  std::ofstream ledger(root / "results.tsv");
  unsigned serial = 0;
  const auto build = [&](LlvmInstrumentationMode mode, OptimizationLevel level,
                         const std::string &selectedCompiler) {
    const auto compilation =
        CompilationRequest(source, toolchain.standardLibrary,
                           TargetInfo::host(), level, CppStandard::Cpp23)
            .withBackend(BackendKind::Llvm)
            .withLlvmInstrumentation(mode);
    const auto start = std::chrono::steady_clock::now();
    auto result = buildExecutable(ExecutableBuildRequest(
        compilation, toolchain, object, output, selectedCompiler, native, true,
        true, true, managed, cCompiler, cache));
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    const auto name = "build-" + std::to_string(serial++) + ".log";
    std::string log = result.driverDiagnostic.value_or("") + "\n";
    if (result.nativeProcess)
      log += result.nativeProcess->output;
    for (const auto &diagnostic : result.compilation.diagnostics)
      log += diagnostic.code + ": " + diagnostic.message + "\n";
    write(root / name, log);
    ledger << name << '\t' << llvmInstrumentationModeName(mode) << '\t'
           << static_cast<int>(level) << '\t' << static_cast<int>(result.status)
           << '\t' << static_cast<int>(result.cache.status) << '\t'
           << result.cache.key << '\t' << seconds << '\n'
           << std::flush;
    return result;
  };
  const auto execute = [&](std::string_view expected) {
    const auto result = invokeProcess({output.string()},
                                      {.outputMode = ProcessOutputMode::Capture,
                                       .captureSuccessfulOutput = true});
    const auto name = "execute-" + std::to_string(serial++) + ".log";
    write(root / name, result.output);
    ledger << name << '\t' << result.exitCode << '\t' << expected << '\n'
           << std::flush;
    if (expected.empty())
      require(result.exitCode == 0 && result.output.empty(),
              "instrumented valid access/cleanup failed");
    else
      require(
          result.exitCode != 0 &&
              result.output.find(expected) != std::string::npos,
          "generated GTI load did not report the expected memory violation");
  };
  std::set<std::string> keys;
  for (const auto level : {OptimizationLevel::O0, OptimizationLevel::O3})
    for (const auto mode :
         {LlvmInstrumentationMode::None, LlvmInstrumentationMode::Address,
          LlvmInstrumentationMode::UndefinedMemory,
          LlvmInstrumentationMode::AddressAndUndefinedMemory}) {
      write(source, program(0, true));
      const auto first = build(mode, level, compiler);
      checkBuild(first);
      require(first.cache.status == BuildCacheStatus::Miss &&
                  keys.insert(first.cache.key).second,
              "optimization/instrumentation policies must have distinct cold "
              "cache identities");
      require(mode == LlvmInstrumentationMode::None ||
                  (first.nativeInstrumentation &&
                   !first.nativeInstrumentation->runtimeFiles.empty()),
              "generated instrumented build must retain its resolved runtime "
              "identity");
      if (first.nativeInstrumentation) {
        const auto &runtime = *first.nativeInstrumentation;
        require(std::filesystem::is_directory(runtime.runtimeDirectory),
                "resolved runtime directory must be installed, not a missing "
                "per-target directory reported by Clang");
        for (const auto &file : runtime.runtimeFiles)
          require(std::filesystem::is_regular_file(file) &&
                      file.parent_path() == runtime.runtimeDirectory,
                  "runtime identity must bind files in the actual selected "
                  "link-plan directory");
      }
      execute({});
      const auto representation = read(object);
      std::filesystem::remove(output);
      std::filesystem::remove(object);
      const auto restored = build(mode, level, compiler);
      checkBuild(restored);
      require(restored.cache.status == BuildCacheStatus::Hit &&
                  read(object) == representation,
              "instrumented cache must restore its exact object under matching "
              "runtime policy");
      execute({});
      if (mode == LlvmInstrumentationMode::None)
        continue;
      const auto retained = read(output);
      const auto rejected = build(mode, level, "/missing-diagnostic-compiler");
      require(!rejected.succeeded() &&
                  rejected.compilation.sources.names().empty() &&
                  !rejected.compilation.artifact &&
                  rejected.cache.status == BuildCacheStatus::Bypassed &&
                  read(output) == retained && read(object) == representation,
              "runtime-resolution failure must preserve published artifacts "
              "before cache or source access");
      write(source, "int main() { return missing_instrumented_name; }\n");
      const auto invalid = build(mode, level, compiler);
      require(!invalid.succeeded() && read(output) == retained &&
                  read(object) == representation,
              "invalid GTI source must not replace an instrumented published "
              "artifact");
      if (mode != LlvmInstrumentationMode::UndefinedMemory) {
        write(source, program(1, false));
        checkBuild(build(mode, level, compiler));
        execute("heap-use-after-free");
      }
      if (mode != LlvmInstrumentationMode::Address) {
        write(source, program(2, false));
        checkBuild(build(mode, level, compiler));
        execute("load of null pointer");
        write(source, program(3, false));
        checkBuild(build(mode, level, compiler));
        execute("load of misaligned address");
      }
    }
  std::cout << "Generated GTI O0/O3 memory instrumentation, runtime identity, "
               "cache restoration, "
               "cleanup and failed-publication controls passed\n";
}

void runChecks(const std::filesystem::path &self) {
  std::string identityError;
  const auto identity =
      queryLlvmBackendIdentity(TargetInfo::host(), {}, identityError);
  require(identity.has_value(), "real backend toolchain identity is required");
  std::string runtimeError;
  const auto noneRuntime =
      resolveLlvmNativeRuntime("/missing-unused-native-compiler", *identity,
                               LlvmInstrumentationMode::None, runtimeError);
  require(noneRuntime && noneRuntime->mode == "none" &&
              noneRuntime->runtimeFiles.empty() &&
              noneRuntime->compileArguments.empty() &&
              noneRuntime->linkArguments.empty(),
          "None mode must not add a native runtime/tool dependency");
  require(!resolveLlvmNativeRuntime("/missing-instrumentation-compiler",
                                    *identity, LlvmInstrumentationMode::Address,
                                    runtimeError) &&
              runtimeError.find("selected instrumentation compiler") !=
                  std::string::npos,
          "missing diagnostic compiler must fail during runtime identity "
          "resolution");
  for (const char *name :
       {"LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "DYLD_IMAGE_SUFFIX"}) {
    const char *old = std::getenv(name);
    const std::optional<std::string> previous =
        old ? std::optional<std::string>(old) : std::nullopt;
    setenv(name, "unidentified-runtime-loader-input", 1);
    const auto rejectedRuntime = resolveLlvmNativeRuntime(
        "/missing-unused-native-compiler", *identity,
        LlvmInstrumentationMode::Address, runtimeError);
    if (previous)
      setenv(name, previous->c_str(), 1);
    else
      unsetenv(name);
    require(!rejectedRuntime &&
                runtimeError.find("loader overrides") != std::string::npos,
            "runtime loader injection must reject before any compiler probe");
  }
  const auto optimizedIdentity = queryLlvmBackendIdentity(
      TargetInfo::host(), {.optimization = LlvmCodegenOptimization::Aggressive},
      identityError);
  require(identity && optimizedIdentity && identity->privateAbi != 0 &&
              identity->schema == "gti-llvm-object-identity-v2" &&
              !identity->debugInfo && identity->instrumentation == "none" &&
              !identity->llvmVersion.empty() && !identity->triple.empty() &&
              !identity->dataLayout.empty() && !identity->cpu.empty() &&
              std::filesystem::is_regular_file(identity->compilerExecutable) &&
              identity->triple == optimizedIdentity->triple &&
              identity->cpu == optimizedIdentity->cpu &&
              identity->features == optimizedIdentity->features &&
              identity->codegenOptimization !=
                  optimizedIdentity->codegenOptimization,
          "backend identity must contain resolved producer facts and actual "
          "optimization");
  require(!queryLlvmBackendIdentity(
              TargetInfo::host(),
              {.optimization = static_cast<LlvmCodegenOptimization>(999)},
              identityError),
          "an invalid producer policy must not yield a cache identity");
  constexpr std::pair<LlvmInstrumentationMode, std::string_view> modes[] = {
      {LlvmInstrumentationMode::None, "none"},
      {LlvmInstrumentationMode::Address, "address"},
      {LlvmInstrumentationMode::UndefinedMemory, "undefined-memory"},
      {LlvmInstrumentationMode::AddressAndUndefinedMemory,
       "address-and-undefined-memory"},
      {LlvmInstrumentationMode::Thread, "thread"}};
  for (const auto &[mode, spelling] : modes) {
    require(llvmInstrumentationModeName(mode) == spelling,
            "diagnostic policy must have an exact stable identity spelling");
    if (mode == LlvmInstrumentationMode::None)
      continue;
    std::string unavailable;
    const auto selected = queryLlvmBackendIdentity(
        TargetInfo::host(), {.instrumentation = mode}, unavailable);
    if (mode == LlvmInstrumentationMode::Thread)
      require(!selected && unavailable.find("communicating runtime") !=
                               std::string::npos,
              "Thread identity requires communicating-runtime provenance");
    else
      require(selected && selected->instrumentation == spelling,
              "admitted memory instrumentation must bind its exact producer "
              "identity");
  }
  std::string invalidMode;
  require(!queryLlvmBackendIdentity(
              TargetInfo::host(),
              {.instrumentation = static_cast<LlvmInstrumentationMode>(999)},
              invalidMode) &&
              invalidMode == "invalid LLVM instrumentation policy",
          "unknown diagnostic mode must reject before machine/object creation");
  for (const char *name :
       {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "LIBRARY_PATH", "SDKROOT", "COMPILER_PATH", "GCC_EXEC_PREFIX",
        "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET"})
    unsetenv(name);

  TemporaryDirectory temporary;
  const auto project = temporary.root / "project";
  const auto managedRoot = project / "build/gti";
  const auto output = managedRoot / "app";
  const auto representation = managedRoot / "app.o";
  std::filesystem::create_directories(managedRoot);
  std::filesystem::copy(GTI_LLVM_CACHE_FIXTURE, project,
                        std::filesystem::copy_options::recursive);
  const auto edges = project / "edges";
  const auto selectEdges = [&](bool swapped) {
    std::filesystem::remove(edges / "left-leaf.gti");
    std::filesystem::remove(edges / "right-leaf.gti");
    std::filesystem::create_symlink(swapped ? "second.gti" : "first.gti",
                                    edges / "left-leaf.gti");
    std::filesystem::create_symlink(swapped ? "first.gti" : "second.gti",
                                    edges / "right-leaf.gti");
  };
  selectEdges(false);
  auto toolchain = discoverToolchainLayout(self.c_str());
  const auto originalRuntime = toolchain.runtimeLibrary;
  toolchain.runtimeLibrary = temporary.root / "libgti_runtime.a";
  std::filesystem::copy_file(originalRuntime, toolchain.runtimeLibrary);
  const auto originalHeaders = toolchain.runtimeInclude;
  toolchain.runtimeInclude = temporary.root / "include";
  std::filesystem::copy(originalHeaders, toolchain.runtimeInclude,
                        std::filesystem::copy_options::recursive);
  const ManagedOutputPolicy managed{.trustedRoot = project,
                                    .outputRoot = managedRoot};
  const auto compilation = [&](const Policy &policy = Policy{}) {
    return CompilationRequest(project / "main.gti", toolchain.standardLibrary,
                              policy.target, policy.optimization,
                              CppStandard::Cpp23, {}, {}, {}, {}, policy.debug,
                              {}, policy.audit)
        .withBackend(policy.backend)
        .withLlvmInstrumentation(policy.instrumentation);
  };
  const auto ordinaryRequest = compilation();
  const auto addressRequest =
      ordinaryRequest.withLlvmInstrumentation(LlvmInstrumentationMode::Address);
  require(
      ordinaryRequest.llvmInstrumentation() == LlvmInstrumentationMode::None &&
          addressRequest.llvmInstrumentation() ==
              LlvmInstrumentationMode::Address &&
          addressRequest.forExecutable()
                  .withDefinitionClosure()
                  .llvmInstrumentation() == LlvmInstrumentationMode::Address,
      "diagnostic request policy must be immutable and survive derived "
      "requests");
  const auto mapped = llvmBackendOptions(addressRequest);
  require(mapped.optimization == LlvmCodegenOptimization::Aggressive &&
              !mapped.debugInfo &&
              mapped.instrumentation == LlvmInstrumentationMode::Address,
          "emission and identity must share the exact driver options mapping");
  const auto invalidRequest = [&](const auto &action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected,
            "invalid diagnostic request must reject before compilation");
  };
  invalidRequest(
      [&] { static_cast<void>(addressRequest.withBackend(BackendKind::Cpp)); });
  invalidRequest([&] {
    static_cast<void>(
        ordinaryRequest.withBackend(BackendKind::Cpp)
            .withLlvmInstrumentation(LlvmInstrumentationMode::Thread));
  });
  invalidRequest([&] {
    static_cast<void>(ordinaryRequest.withLlvmInstrumentation(
        static_cast<LlvmInstrumentationMode>(999)));
  });
  invalidRequest([&] { static_cast<void>(compileToCpp(addressRequest)); });
  invalidRequest([&] {
    static_cast<void>(compileToCpp(addressRequest, CompilationInputs{}));
  });
  const auto build = [&](const Policy &policy = Policy{}) {
    return buildExecutable(ExecutableBuildRequest(
        compilation(policy), toolchain,
        policy.backend == BackendKind::Cpp ? managedRoot / "app.cpp"
                                           : representation,
        output, policy.compiler, policy.native, policy.keep, true, false,
        managed, std::nullopt,
        policy.cache ? std::optional<BuildCachePolicy>(BuildCachePolicy{
                           .root = managedRoot / "cache",
                           .sourceRoot = project,
                           .compilerIdentity = policy.compilerIdentity,
                           .projectModelIdentity = policy.modelIdentity})
                     : std::nullopt));
  };
  const auto execute = [&](int expected) {
    const auto result = invokeProcess({output.string()},
                                      {.outputMode = ProcessOutputMode::Capture,
                                       .captureSuccessfulOutput = true});
    require(result.exitCode == expected && result.output.empty(),
            "cached executable has incorrect observable behavior");
  };
  const auto first = build();
  checkBuild(first);
  require(first.cache.status == BuildCacheStatus::Miss && first.nativeProcess,
          "first native-object build must miss and link");
  require(
      first.cache.entry.parent_path().filename() == "v5" &&
          read(first.cache.entry / "metadata")
              .starts_with("gti-build-cache-v5\n") &&
          read(first.cache.entry / "metadata").find("\nartifact object .o\n") !=
              std::string::npos,
      "native cache must record its exact binary artifact schema");
  const auto object = read(first.cache.entry / "artifact.o");
  require(object.find('\0') != std::string::npos &&
              first.compilation.artifact->contents == object,
          "real object bytes must include NUL and survive storage exactly");
  const auto executable = read(output);
  const auto executableMode = std::filesystem::status(output).permissions();
  execute(17);
  auto retainedPolicy = Policy{};
  retainedPolicy.keep = true;
  const auto retainedForRejection = build(retainedPolicy);
  checkBuild(retainedForRejection);
  require(
      retainedForRejection.cache.status == BuildCacheStatus::Hit &&
          read(representation) == object,
      "rejection control must begin with a genuinely retained native object");
  for (const auto &[mode, spelling] : modes) {
    static_cast<void>(spelling);
    if (mode == LlvmInstrumentationMode::None)
      continue;
    auto unavailable = Policy{};
    unavailable.instrumentation = mode;
    const auto rejected = build(unavailable);
    require(!rejected.succeeded() &&
                rejected.status ==
                    ExecutableBuildStatus::ToolchainConfigurationFailure &&
                !rejected.nativeProcess && !rejected.compilation.artifact &&
                rejected.compilation.sources.names().empty() &&
                rejected.driverDiagnostic.has_value() &&
                rejected.cache.status == BuildCacheStatus::Bypassed &&
                read(output) == executable && read(representation) == object,
            "an unconfigured diagnostic toolchain must preserve published "
            "output and never "
            "restore a none-mode cache entry");
  }
  std::filesystem::remove(representation);
  for (const bool explicitTriple : {false, true}) {
    auto explicitTarget = Policy{};
    if (explicitTriple)
      explicitTarget.native.targetPolicy.triple = "x86_64-unknown-linux-gnu";
    else
      explicitTarget.native.targetPolicy.sysroot = "/";
    const auto targetRejected = build(explicitTarget);
    require(!targetRejected.succeeded() && !targetRejected.nativeProcess &&
                targetRejected.cache.status == BuildCacheStatus::Bypassed &&
                targetRejected.driverDiagnostic.value_or("").find(
                    "matching target-built runtime") != std::string::npos &&
                read(output) == executable,
            "uncoordinated native target policy must reject before cache/link "
            "publication");
  }
#if defined(__APPLE__)
  for (const char *name :
       {"MACOSX_DEPLOYMENT_TARGET", "SDKROOT", "DEVELOPER_DIR"}) {
    setenv(name, "uncoordinated-policy", 1);
    const auto deploymentRejected = build();
    unsetenv(name);
    require(!deploymentRejected.succeeded() &&
                !deploymentRejected.nativeProcess &&
                deploymentRejected.cache.status == BuildCacheStatus::Bypassed &&
                read(output) == executable,
            "deployment/SDK overrides must not publish objects/runtime built "
            "for a different target policy");
  }
  for (const std::string argument :
       {"-mmacosx-version-min=11.0", "-mmacos-version-min=11.0",
        "-miphoneos-version-min=15.0", "-mtargetos=macos11.0",
        "-darwin-target-variant", "-darwin-target-variant-sdk-version=11.0",
        "-target-sdk-version=11.0", "-macosx_version_min",
        "-Wl,-macosx_version_min,11.0", "-Wl,-platform_version,macos,11.0,11.0",
        "-Xlinker=-macosx_version_min"}) {
    for (const bool trailing : {false, true}) {
      auto deployment = Policy{};
      auto &arguments = trailing ? deployment.native.trailingArguments
                                 : deployment.native.linkerArguments;
      arguments.push_back(argument);
      const auto deploymentRejected = build(deployment);
      require(!deploymentRejected.succeeded() &&
                  !deploymentRejected.nativeProcess &&
                  deploymentRejected.cache.status ==
                      BuildCacheStatus::Bypassed &&
                  deploymentRejected.driverDiagnostic.value_or("").find(
                      "matching target-built runtime") != std::string::npos &&
                  read(output) == executable,
              "opaque final-link deployment overrides must reject before "
              "publication and preserve the existing output");
    }
  }
  auto nonDeployment = Policy{};
  const auto libraryDirectory = temporary.root / "library_version_min";
  std::filesystem::create_directories(libraryDirectory);
  nonDeployment.native.trailingArguments = {
      "-DGTI_CACHE_DEPLOYMENT_MARKER=-mmacosx-version-min=11.0",
      "-L" + libraryDirectory.string()};
  nonDeployment.native.compilerArguments = {"-mmacosx-version-min=11.0"};
  const auto nonDeploymentBuild = build(nonDeployment);
  checkBuild(nonDeploymentBuild);
  require(nonDeploymentBuild.cache.status == BuildCacheStatus::Bypassed &&
              nonDeploymentBuild.nativeProcess,
          "non-option text, search paths and inactive source-compile flags "
          "must not be mistaken for LLVM final-link deployment overrides");
  execute(17);
#endif
  std::filesystem::remove(output);
  auto retained = Policy{};
  retained.keep = true;
  const auto hit = build(retained);
  checkBuild(hit);
  require(
      hit.cache.status == BuildCacheStatus::Hit && !hit.nativeProcess &&
          hit.cache.key == first.cache.key &&
          hit.compilation.artifact->kind == BackendArtifactKind::Object &&
          hit.compilation.artifact->extension == ".o" &&
          hit.compilation.artifact->contents == object &&
          read(representation) == object && read(output) == executable &&
          std::filesystem::status(output).permissions() == executableMode,
      "cache hit must restore exact object, executable and executable mode");
  execute(17);
  const auto plain = build();
  checkBuild(plain);
  require(plain.cache.status == BuildCacheStatus::Hit &&
              !std::filesystem::exists(representation),
          "ordinary native hit must remove the transient object");

  const auto miss = [&](const Policy &policy, std::string_view label) {
    const auto result = build(policy);
    checkBuild(result);
    require(result.cache.status == BuildCacheStatus::Miss &&
                result.cache.key != first.cache.key && result.nativeProcess,
            label);
    const auto again = build(policy);
    checkBuild(again);
    require(again.cache.status == BuildCacheStatus::Hit && !again.nativeProcess,
            "changed-identity rebuild must itself become a real hit");
  };
  const auto sourceSnapshot = [&] {
    const auto inputs = loadCompilationInputs(compilation());
    require(inputs.succeeded(), "dependency-edge fixture must load");
    std::map<std::filesystem::path, std::string> files;
    std::set<std::tuple<std::filesystem::path, std::filesystem::path,
                        SourceDependencyKind>>
        dependencies;
    for (const auto &unit : inputs.sourceGraph.sourceUnits())
      files.emplace(unit.path, read(unit.path));
    for (const auto &edge : inputs.sourceGraph.dependencyEdges()) {
      const auto *source = inputs.sourceGraph.findUnit(edge.source);
      const auto *target = inputs.sourceGraph.findUnit(edge.target);
      require(source && target, "dependency edge must name loaded units");
      dependencies.emplace(source->path, target->path, edge.kind);
    }
    return std::pair{std::move(files), std::move(dependencies)};
  };
  const auto originalSources = sourceSnapshot();
  selectEdges(true);
  const auto swappedSources = sourceSnapshot();
  require(originalSources.first == swappedSources.first &&
              originalSources.second.size() == swappedSources.second.size() &&
              originalSources.second != swappedSources.second,
          "edge mutation must preserve all loaded file paths and bytes");
  miss({}, "dependency edges alone must invalidate a native cache entry");
  execute(17);
  selectEdges(false);
  const auto originalEdges = build();
  checkBuild(originalEdges);
  require(originalEdges.cache.status == BuildCacheStatus::Hit &&
              originalEdges.cache.key == first.cache.key &&
              !originalEdges.nativeProcess,
          "restoring the original dependency edges must reuse their entry");
  auto changed = Policy{};
  changed.optimization = OptimizationLevel::O0;
  miss(changed, "optimization changes must invalidate native objects");
  changed = {};
  changed.audit = RawPointerAuditMode::Warn;
  miss(changed, "audit policy must invalidate native objects");
  changed = {};
  changed.target.runtimeCapabilities.threads =
      !changed.target.runtimeCapabilities.threads;
  miss(changed, "target runtime capability must invalidate native objects");
  changed = {};
  changed.target.executionProfile = ExecutionProfile::Concurrent;
  const auto beforeConcurrent = read(output);
  const auto concurrent = build(changed);
  require(!concurrent.succeeded() && concurrent.cache.key != first.cache.key &&
              concurrent.cache.status == BuildCacheStatus::Miss &&
              read(output) == beforeConcurrent,
          "concurrent policy must reanalyze and reject the mutable global, not "
          "restore a single-threaded artifact");
  changed = {};
  changed.compilerIdentity += "-changed";
  miss(changed, "compiler policy identity must invalidate native objects");
  changed = {};
  changed.modelIdentity += "-changed";
  miss(changed, "project identity must invalidate native objects");
  const auto wrapper = temporary.root / "native-cxx";
  write(wrapper, "#!/bin/sh\nexec c++ \"$@\"\n");
  std::filesystem::permissions(wrapper, std::filesystem::perms::owner_all);
  changed = {};
  changed.compiler = wrapper.string();
  miss(changed, "selected native toolchain must invalidate native objects");
  write(wrapper, "#!/bin/sh\n# identity mutation\nexec c++ \"$@\"\n");
  miss(changed, "native compiler executable bytes must invalidate objects");
  write(wrapper, "#!/bin/sh\nif [ \"$1\" = --version ]; then exit 19; fi\n"
                 "exec c++ \"$@\"\n");
  const auto unknownToolchain = build(changed);
  checkBuild(unknownToolchain);
  require(unknownToolchain.cache.status == BuildCacheStatus::Bypassed &&
              unknownToolchain.cache.detail.value_or("").find(
                  "identity unavailable") != std::string::npos,
          "an unavailable required toolchain identity must bypass, not guess");
  write(wrapper,
        "#!/bin/sh\nif [ \"$1\" = --version ]; then exec c++ --version; fi\n"
        "exit 23\n");
  const auto failedLink = build(changed);
  require(
      !failedLink.succeeded() && failedLink.nativeProcess &&
          failedLink.cache.status == BuildCacheStatus::Miss &&
          !std::filesystem::exists(failedLink.cache.entry / "metadata") &&
          read(output) == executable,
      "failed native link must not publish cache metadata or replace output");
  const auto sourceTimestamp =
      std::filesystem::last_write_time(project / "value.gti");
  write(project / "value.gti",
        "export module value;\nexport int cache_value() { return 23; }\n");
  std::filesystem::last_write_time(project / "value.gti", sourceTimestamp);
  miss({}, "transitive GTI source content must invalidate native objects");
  execute(23);
  std::filesystem::copy_file(
      std::filesystem::path(GTI_LLVM_CACHE_FIXTURE) / "value.gti",
      project / "value.gti", std::filesystem::copy_options::overwrite_existing);
  const auto header = toolchain.runtimeInclude / "gti/runtime.h";
  const auto headerContents = read(header);
  write(header, headerContents + "\n// cache identity mutation\n");
  miss({}, "runtime header content must invalidate native objects");
  write(header, headerContents);
  const auto archive = read(toolchain.runtimeLibrary);
  require(archive.starts_with("!<arch>\n") && archive.size() > 36,
          "runtime must be a regular archive for the identity mutation");
  auto changedArchive = archive;
  // Mutate a member timestamp, not object code or archive structure.
  changedArchive[24] = archive[24] == '1' ? '2' : '1';
  write(toolchain.runtimeLibrary, changedArchive);
  miss({}, "runtime archive bytes must invalidate native objects");
  write(toolchain.runtimeLibrary, archive);

  for (const char *payload : {"artifact.o", "executable", "metadata"}) {
    write(first.cache.entry / payload, "corrupt");
    const auto recovered = build();
    checkBuild(recovered);
    require(recovered.cache.status == BuildCacheStatus::RecoveredCorruption &&
                recovered.cache.warning && recovered.nativeProcess,
            "corrupt payload must rebuild instead of being restored");
    execute(17);
  }
  auto metadata = read(first.cache.entry / "metadata");
  const auto kind = metadata.find("artifact object .o");
  require(kind != std::string::npos, "native artifact metadata row is missing");
  metadata.replace(kind, std::string("artifact object .o").size(),
                   "artifact source .cpp");
  write(first.cache.entry / "metadata", metadata);
  const auto wrongKind = build();
  checkBuild(wrongKind);
  require(wrongKind.cache.status == BuildCacheStatus::RecoveredCorruption,
          "wrong kind/extension must not restore native cache bytes");

  changed = {};
  changed.cache = false;
  const auto uncached = build(changed);
  checkBuild(uncached);
  require(uncached.cache.status == BuildCacheStatus::NotConfigured &&
              uncached.nativeProcess && read(output) == executable,
          "explicit no-cache build must regenerate equivalent executable");
  changed = {};
  changed.native.trailingArguments = {"-lm"};
  const auto opaque = build(changed);
  checkBuild(opaque);
  require(opaque.cache.status == BuildCacheStatus::Bypassed,
          "opaque native inputs must retain conservative cache bypass");
  changed = {};
  changed.debug = true;
  changed.keep = true;
  const auto debug = build(changed);
  checkBuild(debug);
  require(debug.cache.status == BuildCacheStatus::Miss &&
              debug.cache.key != first.cache.key && debug.nativeProcess,
          "debug policy must build a distinct real native object/cache entry");
  execute(17);
  const auto debugExecutable = read(output);
  const auto debugObject = read(representation);
  const auto debugHit = build(changed);
  checkBuild(debugHit);
  require(
      debugHit.cache.status == BuildCacheStatus::Hit &&
          !debugHit.nativeProcess && read(output) == debugExecutable &&
          read(representation) == debugObject,
      "debug object/executable must restore from their exact cache identity");
  execute(17);

  const auto outside = temporary.root / "outside";
  write(outside, "preserve");
  std::filesystem::remove(first.cache.entry / "artifact.o");
  std::filesystem::create_symlink(outside, first.cache.entry / "artifact.o");
  const auto symlink = build();
  checkBuild(symlink);
  require(symlink.cache.status == BuildCacheStatus::RecoveredCorruption &&
              read(outside) == "preserve",
          "symlink cache leaf must not be trusted or overwrite its referent");

  changed = {};
  changed.backend = BackendKind::Cpp;
  changed.keep = true;
  const auto cpp = build(changed);
  checkBuild(cpp);
  require(
      cpp.cache.status == BuildCacheStatus::Miss &&
          cpp.cache.key != first.cache.key &&
          cpp.cache.entry.parent_path().filename() == "v4" &&
          read(cpp.cache.entry / "metadata")
              .starts_with("gti-build-cache-v4\n") &&
          std::filesystem::is_regular_file(cpp.cache.entry / "generated.cpp"),
      "C++ artifacts must remain isolated in unchanged schema v4");
  const auto cppHit = build(changed);
  checkBuild(cppHit);
  require(
      cppHit.cache.status == BuildCacheStatus::Hit && !cppHit.nativeProcess &&
          cppHit.compilation.artifact->kind == BackendArtifactKind::Source &&
          cppHit.compilation.artifact->extension == ".cpp",
      "C++ v4 cache must still restore generated source");
  execute(17);
  const auto nativeAgain = build();
  checkBuild(nativeAgain);
  require(nativeAgain.cache.status == BuildCacheStatus::Hit &&
              nativeAgain.cache.key == first.cache.key &&
              read(output) == executable,
          "C++ build must not replace a native-object cache entry");
  const auto validSource = read(project / "main.gti");
  const auto validMetadata = read(first.cache.entry / "metadata");
  // A still-refused native shape; it moves as LLVM coverage grows.
  write(project / "main.gti", read(project / "unadmitted_dispatch.gti"));
  require(checkCompilation(compilation().forExecutable()).succeeded(),
          "backend rejection fixture must pass source-language checking");
  const auto rejectedAdmission = [&](const ExecutableBuildResult &result) {
    bool backendDiagnostic = false;
    for (const auto &diagnostic : result.compilation.diagnostics)
      backendDiagnostic |= diagnostic.code == "GTI-B0003";
    require(
        !result.succeeded() &&
            result.status == ExecutableBuildStatus::CompilationFailure &&
            result.compilation.status == CompilationStatus::BackendFailure &&
            backendDiagnostic && !result.compilation.artifact &&
            !result.nativeProcess &&
            result.cache.status == BuildCacheStatus::Miss &&
            result.cache.key != first.cache.key,
        "unadmitted base-view replacement must fail before link or restore");
  };
  const auto admission = build(retained);
  rejectedAdmission(admission);
  require(!std::filesystem::exists(admission.cache.entry / "metadata") &&
              read(output) == executable,
          "failed admission must not publish or replace an existing output");
  std::filesystem::copy(first.cache.entry, admission.cache.entry,
                        std::filesystem::copy_options::recursive);
  std::filesystem::remove(output);
  std::filesystem::remove(representation);
  const auto staleAdmission = build(retained);
  rejectedAdmission(staleAdmission);
  require(staleAdmission.cache.warning && !std::filesystem::exists(output) &&
              !std::filesystem::exists(representation) &&
              read(first.cache.entry / "metadata") == validMetadata &&
              read(admission.cache.entry / "metadata") == validMetadata,
          "a stale entry for another source must not evade failed admission or "
          "restore any artifact");
  write(project / "main.gti", validSource);
  const auto afterAdmission = build(retained);
  checkBuild(afterAdmission);
  require(afterAdmission.cache.status == BuildCacheStatus::Hit &&
              afterAdmission.cache.key == first.cache.key &&
              !afterAdmission.nativeProcess && read(output) == executable &&
              read(representation) == object,
          "failed admission must leave the valid executable and object "
          "restorable");
  execute(17);

  write(project / "main.gti", "int main() { return missing_cache_symbol; }\n");
  const auto rejected = build();
  require(!rejected.succeeded() &&
              !std::filesystem::exists(rejected.cache.entry / "metadata") &&
              read(output) == executable,
          "source rejection must not publish a cache entry or replace output");
  std::cout << "Backend identity: schema=" << identity->schema
            << " LLVM=" << identity->llvmVersion
            << " private-ABI=" << identity->privateAbi
            << " triple=" << identity->triple << " CPU=" << identity->cpu
            << " features=" << identity->features
            << " debug=" << identity->debugInfo
            << " instrumentation=" << identity->instrumentation << '\n';
  std::cout << "LLVM cache round trips, content/edge invalidation, failed "
               "admission, corruption, restoration, "
               "publication safety and C++ isolation passed\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--generated-memory-probe") {
      runGeneratedMemoryChecks(std::filesystem::absolute(argv[0]), argv[2],
                               argv[3], std::filesystem::absolute(argv[4]));
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--runtime-probe") {
      runNativeRuntimeChecks(argv[2]);
      return 0;
    }
    require(argc == 1 || argc == 2, "usage: llvm cache test [toolchain "
                                    "executable] | --runtime-probe clang++");
    runChecks(std::filesystem::absolute(argv[argc == 2 ? 1 : 0]));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
