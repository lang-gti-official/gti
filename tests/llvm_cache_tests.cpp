#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

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
  }
  require(result.succeeded(), "real cache fixture build failed");
}

void runChecks(const std::filesystem::path &self) {
  for (const char *name :
       {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "LIBRARY_PATH", "SDKROOT", "COMPILER_PATH", "GCC_EXEC_PREFIX"})
    unsetenv(name);

  TemporaryDirectory temporary;
  const auto project = temporary.root / "project";
  const auto managedRoot = project / "build/gti";
  const auto output = managedRoot / "app";
  const auto representation = managedRoot / "app.o";
  std::filesystem::create_directories(managedRoot);
  std::filesystem::copy(GTI_LLVM_CACHE_FIXTURE, project,
                        std::filesystem::copy_options::recursive);
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
  const auto build = [&](const Policy &policy = Policy{}) {
    CompilationRequest compilation(project / "main.gti",
                                   toolchain.standardLibrary, policy.target,
                                   policy.optimization, CppStandard::Cpp23, {},
                                   {}, {}, {}, policy.debug, {}, policy.audit);
    return buildExecutable(ExecutableBuildRequest(
        compilation.withBackend(policy.backend), toolchain,
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
  auto changed = Policy{};
  changed.optimization = OptimizationLevel::O0;
  miss(changed, "optimization changes must invalidate native objects");
  changed = {};
  changed.audit = RawPointerAuditMode::Warn;
  miss(changed, "audit policy must invalidate native objects");
  changed = {};
  changed.target.executionProfile = ExecutionProfile::Concurrent;
  miss(changed, "execution target policy must invalidate native objects");
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
  write(project / "value.gti", "int cache_value() { return 23; }\n");
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
  const auto beforeDebug = read(output);
  changed = {};
  changed.debug = true;
  const auto debug = build(changed);
  require(!debug.succeeded() &&
              debug.cache.status == BuildCacheStatus::Bypassed &&
              read(output) == beforeDebug,
          "unsupported LLVM debug policy must bypass and preserve destination");

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
  write(project / "main.gti", "int main() { return missing_cache_symbol; }\n");
  const auto rejected = build();
  require(!rejected.succeeded() &&
              !std::filesystem::exists(rejected.cache.entry / "metadata") &&
              read(output) == executable,
          "source rejection must not publish a cache entry or replace output");
  std::cout << "LLVM cache round trips, invalidation, corruption, restoration, "
               "publication safety and C++ isolation passed\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 1, "unexpected cache test arguments");
    runChecks(std::filesystem::absolute(argv[0]));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
