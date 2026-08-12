#include "gti/driver/native_toolchain.h"

#include "gti/executable_path.h"

#include <cstdlib>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

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

std::string cStandardFlag(CStandard standard) {
  return "-std=" + std::string(cStandardName(standard));
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

bool isPosixShellBareCharacter(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') ||
         std::string_view("_@%+=:,./-").find(character) !=
             std::string_view::npos;
}

bool canRenderBare(std::string_view argument) {
  if (argument.empty()) {
    return false;
  }
  for (char character : argument) {
    if (!isPosixShellBareCharacter(character)) {
      return false;
    }
  }
  return true;
}

bool canRenderWithReadableDoubleQuotes(std::string_view argument) {
  for (char character : argument) {
    if (!isPosixShellBareCharacter(character) && character != ' ' &&
        character != '\t') {
      return false;
    }
  }
  return true;
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
  if (!std::filesystem::is_regular_file(
          layout.runtimeInclude / "gti/runtime.hpp", error) ||
      !std::filesystem::is_regular_file(layout.runtimeInclude / "gti/runtime.h",
                                        error) ||
      !std::filesystem::is_regular_file(layout.runtimeInclude / "gti/c_abi.h",
                                        error) ||
      !std::filesystem::is_regular_file(layout.runtimeLibrary, error)) {
    return ToolchainResourceError::RuntimeFilesMissing;
  }
  if (standard == CppStandard::Cpp20 &&
      !std::filesystem::is_regular_file(
          layout.vendorInclude / "nonstd/expected.hpp", error)) {
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

std::string discoverCCompiler(const std::optional<std::string> &selected) {
  if (selected) {
    return *selected;
  }
  if (const char *configured = std::getenv("GTI_CC");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  if (const char *configured = std::getenv("CC");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  return "cc";
}

std::string_view cStandardName(CStandard standard) {
  switch (standard) {
  case CStandard::C11:
    return "c11";
  case CStandard::C17:
    return "c17";
  case CStandard::C23:
    return "c23";
  }
  return "c17";
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

NativeCCompileRequest::NativeCCompileRequest(
    std::string compiler, std::filesystem::path source,
    std::filesystem::path output, CStandard standard,
    OptimizationLevel optimization,
    std::vector<std::filesystem::path> includeDirectories,
    std::vector<std::string> compilerArguments)
    : compilerExecutable(std::move(compiler)), sourcePath(std::move(source)),
      outputPath(std::move(output)), cStandard(standard),
      optimizationLevel(optimization),
      nativeIncludeDirectories(std::move(includeDirectories)),
      nativeCompilerArguments(std::move(compilerArguments)) {}

const std::string &NativeCCompileRequest::compiler() const {
  return compilerExecutable;
}

const std::filesystem::path &NativeCCompileRequest::source() const {
  return sourcePath;
}

const std::filesystem::path &NativeCCompileRequest::output() const {
  return outputPath;
}

CStandard NativeCCompileRequest::standard() const { return cStandard; }

OptimizationLevel NativeCCompileRequest::optimization() const {
  return optimizationLevel;
}

const std::vector<std::filesystem::path> &
NativeCCompileRequest::includeDirectories() const {
  return nativeIncludeDirectories;
}

const std::vector<std::string> &
NativeCCompileRequest::compilerArguments() const {
  return nativeCompilerArguments;
}

NativeCppCompileRequest::NativeCppCompileRequest(
    std::string compiler, std::filesystem::path source,
    std::filesystem::path output, CppStandard standard,
    OptimizationLevel optimization,
    std::vector<std::filesystem::path> includeDirectories,
    std::vector<std::string> compilerArguments)
    : compilerExecutable(std::move(compiler)), sourcePath(std::move(source)),
      outputPath(std::move(output)), cppStandard(standard),
      optimizationLevel(optimization),
      nativeIncludeDirectories(std::move(includeDirectories)),
      nativeCompilerArguments(std::move(compilerArguments)) {}

const std::string &NativeCppCompileRequest::compiler() const {
  return compilerExecutable;
}

const std::filesystem::path &NativeCppCompileRequest::source() const {
  return sourcePath;
}

const std::filesystem::path &NativeCppCompileRequest::output() const {
  return outputPath;
}

CppStandard NativeCppCompileRequest::standard() const { return cppStandard; }

OptimizationLevel NativeCppCompileRequest::optimization() const {
  return optimizationLevel;
}

const std::vector<std::filesystem::path> &
NativeCppCompileRequest::includeDirectories() const {
  return nativeIncludeDirectories;
}

const std::vector<std::string> &
NativeCppCompileRequest::compilerArguments() const {
  return nativeCompilerArguments;
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
  if (inputs.orderedLinkOperands.empty()) {
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
  } else {
    for (const NativeLinkOperand &operand : inputs.orderedLinkOperands) {
      switch (operand.kind) {
      case NativeLinkOperandKind::File:
        command.push_back(operand.value);
        break;
      case NativeLinkOperandKind::Library:
        command.emplace_back("-l" + operand.value);
        break;
      case NativeLinkOperandKind::Framework:
        command.emplace_back("-framework");
        command.push_back(operand.value);
        break;
      }
    }
  }
  command.insert(command.end(), inputs.linkerArguments.begin(),
                 inputs.linkerArguments.end());
  command.emplace_back("-o");
  command.push_back(request.output().string());
  command.insert(command.end(), inputs.trailingArguments.begin(),
                 inputs.trailingArguments.end());
  // These language-semantic flags deliberately follow every forwarded native
  // argument so a package cannot re-enable reassociation or contraction.
  command.emplace_back("-fno-fast-math");
  command.emplace_back("-ffp-contract=off");
  // The generated artifact requires this opt-in marker so direct backend
  // consumers cannot unknowingly compile it without the same strict policy.
  command.emplace_back("-D__gti_strict_ieee754=1");
  return command;
}

std::vector<std::string>
NativeToolchain::command(const NativeCCompileRequest &request) const {
  std::vector<std::string> command{
      request.compiler(), cStandardFlag(request.standard()),
      std::string(optimizationFlag(request.optimization()))};
  for (const std::filesystem::path &directory : request.includeDirectories()) {
    command.emplace_back("-I" + directory.string());
  }
  command.insert(command.end(), request.compilerArguments().begin(),
                 request.compilerArguments().end());
  command.emplace_back("-c");
  command.push_back(request.source().string());
  command.emplace_back("-o");
  command.push_back(request.output().string());
  // Foreign C sources are separate translation units. The generated GTI
  // artifact's strict binary32 policy flags deliberately do not apply here.
  return command;
}

std::vector<std::string>
NativeToolchain::command(const NativeCppCompileRequest &request) const {
  std::vector<std::string> command{
      request.compiler(), std::string(standardFlag(request.standard())),
      std::string(optimizationFlag(request.optimization()))};
  for (const std::filesystem::path &directory : request.includeDirectories()) {
    command.emplace_back("-I" + directory.string());
  }
  command.insert(command.end(), request.compilerArguments().begin(),
                 request.compilerArguments().end());
  command.emplace_back("-c");
  command.push_back(request.source().string());
  command.emplace_back("-o");
  command.push_back(request.output().string());
  // Foreign C++ sources are separate translation units. The generated GTI
  // artifact's strict binary32 policy flags deliberately do not apply here.
  return command;
}

NativeProcessResult
NativeToolchain::invoke(const NativeCompileRequest &request,
                        NativeInvocationOptions options) const {
  return invokeProcess(
      command(request),
      {.outputMode = ProcessOutputMode::Capture,
       .captureSuccessfulOutput = options.captureSuccessfulOutput,
       .description = "native compiler"});
}

NativeProcessResult
NativeToolchain::invoke(const NativeCCompileRequest &request,
                        NativeInvocationOptions options) const {
  return invokeProcess(
      command(request),
      {.outputMode = ProcessOutputMode::Capture,
       .captureSuccessfulOutput = options.captureSuccessfulOutput,
       .description = "native C compiler"});
}

NativeProcessResult
NativeToolchain::invoke(const NativeCppCompileRequest &request,
                        NativeInvocationOptions options) const {
  return invokeProcess(
      command(request),
      {.outputMode = ProcessOutputMode::Capture,
       .captureSuccessfulOutput = options.captureSuccessfulOutput,
       .description = "native C++ source compiler"});
}

std::string renderCommand(std::span<const std::string> arguments) {
  std::ostringstream output;
  output << '+';
  for (const std::string &argument : arguments) {
    output << ' ';
    if (canRenderBare(argument)) {
      output << argument;
      continue;
    }
    if (!argument.empty() && canRenderWithReadableDoubleQuotes(argument)) {
      output << '"' << argument << '"';
      continue;
    }
    output << '\'';
    for (char character : argument) {
      if (character == '\'') {
        output << "'\\''";
        continue;
      }
      output << character;
    }
    output << '\'';
  }
  return output.str();
}

} // namespace lang::driver
