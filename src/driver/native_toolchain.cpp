#include "gti/driver/native_toolchain.h"

#include "gti/executable_path.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(GTI_BUILD_STDLIB_ROOT)
#define GTI_BUILD_STDLIB_ROOT ""
#endif
#if !defined(GTI_BUILD_RUNTIME_INCLUDE_DIR)
#define GTI_BUILD_RUNTIME_INCLUDE_DIR ""
#endif
#if !defined(GTI_BUILD_RUNTIME_LIBRARY_PATH)
#define GTI_BUILD_RUNTIME_LIBRARY_PATH ""
#endif
#if !defined(GTI_RUNTIME_LIBRARY_NAME)
#define GTI_RUNTIME_LIBRARY_NAME "libgti_runtime.a"
#endif
#if !defined(GTI_BUILD_VENDOR_INCLUDE_DIR)
#define GTI_BUILD_VENDOR_INCLUDE_DIR ""
#endif

namespace lang::driver {
namespace {

std::filesystem::path
selectToolchainPath(const char *environmentName,
                    const std::filesystem::path &installed,
                    const std::filesystem::path &buildPath,
                    const std::filesystem::path &requiredChild = {}) {
  if (const char *configured = std::getenv(environmentName);
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  std::error_code error;
  const auto exists = [&](const std::filesystem::path &path) {
    error.clear();
    return !path.empty() &&
           std::filesystem::exists(
               requiredChild.empty() ? path : path / requiredChild, error);
  };
  if (exists(installed)) {
    return installed;
  }
  if (exists(buildPath)) {
    return buildPath;
  }
  return buildPath;
}

std::string_view standardFlag(CppStandard standard) {
  return standard == CppStandard::Cpp23 ? "-std=c++23" : "-std=c++20";
}

std::string_view optimizationFlag(OptimizationLevel level) {
  switch (level) {
  case OptimizationLevel::O0:
    return "-O0";
  case OptimizationLevel::O1:
    return "-O1";
  case OptimizationLevel::O2:
    return "-O2";
  case OptimizationLevel::O3:
    return "-O3";
  }
  return "-O0";
}

std::string readCapture(std::FILE *capture) {
  if (std::fseek(capture, 0, SEEK_END) != 0) {
    return {};
  }
  const long capturedSize = std::ftell(capture);
  if (capturedSize <= 0 || std::fseek(capture, 0, SEEK_SET) != 0) {
    return {};
  }

  std::string output(static_cast<std::size_t>(capturedSize), '\0');
  std::size_t total = 0;
  while (total < output.size()) {
    const std::size_t size = std::fread(output.data() + total, sizeof(char),
                                        output.size() - total, capture);
    total += size;
    if (size == 0 || std::feof(capture) != 0 || std::ferror(capture) != 0) {
      break;
    }
  }
  output.resize(total);
  return output;
}

NativeProcessResult finishProcess(std::FILE *capture, int status,
                                  bool captureSuccessfulOutput) {
  NativeProcessResult result{.exitCode = status};
  if (status != 0 || captureSuccessfulOutput) {
    result.output = readCapture(capture);
  }
  std::fclose(capture);
  return result;
}

NativeProcessResult runProcess(const std::vector<std::string> &arguments,
                               NativeInvocationOptions options) {
  if (arguments.empty() || arguments.front().empty()) {
    return {.exitCode = 127,
            .driverDiagnostic = "gti: native compiler command is empty"};
  }

  std::vector<char *> processArguments;
  processArguments.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    processArguments.push_back(const_cast<char *>(argument.c_str()));
  }
  processArguments.push_back(nullptr);

  std::FILE *capture = std::tmpfile();
  if (capture == nullptr) {
    return {.exitCode = 74,
            .driverDiagnostic =
                "gti: failed to create native compiler output capture: " +
                std::string(std::strerror(errno))};
  }
  std::cout.flush();
  std::cerr.flush();

#if defined(_WIN32)
  const int standardOutput = _fileno(stdout);
  const int standardError = _fileno(stderr);
  const int savedOutput = _dup(standardOutput);
  const int savedError = _dup(standardError);
  if (savedOutput == -1 || savedError == -1 ||
      _dup2(_fileno(capture), standardOutput) != 0 ||
      _dup2(_fileno(capture), standardError) != 0) {
    if (savedOutput != -1) {
      _dup2(savedOutput, standardOutput);
      _close(savedOutput);
    }
    if (savedError != -1) {
      _dup2(savedError, standardError);
      _close(savedError);
    }
    std::fclose(capture);
    return {.exitCode = 74,
            .driverDiagnostic =
                "gti: failed to redirect native compiler output"};
  }

  const intptr_t status =
      _spawnvp(_P_WAIT, processArguments.front(), processArguments.data());
  const int spawnError = errno;
  std::fflush(stdout);
  std::fflush(stderr);
  _dup2(savedOutput, standardOutput);
  _dup2(savedError, standardError);
  _close(savedOutput);
  _close(savedError);
  if (status == -1) {
    NativeProcessResult result = finishProcess(capture, 127, true);
    result.driverDiagnostic = "gti: failed to execute '" + arguments.front() +
                              "': " + std::strerror(spawnError);
    return result;
  }
  return finishProcess(capture, static_cast<int>(status),
                       options.captureSuccessfulOutput);
#else
  const pid_t child = fork();
  if (child == -1) {
    const int forkError = errno;
    std::fclose(capture);
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to start native compiler: " +
                                std::string(std::strerror(forkError))};
  }
  if (child == 0) {
    const int descriptor = fileno(capture);
    if (descriptor == -1 || dup2(descriptor, STDOUT_FILENO) == -1 ||
        dup2(descriptor, STDERR_FILENO) == -1) {
      const int redirectError = errno;
      std::fprintf(stderr,
                   "gti: failed to redirect native compiler output: %s\n",
                   std::strerror(redirectError));
      std::fflush(stderr);
      _exit(127);
    }
    if (descriptor > STDERR_FILENO) {
      close(descriptor);
    }
    execvp(processArguments.front(), processArguments.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute '%s': %s\n",
                 arguments.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    _exit(127);
  }

  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR) {
      const int waitError = errno;
      std::fclose(capture);
      return {.exitCode = 127,
              .driverDiagnostic =
                  "gti: failed while waiting for native compiler: " +
                  std::string(std::strerror(waitError))};
    }
  }
  if (WIFEXITED(status)) {
    return finishProcess(capture, WEXITSTATUS(status),
                         options.captureSuccessfulOutput);
  }
  if (WIFSIGNALED(status)) {
    return finishProcess(capture, 128 + WTERMSIG(status),
                         options.captureSuccessfulOutput);
  }
  std::fclose(capture);
  return {.exitCode = 127};
#endif
}

} // namespace

ToolchainLayout discoverToolchainLayout(const char *driver) {
  const std::filesystem::path executable = executablePath(driver);
  const std::filesystem::path prefix = executable.parent_path().parent_path();

  return {
      .standardLibrary = discoverStandardLibrary(driver, GTI_BUILD_STDLIB_ROOT),
      .runtimeInclude =
          selectToolchainPath("GTI_RUNTIME_INCLUDE", prefix / "include",
                              GTI_BUILD_RUNTIME_INCLUDE_DIR, "gti/runtime.hpp"),
      .runtimeLibrary = selectToolchainPath(
          "GTI_RUNTIME_LIBRARY", prefix / "lib" / GTI_RUNTIME_LIBRARY_NAME,
          GTI_BUILD_RUNTIME_LIBRARY_PATH),
      .vendorInclude = selectToolchainPath(
          "GTI_VENDOR_INCLUDE", prefix / "include",
          GTI_BUILD_VENDOR_INCLUDE_DIR, "nonstd/expected.hpp"),
  };
}

std::optional<ToolchainResourceError>
validateToolchainLayout(const ToolchainLayout &layout, CppStandard standard) {
  std::error_code error;
  if (!std::filesystem::exists(layout.runtimeInclude / "gti/runtime.hpp",
                               error) ||
      !std::filesystem::exists(layout.runtimeLibrary, error)) {
    return ToolchainResourceError::RuntimeFilesMissing;
  }
  if (standard == CppStandard::Cpp20 &&
      !std::filesystem::exists(layout.vendorInclude / "nonstd/expected.hpp",
                               error)) {
    return ToolchainResourceError::ExpectedCompatibilityHeaderMissing;
  }
  return std::nullopt;
}

std::string discoverNativeCompiler(const std::optional<std::string> &selected) {
  if (selected) {
    return *selected;
  }
  if (const char *configured = std::getenv("GTI_CXX");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  if (const char *configured = std::getenv("CXX");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  return "c++";
}

NativeCompileRequest::NativeCompileRequest(
    std::string compiler, std::filesystem::path generatedSource,
    std::filesystem::path output, CppStandard standard,
    OptimizationLevel optimization, NativeInputs inputs)
    : compilerExecutable(std::move(compiler)),
      generatedSourcePath(std::move(generatedSource)),
      outputPath(std::move(output)), cppStandard(standard),
      optimizationLevel(optimization), nativeInputs(std::move(inputs)) {}

const std::string &NativeCompileRequest::compiler() const {
  return compilerExecutable;
}

const std::filesystem::path &NativeCompileRequest::generatedSource() const {
  return generatedSourcePath;
}

const std::filesystem::path &NativeCompileRequest::output() const {
  return outputPath;
}

CppStandard NativeCompileRequest::standard() const { return cppStandard; }

OptimizationLevel NativeCompileRequest::optimization() const {
  return optimizationLevel;
}

const NativeInputs &NativeCompileRequest::inputs() const {
  return nativeInputs;
}

std::vector<std::string>
NativeToolchain::command(const NativeCompileRequest &request) const {
  std::vector<std::string> command{
      request.compiler(), std::string(standardFlag(request.standard())),
      std::string(optimizationFlag(request.optimization()))};
  const NativeInputs &inputs = request.inputs();
  for (const std::filesystem::path &directory : inputs.includeDirectories) {
    command.emplace_back("-I" + directory.string());
  }
  command.insert(command.end(), inputs.compilerArguments.begin(),
                 inputs.compilerArguments.end());
  command.push_back(request.generatedSource().string());
  for (const std::filesystem::path &directory : inputs.libraryDirectories) {
    command.emplace_back("-L" + directory.string());
  }
  for (const std::filesystem::path &library : inputs.libraryFiles) {
    command.push_back(library.string());
  }
  for (const std::string &library : inputs.libraries) {
    command.emplace_back("-l" + library);
  }
  for (const std::string &framework : inputs.frameworks) {
    command.emplace_back("-framework");
    command.push_back(framework);
  }
  command.insert(command.end(), inputs.linkerArguments.begin(),
                 inputs.linkerArguments.end());
  command.emplace_back("-o");
  command.push_back(request.output().string());
  command.insert(command.end(), inputs.trailingArguments.begin(),
                 inputs.trailingArguments.end());
  return command;
}

NativeProcessResult
NativeToolchain::invoke(const NativeCompileRequest &request,
                        NativeInvocationOptions options) const {
  return runProcess(command(request), options);
}

std::string renderCommand(std::span<const std::string> arguments) {
  std::ostringstream output;
  output << '+';
  for (const std::string &argument : arguments) {
    output << ' ';
    if (argument.find_first_of(" \t\"") == std::string::npos) {
      output << argument;
      continue;
    }
    output << '"';
    for (char character : argument) {
      if (character == '"' || character == '\\') {
        output << '\\';
      }
      output << character;
    }
    output << '"';
  }
  return output.str();
}

} // namespace lang::driver
