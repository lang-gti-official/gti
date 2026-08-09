#pragma once

#include "gti/cpp_emitter.h"
#include "gti/driver/process.h"
#include "gti/optimizer.h"
#include "gti/standard_library.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lang::driver {

struct ToolchainLayout {
  StandardLibraryLayout standardLibrary;
  std::filesystem::path runtimeInclude;
  std::filesystem::path runtimeLibrary;
  std::filesystem::path vendorInclude;
};

[[nodiscard]] ToolchainLayout discoverToolchainLayout(const char *driver);

enum class ToolchainResourceError {
  RuntimeFilesMissing,
  ExpectedCompatibilityHeaderMissing,
};

[[nodiscard]] std::optional<ToolchainResourceError>
validateToolchainLayout(const ToolchainLayout &layout, CppStandard standard);

[[nodiscard]] std::string discoverNativeCompiler(
    const std::optional<std::string> &selected = std::nullopt);

struct NativeInputs {
  std::vector<std::filesystem::path> includeDirectories;
  std::vector<std::string> compilerArguments;
  std::vector<std::filesystem::path> libraryDirectories;
  std::vector<std::filesystem::path> libraryFiles;
  std::vector<std::string> libraries;
  std::vector<std::string> frameworks;
  std::vector<std::string> linkerArguments;
  std::vector<std::string> trailingArguments;
};

class NativeCompileRequest final {
public:
  NativeCompileRequest(std::string compiler,
                       std::filesystem::path generatedSource,
                       std::filesystem::path output, CppStandard standard,
                       OptimizationLevel optimization, NativeInputs inputs);

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CppStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] const NativeInputs &inputs() const;

private:
  std::string compilerExecutable;
  std::filesystem::path generatedSourcePath;
  std::filesystem::path outputPath;
  CppStandard cppStandard;
  OptimizationLevel optimizationLevel;
  NativeInputs nativeInputs;
};

using NativeProcessResult = ProcessResult;

struct NativeInvocationOptions {
  bool captureSuccessfulOutput = false;
};

class NativeToolchain final {
public:
  [[nodiscard]] std::vector<std::string>
  command(const NativeCompileRequest &request) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeCompileRequest &request,
         NativeInvocationOptions options = {}) const;
};

[[nodiscard]] std::string renderCommand(std::span<const std::string> arguments);

} // namespace lang::driver
