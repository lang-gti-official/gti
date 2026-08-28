#include "../src/driver/build_source_identity.h"
#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/build_report.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gti-driver-test-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  [[nodiscard]] const std::filesystem::path &root() const { return path; }

private:
  std::filesystem::path path;
};

class ScopedEnvironment final {
public:
  explicit ScopedEnvironment(std::string name) : variable(std::move(name)) {
    rememberCurrentValue();
    clear();
  }

  ScopedEnvironment(std::string name, std::string value)
      : variable(std::move(name)) {
    rememberCurrentValue();
    set(value);
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

  ~ScopedEnvironment() {
    if (previous) {
      set(*previous);
      return;
    }
    clear();
  }

private:
  void rememberCurrentValue() {
    if (const char *current = std::getenv(variable.c_str())) {
      previous = current;
    }
  }

  void clear() {
#if defined(_WIN32)
    (void)_putenv_s(variable.c_str(), "");
#else
    (void)unsetenv(variable.c_str());
#endif
  }

  void set(const std::string &value) {
#if defined(_WIN32)
    (void)_putenv_s(variable.c_str(), value.c_str());
#else
    (void)setenv(variable.c_str(), value.c_str(), 1);
#endif
  }

  std::string variable;
  std::optional<std::string> previous;
};

class ThrowingBackend final : public lang::Backend {
public:
  explicit ThrowingBackend(bool throwStandardException,
                           bool throwFromName = false)
      : standardException(throwStandardException), throwingName(throwFromName) {
  }

  [[nodiscard]] std::string_view name() const override {
    if (throwingName) {
      throw std::runtime_error("deliberate backend name failure");
    }
    return "throwing-test";
  }

  [[nodiscard]] lang::BackendArtifact
  generate(const lang::LoweredProgram &) override {
    invoked = true;
    if (standardException) {
      throw std::runtime_error("deliberate backend failure");
    }
    throw 17;
  }

  [[nodiscard]] bool wasInvoked() const { return invoked; }

private:
  bool standardException = true;
  bool throwingName = false;
  bool invoked = false;
};

class UnsupportedBackend final : public lang::Backend {
public:
  [[nodiscard]] std::string_view name() const override {
    return "unsupported-test";
  }

  [[nodiscard]] lang::BackendArtifact
  generate(const lang::LoweredProgram &) override {
    invoked = true;
    throw lang::BackendUnsupportedError("missing bounded test capability");
  }

  [[nodiscard]] bool wasInvoked() const { return invoked; }

private:
  bool invoked = false;
};

bool writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path);
  output << contents;
  return static_cast<bool>(output);
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::string> activeSanitizerArguments() {
  bool addressSanitizer = false;
  bool undefinedBehaviorSanitizer = false;
#if defined(__SANITIZE_ADDRESS__)
  addressSanitizer = true;
#endif
#if defined(__SANITIZE_UNDEFINED__)
  undefinedBehaviorSanitizer = true;
#endif
#if defined(__clang__)
#if __has_feature(address_sanitizer)
  addressSanitizer = true;
#endif
#if __has_feature(undefined_behavior_sanitizer)
  undefinedBehaviorSanitizer = true;
#endif
#endif
  std::vector<std::string> arguments;
  if (addressSanitizer) {
    arguments.emplace_back("-fsanitize=address");
  }
  if (undefinedBehaviorSanitizer) {
    arguments.emplace_back("-fsanitize=undefined");
  }
  return arguments;
}

void testAnalysisConfigurationResolution() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  expect(!host.runtimeCapabilities.threads,
         "raw target values should default to no runtime capabilities");

  const lang::driver::AnalysisConfiguration safeDefault(host);
  expect(safeDefault.target().executionProfile ==
                 lang::ExecutionProfile::SingleThreaded &&
             !safeDefault.target().runtimeCapabilities.threads &&
             safeDefault.runtimeProfile() == lang::driver::RuntimeProfile::None,
         "analysis configuration should fail closed without an explicit "
         "runtime profile");

  const lang::driver::AnalysisConfiguration unavailableConcurrent(
      host, lang::ExecutionProfile::Concurrent);
  expect(unavailableConcurrent.target().executionProfile ==
                 lang::ExecutionProfile::Concurrent &&
             !unavailableConcurrent.target().runtimeCapabilities.threads,
         "a concurrent execution profile must not synthesize runtime thread "
         "support");

  const lang::driver::AnalysisConfiguration hostedConcurrent(
      host, lang::ExecutionProfile::Concurrent,
      lang::driver::RuntimeProfile::Hosted, {"ZETA", "ALPHA", "ZETA"});
  expect(hostedConcurrent.target().runtimeCapabilities.threads &&
             hostedConcurrent.configurationFlags() ==
                 lang::ConfigurationFlags({"ALPHA", "ZETA"}),
         "the supported hosted runtime profile should derive threads and "
         "normalize frontend flags once");

  const lang::driver::AnalysisConfiguration unsupportedHosted(
      lang::TargetInfo{
          .os = "unsupported", .vendor = "unknown", .arch = "x86_64"},
      lang::ExecutionProfile::Concurrent, lang::driver::RuntimeProfile::Hosted);
  expect(!unsupportedHosted.target().runtimeCapabilities.threads,
         "the hosted runtime profile must not advertise threads for an "
         "unsupported target");
}

void testCompilationRequestAndTargetPropagation() {
  TemporaryDirectory temporary;
  const std::filesystem::path prelude = temporary.root() / "prelude.gti";
  const std::filesystem::path source = temporary.root() / "main.gti";
  expect(writeFile(prelude, ""), "the driver test prelude should be writable");
  expect(writeFile(source, R"(#if target.os == "linux"
int selected() { return 41; }
#else
int selected() { return 82; }
#endif
int main() {
  bool folded = 1 < 2;
  if (!folded) { return 1; }
  return selected() - 41;
}
)"),
         "the driver test source should be writable");

  const lang::TargetInfo target{
      .os = "linux", .vendor = "unknown", .arch = "x86_64"};
  const lang::driver::CompilationRequest request(
      source, lang::standardLibraryLayout(temporary.root()), target,
      lang::OptimizationLevel::O1, lang::CppStandard::Cpp23);
  expect(request.entry() == source &&
             request.standardLibrary().prelude == prelude,
         "compilation requests should retain resolved source and stdlib paths");
  expect(request.target().os == "linux" &&
             request.optimization() == lang::OptimizationLevel::O1 &&
             request.cppStandard() == lang::CppStandard::Cpp23 &&
             !request.optimizationOutputs().requested(),
         "compilation requests should retain target and backend policy");

  const lang::driver::CompilationResult result =
      lang::driver::compileToCpp(request);
  expect(result.succeeded(),
         "the driver should compile a valid whole-program request");
  expect(!result.preOptimizationMir && !result.postOptimizationMir &&
             !result.optimizationReport,
         "ordinary driver compilation should not capture optimizer output");
  if (result.artifact) {
    expect(
        result.artifact->contents.find(
            "// GTI verified-MIR body: scalar-cfg-v1") != std::string::npos &&
            result.artifact->contents.find("static_cast<std::int32_t>(41)") !=
                std::string::npos &&
            result.artifact->contents.find("static_cast<std::int32_t>(82)") ==
                std::string::npos,
        "one resolved target must reach frontend conditionals and backend "
        "emission");
  }

  const lang::driver::CompilationRequest observedRequest(
      source, lang::standardLibraryLayout(temporary.root()), target,
      lang::OptimizationLevel::O1, lang::CppStandard::Cpp23, {}, {},
      {.capturePreMir = true, .capturePostMir = true, .captureReport = true});
  const lang::driver::CompilationResult observed =
      lang::driver::compileToCpp(observedRequest);
  expect(observed.succeeded() && observed.preOptimizationMir &&
             observed.postOptimizationMir && observed.optimizationReport &&
             *observed.preOptimizationMir != *observed.postOptimizationMir &&
             observed.optimizationReport->passes.size() == 4,
         "the reusable driver should expose opt-in pre/post MIR and the "
         "verified optimizer report");

  const lang::driver::CheckResult checked =
      lang::driver::checkCompilation(request);
  expect(checked.succeeded() && checked.diagnostics.empty(),
         "driver checks should stop successfully after the shared frontend");

  const std::filesystem::path invalidSource = temporary.root() / "invalid.gti";
  expect(writeFile(invalidSource, "int main( { return 0; }\n"),
         "the invalid driver fixture should be writable");
  const lang::driver::CompilationResult invalid =
      lang::driver::compileToCpp(lang::driver::CompilationRequest(
          invalidSource, lang::standardLibraryLayout(temporary.root()), target,
          lang::OptimizationLevel::O0, lang::CppStandard::Cpp23));
  expect(!invalid.succeeded() &&
             invalid.status ==
                 lang::driver::CompilationStatus::FrontendFailure &&
             !invalid.diagnostics.empty() &&
             invalid.sources.find(invalid.diagnostics.front().primary.source) !=
                 nullptr,
         "frontend failures should remain structured for CLI presentation");
  const lang::driver::CheckResult invalidCheck =
      lang::driver::checkCompilation(lang::driver::CompilationRequest(
          invalidSource, lang::standardLibraryLayout(temporary.root()), target,
          lang::OptimizationLevel::O3, lang::CppStandard::Cpp20));
  expect(!invalidCheck.succeeded() && !invalidCheck.diagnostics.empty(),
         "driver checks should preserve structured frontend failures without "
         "entering a backend");
}

void testBackendExceptionBoundary() {
  TemporaryDirectory temporary;
  const std::filesystem::path prelude = temporary.root() / "prelude.gti";
  const std::filesystem::path source = temporary.root() / "main.gti";
  expect(writeFile(prelude, ""),
         "the backend-failure test prelude should be writable");
  expect(writeFile(source, "int main() { return 0; }\n"),
         "the backend-failure test source should be writable");

  const lang::driver::CompilationRequest request(
      source, lang::standardLibraryLayout(temporary.root()),
      lang::TargetInfo::host(), lang::OptimizationLevel::O0,
      lang::CppStandard::Cpp23);
  const std::filesystem::path canonicalSource =
      std::filesystem::weakly_canonical(source);

  ThrowingBackend standardBackend(true);
  const lang::driver::CompilationResult standardFailure =
      lang::driver::compileWithBackend(request, standardBackend);
  const auto standardDiagnostic = std::find_if(
      standardFailure.diagnostics.begin(), standardFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(standardBackend.wasInvoked() && !standardFailure.succeeded() &&
             standardFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !standardFailure.artifact,
         "a standard backend exception should become a backend compilation "
         "failure without publishing an artifact");
  expect(standardDiagnostic != standardFailure.diagnostics.end() &&
             standardDiagnostic->phase == lang::DiagnosticPhase::Backend &&
             standardDiagnostic->primary.source == canonicalSource.string() &&
             standardDiagnostic->primary.start == 0 &&
             standardDiagnostic->primary.end == 0 &&
             standardDiagnostic->message.find("throwing-test") !=
                 std::string::npos &&
             standardDiagnostic->message.find("deliberate backend failure") !=
                 std::string::npos &&
             !standardDiagnostic->hints.empty() &&
             standardFailure.sources.find(canonicalSource.string()) != nullptr,
         "a standard backend exception should retain the source snapshot and "
         "produce an actionable entry-anchored GTI-B0001 diagnostic");

  ThrowingBackend unknownBackend(false);
  const lang::driver::CompilationResult unknownFailure =
      lang::driver::compileWithBackend(request, unknownBackend);
  const auto unknownDiagnostic = std::find_if(
      unknownFailure.diagnostics.begin(), unknownFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(unknownBackend.wasInvoked() && !unknownFailure.succeeded() &&
             unknownFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !unknownFailure.artifact &&
             unknownDiagnostic != unknownFailure.diagnostics.end() &&
             unknownDiagnostic->message.find("non-standard exception") !=
                 std::string::npos &&
             unknownFailure.sources.find(canonicalSource.string()) != nullptr,
         "a non-standard backend exception should use the same structured "
         "driver failure boundary");

  ThrowingBackend unnamedBackend(true, true);
  const lang::driver::CompilationResult unnamedFailure =
      lang::driver::compileWithBackend(request, unnamedBackend);
  const auto unnamedDiagnostic = std::find_if(
      unnamedFailure.diagnostics.begin(), unnamedFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(unnamedBackend.wasInvoked() && !unnamedFailure.succeeded() &&
             unnamedFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             unnamedDiagnostic != unnamedFailure.diagnostics.end() &&
             unnamedDiagnostic->message.find("backend 'unknown'") !=
                 std::string::npos &&
             unnamedDiagnostic->message.find("deliberate backend failure") !=
                 std::string::npos,
         "an exception from backend name metadata should not defeat the "
         "generation exception boundary");

  UnsupportedBackend unsupportedBackend;
  const lang::driver::CompilationResult unsupported =
      lang::driver::compileWithBackend(request, unsupportedBackend);
  const auto unsupportedDiagnostic = std::find_if(
      unsupported.diagnostics.begin(), unsupported.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0003";
      });
  expect(unsupportedBackend.wasInvoked() && !unsupported.succeeded() &&
             unsupported.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !unsupported.artifact &&
             unsupportedDiagnostic != unsupported.diagnostics.end() &&
             unsupportedDiagnostic->phase == lang::DiagnosticPhase::Backend &&
             unsupportedDiagnostic->primary.source ==
                 canonicalSource.string() &&
             unsupportedDiagnostic->message.find("unsupported-test") !=
                 std::string::npos &&
             unsupportedDiagnostic->message.find(
                 "missing bounded test capability") != std::string::npos &&
             std::none_of(unsupported.diagnostics.begin(),
                          unsupported.diagnostics.end(),
                          [](const lang::Diagnostic &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }) &&
             unsupportedDiagnostic->hints.empty(),
         "a classified backend limitation should use GTI-B0003 without "
         "asking the user to report an internal compiler error");
}

void testProcessInvocation(const std::filesystem::path &testExecutable) {
  const lang::driver::ProcessResult captured = lang::driver::invokeProcess(
      {testExecutable.string(), "--process-child", "alpha", "two words", ""},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "test child"});
  expect(captured.succeeded() &&
             captured.output == "process arguments preserved\n",
         "the reusable process runner should preserve exact argument-vector "
         "elements");

  const lang::driver::ProcessResult inherited = lang::driver::invokeProcess(
      {testExecutable.string(), "--process-child-exit"},
      {.outputMode = lang::driver::ProcessOutputMode::Inherit,
       .captureSuccessfulOutput = false,
       .description = "test child"});
  expect(inherited.exitCode == 23 && inherited.output.empty(),
         "inherited process execution should propagate the child exit status "
         "without capturing terminal output");

  const lang::driver::ProcessResult empty =
      lang::driver::invokeProcess({}, {.description = "test child"});
  expect(empty.exitCode == 127 && empty.driverDiagnostic &&
             empty.driverDiagnostic->find("test child command is empty") !=
                 std::string::npos,
         "empty process requests should fail before attempting execution");
}

void testNativeCommandConstruction() {
  lang::driver::NativeInputs inputs;
  inputs.includeDirectories = {"runtime include", "vendor"};
  inputs.compilerArguments = {"-DDEBUG=1"};
  inputs.libraryDirectories = {"native lib"};
  inputs.libraryFiles = {"libgti_runtime.a"};
  inputs.libraries = {"engine"};
  inputs.frameworks = {"Metal"};
  inputs.linkerArguments = {"-pthread"};
  inputs.trailingArguments = {"-Wl,--as-needed", "-DTAIL=1"};

  const lang::driver::NativeCompileRequest request(
      "custom-c++", "generated source.cpp", "game", lang::CppStandard::Cpp20,
      lang::OptimizationLevel::O3, std::move(inputs));
  const std::vector<std::string> command =
      lang::driver::NativeToolchain().command(request);
  std::vector<std::string> expected{
      "custom-c++",
      "-std=c++20",
      "-O3",
      "-Iruntime include",
      "-Ivendor",
      "-DDEBUG=1",
      "generated source.cpp",
      "-Lnative lib",
      "libgti_runtime.a",
      "-lengine",
      "-framework",
      "Metal",
      "-pthread",
      "-o",
      "game",
      "-Wl,--as-needed",
      "-DTAIL=1",
      "-fno-fast-math",
      "-ffp-contract=off",
      "-D__gti_strict_ieee754=1",
  };
#if defined(__APPLE__)
  expected.emplace_back("-Wl,-reproducible");
#endif
  expect(command == expected,
         "native command construction should preserve structured input order");
  const std::string rendered = lang::driver::renderCommand(command);
  expect(rendered.starts_with("+ custom-c++ -std=c++20 -O3") &&
             rendered.find("\"-Iruntime include\"") != std::string::npos &&
             rendered.find("\"generated source.cpp\"") != std::string::npos,
         "verbose command rendering should quote arguments without changing "
         "the process vector");
  expect(lang::driver::discoverNativeCompiler(std::string("selected-c++")) ==
             "selected-c++",
         "an explicitly selected compiler should have highest precedence");
  expect(lang::driver::discoverCCompiler(std::string("selected-cc")) ==
             "selected-cc",
         "an explicitly selected C compiler should have highest precedence");

  const lang::driver::NativeCCompileRequest cRequest(
      "custom-cc", "native source.c", "native object.o",
      lang::driver::CStandard::C17, lang::OptimizationLevel::O2,
      {"native include"}, {"-DNATIVE_VALUE=42"});
  expect(lang::driver::NativeToolchain().command(cRequest) ==
             std::vector<std::string>({"custom-cc", "-std=c17", "-O2",
                                       "-Inative include", "-DNATIVE_VALUE=42",
                                       "-c", "native source.c", "-o",
                                       "native object.o"}),
         "native C compilation should preserve its distinct standard, include, "
         "argument, source, and output policy");
  expect(
      lang::driver::NativeToolchain().preprocessCommand(
          cRequest, "native dependencies.d") ==
          std::vector<std::string>(
              {"custom-cc", "-std=c17", "-O2", "-Inative include",
               "-DNATIVE_VALUE=42", "-E", "-MD", "-MF", "native dependencies.d",
               "native source.c", "-o", "native object.o"}),
      "native C preprocessing should preserve the compile request while "
      "replacing object compilation with preprocessing and dependency "
      "discovery");

  const lang::driver::NativeCppCompileRequest cppRequest(
      "custom-c++", "native source.cpp", "native object.o",
      lang::CppStandard::Cpp23, lang::OptimizationLevel::O1,
      {"runtime include", "native include"}, {"-DNATIVE_CPP_VALUE=42"});
  expect(lang::driver::NativeToolchain().command(cppRequest) ==
             std::vector<std::string>(
                 {"custom-c++", "-std=c++23", "-O1", "-Iruntime include",
                  "-Inative include", "-DNATIVE_CPP_VALUE=42", "-c",
                  "native source.cpp", "-o", "native object.o"}),
         "native C++ source compilation should preserve the resolved standard, "
         "include, argument, source, and output policy");

  lang::driver::NativeInputs orderedInputs;
  orderedInputs.libraryDirectories = {"native lib"};
  orderedInputs.libraryFiles = {"ignored-file.a"};
  orderedInputs.libraries = {"ignored-library"};
  orderedInputs.frameworks = {"IgnoredFramework"};
  orderedInputs.orderedLinkOperands = {
      {lang::driver::NativeLinkOperandKind::File, "libgti_runtime.a"},
      {lang::driver::NativeLinkOperandKind::Library, "target"},
      {lang::driver::NativeLinkOperandKind::File, "package-provider.a"},
      {lang::driver::NativeLinkOperandKind::Framework, "CoreFoundation"},
  };
  orderedInputs.linkerArguments = {"-pthread"};
  const std::vector<std::string> orderedCommand =
      lang::driver::NativeToolchain().command(
          lang::driver::NativeCompileRequest(
              "custom-c++", "generated.cpp", "program",
              lang::CppStandard::Cpp23, lang::OptimizationLevel::O2,
              std::move(orderedInputs)));
  std::vector<std::string> expectedOrderedCommand{
      "custom-c++",
      "-std=c++23",
      "-O2",
      "generated.cpp",
      "-Lnative lib",
      "libgti_runtime.a",
      "-ltarget",
      "package-provider.a",
      "-framework",
      "CoreFoundation",
      "-pthread",
      "-o",
      "program",
      "-fno-fast-math",
      "-ffp-contract=off",
      "-D__gti_strict_ieee754=1",
  };
#if defined(__APPLE__)
  expectedOrderedCommand.emplace_back("-Wl,-reproducible");
#endif
  expect(orderedCommand == expectedOrderedCommand,
         "ordered native link operands should preserve mixed file, -l, and "
         "framework dependency order after the build-owned runtime and be "
         "authoritative over compatibility category vectors");

  expect(lang::driver::isReservedNativeBuildArgument("-o") &&
             lang::driver::isReservedNativeBuildArgument(
                 "--target=other-unknown-os") &&
             lang::driver::isReservedNativeBuildArgument("-target=arm64") &&
             lang::driver::isReservedNativeBuildArgument("-m32") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Wl,-syslibroot,/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Wl,--sysroot=/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Xlinker=--sysroot=/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument("-Xlinker") &&
             lang::driver::isReservedNativeBuildArgument("-Xclang") &&
             lang::driver::isReservedNativeBuildArgument("-Xclang=-triple") &&
             lang::driver::isReservedNativeBuildArgument("-cc1") &&
             lang::driver::isReservedNativeBuildArgument("-cc1as") &&
             lang::driver::isReservedNativeBuildArgument("--driver-mode=cl") &&
             lang::driver::isReservedNativeBuildArgument("-Wl,-m,elf_i386") &&
             lang::driver::isReservedNativeBuildArgument("/machine:x64") &&
             lang::driver::isReservedNativeBuildArgument("/O2") &&
             !lang::driver::isReservedNativeBuildArgument(
                 "-DGTI_NATIVE_POLICY=1") &&
             !lang::driver::isReservedNativeBuildArgument(
                 "-DGTI_LIST=alpha,@beta") &&
             !lang::driver::isReservedNativeBuildArgument("-lprovider") &&
             !lang::driver::isReservedNativeBuildArgument("/openmp") &&
             !lang::driver::isReservedNativeBuildArgument("/FORCE:MULTIPLE"),
         "the shared native argument policy should reserve driver-owned "
         "output and target/data-layout settings, including raw native-driver "
         "escape modes, while preserving ordinary compiler definitions and "
         "link operands without prefix-based false positives");
}

void testRenderedCommandReplay(const std::filesystem::path &testExecutable) {
  const std::vector<std::string> arguments{
      "plain",
      "safe/path-1.2_@%+=:,",
      "two words",
      "tab\tvalue",
      "",
      "a\\b",
      "$HOME",
      "`command`",
      "x;y",
      "a'b",
      "line\nbreak",
      "*.cpp",
      "\"quoted\"",
      "left&right",
      "left|right",
      "<input>",
      "(group)",
      "question?",
  };
  const std::string rendered = lang::driver::renderCommand(arguments);
  const std::string expected =
      "+ plain safe/path-1.2_@%+=:, \"two words\" \"tab\tvalue\" '' "
      "'a\\b' '$HOME' '`command`' 'x;y' 'a'\\''b' 'line\nbreak' "
      "'*.cpp' '\"quoted\"' 'left&right' 'left|right' '<input>' "
      "'(group)' 'question?'";
  expect(rendered == expected,
         "POSIX command rendering should use readable whitespace quotes and "
         "fail-safe quoting for every other shell-sensitive argument");

#if !defined(_WIN32)
  std::vector<std::string> replayArguments{testExecutable.string(),
                                           "--render-command-child"};
  replayArguments.insert(replayArguments.end(), arguments.begin(),
                         arguments.end());
  const std::string replay = lang::driver::renderCommand(replayArguments);
  const lang::driver::ProcessResult result = lang::driver::invokeProcess(
      {"/bin/sh", "-c", replay.substr(2)},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "rendered POSIX command"});
  expect(result.succeeded() &&
             result.output == "rendered arguments preserved\n",
         "a POSIX shell should reconstruct every rendered argument exactly");
#endif
}

void testNativeCCompilerFailure() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  const std::filesystem::path generated = temporary.root() / "generated.cpp";
  const std::filesystem::path nativeSource = temporary.root() / "helper.c";
#if defined(_WIN32)
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.obj";
#else
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.o";
#endif
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(nativeSource, "int helper(void) { return 42; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(nativeObject, "previous object"),
         "native C compiler failure fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cSources = {nativeSource};
  inputs.cStandard = lang::driver::CStandard::C17;
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a",
           .vendorInclude = include},
          generated, temporary.root() / "program", "unused-c++",
          std::move(inputs), false, false, false, std::nullopt,
          (temporary.root() / "missing-c-compiler").string()));
  expect(result.status ==
                 lang::driver::ExecutableBuildStatus::NativeCCompilerFailure &&
             result.cCompilations.size() == 1 &&
             result.cCompilations.front().source == nativeSource &&
             result.cCompilations.front().object == nativeObject &&
             result.cCompilations.front().process.exitCode != 0 &&
             result.nativeCommand.empty() && result.generatedSourceRetained &&
             std::filesystem::is_regular_file(generated) &&
             readFile(nativeObject) == "previous object" &&
             !std::filesystem::exists(temporary.root() / "program"),
         "a failed C compiler invocation should stop before final linking, "
         "retain generated C++ for diagnosis, and preserve a prior object");
}

void testNativeCppCompilerFailure() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  const std::filesystem::path generated = temporary.root() / "generated.cpp";
  const std::filesystem::path nativeSource = temporary.root() / "helper.cpp";
#if defined(_WIN32)
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.obj";
#else
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.o";
#endif
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(nativeSource, "int helper() { return 42; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(nativeObject, "previous object"),
         "native C++ compiler failure fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cppSources = {nativeSource};
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a",
           .vendorInclude = include},
          generated, temporary.root() / "program",
          (temporary.root() / "missing-cpp-compiler").string(),
          std::move(inputs), false, false, false));
  expect(
      result.status ==
              lang::driver::ExecutableBuildStatus::NativeCppCompilerFailure &&
          result.cppCompilations.size() == 1 &&
          result.cppCompilations.front().source == nativeSource &&
          result.cppCompilations.front().object == nativeObject &&
          result.cppCompilations.front().process.exitCode != 0 &&
          result.nativeCommand.empty() && result.generatedSourceRetained &&
          std::filesystem::is_regular_file(generated) &&
          readFile(nativeObject) == "previous object" &&
          !std::filesystem::exists(temporary.root() / "program"),
      "a failed C++ source compiler invocation should stop before final "
      "linking, "
      "retain generated C++ for diagnosis, and preserve a prior object");
}

void testOrderedExecutableBuildCommand() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(temporary.root() / "provider.c",
                       "int native_provider(void) { return 42; }\n") &&
             writeFile(temporary.root() / "provider.a", ""),
         "ordered executable-build fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cSources = {temporary.root() / "provider.c"};
  inputs.cStandard = lang::driver::CStandard::C17;
  inputs.libraryFiles = {"ignored-category-file.a"};
  inputs.libraries = {"ignored-category-library"};
  inputs.orderedLinkOperands = {
      {lang::driver::NativeLinkOperandKind::Library, "target"},
      {lang::driver::NativeLinkOperandKind::File,
       (temporary.root() / "provider.a").string()},
  };
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a",
           .vendorInclude = include},
          temporary.root() / "generated.cpp", temporary.root() / "program",
          (temporary.root() / "missing-native-compiler").string(),
          std::move(inputs), false, false, false, std::nullopt,
          lang::driver::discoverCCompiler()));
  const std::vector<std::string> &command = result.nativeCommand;
#if defined(_WIN32)
  const std::filesystem::path expectedNativeObject =
      temporary.root() / "generated.native-0-provider.obj";
#else
  const std::filesystem::path expectedNativeObject =
      temporary.root() / "generated.native-0-provider.o";
#endif
  const auto nativeObject =
      std::find(command.begin(), command.end(), expectedNativeObject.string());
  const auto runtime =
      std::find(command.begin(), command.end(),
                (temporary.root() / "libgti_runtime.a").string());
  const auto target = std::find(command.begin(), command.end(), "-ltarget");
  const auto provider = std::find(command.begin(), command.end(),
                                  (temporary.root() / "provider.a").string());
  expect(result.status ==
                 lang::driver::ExecutableBuildStatus::NativeCompilerFailure &&
             runtime != command.end() && target != command.end() &&
             provider != command.end() && nativeObject != command.end() &&
             result.cCompilations.size() == 1 && nativeObject < runtime &&
             runtime < target && target < provider &&
             std::find(command.begin(), command.end(),
                       "ignored-category-file.a") == command.end() &&
             std::find(command.begin(), command.end(),
                       "-lignored-category-library") == command.end(),
         "executable builds should place compiled C objects before the runtime "
         "and authoritative ordered project operands without duplicating split "
         "metadata categories");
}

void testManagedOutputSafety() {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path managedRoot = project / "build/gti";
  const std::filesystem::path generated =
      managedRoot / "dev/host/intermediate/program.gti.cpp";
  const std::filesystem::path externalOutput =
      temporary.root() / "external/program";
  std::filesystem::create_directory(project);

  const lang::driver::ExecutableBuildResult outside =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              project / "missing.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23),
          {}, managedRoot / "intermediate/program.gti.cpp", externalOutput,
          "unused-c++", {}, false, true, false,
          lang::driver::ManagedOutputPolicy{.trustedRoot = project,
                                            .outputRoot = managedRoot}));
  expect(outside.status ==
                 lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
             outside.driverDiagnostic &&
             outside.driverDiagnostic->find(
                 "outside the managed project output root") !=
                 std::string::npos &&
             !std::filesystem::exists(externalOutput.parent_path()),
         "managed builds should reject output paths outside their declared "
         "project subtree before compiling or mutating the filesystem");

  const std::filesystem::path externalGenerated =
      temporary.root() / "external-generated.cpp";
  expect(writeFile(externalGenerated, "preserve me\n"),
         "the external generated-source fixture should be writable");
  std::filesystem::create_directories(generated.parent_path());
  std::error_code symlinkError;
  std::filesystem::create_symlink(externalGenerated, generated, symlinkError);
  if (!symlinkError) {
    const lang::driver::ExecutableBuildResult symlinkedGenerated =
        lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
            lang::driver::CompilationRequest(
                project / "missing.gti",
                lang::standardLibraryLayout(temporary.root()),
                lang::TargetInfo::host(), lang::OptimizationLevel::O0,
                lang::CppStandard::Cpp23),
            {}, generated, managedRoot / "dev/host/program", "unused-c++", {},
            true, true, false,
            lang::driver::ManagedOutputPolicy{.trustedRoot = project,
                                              .outputRoot = managedRoot}));
    expect(
        symlinkedGenerated.status ==
                lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
            symlinkedGenerated.driverDiagnostic &&
            symlinkedGenerated.driverDiagnostic->find(
                "symbolic-link managed output artifact") != std::string::npos &&
            readFile(externalGenerated) == "preserve me\n",
        "managed builds should reject a symbolic-link generated-source "
        "leaf before loading source or overwriting its external target");
  }

  const std::filesystem::path externalProject =
      temporary.root() / "external-project";
  const std::filesystem::path linkedProject = temporary.root() / "project-link";
  std::filesystem::create_directory(externalProject);
  symlinkError.clear();
  std::filesystem::create_directory_symlink(externalProject, linkedProject,
                                            symlinkError);
  if (!symlinkError) {
    const std::filesystem::path linkedManagedRoot = linkedProject / "build/gti";
    const lang::driver::ExecutableBuildResult symlinkedTrustRoot =
        lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
            lang::driver::CompilationRequest(
                linkedProject / "missing.gti",
                lang::standardLibraryLayout(temporary.root()),
                lang::TargetInfo::host(), lang::OptimizationLevel::O0,
                lang::CppStandard::Cpp23),
            {}, linkedManagedRoot / "intermediate/program.gti.cpp",
            linkedManagedRoot / "program", "unused-c++", {}, false, true, false,
            lang::driver::ManagedOutputPolicy{.trustedRoot = linkedProject,
                                              .outputRoot =
                                                  linkedManagedRoot}));
    expect(
        symlinkedTrustRoot.status ==
                lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
            symlinkedTrustRoot.driverDiagnostic &&
            symlinkedTrustRoot.driverDiagnostic->find(
                "symbolic-link managed output trust root") !=
                std::string::npos &&
            !std::filesystem::exists(externalProject / "build"),
        "managed builds should reject a symbolic-link trust root before "
        "creating output through its external target");
  }
}

void testWholeProgramBuildCache(const std::filesystem::path &testExecutable) {
  ScopedEnvironment clearCpath("CPATH");
  ScopedEnvironment clearCIncludePath("C_INCLUDE_PATH");
  ScopedEnvironment clearCxxIncludePath("CPLUS_INCLUDE_PATH");
  ScopedEnvironment clearObjcIncludePath("OBJC_INCLUDE_PATH");
  ScopedEnvironment clearLibraryPath("LIBRARY_PATH");
  ScopedEnvironment clearCompilerPath("COMPILER_PATH");
  ScopedEnvironment clearGccExecPrefix("GCC_EXEC_PREFIX");
  ScopedEnvironment clearSdkRoot("SDKROOT");
  ScopedEnvironment clearDeveloperDirectory("DEVELOPER_DIR");
  ScopedEnvironment clearLdLibraryPath("LD_LIBRARY_PATH");
  ScopedEnvironment clearDyldLibraryPath("DYLD_LIBRARY_PATH");
  ScopedEnvironment clearMsvcIncludePath("INCLUDE");
  ScopedEnvironment clearMsvcLibraryPath("LIB");
  expect(std::getenv("COMPILER_PATH") == nullptr &&
             std::getenv("GCC_EXEC_PREFIX") == nullptr,
         "the cache fixture should remove compiler search-path variables "
         "rather than passing empty overrides to GCC");

  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path managedRoot = project / "build/gti";
  const std::filesystem::path generated =
      managedRoot / "dev/host/intermediate/app.gti.cpp";
  const std::filesystem::path output = managedRoot / "dev/host/app";
  const std::filesystem::path source = project / "main.gti";
  const std::filesystem::path helper = project / "helper.gti";
  const std::filesystem::path leaf = project / "leaf.gti";
  std::filesystem::create_directories(project);
  expect(writeFile(source, "#include \"helper.gti\"\n"
                           "int main() { return cached_value(); }\n") &&
             writeFile(helper,
                       "#include \"leaf.gti\"\n"
                       "int cached_value() { return leaf_value(); }\n") &&
             writeFile(leaf, "int leaf_value() { return 0; }\n"),
         "whole-program cache source fixtures should be writable");

  const std::string executablePath = testExecutable.string();
  const lang::driver::ToolchainLayout discoveredToolchain =
      lang::driver::discoverToolchainLayout(executablePath.c_str());
  const std::filesystem::path localRuntimeInclude =
      temporary.root() / "toolchain/include";
  const std::filesystem::path localRuntimeLibrary =
      temporary.root() / "toolchain/libgti_runtime.a";
  std::filesystem::create_directories(localRuntimeInclude / "gti");
  std::error_code copyError;
  for (const std::string_view header :
       {"runtime.hpp", "runtime.h", "c_abi.h", "runtime_failure.h",
        "runtime_thread.h", "runtime_thread.hpp"}) {
    std::filesystem::copy_file(discoveredToolchain.runtimeInclude / "gti" /
                                   header,
                               localRuntimeInclude / "gti" / header, copyError);
    expect(!copyError, "runtime cache fixture headers should be copyable");
    copyError.clear();
  }
  std::filesystem::copy_file(discoveredToolchain.runtimeLibrary,
                             localRuntimeLibrary, copyError);
  expect(!copyError, "the runtime cache fixture archive should be copyable");
  const lang::driver::ToolchainLayout toolchain{
      .standardLibrary = discoveredToolchain.standardLibrary,
      .runtimeInclude = localRuntimeInclude,
      .runtimeLibrary = localRuntimeLibrary,
      .vendorInclude = discoveredToolchain.vendorInclude,
  };
  const lang::driver::ManagedOutputPolicy managed{
      .trustedRoot = project,
      .outputRoot = managedRoot,
  };
  const lang::driver::BuildCachePolicy cache{
      .root = managedRoot / "cache",
      .sourceRoot = project,
      .compilerIdentity = "driver-cache-test-v1",
      .projectModelIdentity = "driver-project-model-v1",
  };
  std::string nativeCompiler = "c++";
  if (!activeSanitizerArguments().empty()) {
    // Keep sanitizer runtime linkage in the compiler identity rather than in
    // NativeInputs: opaque user arguments correctly bypass cache lookup, while
    // sanitizer CI still needs to exercise the cache implementation itself.
    std::filesystem::path wrapper = temporary.root() / "native-cxx-wrapper";
#if defined(_WIN32)
    wrapper += ".exe";
#endif
    std::error_code wrapperError;
    std::filesystem::create_symlink(testExecutable, wrapper, wrapperError);
    if (wrapperError) {
      wrapperError.clear();
      std::filesystem::copy_file(testExecutable, wrapper, wrapperError);
    }
    expect(!wrapperError,
           "the sanitizer-aware native compiler wrapper should be creatable");
    if (!wrapperError) {
      nativeCompiler = wrapper.string();
    }
  }
  const auto build = [&](lang::OptimizationLevel optimization,
                         lang::driver::NativeInputs nativeInputs = {},
                         bool keepCpp = false,
                         lang::CppStandard standard = lang::CppStandard::Cpp23,
                         lang::TargetInfo target = lang::TargetInfo::host()) {
    return lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
        lang::driver::CompilationRequest(source, toolchain.standardLibrary,
                                         std::move(target), optimization,
                                         standard),
        toolchain, generated, output, nativeCompiler, std::move(nativeInputs),
        keepCpp, true, false, managed, std::nullopt, cache));
  };

  const lang::driver::ExecutableBuildResult first =
      build(lang::OptimizationLevel::O0);
  if (!first.succeeded()) {
    if (first.driverDiagnostic) {
      std::cerr << "whole-program cache first-build diagnostic: "
                << *first.driverDiagnostic << '\n';
    }
    if (first.nativeProcess) {
      std::cerr << "whole-program cache first-build native output:\n"
                << first.nativeProcess->output;
    }
    for (const lang::Diagnostic &diagnostic : first.compilation.diagnostics) {
      std::cerr << "whole-program cache first-build compiler diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  if (first.cache.status != lang::driver::BuildCacheStatus::Miss) {
    std::cerr << "whole-program cache first-build status: "
              << static_cast<int>(first.cache.status) << '\n';
    if (first.cache.detail) {
      std::cerr << "whole-program cache first-build detail: "
                << *first.cache.detail << '\n';
    }
  }
  if (first.cache.warning) {
    std::cerr << "whole-program cache first-build warning: "
              << *first.cache.warning << '\n';
  }
  expect(first.succeeded() &&
             first.cache.status == lang::driver::BuildCacheStatus::Miss &&
             first.nativeProcess && first.nativeProcess->succeeded() &&
             !first.cache.key.empty() &&
             std::filesystem::is_regular_file(first.cache.entry / "metadata") &&
             std::filesystem::is_regular_file(first.cache.entry /
                                              "generated.cpp") &&
             std::filesystem::is_regular_file(first.cache.entry / "executable"),
         "the first project build should compile natively and atomically "
         "publish a complete content-addressed cache entry");
  const std::string firstKey = first.cache.key;
  const std::filesystem::path firstEntry = first.cache.entry;
  expect(
      firstEntry.parent_path() == cache.root / "v2" &&
          readFile(firstEntry / "metadata").starts_with("gti-build-cache-v2\n"),
      "the current one-root driver request should publish only the v2 "
      "ordered-prelude cache schema");

  std::error_code removeError;
  std::filesystem::remove(output, removeError);
  const lang::driver::ExecutableBuildResult hit =
      build(lang::OptimizationLevel::O0);
  expect(hit.succeeded() &&
             hit.cache.status == lang::driver::BuildCacheStatus::Hit &&
             hit.cache.key == firstKey && !hit.nativeProcess &&
             hit.cCompilations.empty() && hit.cppCompilations.empty() &&
             std::filesystem::is_regular_file(output),
         "an unchanged build should verify and restore the cached executable "
         "without invoking any native compilation");

  expect(writeFile(leaf, "int leaf_value() { return 1; }\n"),
         "the transitive cache invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult transitiveChange =
      build(lang::OptimizationLevel::O0);
  expect(transitiveChange.succeeded() &&
             transitiveChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             transitiveChange.cache.key != firstKey &&
             transitiveChange.nativeProcess,
         "changing a transitive GTI include should produce a different cache "
         "identity and invoke native compilation");

  expect(writeFile(leaf, "int leaf_value() { return 0; }\n"),
         "the original transitive source should be restorable");
  const lang::driver::ExecutableBuildResult restoredSource =
      build(lang::OptimizationLevel::O0);
  expect(restoredSource.succeeded() &&
             restoredSource.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             restoredSource.cache.key == firstKey,
         "cache identity should depend on source content rather than file "
         "timestamps");

  const lang::driver::ExecutableBuildResult optimizationChange =
      build(lang::OptimizationLevel::O1);
  expect(optimizationChange.succeeded() &&
             optimizationChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             optimizationChange.cache.key != firstKey,
         "optimization policy should participate in whole-program cache "
         "identity");

  const lang::driver::ExecutableBuildResult standardChange =
      build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp20);
  expect(standardChange.succeeded() &&
             standardChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             standardChange.cache.key != firstKey,
         "the selected backend C++ standard should participate in cache "
         "identity");

  lang::TargetInfo concurrentTarget = lang::TargetInfo::host();
  concurrentTarget.executionProfile = lang::ExecutionProfile::Concurrent;
  const lang::driver::ExecutableBuildResult executionProfileChange =
      build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
            std::move(concurrentTarget));
  expect(executionProfileChange.succeeded() &&
             executionProfileChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             executionProfileChange.cache.key != firstKey,
         "the selected GTI execution profile should participate in cache "
         "identity");

  const std::filesystem::path runtimeHeader =
      localRuntimeInclude / "gti/runtime.hpp";
  const std::string originalRuntimeHeader = readFile(runtimeHeader);
  expect(writeFile(runtimeHeader,
                   originalRuntimeHeader + "\n// cache identity change\n"),
         "the runtime invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult runtimeChange =
      build(lang::OptimizationLevel::O0);
  expect(runtimeChange.succeeded() &&
             runtimeChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             runtimeChange.cache.key != firstKey,
         "runtime header content should participate in cache identity");
  expect(writeFile(runtimeHeader, originalRuntimeHeader),
         "the original runtime header should be restorable");

  const std::filesystem::path forcedHeader = project / "forced-cache.hpp";
  expect(writeFile(forcedHeader, "#define GTI_FORCED_CACHE_VALUE 1\n"),
         "the forced native header fixture should be writable");
  const auto buildWithForcedHeader = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.compilerArguments = {"-include", forcedHeader.string()};
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult forcedHeaderBuild =
      buildWithForcedHeader();
  expect(forcedHeaderBuild.succeeded() &&
             forcedHeaderBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             forcedHeaderBuild.cache.detail &&
             forcedHeaderBuild.cache.detail->find(
                 "opaque native argument vectors") != std::string::npos &&
             forcedHeaderBuild.nativeProcess,
         "opaque compiler arguments should bypass cache lookup because they "
         "can introduce dependencies such as forced native headers");
  expect(writeFile(forcedHeader, "#define GTI_FORCED_CACHE_VALUE 2\n"),
         "the forced native header should remain mutable between builds");
  const lang::driver::ExecutableBuildResult forcedHeaderChange =
      buildWithForcedHeader();
  expect(forcedHeaderChange.succeeded() &&
             forcedHeaderChange.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             forcedHeaderChange.nativeProcess,
         "changing a header introduced through an opaque compiler argument "
         "should force native compilation rather than restore a stale entry");

  const auto buildWithExactLinkFile = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryFiles.push_back(localRuntimeLibrary);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult exactNativeLinkBuild =
      buildWithExactLinkFile();
  expect(exactNativeLinkBuild.succeeded() &&
             exactNativeLinkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             exactNativeLinkBuild.cache.key != firstKey &&
             exactNativeLinkBuild.nativeProcess,
         "a content-complete exact link archive should join cache identity "
         "with its own key instead of bypassing");
  const lang::driver::ExecutableBuildResult exactNativeLinkHit =
      buildWithExactLinkFile();
  expect(exactNativeLinkHit.succeeded() &&
             exactNativeLinkHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             exactNativeLinkHit.cache.key == exactNativeLinkBuild.cache.key &&
             !exactNativeLinkHit.nativeProcess,
         "an unchanged exact link archive should verify and restore the "
         "cached executable");

  const std::filesystem::path linkerScript = project / "script.a";
  expect(writeFile(linkerScript, "INPUT(missing.o)\n"),
         "the linker-script link-input fixture should be writable");
  lang::driver::NativeInputs scriptLinkInput;
  scriptLinkInput.libraryFiles.push_back(linkerScript);
  const lang::driver::ExecutableBuildResult scriptLinkBuild =
      build(lang::OptimizationLevel::O0, std::move(scriptLinkInput));
  expect(scriptLinkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             scriptLinkBuild.cache.detail &&
             scriptLinkBuild.cache.detail->find(
                 "not a content-complete archive or object") !=
                 std::string::npos,
         "a link input that is not a content-complete archive or object "
         "should bypass the whole-program cache with its reason");

  const std::filesystem::path nativeSearchDirectory = project / "native-search";
  std::filesystem::create_directories(nativeSearchDirectory);
  const auto buildWithIncludeDirectory = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.includeDirectories.push_back(nativeSearchDirectory);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryBuild =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryBuild.succeeded() &&
             nativeIncludeDirectoryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeIncludeDirectoryBuild.cache.key != firstKey &&
             nativeIncludeDirectoryBuild.nativeProcess,
         "declared native include directories should join cache identity "
         "through full-tree content instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryHit =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryHit.succeeded() &&
             nativeIncludeDirectoryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeIncludeDirectoryHit.cache.key ==
                 nativeIncludeDirectoryBuild.cache.key,
         "an unchanged declared include tree should verify and restore the "
         "cached executable");
  expect(writeFile(nativeSearchDirectory / "added.h", "#define ADDED 1\n"),
         "the include-tree invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryChange =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryChange.succeeded() &&
             nativeIncludeDirectoryChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeIncludeDirectoryChange.cache.key !=
                 nativeIncludeDirectoryBuild.cache.key,
         "adding any file below a declared include directory should change "
         "cache identity so header shadowing can never restore a stale "
         "executable");

  const std::filesystem::path nativeLibrarySearchDirectory =
      project / "native-library-search";
  std::filesystem::create_directories(nativeLibrarySearchDirectory);
  const auto buildWithLibraryDirectory = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryDirectories.push_back(nativeLibrarySearchDirectory);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryBuild =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryBuild.succeeded() &&
             nativeLibraryDirectoryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeLibraryDirectoryBuild.cache.key != firstKey &&
             nativeLibraryDirectoryBuild.nativeProcess,
         "declared native library search directories should join cache "
         "identity through full-tree content instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryHit =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryHit.succeeded() &&
             nativeLibraryDirectoryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeLibraryDirectoryHit.cache.key ==
                 nativeLibraryDirectoryBuild.cache.key,
         "an unchanged declared library search tree should verify and restore "
         "the cached executable");
  expect(writeFile(nativeLibrarySearchDirectory / "added.txt", "added\n"),
         "the library-tree invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryChange =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryChange.succeeded() &&
             nativeLibraryDirectoryChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeLibraryDirectoryChange.cache.key !=
                 nativeLibraryDirectoryBuild.cache.key,
         "adding any file below a declared library search directory should "
         "change cache identity, so a file dropped there can never shadow an "
         "implicitly resolved library behind a stale entry");
  std::filesystem::remove(nativeLibrarySearchDirectory / "added.txt");

  lang::driver::NativeInputs namedLibraryInput;
  namedLibraryInput.libraries.emplace_back("m");
  const lang::driver::ExecutableBuildResult namedLibraryBuild =
      build(lang::OptimizationLevel::O0, std::move(namedLibraryInput));
  expect(namedLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             namedLibraryBuild.cache.detail &&
             namedLibraryBuild.cache.detail->find(
                 "does not resolve inside the declared native library search "
                 "directories") != std::string::npos,
         "a named library with no declared search directory holding it would "
         "resolve through the linker's implicit directories and must bypass");

  // A `-l` name that resolves to exactly one content-complete archive inside
  // a declared search directory participates with that file's content.
  const std::filesystem::path resolvedArchive =
      nativeLibrarySearchDirectory / "libgtiruntimealias.a";
  copyError.clear();
  std::filesystem::copy_file(localRuntimeLibrary, resolvedArchive, copyError);
  expect(!copyError, "the resolvable library fixture should be copyable");
  const auto buildWithResolvedLibrary = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryDirectories.push_back(nativeLibrarySearchDirectory);
    inputs.libraries.emplace_back("gtiruntimealias");
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult resolvedLibraryBuild =
      buildWithResolvedLibrary();
  expect(resolvedLibraryBuild.succeeded() &&
             resolvedLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             resolvedLibraryBuild.nativeProcess,
         "a named library resolving to one content-complete archive in a "
         "declared search directory should join cache identity");
  const lang::driver::ExecutableBuildResult resolvedLibraryHit =
      buildWithResolvedLibrary();
  expect(resolvedLibraryHit.succeeded() &&
             resolvedLibraryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             resolvedLibraryHit.cache.key == resolvedLibraryBuild.cache.key &&
             !resolvedLibraryHit.nativeProcess,
         "an unchanged resolved library should verify and restore the cached "
         "executable");

  // Two modeled candidates in the same directory: which one the linker would
  // prefer is a guess, so resolution must refuse.
  const std::filesystem::path ambiguousCandidate =
      nativeLibrarySearchDirectory /
#if defined(_WIN32)
      "libgtiruntimealias.dll.a";
#elif defined(__APPLE__)
      "libgtiruntimealias.dylib";
#else
      "libgtiruntimealias.so";
#endif
  expect(writeFile(ambiguousCandidate, "not a real dynamic library\n"),
         "the ambiguous library fixture should be writable");
  const lang::driver::ExecutableBuildResult ambiguousLibraryBuild =
      buildWithResolvedLibrary();
  expect(ambiguousLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             ambiguousLibraryBuild.cache.detail &&
             ambiguousLibraryBuild.cache.detail->find("resolves ambiguously") !=
                 std::string::npos,
         "two modeled candidates in one search directory should bypass rather "
         "than guess the linker's preference");
  std::filesystem::remove(ambiguousCandidate);

  // An unambiguous resolution whose bytes do not determine its contribution
  // to the link still bypasses.
  std::filesystem::remove(resolvedArchive);
  expect(writeFile(resolvedArchive, "INPUT(missing.o)\n"),
         "the content-incomplete resolved library fixture should be writable");
  const lang::driver::ExecutableBuildResult incompleteLibraryBuild =
      buildWithResolvedLibrary();
  expect(incompleteLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             incompleteLibraryBuild.cache.detail &&
             incompleteLibraryBuild.cache.detail->find(
                 "not a content-complete archive or object") !=
                 std::string::npos,
         "a resolved library that is not content-complete should bypass with "
         "the link-input reason");
  std::filesystem::remove(resolvedArchive);

  lang::driver::NativeInputs frameworkInput;
  frameworkInput.frameworks.emplace_back("CoreFoundation");
  const lang::driver::ExecutableBuildResult frameworkBuild =
      build(lang::OptimizationLevel::O0, std::move(frameworkInput));
  expect(frameworkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             frameworkBuild.cache.detail &&
             frameworkBuild.cache.detail->find(
                 "name-resolved frameworks require framework search "
                 "resolution") != std::string::npos,
         "frameworks resolve through undeclared search roots to a bundle and "
         "must keep bypassing");

  {
    ScopedEnvironment injectedIncludePath("CPATH",
                                          nativeSearchDirectory.string());
    const lang::driver::ExecutableBuildResult nativeEnvironmentBuild =
        build(lang::OptimizationLevel::O0);
    expect(nativeEnvironmentBuild.succeeded() &&
               nativeEnvironmentBuild.cache.status ==
                   lang::driver::BuildCacheStatus::Bypassed &&
               nativeEnvironmentBuild.cache.detail &&
               nativeEnvironmentBuild.cache.detail->find(
                   "environment search paths") != std::string::npos &&
               nativeEnvironmentBuild.nativeProcess,
           "dependency-injecting native environment search paths should "
           "bypass cache lookup until their compiler/linker-discovered "
           "transitive inputs can be modeled");
  }

  const std::filesystem::path nativeHeader = project / "cache-native.hpp";
  const std::filesystem::path nativeSource = project / "cache-native.cpp";
  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 1\n") &&
             writeFile(nativeSource,
                       "#include \"cache-native.hpp\"\n"
                       "extern \"C\" int gti_cache_native_value() { "
                       "return GTI_CACHE_NATIVE_VALUE; }\n"),
         "native source cache fixtures should be writable");
  const auto buildWithNativeSource = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.cppSources.push_back(nativeSource);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeSourceBuild =
      buildWithNativeSource();
  expect(nativeSourceBuild.succeeded() &&
             nativeSourceBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeSourceBuild.cache.key != firstKey &&
             nativeSourceBuild.cppCompilations.size() == 1,
         "a declared native source should join cache identity through "
         "compiler dependency discovery instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeSourceHit =
      buildWithNativeSource();
  expect(nativeSourceHit.succeeded() &&
             nativeSourceHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeSourceHit.cache.key == nativeSourceBuild.cache.key &&
             nativeSourceHit.cppCompilations.empty() &&
             !nativeSourceHit.nativeProcess,
         "an unchanged declared native source should verify and restore the "
         "cached executable without native compilation");
  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 2\n"),
         "the native transitive-header fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeHeaderChange =
      buildWithNativeSource();
  expect(nativeHeaderChange.succeeded() &&
             nativeHeaderChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeHeaderChange.cache.key != nativeSourceBuild.cache.key &&
             nativeHeaderChange.cppCompilations.size() == 1,
         "changing a compiler-discovered native header must change cache "
         "identity and force native recompilation");

  expect(
      writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE (__TIME__[0])\n"),
      "the time-sensitive native header fixture should be writable");
  const lang::driver::ExecutableBuildResult timeSensitiveNative =
      buildWithNativeSource();
  expect(timeSensitiveNative.succeeded() &&
             timeSensitiveNative.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             timeSensitiveNative.cache.detail &&
             timeSensitiveNative.cache.detail->find(
                 "time-and-date preprocessor macros") != std::string::npos,
         "a discovered native input using time-and-date preprocessor macros "
         "should bypass the cache under the deterministic preprocessing "
         "policy");
  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 1\n"),
         "the original native header should be restorable");

  expect(writeFile(firstEntry / "executable", "corrupt"),
         "the cache corruption fixture should be writable");
  const lang::driver::ExecutableBuildResult recovered =
      build(lang::OptimizationLevel::O0);
  expect(recovered.succeeded() &&
             recovered.cache.status ==
                 lang::driver::BuildCacheStatus::RecoveredCorruption &&
             recovered.cache.warning && recovered.nativeProcess &&
             recovered.nativeProcess->succeeded(),
         "a digest mismatch should never be used and should be replaced only "
         "after a successful native rebuild");

  const lang::driver::ExecutableBuildResult retainedCpp =
      build(lang::OptimizationLevel::O0, {}, true);
  expect(retainedCpp.succeeded() &&
             retainedCpp.cache.status == lang::driver::BuildCacheStatus::Hit &&
             retainedCpp.generatedSourceRetained &&
             std::filesystem::is_regular_file(generated) &&
             readFile(generated) == retainedCpp.compilation.artifact->contents,
         "keep-cpp should restore verified generated source on a cache hit "
         "without changing cache identity");

  const std::string publishedExecutable = readFile(output);
  std::filesystem::remove_all(firstEntry, removeError);
  expect(!removeError && readFile(output) == publishedExecutable,
         "deleting a cache entry should not damage the published executable");
  const std::filesystem::path legacyEntry = cache.root / "v1" / firstKey;
  std::filesystem::create_directories(legacyEntry);
  expect(writeFile(legacyEntry / "metadata", "gti-build-cache-v1\n") &&
             writeFile(legacyEntry / "generated.cpp", "stale") &&
             writeFile(legacyEntry / "executable", "stale"),
         "the legacy cache-schema fixture should be writable");
  const lang::driver::ExecutableBuildResult afterDeletion =
      build(lang::OptimizationLevel::O0);
  expect(afterDeletion.succeeded() &&
             afterDeletion.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             afterDeletion.nativeProcess &&
             afterDeletion.cache.entry.parent_path() == cache.root / "v2",
         "a deleted v2 cache entry should ignore a same-key v1 entry and "
         "degrade to a clean rebuild");

  const lang::driver::ExecutableBuildResult unmanagedCache =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              source, toolchain.standardLibrary, lang::TargetInfo::host(),
              lang::OptimizationLevel::O0, lang::CppStandard::Cpp23),
          toolchain, temporary.root() / "unmanaged.gti.cpp",
          temporary.root() / "unmanaged-program", nativeCompiler, {}, false,
          true, false, std::nullopt, std::nullopt, cache));
  expect(unmanagedCache.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             unmanagedCache.cache.detail &&
             unmanagedCache.cache.detail->find("managed project output") !=
                 std::string::npos,
         "a cache policy without a managed project output should bypass with "
         "its reason rather than publish unchecked paths");

  const lang::driver::ExecutableBuildResult unresolvableIdentity =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              source, toolchain.standardLibrary, lang::TargetInfo::host(),
              lang::OptimizationLevel::O0, lang::CppStandard::Cpp23),
          toolchain, generated, output,
          (temporary.root() / "missing-native-compiler").string(), {}, false,
          true, false, managed, std::nullopt, cache));
  expect(unresolvableIdentity.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             unresolvableIdentity.cache.detail &&
             unresolvableIdentity.cache.detail->find("identity unavailable") !=
                 std::string::npos,
         "an unresolvable native compiler identity should bypass the cache "
         "with its reason instead of guessing a key");
}

void testNativeDependencyDiscoveryPrimitives() {
  std::string errorMessage;
  const auto parsed = [&](std::string_view contents) {
    errorMessage.clear();
    return lang::driver::parseNativeDependencyFile(contents, errorMessage);
  };

  const auto simple = parsed("main.o: main.c include/support.h\n");
  expect(simple && simple->size() == 2 &&
             (*simple)[0] == std::filesystem::path("main.c") &&
             (*simple)[1] == std::filesystem::path("include/support.h"),
         "a plain dependency rule should list every prerequisite in order");

  const auto continued = parsed("main.o: main.c \\\n  a.h \\\r\n  b.h\n");
  expect(continued && continued->size() == 3 &&
             (*continued)[1] == std::filesystem::path("a.h") &&
             (*continued)[2] == std::filesystem::path("b.h"),
         "backslash line continuations should join one dependency rule");

  const auto escaped =
      parsed("out.o: with\\ space.h dollar$$sign.h back\\\\slash.h\n");
  expect(escaped && escaped->size() == 3 &&
             (*escaped)[0] == std::filesystem::path("with space.h") &&
             (*escaped)[1] == std::filesystem::path("dollar$sign.h") &&
             (*escaped)[2] == std::filesystem::path("back\\slash.h"),
         "escaped spaces, doubled dollars, and escaped backslashes should "
         "decode to literal path characters");

  const auto driveLetters = parsed("C:/build/out.obj: C:/src/main.c\n");
  expect(driveLetters && driveLetters->size() == 1 &&
             (*driveLetters)[0] == std::filesystem::path("C:/src/main.c"),
         "Windows drive colons should not terminate the dependency rule");

  const auto noDependencies = parsed("main.o:\n");
  expect(noDependencies && noDependencies->empty(),
         "a rule without prerequisites should parse to an empty list");

  expect(!parsed("no separator here\n") && !errorMessage.empty(),
         "a report without a rule separator should be rejected with a "
         "diagnostic");
  expect(!parsed("main.o: a.h\nphony.h:\n"),
         "an unexpected second rule should be rejected rather than guessed "
         "at");

  TemporaryDirectory temporary;
  const auto classify = [&](std::string_view name, std::string_view contents) {
    const std::filesystem::path path = temporary.root() / name;
    writeFile(path, contents);
    return lang::driver::classifyNativeLinkInput(path);
  };
  expect(classify("regular.a", "!<arch>\ncontents") ==
             lang::driver::NativeLinkInputClass::StaticArchive,
         "a regular ar archive should classify as content-complete");
  expect(classify("thin.a", "!<thin>\ncontents") ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a thin archive references external members and requires discovery");
  const std::string elfRelocatable{
      '\x7f', 'E', 'L', 'F', 2, 1, 1, 0, 0,
      0,      0,   0,   0,   0, 0, 0, 1, 0, /* ET_REL little-endian */
      0x3e,   0};
  expect(classify("object.o", elfRelocatable) ==
             lang::driver::NativeLinkInputClass::RelocatableObject,
         "an ELF relocatable object should classify as content-complete");
  const std::string elfShared{'\x7f', 'E', 'L', 'F', 2, 1, 1, 0, 0,
                              0,      0,   0,   0,   0, 0, 0, 3, 0, /* ET_DYN */
                              0x3e,   0};
  expect(classify("shared.so", elfShared) ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "an ELF shared library requires transitive dependency discovery");
  const std::string machOObject{
      '\xcf', '\xfa', '\xed', '\xfe', 12, 0, 0, 1,
      0,      0,      0,      0,      1,  0, 0, 0, /* MH_OBJECT */
      2,      0,      0,      0};
  expect(classify("object.mach.o", machOObject) ==
             lang::driver::NativeLinkInputClass::RelocatableObject,
         "a Mach-O relocatable object should classify as content-complete");
  const std::string machODylib{
      '\xcf', '\xfa', '\xed', '\xfe', 12, 0, 0, 1,
      0,      0,      0,      0,      6,  0, 0, 0, /* MH_DYLIB */
      2,      0,      0,      0};
  expect(classify("shared.dylib", machODylib) ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a Mach-O dynamic library requires transitive dependency discovery");
  expect(classify("script.a", "INPUT(other.o)\n") ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a text linker script must never classify as content-complete");
  expect(
      lang::driver::classifyNativeLinkInput(temporary.root() / "missing.a") ==
          lang::driver::NativeLinkInputClass::Unreadable,
      "a missing link input should classify as unreadable");

  expect(lang::driver::containsTimeSensitivePreprocessorUse(
             "const char *when = __DATE__;") &&
             lang::driver::containsTimeSensitivePreprocessorUse(
                 "#define STAMP __TIMESTAMP__") &&
             lang::driver::containsTimeSensitivePreprocessorUse(
                 "int t = __TIME__[0];") &&
             !lang::driver::containsTimeSensitivePreprocessorUse(
                 "int date = 0; // date and time words alone\n"),
         "the deterministic preprocessing policy should detect exactly the "
         "time-and-date macro spellings");
}

void testNativeLibraryNameResolution() {
  using lang::driver::NativeLibraryResolutionStatus;
  TemporaryDirectory temporary;
  const std::filesystem::path first = temporary.root() / "first";
  const std::filesystem::path second = temporary.root() / "second";
  std::filesystem::create_directories(first);
  std::filesystem::create_directories(second);
  const std::vector<std::filesystem::path> directories{first, second};

#if defined(_WIN32)
  const std::string dynamicName = "libsupport.dll.a";
#elif defined(__APPLE__)
  const std::string dynamicName = "libsupport.dylib";
#else
  const std::string dynamicName = "libsupport.so";
#endif

  const auto resolve = [&](std::string_view name) {
    return lang::driver::resolveNativeLibraryName(name, directories);
  };

  expect(resolve("support").status == NativeLibraryResolutionStatus::Unresolved,
         "a name with no candidate in any declared directory should resolve "
         "outside them and refuse to participate");

  writeFile(second / "libsupport.a", "!<arch>\ncontents");
  const lang::driver::NativeLibraryResolution resolved = resolve("support");
  expect(resolved.status == NativeLibraryResolutionStatus::Resolved &&
             resolved.file == second / "libsupport.a",
         "a single modeled candidate should resolve to that exact file, "
         "skipping earlier directories that hold none");

  writeFile(first / dynamicName, "dynamic");
  const lang::driver::NativeLibraryResolution shadowed = resolve("support");
  expect(shadowed.status == NativeLibraryResolutionStatus::Resolved &&
             shadowed.file == first / dynamicName,
         "the first declared directory holding a candidate should win, as it "
         "does on the link command line");

  writeFile(first / "libsupport.a", "!<arch>\ncontents");
  const lang::driver::NativeLibraryResolution ambiguous = resolve("support");
  expect(ambiguous.status == NativeLibraryResolutionStatus::Ambiguous &&
             ambiguous.directory == first,
         "two modeled candidates in one directory should refuse rather than "
         "guess the linker's preference");

  std::filesystem::remove(first / dynamicName);
  writeFile(first / "libsupport.la", "# libtool archive\n");
  expect(resolve("support").status == NativeLibraryResolutionStatus::Ambiguous,
         "a candidate spelling the driver does not model should make "
         "resolution ambiguous, never invisible");
  std::filesystem::remove(first / "libsupport.la");

  // A neighbouring name must not be mistaken for a candidate.
  writeFile(second / "libsupport_extra.a", "!<arch>\ncontents");
  expect(resolve("support_extra").status ==
                 NativeLibraryResolutionStatus::Resolved &&
             resolve("support").status ==
                 NativeLibraryResolutionStatus::Resolved,
         "a longer library name sharing a prefix should not disturb either "
         "resolution");

  expect(resolve(":libsupport.a").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling &&
             resolve("sub/support").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling &&
             resolve("").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling,
         "exact-name selections and path-bearing spellings mean different "
         "things to different linkers and must not be resolved");

  const std::vector<std::filesystem::path> missingDirectory{temporary.root() /
                                                            "absent"};
  expect(lang::driver::resolveNativeLibraryName("support", missingDirectory)
                 .status == NativeLibraryResolutionStatus::DirectoryUnreadable,
         "a search directory that cannot be enumerated must not be reported "
         "as holding no candidate");
}

void testBuildSourceMaterialIdentityProjection() {
  lang::driver::detail::BuildSourceMaterialProjection baseline;
  baseline.units = {
      {.id = 41,
       .role = lang::SourceUnitRole::Application,
       .logicalIdentity = "package:main.gti",
       .entry = true,
       .bytes = 20,
       .sha256 = std::string(64, '1')},
      {.id = 17,
       .role = lang::SourceUnitRole::Prelude,
       .logicalIdentity = "prelude:first",
       .prelude = true,
       .bytes = 10,
       .sha256 = std::string(64, '2')},
      {.id = 23,
       .role = lang::SourceUnitRole::Prelude,
       .logicalIdentity = "prelude:second",
       .prelude = true,
       .bytes = 11,
       .sha256 = std::string(64, '3')},
  };
  baseline.preludeRoots = {"prelude:first", "prelude:second"};
  baseline.dependencyEdges = {
      "package:main.gti\nprelude:first\n1",
      "package:main.gti\nprelude:second\n1",
  };
  const std::string baselineIdentity =
      lang::driver::detail::buildSourceMaterialsSha256(baseline);

  lang::driver::detail::BuildSourceMaterialProjection relocated = baseline;
  relocated.units[0].id = 1;
  relocated.units[1].id = 2;
  relocated.units[2].id = 3;
  expect(lang::driver::detail::buildSourceMaterialsSha256(relocated) ==
             baselineIdentity,
         "source material identity should exclude ephemeral unit identifiers");

  lang::driver::detail::BuildSourceMaterialProjection contentChanged = baseline;
  contentChanged.units[0].sha256 = std::string(64, '4');
  expect(lang::driver::detail::buildSourceMaterialsSha256(contentChanged) !=
             baselineIdentity,
         "source material identity should change with consumed content");

  lang::driver::detail::BuildSourceMaterialProjection rootsReordered = baseline;
  std::reverse(rootsReordered.preludeRoots.begin(),
               rootsReordered.preludeRoots.end());
  expect(lang::driver::detail::buildSourceMaterialsSha256(rootsReordered) !=
             baselineIdentity,
         "source material identity should preserve prelude-root order");

  lang::driver::detail::BuildSourceMaterialProjection edgeChanged = baseline;
  edgeChanged.dependencyEdges.back() = "prelude:first\nprelude:second\n0";
  std::sort(edgeChanged.dependencyEdges.begin(),
            edgeChanged.dependencyEdges.end());
  expect(lang::driver::detail::buildSourceMaterialsSha256(edgeChanged) !=
             baselineIdentity,
         "source material identity should retain logical dependency edges");
}

void testBuildProvenance(const std::filesystem::path &testExecutable) {
  TemporaryDirectory temporary;
  const lang::driver::ToolchainLayout discovered =
      lang::driver::discoverToolchainLayout(testExecutable.string().c_str());
  const std::filesystem::path runtimeInclude = temporary.root() / "runtime";
  const std::filesystem::path runtimeLibrary =
      temporary.root() / discovered.runtimeLibrary.filename();
  std::error_code copyError;
  std::filesystem::copy(discovered.runtimeInclude, runtimeInclude,
                        std::filesystem::copy_options::recursive, copyError);
  if (!copyError) {
    std::filesystem::copy_file(discovered.runtimeLibrary, runtimeLibrary,
                               copyError);
  }
  if (copyError) {
    expect(false, "build-provenance runtime fixtures should be copyable: " +
                      copyError.message());
    return;
  }

  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path dependency = project / "dependency.gti";
  const std::filesystem::path entry = project / "main.gti";
  std::filesystem::create_directories(project);
  expect(writeFile(dependency, "int dependency_value() { return 1; }\n") &&
             writeFile(entry,
                       "#include \"dependency.gti\"\n"
                       "expected<int, int> calculated() { return 1; }\n"
                       "int main() {\n"
                       "  expected<int, int> value = calculated();\n"
                       "  if (!value) { return value.error(); }\n"
                       "  return dependency_value() + value.value() - 2;\n"
                       "}\n"),
         "build-provenance GTI sources should be writable");

  const lang::driver::ToolchainLayout toolchain{
      .standardLibrary = discovered.standardLibrary,
      .runtimeInclude = runtimeInclude,
      .runtimeLibrary = runtimeLibrary,
      .vendorInclude = discovered.vendorInclude,
  };
  lang::driver::NativeInputs nestedNativeInputs;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
  nestedNativeInputs.compilerArguments = {"-fsanitize=address,undefined",
                                          "-fno-omit-frame-pointer"};
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
  if (nestedNativeInputs.compilerArguments.empty()) {
    nestedNativeInputs.compilerArguments = {"-fsanitize=address,undefined",
                                            "-fno-omit-frame-pointer"};
  }
#endif
  const auto build = [&](std::string_view suffix,
                         const std::filesystem::path &buildEntry,
                         const lang::driver::ToolchainLayout &buildToolchain,
                         lang::CppStandard standard,
                         lang::ConfigurationFlags flags = {}) {
    const std::filesystem::path generated =
        temporary.root() / ("generated-" + std::string(suffix) + ".cpp");
    const std::filesystem::path output =
        temporary.root() / ("program-" + std::string(suffix));
    const std::filesystem::path report =
        temporary.root() / ("report-" + std::string(suffix) + ".json");
    return lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
        lang::driver::CompilationRequest(
            buildEntry, buildToolchain.standardLibrary,
            lang::TargetInfo::host(), lang::OptimizationLevel::O0, standard, {},
            std::move(flags)),
        buildToolchain, generated, output,
        lang::driver::discoverNativeCompiler(), nestedNativeInputs, false, true,
        false, std::nullopt, std::nullopt, std::nullopt,
        lang::driver::BuildReportRequest{
            .output = report,
            .producerExecutable = testExecutable,
        }));
  };

  const lang::driver::ExecutableBuildResult first =
      build("first", entry, toolchain, lang::CppStandard::Cpp23);
  expect(first.succeeded() && first.provenance.has_value(),
         "an opted-in executable build should capture structured provenance "
         "(status=" +
             std::to_string(static_cast<int>(first.status)) + ", detail=" +
             first.driverDiagnostic.value_or("none") + ", native output=" +
             (first.nativeProcess ? first.nativeProcess->output : "none") +
             ")");
  if (!first.provenance) {
    return;
  }
  const lang::driver::BuildProvenance &firstProvenance = *first.provenance;
  const bool hasDependency = std::any_of(
      firstProvenance.source.units.begin(), firstProvenance.source.units.end(),
      [&](const lang::driver::BuildSourceUnitProvenance &unit) {
        return unit.material.canonicalPath ==
                   std::filesystem::weakly_canonical(dependency) &&
               !unit.material.sha256.empty();
      });
  const bool hasRuntimeHeader = std::any_of(
      firstProvenance.runtime.headers.begin(),
      firstProvenance.runtime.headers.end(),
      [](const lang::driver::BuildRuntimeHeaderProvenance &header) {
        return header.kind == lang::driver::BuildRuntimeHeaderKind::Runtime &&
               !header.material.sha256.empty();
      });
  expect(hasDependency && hasRuntimeHeader &&
             firstProvenance.runtime.archive.canonicalPath ==
                 std::filesystem::weakly_canonical(runtimeLibrary) &&
             firstProvenance.producerExecutable.canonicalPath ==
                 std::filesystem::weakly_canonical(testExecutable) &&
             !firstProvenance.nativeCompilerExecutable.sha256.empty() &&
             std::find(firstProvenance.nativeCommand.begin(),
                       firstProvenance.nativeCommand.end(),
                       "-MD") != firstProvenance.nativeCommand.end() &&
             std::find(firstProvenance.nativeCommand.begin(),
                       firstProvenance.nativeCommand.end(),
                       "-MF") != firstProvenance.nativeCommand.end(),
         "build provenance should retain exact loaded source, used runtime, "
         "producer, native compiler, and depfile-bearing argv facts");
  expect((firstProvenance.compilerBuild.state ==
          lang::driver::CompilerSourceState::Unavailable) ==
             !firstProvenance.compilerBuild.commit.has_value(),
         "compiler build identity should claim a commit only with established "
         "clean/dirty state");

  const std::filesystem::path relocatedProject =
      temporary.root() / "relocated/project";
  const std::filesystem::path relocatedDependency =
      relocatedProject / dependency.filename();
  const std::filesystem::path relocatedEntry =
      relocatedProject / entry.filename();
  std::filesystem::create_directories(relocatedProject);
  copyError.clear();
  std::filesystem::copy_file(dependency, relocatedDependency, copyError);
  if (!copyError) {
    std::filesystem::copy_file(entry, relocatedEntry, copyError);
  }
  expect(!copyError, "relocated provenance sources should be copyable");
  const lang::driver::ExecutableBuildResult relocatedSource = build(
      "relocated-source", relocatedEntry, toolchain, lang::CppStandard::Cpp23);
  const bool relocatedPathRecorded =
      relocatedSource.provenance &&
      std::any_of(relocatedSource.provenance->source.units.begin(),
                  relocatedSource.provenance->source.units.end(),
                  [&](const lang::driver::BuildSourceUnitProvenance &unit) {
                    return unit.material.canonicalPath ==
                           std::filesystem::weakly_canonical(relocatedEntry);
                  });
  expect(relocatedSource.succeeded() && relocatedSource.provenance &&
             relocatedSource.provenance->source.materialsSha256 ==
                 firstProvenance.source.materialsSha256 &&
             relocatedPathRecorded,
         "moving an identical logical source package should preserve its "
         "aggregate while recording the new canonical paths");

  const std::filesystem::path relocatedRuntimeInclude =
      temporary.root() / "relocated-toolchain/include";
  const std::filesystem::path relocatedRuntimeLibrary =
      temporary.root() / "relocated-toolchain" / runtimeLibrary.filename();
  std::filesystem::create_directories(relocatedRuntimeLibrary.parent_path());
  copyError.clear();
  std::filesystem::copy(runtimeInclude, relocatedRuntimeInclude,
                        std::filesystem::copy_options::recursive, copyError);
  if (!copyError) {
    std::filesystem::copy_file(runtimeLibrary, relocatedRuntimeLibrary,
                               copyError);
  }
  expect(!copyError, "relocated runtime materials should be copyable");
  const lang::driver::ToolchainLayout relocatedToolchain{
      .standardLibrary = toolchain.standardLibrary,
      .runtimeInclude = relocatedRuntimeInclude,
      .runtimeLibrary = relocatedRuntimeLibrary,
      .vendorInclude = toolchain.vendorInclude,
  };
  const lang::driver::ExecutableBuildResult relocatedRuntime = build(
      "relocated-runtime", entry, relocatedToolchain, lang::CppStandard::Cpp23);
  expect(relocatedRuntime.succeeded() && relocatedRuntime.provenance &&
             relocatedRuntime.provenance->runtime.materialsSha256 ==
                 firstProvenance.runtime.materialsSha256 &&
             relocatedRuntime.provenance->runtime.archive.canonicalPath ==
                 std::filesystem::weakly_canonical(relocatedRuntimeLibrary),
         "moving identical runtime headers and archive should preserve their "
         "aggregate while recording new canonical paths");

  const lang::driver::ExecutableBuildResult separateCpp20 =
      build("cpp20-separate", entry, toolchain, lang::CppStandard::Cpp20);
  const std::filesystem::path sharedInclude =
      temporary.root() / "installed/include";
  std::filesystem::create_directories(sharedInclude);
  copyError.clear();
  std::filesystem::copy(runtimeInclude / "gti", sharedInclude / "gti",
                        std::filesystem::copy_options::recursive, copyError);
  if (!copyError) {
    std::filesystem::copy(discovered.vendorInclude / "nonstd",
                          sharedInclude / "nonstd",
                          std::filesystem::copy_options::recursive, copyError);
  }
  expect(!copyError,
         "a shared installed runtime/compatibility include root should be "
         "copyable");
  const lang::driver::ToolchainLayout sharedRootToolchain{
      .standardLibrary = toolchain.standardLibrary,
      .runtimeInclude = sharedInclude,
      .runtimeLibrary = runtimeLibrary,
      .vendorInclude = sharedInclude,
  };
  const lang::driver::ExecutableBuildResult sharedCpp20 = build(
      "cpp20-shared", entry, sharedRootToolchain, lang::CppStandard::Cpp20);
  const bool sharedCompatibility =
      sharedCpp20.provenance &&
      std::any_of(
          sharedCpp20.provenance->runtime.headers.begin(),
          sharedCpp20.provenance->runtime.headers.end(),
          [](const lang::driver::BuildRuntimeHeaderProvenance &header) {
            return header.kind ==
                       lang::driver::BuildRuntimeHeaderKind::Compatibility &&
                   header.logicalIdentity.starts_with("nonstd/");
          });
  const bool misclassifiedCompatibility =
      sharedCpp20.provenance &&
      std::any_of(sharedCpp20.provenance->runtime.headers.begin(),
                  sharedCpp20.provenance->runtime.headers.end(),
                  [](const lang::driver::BuildRuntimeHeaderProvenance &header) {
                    return header.kind ==
                               lang::driver::BuildRuntimeHeaderKind::Runtime &&
                           header.logicalIdentity.starts_with("nonstd/");
                  });
  const auto runtimeSummary =
      [](const lang::driver::ExecutableBuildResult &value) {
        std::string summary;
        if (!value.provenance) {
          return summary;
        }
        summary = value.provenance->runtime.materialsSha256;
        for (const lang::driver::BuildRuntimeHeaderProvenance &header :
             value.provenance->runtime.headers) {
          summary +=
              " [" +
              std::string(
                  lang::driver::buildRuntimeHeaderKindName(header.kind)) +
              ":" + header.logicalIdentity + ":" + header.material.sha256 + "]";
        }
        return summary;
      };
  expect(
      separateCpp20.succeeded() && separateCpp20.provenance &&
          sharedCpp20.succeeded() && sharedCpp20.provenance &&
          sharedCompatibility && !misclassifiedCompatibility &&
          sharedCpp20.provenance->runtime.materialsSha256 ==
              separateCpp20.provenance->runtime.materialsSha256,
      "a shared installed C++20 include root should classify nonstd as "
      "compatibility and preserve normalized runtime identity (separate "
      "status=" +
          std::to_string(static_cast<int>(separateCpp20.status)) +
          ", shared status=" +
          std::to_string(static_cast<int>(sharedCpp20.status)) +
          ", shared detail=" + sharedCpp20.driverDiagnostic.value_or("none") +
          ", separate materials=" + runtimeSummary(separateCpp20) +
          ", shared materials=" + runtimeSummary(sharedCpp20) + ")");

  expect(writeFile(dependency, "int dependency_value() { return 2; }\n"),
         "the included provenance source should be editable");
  const lang::driver::ExecutableBuildResult sourceChanged =
      build("source", entry, toolchain, lang::CppStandard::Cpp23);
  expect(sourceChanged.succeeded() && sourceChanged.provenance &&
             sourceChanged.provenance->source.materialsSha256 !=
                 firstProvenance.source.materialsSha256 &&
             sourceChanged.provenance->runtime.materialsSha256 ==
                 firstProvenance.runtime.materialsSha256,
         "changing a consumed included GTI source should alter only source "
         "material provenance");

  if (firstProvenance.runtime.headers.empty()) {
    return;
  }
  const std::filesystem::path runtimeHeader =
      firstProvenance.runtime.headers.front().material.canonicalPath;
  expect(writeFile(runtimeHeader,
                   readFile(runtimeHeader) + "\n// provenance change\n"),
         "the copied used runtime header should be editable");
  const lang::driver::ExecutableBuildResult runtimeChanged =
      build("runtime", entry, toolchain, lang::CppStandard::Cpp23);
  expect(runtimeChanged.succeeded() && runtimeChanged.provenance &&
             sourceChanged.provenance &&
             runtimeChanged.provenance->runtime.materialsSha256 !=
                 sourceChanged.provenance->runtime.materialsSha256,
         "changing a used runtime header should alter runtime provenance "
         "(status=" +
             std::to_string(static_cast<int>(runtimeChanged.status)) +
             ", detail=" + runtimeChanged.driverDiagnostic.value_or("none") +
             ")");
}

// The sidecar is the interchange format `gti verify` reads back, so writing
// and reading it must round-trip exactly and comparison must name the exact
// fact that moved.
void testBuildReportRecordRoundTrip() {
  lang::driver::BuildProvenance recorded;
  recorded.producerExecutable = {.canonicalPath = "/opt/gti/bin/gti",
                                 .bytes = 128,
                                 .sha256 = std::string(64, 'a')};
  recorded.compilerBuild = {.state = lang::driver::CompilerSourceState::Dirty,
                            .commit = std::string(40, 'b')};
  recorded.project = lang::driver::BuildProjectProvenance{
      .packageName = "sample",
      .packageVersion = "0.1.0",
      .targetName = "sample",
      .targetKind = "executable",
      .profileName = "dev",
      .targetTriple = "arm64-apple-macos",
      // A multi-line identity exercises string escaping in both directions.
      .modelIdentity = "gti-workspace-v1\npackage:sample@0.1.0"};
  recorded.source.entryRoot = 1;
  recorded.source.preludeRoots = {2};
  recorded.source.compilationOrder = {2, 1};
  recorded.source.materialsSha256 = std::string(64, 'c');
  recorded.source.units.push_back(
      {.id = 1,
       .role = lang::SourceUnitRole::Application,
       .material = {.canonicalPath = "/work/sample/src/main.gti",
                    .bytes = 25,
                    .sha256 = std::string(64, 'd')},
       .logicalIdentity = "package:sample@0.1.0:src/main.gti",
       .standardLibraryName = std::nullopt,
       .packageIdentity = "sample@0.1.0",
       .packageRelativePath = "src/main.gti",
       .entry = true,
       .prelude = false});
  recorded.source.units.push_back(
      {.id = 2,
       .role = lang::SourceUnitRole::Prelude,
       .material = {.canonicalPath = "/opt/gti/stdlib/prelude.gti",
                    .bytes = 64,
                    .sha256 = std::string(64, 'e')},
       .logicalIdentity = "prelude",
       .standardLibraryName = std::nullopt,
       .packageIdentity = std::nullopt,
       .packageRelativePath = std::nullopt,
       .entry = false,
       .prelude = true});
  recorded.source.dependencies.push_back(
      {.source = 1,
       .target = 2,
       .kind = lang::SourceDependencyKind::Prelude,
       .includeSpelling = "prelude.gti",
       .includeOccurrence = 0});
  recorded.nativeCompilerExecutable = {.canonicalPath = "/usr/bin/c++",
                                       .bytes = 512,
                                       .sha256 = std::string(64, 'f')};
  recorded.nativeCommand = {
      "c++",
      "-std=c++23",
      "-O0",
      "-MD",
      "-MF",
      "/tmp/gti-1-2-3-sample.build-report.d",
      "/work/sample/build/gti/sample.gti.cpp",
      "-o",
      "/work/sample/build/gti/.sample.gti-stage-1-2-3/sample"};
  recorded.runtime.materialsSha256 = std::string(64, '0');
  recorded.runtime.headers.push_back(
      {.kind = lang::driver::BuildRuntimeHeaderKind::Runtime,
       .logicalIdentity = "gti/runtime.h",
       .material = {.canonicalPath = "/opt/gti/include/gti/runtime.h",
                    .bytes = 900,
                    .sha256 = std::string(64, '1')}});
  recorded.runtime.headers.push_back(
      {.kind = lang::driver::BuildRuntimeHeaderKind::Compatibility,
       .logicalIdentity = "nonstd/print.h",
       .material = {.canonicalPath = "/opt/gti/include/nonstd/print.h",
                    .bytes = 300,
                    .sha256 = std::string(64, '2')}});
  recorded.runtime.archive = {.canonicalPath = "/opt/gti/lib/libgti_runtime.a",
                              .bytes = 4096,
                              .sha256 = std::string(64, '3')};
  recorded.artifact = {.canonicalPath = "/work/sample/build/gti/sample",
                       .bytes = 58008,
                       .sha256 = std::string(64, '4')};

  const std::string serialized =
      lang::driver::serializeBuildProvenanceJson(recorded);
  expect(serialized.find("\"target_kind\":\"executable\"") != std::string::npos,
         "a project record should serialize its resolved target kind");
  expect(serialized.find("gti-workspace-v1\\npackage:sample@0.1.0") !=
             std::string::npos,
         "a multi-line model identity should be escaped, not truncated");
  std::string parseError;
  const std::optional<lang::driver::BuildProvenance> parsed =
      lang::driver::parseBuildProvenanceJson(serialized, parseError);
  expect(parsed.has_value(),
         "a serialized build report should parse back: " + parseError);
  if (!parsed) {
    return;
  }
  expect(lang::driver::compareBuildProvenance(recorded, *parsed).empty(),
         "a round-tripped record should compare equal to its source");
  expect(lang::driver::serializeBuildProvenanceJson(*parsed) == serialized,
         "re-serializing a parsed record should reproduce the sidecar");

  // The driver owns the staged link output and the dependency-file operand,
  // and both carry a per-invocation identity. Comparing them literally would
  // report a difference for every verification.
  lang::driver::BuildProvenance restaged = *parsed;
  restaged.nativeCommand[5] = "/tmp/gti-9-9-9-sample.build-report.d";
  restaged.nativeCommand[8] =
      "/work/sample/build/gti/.sample.gti-stage-9-9-9/sample";
  expect(lang::driver::compareBuildProvenance(recorded, restaged).empty(),
         "per-invocation staging paths should not read as a divergence");

  lang::driver::BuildProvenance drifted = *parsed;
  drifted.source.units[0].material.sha256 = std::string(64, '9');
  drifted.source.units[0].material.bytes = 59;
  drifted.source.materialsSha256 = std::string(64, '8');
  drifted.runtime.headers.pop_back();
  drifted.nativeCommand[2] = "-O2";
  drifted.artifact.sha256 = std::string(64, '7');
  drifted.project->profileName = "release";
  // A record built for another target describes another build, even when
  // every source unit and command operand is unchanged.
  drifted.project->targetTriple = "x86_64-unknown-linux-gnu";
  const std::vector<lang::driver::BuildProvenanceDifference> differences =
      lang::driver::compareBuildProvenance(recorded, drifted);
  const auto named = [&](std::string_view subject, std::string_view field) {
    return std::any_of(
        differences.begin(), differences.end(),
        [&](const lang::driver::BuildProvenanceDifference &difference) {
          return difference.subject.find(subject) != std::string::npos &&
                 difference.field == field;
        });
  };
  expect(named("package:sample@0.1.0:src/main.gti", "content"),
         "a changed source unit should be named with its content identity");
  expect(named("project", "target triple"),
         "a record built for another target should diverge on its triple");
  expect(named("source materials", "aggregate sha256"),
         "the source aggregate should be compared");
  expect(named("nonstd/print.h", "presence"),
         "a runtime header the rebuild no longer consumes should be named");
  expect(named("native argv[2]", "operand"),
         "a changed native operand should be named by position");
  expect(named("built artifact", "content"),
         "the produced artifact's bytes should be compared");
  expect(named("project", "profile name"),
         "the recorded profile selection should be compared");
  const bool preludeUntouched = std::none_of(
      differences.begin(), differences.end(),
      [](const lang::driver::BuildProvenanceDifference &difference) {
        return difference.subject.find("prelude") != std::string::npos;
      });
  expect(preludeUntouched,
         "an unchanged source unit should not be reported as a difference");

  // A record whose content identity matches at another path is not a
  // divergence; the tree may legitimately have moved.
  lang::driver::BuildProvenance relocated = *parsed;
  relocated.source.units[0].material.canonicalPath =
      "/elsewhere/sample/src/main.gti";
  relocated.artifact.canonicalPath = "/elsewhere/sample/build/gti/sample";
  expect(lang::driver::compareBuildProvenance(recorded, relocated).empty(),
         "identical content at a new canonical path should compare equal");

  std::string rejectedError;
  expect(
      !lang::driver::parseBuildProvenanceJson("{\"schema\": 1}", rejectedError)
              .has_value() &&
          rejectedError.find("schema") != std::string::npos,
      "an older schema should be refused rather than partially read");
  expect(
      !lang::driver::parseBuildProvenanceJson(serialized + "{", rejectedError)
           .has_value(),
      "trailing content should be refused");
  expect(!lang::driver::parseBuildProvenanceJson("", rejectedError).has_value(),
         "an empty report should be refused");
}

void testResourcesAndArtifactOwnership() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  const std::filesystem::path runtime = temporary.root() / "libgti_runtime.a";
  std::filesystem::create_directories(include / "gti");
  expect(writeFile(include / "gti/runtime.hpp", ""),
         "the runtime header fixture should be writable");
  expect(writeFile(include / "gti/runtime.h", ""),
         "the C runtime header fixture should be writable");
  expect(writeFile(runtime, ""),
         "the runtime library fixture should be writable");

  lang::driver::ToolchainLayout layout{
      .standardLibrary = lang::standardLibraryLayout(temporary.root()),
      .runtimeInclude = include,
      .runtimeLibrary = runtime,
      .vendorInclude = include,
  };
  expect(
      lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23) ==
          lang::driver::ToolchainResourceError::RuntimeFilesMissing,
      "runtime validation should require the installed public C ABI header");
  expect(writeFile(include / "gti/c_abi.h", ""),
         "the C ABI header fixture should be writable");
  expect(
      lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23) ==
          lang::driver::ToolchainResourceError::RuntimeFilesMissing,
      "runtime validation should require the defined-failure ABI header");
  expect(writeFile(include / "gti/runtime_failure.h", ""),
         "the defined-failure ABI header fixture should be writable");
  expect(
      lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23) ==
          lang::driver::ToolchainResourceError::RuntimeFilesMissing,
      "runtime validation should require the managed-thread C ABI header");
  expect(writeFile(include / "gti/runtime_thread.h", ""),
         "the managed-thread C ABI header fixture should be writable");
  expect(
      lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23) ==
          lang::driver::ToolchainResourceError::RuntimeFilesMissing,
      "runtime validation should require the managed-thread C++ bridge");
  expect(writeFile(include / "gti/runtime_thread.hpp", ""),
         "the managed-thread C++ bridge fixture should be writable");
  expect(
      !lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23),
      "C++23 resource validation should accept the complete runtime surface");
  expect(
      lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp20) ==
          lang::driver::ToolchainResourceError::
              ExpectedCompatibilityHeaderMissing,
      "C++20 resource validation should require expected-lite");
  std::filesystem::create_directories(include / "nonstd");
  expect(writeFile(include / "nonstd/expected.hpp", ""),
         "the compatibility header fixture should be writable");
  expect(
      !lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp20),
      "a complete C++20 layout should pass validation");

  std::error_code resourceError;
  std::filesystem::remove(runtime, resourceError);
  resourceError.clear();
  std::filesystem::create_directory(runtime, resourceError);
  expect(!resourceError &&
             lang::driver::validateToolchainLayout(layout,
                                                   lang::CppStandard::Cpp23) ==
                 lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should reject a directory where the runtime "
         "archive must be a regular file");

  const std::filesystem::path removed = temporary.root() / "removed.cpp";
  expect(lang::driver::writeArtifact(removed, "generated") ==
                 lang::driver::ArtifactWriteStatus::Success &&
             readFile(removed) == "generated",
         "artifact writes should report success and preserve contents");
  lang::SourceManager loadedSources;
  loadedSources.set(removed.string(), "generated");
  const std::filesystem::path hardLink = temporary.root() / "source-alias";
  std::error_code hardLinkError;
  std::filesystem::create_hard_link(removed, hardLink, hardLinkError);
  if (!hardLinkError) {
    expect(lang::driver::findLoadedSourceCollision(hardLink, loadedSources) ==
               std::optional<std::filesystem::path>(removed),
           "artifact collision checks should recognize hard-link aliases of "
           "loaded sources");
  }
  {
    lang::driver::TemporaryArtifact artifact(removed, true);
  }
  expect(!std::filesystem::exists(removed),
         "temporary artifacts should be removed at scope exit");

  const std::filesystem::path retained = temporary.root() / "retained.cpp";
  expect(writeFile(retained, "generated"),
         "the retained artifact fixture should be writable");
  {
    lang::driver::TemporaryArtifact artifact(retained, true);
    artifact.keep();
  }
  expect(std::filesystem::exists(retained),
         "explicitly retained artifacts should survive scope exit");

  const std::filesystem::path firstTemporary =
      lang::driver::temporaryCppPath(temporary.root() / "main.gti");
  const std::filesystem::path secondTemporary =
      lang::driver::temporaryCppPath(temporary.root() / "main.gti");
  expect(firstTemporary != secondTemporary,
         "temporary artifact paths should be process-unique");
  expect(firstTemporary.extension() == ".cpp",
         "temporary C++ artifacts should retain their backend extension");
  const std::filesystem::path preprocessedTemporary =
      lang::driver::temporaryArtifactPath(temporary.root() / "native.h", ".i");
  expect(preprocessedTemporary.extension() == ".i" &&
             preprocessedTemporary != firstTemporary,
         "generic temporary artifacts should be unique and retain the "
         "requested extension");
  std::error_code firstError;
  std::error_code temporaryError;
  const std::filesystem::path canonicalParent =
      std::filesystem::weakly_canonical(firstTemporary.parent_path(),
                                        firstError);
  const std::filesystem::path canonicalTemporary =
      std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),
                                        temporaryError);
  expect(!firstError && !temporaryError &&
             canonicalParent == canonicalTemporary,
         "temporary artifacts should stay in the system temporary directory");

  const std::filesystem::path published = temporary.root() / "program.exe";
  const std::filesystem::path firstStaged =
      lang::driver::stagedArtifactPath(published);
  const std::filesystem::path secondStaged =
      lang::driver::stagedArtifactPath(published);
  expect(firstStaged != secondStaged,
         "staged artifact paths should be process-unique");
  expect(firstStaged.parent_path() == published.parent_path() &&
             firstStaged.extension() == published.extension() &&
             firstStaged.filename().string().starts_with(".program.gti-stage-"),
         "native outputs should stage beside the destination and preserve its "
         "extension");
  expect(writeFile(published, "previous") &&
             writeFile(firstStaged, "replacement"),
         "publication fixtures should be writable");
  const lang::driver::ArtifactPublishResult publication =
      lang::driver::publishArtifact(firstStaged, published);
  expect(publication.succeeded() && readFile(published) == "replacement" &&
             !std::filesystem::exists(firstStaged),
         "successful publication should atomically replace the destination");

  const lang::driver::ArtifactPublishResult missingPublication =
      lang::driver::publishArtifact(temporary.root() / "missing-stage",
                                    published);
  expect(!missingPublication.succeeded() &&
             readFile(published) == "replacement",
         "failed publication should preserve the previous destination");
}

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti_driver_tests");
  if (argc > 0 &&
      std::filesystem::path(argv[0]).stem() == "native-cxx-wrapper") {
    std::vector<std::string> command{"c++"};
    const std::vector<std::string> sanitizerArguments =
        activeSanitizerArguments();
    command.insert(command.end(), sanitizerArguments.begin(),
                   sanitizerArguments.end());
    for (int index = 1; index < argc; ++index) {
      command.emplace_back(argv[index]);
    }
    const lang::driver::ProcessResult result = lang::driver::invokeProcess(
        command, {.outputMode = lang::driver::ProcessOutputMode::Inherit,
                  .description = "sanitizer-aware native compiler wrapper"});
    if (result.driverDiagnostic) {
      std::cerr << *result.driverDiagnostic << '\n';
    }
    return result.exitCode;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--process-child") {
    if (argc != 5 || std::string_view(argv[2]) != "alpha" ||
        std::string_view(argv[3]) != "two words" ||
        std::string_view(argv[4]) != "") {
      return 31;
    }
    std::cout << "process arguments preserved\n";
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--process-child-exit") {
    return 23;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--render-command-child") {
    const std::vector<std::string_view> expected{
        "plain",
        "safe/path-1.2_@%+=:,",
        "two words",
        "tab\tvalue",
        "",
        "a\\b",
        "$HOME",
        "`command`",
        "x;y",
        "a'b",
        "line\nbreak",
        "*.cpp",
        "\"quoted\"",
        "left&right",
        "left|right",
        "<input>",
        "(group)",
        "question?",
    };
    if (argc != static_cast<int>(expected.size()) + 2) {
      return 32;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (std::string_view(argv[index + 2]) != expected[index]) {
        return 33;
      }
    }
    std::cout << "rendered arguments preserved\n";
    return 0;
  }

  testAnalysisConfigurationResolution();
  testCompilationRequestAndTargetPropagation();
  testBackendExceptionBoundary();
  testProcessInvocation(std::filesystem::absolute(argv[0]));
  testNativeCommandConstruction();
  testRenderedCommandReplay(std::filesystem::absolute(argv[0]));
  testNativeCCompilerFailure();
  testNativeCppCompilerFailure();
  testOrderedExecutableBuildCommand();
  testManagedOutputSafety();
  testWholeProgramBuildCache(std::filesystem::absolute(argv[0]));
  testNativeDependencyDiscoveryPrimitives();
  testNativeLibraryNameResolution();
  testBuildSourceMaterialIdentityProjection();
  testBuildProvenance(std::filesystem::absolute(argv[0]));
  testBuildReportRecordRoundTrip();
  testResourcesAndArtifactOwnership();

  if (failures != 0) {
    std::cerr << failures << " driver test(s) failed\n";
    return 1;
  }
  std::cout << "All driver tests passed\n";
  return 0;
}
