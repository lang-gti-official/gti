#include "gti/driver/artifact.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
  expect(writeFile(source, R"(#if target.os == "driver-test"
int selected() { return 41; }
#else
int selected() { return 82; }
#endif
int main() { return selected() - 41; }
)"),
         "the driver test source should be writable");

  const lang::TargetInfo target{
      .os = "driver-test", .vendor = "test-vendor", .arch = "test-arch"};
  const lang::driver::CompilationRequest request(
      source, lang::standardLibraryLayout(temporary.root()), target,
      lang::OptimizationLevel::O1, lang::CppStandard::Cpp23);
  expect(request.entry() == source &&
             request.standardLibrary().prelude == prelude,
         "compilation requests should retain resolved source and stdlib paths");
  expect(request.target().os == "driver-test" &&
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

  const std::filesystem::path removed = temporary.root() / "removed.cpp";
  expect(lang::driver::writeArtifact(removed, "generated") ==
                 lang::driver::ArtifactWriteStatus::Success &&
             readFile(removed) == "generated",
         "artifact writes should report success and preserve contents");
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

  testCompilationRequestAndTargetPropagation();
  testProcessInvocation(std::filesystem::absolute(argv[0]));
  testNativeCommandConstruction();
  testResourcesAndArtifactOwnership();

  if (failures != 0) {
    std::cerr << failures << " driver test(s) failed\n";
    return 1;
  }
  std::cout << "All driver tests passed\n";
  return 0;
}
