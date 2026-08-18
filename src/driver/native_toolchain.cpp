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

std::string asciiLower(std::string_view value) {
  std::string lower(value);
  for (char &character : lower) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lower;
}

bool isMsvcOptimizationArgument(std::string_view lower) {
  if (lower == "/o" || lower == "/o1" || lower == "/o2" || lower == "/od" ||
      lower == "/og" || lower == "/oi" || lower == "/oi-" || lower == "/os" ||
      lower == "/ot" || lower == "/ox" || lower == "/oy" || lower == "/oy-") {
    return true;
  }
  return lower.size() == 4 && lower.starts_with("/ob") && lower.back() >= '0' &&
         lower.back() <= '3';
}

bool isMsvcForceLinkerArgument(std::string_view lower) {
  return lower == "/force" || lower.starts_with("/force:");
}

bool reservedForwardedLinkerComponent(std::string_view component) {
  const std::string lower = asciiLower(component);
  return component.starts_with('@') || component == "-o" ||
         (component.size() > 2 && component.starts_with("-o")) ||
         component == "--output" || component.starts_with("--output=") ||
         component == "-r" || component == "-i" ||
         component == "--relocatable" || component == "-shared" ||
         component == "--shared" || component == "-dynamiclib" ||
         component == "-dylib" || component == "-bundle" ||
         component == "--config" || component.starts_with("--config=") ||
         component == "-arch" || component.starts_with("-arch=") ||
         component == "-syslibroot" || component.starts_with("-syslibroot=") ||
         component == "--sysroot" || component.starts_with("--sysroot=") ||
         component == "-m" || component.starts_with("-m=") ||
         component == "--architecture" ||
         component.starts_with("--architecture=") || component == "-A" ||
         component == "-platform_version" || component == "--oformat" ||
         component.starts_with("--oformat=") || lower.starts_with("/machine:");
}

bool reservedForwardedLinkerArgument(std::string_view argument) {
  constexpr std::string_view joinedPrefix = "-Xlinker=";
  if (argument.starts_with(joinedPrefix)) {
    return reservedForwardedLinkerComponent(
        argument.substr(joinedPrefix.size()));
  }
  constexpr std::string_view listPrefix = "-Wl,";
  if (!argument.starts_with(listPrefix)) {
    return false;
  }
  std::string_view values = argument.substr(listPrefix.size());
  while (true) {
    const std::size_t separator = values.find(',');
    const std::string_view component = values.substr(0, separator);
    if (reservedForwardedLinkerComponent(component)) {
      return true;
    }
    if (separator == std::string_view::npos) {
      return false;
    }
    values.remove_prefix(separator + 1);
  }
}

} // namespace

bool isReservedNativeBuildArgument(std::string_view argument) {
  const std::string lower = asciiLower(argument);
  return reservedForwardedLinkerArgument(argument) || argument == "-Xclang" ||
         argument.starts_with("-Xclang=") || argument == "-cc1" ||
         argument == "-cc1as" || argument == "--driver-mode" ||
         argument.starts_with("--driver-mode=") || argument == "-Xlinker" ||
         argument.starts_with('@') || argument == "-o" ||
         (argument.size() > 2 && argument.starts_with("-o")) ||
         argument == "--output" || argument.starts_with("--output=") ||
         argument == "--options-file" ||
         argument.starts_with("--options-file=") || argument == "--config" ||
         argument.starts_with("--config=") || argument == "-x" ||
         (argument.size() > 2 && argument.starts_with("-x")) ||
         argument == "--language" || argument.starts_with("--language=") ||
         lower.starts_with("/tc") || lower.starts_with("/tp") ||
         lower.starts_with("/fe") ||
         (lower.starts_with("/fo") && !isMsvcForceLinkerArgument(lower)) ||
         lower.starts_with("/out:") || argument == "-c" || argument == "-E" ||
         argument == "-S" || argument == "-M" || argument == "-MM" ||
         argument == "-fsyntax-only" || argument == "--precompile" ||
         argument == "-emit-llvm" || argument == "-emit-ast" ||
         argument == "-analyze" || argument == "--analyze" ||
         argument == "-shared" || argument == "--shared" ||
         argument == "-dynamiclib" || argument == "-r" || argument == "-i" ||
         lower == "/c" || lower == "/e" || lower == "/p" || lower == "/ep" ||
         lower == "/zs" || lower == "/ld" || argument == "-std" ||
         argument.starts_with("-std=") || argument == "--std" ||
         argument.starts_with("--std=") || lower.starts_with("/std:") ||
         argument == "-ansi" || argument.starts_with("-O") ||
         isMsvcOptimizationArgument(lower) || argument == "--target" ||
         argument.starts_with("--target=") || argument == "-target" ||
         argument.starts_with("-target=") || argument == "-arch" ||
         argument.starts_with("-arch=") || argument == "-march" ||
         argument.starts_with("-march=") || argument == "-mcpu" ||
         argument.starts_with("-mcpu=") || argument == "-mfloat-abi" ||
         argument.starts_with("-mfloat-abi=") || argument == "-mfpu" ||
         argument.starts_with("-mfpu=") || argument == "-mthumb" ||
         argument == "-marm" || argument == "-m" ||
         argument == "--architecture" ||
         argument.starts_with("--architecture=") || argument == "-A" ||
         argument == "-platform_version" || argument == "--oformat" ||
         argument.starts_with("--oformat=") || argument == "-m32" ||
         argument == "-m64" || argument == "-EL" || argument == "-EB" ||
         argument == "-mlittle-endian" || argument == "-mbig-endian" ||
         argument == "--sysroot" || argument.starts_with("--sysroot=") ||
         argument == "-isysroot" || argument.starts_with("-isysroot=") ||
         argument == "-syslibroot" || argument.starts_with("-syslibroot=") ||
         argument.starts_with("-mabi=") || argument == "-fsigned-char" ||
         argument == "-funsigned-char" || argument == "-fno-signed-char" ||
         argument == "-fno-unsigned-char" || argument == "-fabi-version" ||
         argument.starts_with("-fabi-version=") ||
         argument == "-fshort-enums" || argument == "-fshort-wchar" ||
         argument == "-fno-short-wchar" || argument == "-fpack-struct" ||
         argument.starts_with("-fpack-struct=") ||
         lower.starts_with("/machine:") || lower.starts_with("/zp") ||
         lower == "/j" || lower.starts_with("/arch:") ||
         lower.starts_with("/favor:") || lower.starts_with("/vd") ||
         lower.starts_with("/vm") || lower.starts_with("/zc:wchar_t");
}

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
      !std::filesystem::is_regular_file(
          layout.runtimeInclude / "gti/runtime_failure.h", error) ||
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
#if defined(__APPLE__)
  // Apple's linker randomizes LC_UUID in its fast (-O0) mode, which would
  // make identical builds produce different executables. Equal inputs must
  // produce equal bytes for serial/parallel build equivalence and cache
  // verification.
  command.emplace_back("-Wl,-reproducible");
#endif
  return command;
}

namespace {

std::vector<std::string> nativeSourceCommand(
    const std::string &compiler, std::string standardFlagValue,
    OptimizationLevel optimization,
    const std::vector<std::filesystem::path> &includeDirectories,
    const std::vector<std::string> &compilerArguments,
    const std::filesystem::path &source, const std::filesystem::path &output,
    const std::filesystem::path *depfile) {
  std::vector<std::string> command{compiler, std::move(standardFlagValue),
                                   std::string(optimizationFlag(optimization))};
  for (const std::filesystem::path &directory : includeDirectories) {
    command.emplace_back("-I" + directory.string());
  }
  command.insert(command.end(), compilerArguments.begin(),
                 compilerArguments.end());
  if (depfile == nullptr) {
    command.emplace_back("-c");
  } else {
    command.emplace_back("-E");
    command.emplace_back("-MD");
    command.emplace_back("-MF");
    command.push_back(depfile->string());
  }
  command.push_back(source.string());
  command.emplace_back("-o");
  command.push_back(output.string());
  // Foreign C and C++ sources are separate translation units. The generated
  // GTI artifact's strict binary32 policy flags deliberately do not apply.
  return command;
}

} // namespace

std::vector<std::string>
NativeToolchain::command(const NativeCCompileRequest &request) const {
  return nativeSourceCommand(
      request.compiler(), cStandardFlag(request.standard()),
      request.optimization(), request.includeDirectories(),
      request.compilerArguments(), request.source(), request.output(), nullptr);
}

std::vector<std::string>
NativeToolchain::command(const NativeCppCompileRequest &request) const {
  return nativeSourceCommand(
      request.compiler(), std::string(standardFlag(request.standard())),
      request.optimization(), request.includeDirectories(),
      request.compilerArguments(), request.source(), request.output(), nullptr);
}

std::vector<std::string>
NativeToolchain::preprocessCommand(const NativeCCompileRequest &request,
                                   const std::filesystem::path &depfile) const {
  return nativeSourceCommand(
      request.compiler(), cStandardFlag(request.standard()),
      request.optimization(), request.includeDirectories(),
      request.compilerArguments(), request.source(), request.output(),
      &depfile);
}

std::vector<std::string>
NativeToolchain::preprocessCommand(const NativeCppCompileRequest &request,
                                   const std::filesystem::path &depfile) const {
  return nativeSourceCommand(
      request.compiler(), std::string(standardFlag(request.standard())),
      request.optimization(), request.includeDirectories(),
      request.compilerArguments(), request.source(), request.output(),
      &depfile);
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
