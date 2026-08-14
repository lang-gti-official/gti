#pragma once

#include "gti/cpp_standard.h"
#include "gti/driver/process.h"
#include "gti/optimizer.h"
#include "gti/standard_library.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] std::string
discoverCCompiler(const std::optional<std::string> &selected = std::nullopt);

enum class CStandard {
  C11,
  C17,
  C23,
};

[[nodiscard]] std::string_view cStandardName(CStandard standard);

enum class NativeLinkOperandKind {
  File,
  Library,
  Framework,
};

struct NativeLinkOperand {
  NativeLinkOperandKind kind = NativeLinkOperandKind::File;
  std::string value;

  bool operator==(const NativeLinkOperand &) const = default;
};

struct NativeInputs {
  std::vector<std::filesystem::path> includeDirectories;
  std::vector<std::string> compilerArguments;
  std::vector<std::filesystem::path> cSources;
  std::vector<std::string> cCompilerArguments;
  std::optional<CStandard> cStandard;
  std::vector<std::filesystem::path> cppSources;
  std::vector<std::filesystem::path> libraryDirectories;
  std::vector<std::filesystem::path> libraryFiles;
  std::vector<std::string> libraries;
  std::vector<std::string> frameworks;
  // When populated, this is the authoritative file/library/framework order.
  // Category vectors remain available for manifest metadata and validation.
  std::vector<NativeLinkOperand> orderedLinkOperands;
  std::vector<std::string> linkerArguments;
  std::vector<std::string> trailingArguments;
};

// Native escape hatches may add ordinary compiler/linker policy, but they may
// not replace driver-owned output, compilation mode, language/optimization,
// target/data-layout selection, or response-file inputs.
[[nodiscard]] bool isReservedNativeBuildArgument(std::string_view argument);

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

class NativeCCompileRequest final {
public:
  NativeCCompileRequest(std::string compiler, std::filesystem::path source,
                        std::filesystem::path output, CStandard standard,
                        OptimizationLevel optimization,
                        std::vector<std::filesystem::path> includeDirectories,
                        std::vector<std::string> compilerArguments);

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &source() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] const std::vector<std::filesystem::path> &
  includeDirectories() const;
  [[nodiscard]] const std::vector<std::string> &compilerArguments() const;

private:
  std::string compilerExecutable;
  std::filesystem::path sourcePath;
  std::filesystem::path outputPath;
  CStandard cStandard;
  OptimizationLevel optimizationLevel;
  std::vector<std::filesystem::path> nativeIncludeDirectories;
  std::vector<std::string> nativeCompilerArguments;
};

class NativeCppCompileRequest final {
public:
  NativeCppCompileRequest(std::string compiler, std::filesystem::path source,
                          std::filesystem::path output, CppStandard standard,
                          OptimizationLevel optimization,
                          std::vector<std::filesystem::path> includeDirectories,
                          std::vector<std::string> compilerArguments);

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &source() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CppStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] const std::vector<std::filesystem::path> &
  includeDirectories() const;
  [[nodiscard]] const std::vector<std::string> &compilerArguments() const;

private:
  std::string compilerExecutable;
  std::filesystem::path sourcePath;
  std::filesystem::path outputPath;
  CppStandard cppStandard;
  OptimizationLevel optimizationLevel;
  std::vector<std::filesystem::path> nativeIncludeDirectories;
  std::vector<std::string> nativeCompilerArguments;
};

using NativeProcessResult = ProcessResult;

struct NativeInvocationOptions {
  bool captureSuccessfulOutput = false;
};

class NativeToolchain final {
public:
  [[nodiscard]] std::vector<std::string>
  command(const NativeCompileRequest &request) const;

  [[nodiscard]] std::vector<std::string>
  command(const NativeCCompileRequest &request) const;

  [[nodiscard]] std::vector<std::string>
  command(const NativeCppCompileRequest &request) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeCompileRequest &request,
         NativeInvocationOptions options = {}) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeCCompileRequest &request,
         NativeInvocationOptions options = {}) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeCppCompileRequest &request,
         NativeInvocationOptions options = {}) const;
};

// Renders a human-facing POSIX-shell replay line prefixed with "+ ". Removing
// that display prefix and evaluating the remainder in a POSIX shell recreates
// each argument exactly for process arguments without embedded NUL bytes. This
// contract does not describe cmd.exe or PowerShell quoting.
[[nodiscard]] std::string renderCommand(std::span<const std::string> arguments);

} // namespace lang::driver
