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

// One dependency package's composable native contribution, target-selected
// and validated by project resolution. Include directories and macro
// definitions are scoped to this group's own declared sources only — they
// never leak into the application's sources or the generated translation
// unit. Link operands and library directories join the final link after the
// application's own contribution, in dependents-before-dependencies order.
struct NativeDependencyGroup {
  // The contributing package's `name@version`, for diagnostics and metadata.
  std::string packageIdentity;
  std::vector<std::filesystem::path> includeDirectories;
  // The only argument form a dependency may contribute: exact `-D`/`-U`
  // macro spellings validated by composition, keeping the manifest's C
  // versus C++ argument scoping. Opaque argument vectors do not compose
  // across package boundaries.
  std::vector<std::string> cMacroDefinitions;
  std::vector<std::string> cppMacroDefinitions;
  std::vector<std::filesystem::path> cSources;
  CStandard cStandard = CStandard::C17;
  std::vector<std::filesystem::path> cppSources;
  std::vector<std::filesystem::path> libraryDirectories;
  std::vector<NativeLinkOperand> linkOperands;
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
  // Composed native contributions of the selected package's transitive
  // dependency closure, in dependents-before-dependencies order. Only
  // project resolution populates this; direct mode has no package graph.
  std::vector<NativeDependencyGroup> dependencyGroups;
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

  // Dependency-discovery variants of the object-compile commands above. They
  // preserve the exact argument vector of the corresponding compile and only
  // replace `-c` with `-E -MD -MF <depfile>`, so the preprocessor resolves
  // includes exactly as the object compile would. The request output receives
  // the preprocessed translation unit and `depfile` receives the compiler's
  // make-style dependency report.
  [[nodiscard]] std::vector<std::string>
  preprocessCommand(const NativeCCompileRequest &request,
                    const std::filesystem::path &depfile) const;

  [[nodiscard]] std::vector<std::string>
  preprocessCommand(const NativeCppCompileRequest &request,
                    const std::filesystem::path &depfile) const;

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
