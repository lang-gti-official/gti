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
};

[[nodiscard]] ToolchainLayout discoverToolchainLayout(const char *driver);

enum class ToolchainResourceError {
  RuntimeFilesMissing,
};

[[nodiscard]] std::optional<ToolchainResourceError>
validateToolchainLayout(const ToolchainLayout &layout);

[[nodiscard]] std::string discoverNativeCompiler(
    const std::optional<std::string> &selected = std::nullopt);

[[nodiscard]] std::string
discoverCCompiler(const std::optional<std::string> &selected = std::nullopt);

// The Clang executable bindgen preprocessing uses: an explicit selection,
// then GTI_CLANG, then `clang`. Bindgen requires Clang specifically
// (`-fkeep-system-includes`), so the C-compiler convention does not apply.
[[nodiscard]] std::string
discoverBindgenClang(const std::optional<std::string> &selected = std::nullopt);

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

// Driver-owned cross-compilation policy for one native invocation. A host
// build leaves this empty. `--target` and `--sysroot` are rejected in every
// user-supplied native argument vector precisely because they select the
// target and the platform SDK, so the driver is the only layer allowed to
// emit them, and it emits only what a resolved toolchain declared.
struct NativeTargetPolicy {
  // Emitted as `--target=<triple>` when non-empty. Deliberately the native
  // toolchain's own spelling rather than GTI's normalized triple: a
  // target-prefixed cross compiler must not be told a target at all.
  std::string triple;
  // Emitted as `--sysroot=<path>` when non-empty.
  std::filesystem::path sysroot;

  [[nodiscard]] bool empty() const { return triple.empty() && sysroot.empty(); }

  bool operator==(const NativeTargetPolicy &) const = default;
};

// Appends a target policy's driver-owned arguments to a native command.
void appendNativeTargetPolicy(std::vector<std::string> &command,
                              const NativeTargetPolicy &policy);

// Resolved diagnostic native inputs. Only the driver resolver establishes this
// record; NativeToolchain consumes its exact arguments without selecting
// policy.
struct NativeInstrumentationPlan {
  std::string mode;
  std::string llvmVersion;
  std::string compilerCommand;
  std::filesystem::path compilerExecutable;
  std::string compilerVersion;
  std::vector<std::filesystem::path> configurationFiles;
  std::filesystem::path resourceDirectory;
  std::filesystem::path runtimeDirectory;
  std::vector<std::filesystem::path> sdkDirectories;
  std::vector<std::filesystem::path> runtimeFiles;
  std::vector<std::filesystem::path> implementationFiles;
  std::vector<std::string> linkTrace;
  std::vector<std::string> compileArguments;
  std::vector<std::string> linkArguments;
  std::string identity;
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
  // Driver-owned archives that every other input may reference or replace,
  // such as the GTI runtime. They follow every other input and forwarded
  // argument: order-sensitive linkers search an archive only for symbols
  // already undefined when they reach it.
  std::vector<std::filesystem::path> finalLibraryFiles;
  // Composed native contributions of the selected package's transitive
  // dependency closure, in dependents-before-dependencies order. Only
  // project resolution populates this; direct mode has no package graph.
  std::vector<NativeDependencyGroup> dependencyGroups;
  // Resolved cross-compilation policy. Empty for a host build.
  NativeTargetPolicy targetPolicy;
  // Populated by buildExecutable after resolving the requested LLVM policy.
  std::optional<NativeInstrumentationPlan> instrumentation;
};

// Native escape hatches may add ordinary compiler/linker policy, but they may
// not replace driver-owned output, compilation mode, language/optimization,
// target/data-layout selection, or response-file inputs.
[[nodiscard]] bool isReservedNativeBuildArgument(std::string_view argument);

class NativeCompileRequest final {
public:
  NativeCompileRequest(
      std::string compiler, std::filesystem::path generatedSource,
      std::filesystem::path output, CppStandard standard,
      OptimizationLevel optimization, NativeInputs inputs,
      std::optional<std::filesystem::path> dependencyFile = std::nullopt,
      bool debugInfo = false);

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CppStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] bool debugInfo() const;
  [[nodiscard]] const NativeInputs &inputs() const;
  [[nodiscard]] const std::optional<std::filesystem::path> &
  dependencyFile() const;

private:
  std::string compilerExecutable;
  std::filesystem::path generatedSourcePath;
  std::filesystem::path outputPath;
  CppStandard cppStandard;
  OptimizationLevel optimizationLevel;
  NativeInputs nativeInputs;
  std::optional<std::filesystem::path> nativeDependencyFile;
  bool sourceDebugInfo = false;
};

// The language level and optimization a native invocation gives the native
// sources named among its forwarded arguments.
struct ForwardedSourcePolicy {
  CppStandard standard = CppStandard::Cpp23;
  OptimizationLevel optimization = OptimizationLevel::O0;
};

// Links a backend-produced native object without applying source-language,
// preprocessing, or frontend code-generation flags to it. The backend owns the
// object's target and code-generation policy; the driver owns executable
// publication and composition with native dependencies and the GTI runtime.
// Forwarded arguments may name native sources that this invocation compiles.
// Given a policy, they compile exactly as they would beside generated C++:
// with its language level, optimization, include directories, compiler
// arguments, strict floating point and strict-policy marker.
class NativeLinkRequest final {
public:
  NativeLinkRequest(
      std::string compiler, std::filesystem::path object,
      std::filesystem::path output, NativeInputs inputs, bool debugInfo = false,
      std::optional<ForwardedSourcePolicy> forwardedSources = std::nullopt);

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &object() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] bool debugInfo() const;
  [[nodiscard]] const NativeInputs &inputs() const;
  [[nodiscard]] const std::optional<ForwardedSourcePolicy> &
  forwardedSources() const;

private:
  std::string compilerExecutable;
  std::filesystem::path objectPath;
  std::filesystem::path outputPath;
  NativeInputs nativeInputs;
  bool sourceDebugInfo = false;
  std::optional<ForwardedSourcePolicy> forwardedSourcePolicy;
};

class NativeCCompileRequest final {
public:
  NativeCCompileRequest(std::string compiler, std::filesystem::path source,
                        std::filesystem::path output, CStandard standard,
                        OptimizationLevel optimization,
                        std::vector<std::filesystem::path> includeDirectories,
                        std::vector<std::string> compilerArguments,
                        NativeTargetPolicy targetPolicy = {});

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &source() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] const std::vector<std::filesystem::path> &
  includeDirectories() const;
  [[nodiscard]] const std::vector<std::string> &compilerArguments() const;
  [[nodiscard]] const NativeTargetPolicy &targetPolicy() const;

private:
  std::string compilerExecutable;
  std::filesystem::path sourcePath;
  std::filesystem::path outputPath;
  CStandard cStandard;
  OptimizationLevel optimizationLevel;
  std::vector<std::filesystem::path> nativeIncludeDirectories;
  std::vector<std::string> nativeCompilerArguments;
  NativeTargetPolicy nativeTargetPolicy;
};

class NativeCppCompileRequest final {
public:
  NativeCppCompileRequest(std::string compiler, std::filesystem::path source,
                          std::filesystem::path output, CppStandard standard,
                          OptimizationLevel optimization,
                          std::vector<std::filesystem::path> includeDirectories,
                          std::vector<std::string> compilerArguments,
                          NativeTargetPolicy targetPolicy = {});

  [[nodiscard]] const std::string &compiler() const;
  [[nodiscard]] const std::filesystem::path &source() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] CppStandard standard() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] const std::vector<std::filesystem::path> &
  includeDirectories() const;
  [[nodiscard]] const std::vector<std::string> &compilerArguments() const;
  [[nodiscard]] const NativeTargetPolicy &targetPolicy() const;

private:
  std::string compilerExecutable;
  std::filesystem::path sourcePath;
  std::filesystem::path outputPath;
  CppStandard cppStandard;
  OptimizationLevel optimizationLevel;
  std::vector<std::filesystem::path> nativeIncludeDirectories;
  std::vector<std::string> nativeCompilerArguments;
  NativeTargetPolicy nativeTargetPolicy;
};

using NativeProcessResult = ProcessResult;

struct NativeInvocationOptions {
  bool captureSuccessfulOutput = false;
};

struct NativePreprocessingResult {
  std::vector<std::string> command;
  NativeProcessResult process;

  [[nodiscard]] bool succeeded() const { return process.succeeded(); }
};

class NativeToolchain final {
public:
  [[nodiscard]] std::vector<std::string>
  command(const NativeCompileRequest &request) const;

  [[nodiscard]] std::vector<std::string>
  command(const NativeLinkRequest &request) const;

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

  [[nodiscard]] NativePreprocessingResult
  preprocess(const NativeCCompileRequest &request,
             const std::filesystem::path &depfile,
             NativeInvocationOptions options = {}) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeCompileRequest &request,
         NativeInvocationOptions options = {}) const;

  [[nodiscard]] NativeProcessResult
  invoke(const NativeLinkRequest &request,
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
