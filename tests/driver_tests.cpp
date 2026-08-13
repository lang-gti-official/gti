#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/support.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
int main() { return selected() - 41; }
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
             request.cppStandard() == lang::CppStandard::Cpp23,
         "compilation requests should retain target and backend policy");

  const lang::driver::CompilationResult result =
      lang::driver::compileToCpp(request);
  expect(result.succeeded(),
         "the driver should compile a valid whole-program request");
  if (result.artifact) {
    expect(result.artifact->contents.find("return 41;") != std::string::npos &&
               result.artifact->contents.find("return 82;") ==
                   std::string::npos,
           "one resolved target must reach frontend conditionals and backend "
           "emission");
  }

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
  const std::vector<std::string> expected{
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
  expect(orderedCommand == std::vector<std::string>({
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
                           }),
         "ordered native link operands should preserve mixed file, -l, and "
         "framework dependency order after the build-owned runtime and be "
         "authoritative over compatibility category vectors");
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
}

void testWholeProgramBuildCache(const std::filesystem::path &testExecutable) {
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
       {"runtime.hpp", "runtime.h", "c_abi.h"}) {
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
  const auto build = [&](lang::OptimizationLevel optimization,
                         lang::driver::NativeInputs nativeInputs = {},
                         bool keepCpp = false,
                         lang::CppStandard standard = lang::CppStandard::Cpp23,
                         lang::TargetInfo target = lang::TargetInfo::host()) {
    return lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
        lang::driver::CompilationRequest(source, toolchain.standardLibrary,
                                         std::move(target), optimization,
                                         standard),
        toolchain, generated, output, "c++", std::move(nativeInputs), keepCpp,
        true, false, managed, std::nullopt, cache));
  };

  const lang::driver::ExecutableBuildResult first =
      build(lang::OptimizationLevel::O0);
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

  lang::driver::NativeInputs flaggedInputs;
  flaggedInputs.compilerArguments = {"-DGTI_CACHE_VARIANT=1"};
  const lang::driver::ExecutableBuildResult nativeFlagChange =
      build(lang::OptimizationLevel::O0, std::move(flaggedInputs));
  expect(nativeFlagChange.succeeded() &&
             nativeFlagChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeFlagChange.cache.key != firstKey,
         "native compiler arguments should participate in cache identity");

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
  const lang::driver::ExecutableBuildResult afterDeletion =
      build(lang::OptimizationLevel::O0);
  expect(afterDeletion.succeeded() &&
             afterDeletion.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             afterDeletion.nativeProcess,
         "a deleted cache entry should degrade to a clean rebuild");
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
      !lang::driver::validateToolchainLayout(layout, lang::CppStandard::Cpp23),
      "C++23 resource validation should not require expected-lite");
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

  testCompilationRequestAndTargetPropagation();
  testProcessInvocation(std::filesystem::absolute(argv[0]));
  testNativeCommandConstruction();
  testRenderedCommandReplay(std::filesystem::absolute(argv[0]));
  testNativeCCompilerFailure();
  testNativeCppCompilerFailure();
  testOrderedExecutableBuildCommand();
  testManagedOutputSafety();
  testWholeProgramBuildCache(std::filesystem::absolute(argv[0]));
  testResourcesAndArtifactOwnership();

  if (failures != 0) {
    std::cerr << failures << " driver test(s) failed\n";
    return 1;
  }
  std::cout << "All driver tests passed\n";
  return 0;
}
