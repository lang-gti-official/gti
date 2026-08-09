#include "gti/diagnostic.h"
#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/project.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

#if !defined(GTI_VERSION)
#define GTI_VERSION "0.1.0"
#endif

constexpr std::string_view version = GTI_VERSION;

struct Options {
  std::filesystem::path input;
  std::filesystem::path output;
  std::optional<std::string> cxx;
  std::vector<std::string> compilerArguments;
  lang::CppStandard standard = lang::CppStandard::Cpp23;
  lang::OptimizationLevel optimization = lang::OptimizationLevel::O0;
  bool emitCpp = false;
  bool keepCpp = false;
  bool verbose = false;
};

struct ProjectOptions {
  std::optional<std::string> target;
  std::string profile = "dev";
  std::optional<std::string> cxx;
  std::optional<lang::CppStandard> standard;
  std::optional<lang::OptimizationLevel> optimization;
  std::optional<bool> keepCpp;
  bool verbose = false;
};

enum class ArgumentResult {
  Run,
  ExitSuccess,
  ExitFailure,
};

enum class ExitStatus : int {
  Success = 0,
  Usage = 64,
  Compilation = 65,
  Io = 74,
  ToolchainConfiguration = 78,
};

constexpr int exitCode(ExitStatus status) { return static_cast<int>(status); }

void printUsage(std::ostream &stream) {
  stream
      << "Usage: gti <source.gti> [options] [-- <c++ compiler arguments>]\n"
         "       gti build [target] [options]\n"
         "\n"
         "Direct compiler options:\n"
         "  -o, --output <path>  Set the executable or emitted C++ path.\n"
         "      --emit-cpp       Emit C++ without building an executable.\n"
         "      --keep-cpp       Keep the generated C++ beside the "
         "executable.\n"
         "      --cxx <path>     Select the native C++ compiler.\n"
         "      --std <version>  Select c++20 or c++23 (default: c++23).\n"
         "  -O0, -O1, -O2, -O3  Select the optimization level (default: -O0).\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "\n"
         "Project build options:\n"
         "      --profile <name> Select a manifest profile (default: dev).\n"
         "      --cxx <path>     Override the native C++ compiler.\n"
         "      --std <version>  Override c++20 or c++23.\n"
         "  -O0, -O1, -O2, -O3  Override the profile optimization level.\n"
         "      --keep-cpp       Retain generated C++ in the intermediate "
         "directory.\n"
         "      --no-keep-cpp    Remove generated C++ after a successful "
         "build.\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "\n"
         "General options:\n"
         "  -h, --help           Show this help text.\n"
         "      --version        Print the GTI compiler version.\n";
}

std::optional<lang::CppStandard> parseStandard(std::string_view value) {
  if (value == "c++20") {
    return lang::CppStandard::Cpp20;
  }
  if (value == "c++23") {
    return lang::CppStandard::Cpp23;
  }
  return std::nullopt;
}

std::optional<lang::OptimizationLevel>
parseOptimization(std::string_view value) {
  if (value == "-O0") {
    return lang::OptimizationLevel::O0;
  }
  if (value == "-O1") {
    return lang::OptimizationLevel::O1;
  }
  if (value == "-O2") {
    return lang::OptimizationLevel::O2;
  }
  if (value == "-O3") {
    return lang::OptimizationLevel::O3;
  }
  return std::nullopt;
}

std::filesystem::path
defaultExecutablePath(const std::filesystem::path &input) {
  std::string filename = input.stem().string();
#if defined(_WIN32)
  filename += ".exe";
#endif
  return input.parent_path() / filename;
}

std::filesystem::path defaultCppPath(const std::filesystem::path &input) {
  return input.parent_path() / (input.stem().string() + ".cpp");
}

ArgumentResult parseArguments(int argc, char *argv[], Options &options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];

    if (argument == "--") {
      for (++index; index < argc; ++index) {
        options.compilerArguments.emplace_back(argv[index]);
      }
      break;
    }
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "--version") {
      std::cout << "gti " << version << '\n';
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "-o" || argument == "--output") {
      if (++index >= argc) {
        std::cerr << "gti: missing path after " << argument << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.output = argv[index];
      continue;
    }
    if (argument == "--cxx") {
      if (++index >= argc) {
        std::cerr << "gti: missing compiler path after --cxx\n";
        return ArgumentResult::ExitFailure;
      }
      options.cxx = argv[index];
      continue;
    }
    if (argument == "--std") {
      if (++index >= argc) {
        std::cerr << "gti: missing version after --std\n";
        return ArgumentResult::ExitFailure;
      }
      const std::optional<lang::CppStandard> standard =
          parseStandard(argv[index]);
      if (!standard) {
        std::cerr << "gti: --std must be c++20 or c++23\n";
        return ArgumentResult::ExitFailure;
      }
      options.standard = *standard;
      continue;
    }
    if (argument == "--emit-cpp") {
      options.emitCpp = true;
      continue;
    }
    if (argument == "-O0" || argument == "-O1" || argument == "-O2" ||
        argument == "-O3") {
      options.optimization = *parseOptimization(argument);
      continue;
    }
    if (argument.starts_with("-O")) {
      std::cerr << "gti: optimization level must be -O0, -O1, -O2, or -O3\n";
      return ArgumentResult::ExitFailure;
    }
    if (argument == "--keep-cpp") {
      options.keepCpp = true;
      continue;
    }
    if (argument == "-v" || argument == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown option '" << argument << "'\n";
      return ArgumentResult::ExitFailure;
    }
    if (!options.input.empty()) {
      std::cerr << "gti: only one input file is supported\n";
      return ArgumentResult::ExitFailure;
    }
    options.input = argument;
  }

  if (options.input.empty()) {
    std::cerr << "gti: no input file\n";
    printUsage(std::cerr);
    return ArgumentResult::ExitFailure;
  }
  if (options.input.extension() != ".gti") {
    std::cerr << "gti: input file must use the .gti extension\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.emitCpp && options.keepCpp) {
    std::cerr << "gti: --emit-cpp and --keep-cpp cannot be used together\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.emitCpp && !options.compilerArguments.empty()) {
    std::cerr << "gti: native compiler arguments require executable output\n";
    return ArgumentResult::ExitFailure;
  }

  if (options.output.empty()) {
    options.output = options.emitCpp ? defaultCppPath(options.input)
                                     : defaultExecutablePath(options.input);
  }
  return ArgumentResult::Run;
}

ArgumentResult parseProjectArguments(int argc, char *argv[],
                                     ProjectOptions &options) {
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "--profile") {
      if (++index >= argc) {
        std::cerr << "gti: missing profile name after --profile\n";
        return ArgumentResult::ExitFailure;
      }
      options.profile = argv[index];
      continue;
    }
    if (argument == "--cxx") {
      if (++index >= argc) {
        std::cerr << "gti: missing compiler path after --cxx\n";
        return ArgumentResult::ExitFailure;
      }
      options.cxx = argv[index];
      continue;
    }
    if (argument == "--std") {
      if (++index >= argc) {
        std::cerr << "gti: missing version after --std\n";
        return ArgumentResult::ExitFailure;
      }
      options.standard = parseStandard(argv[index]);
      if (!options.standard) {
        std::cerr << "gti: --std must be c++20 or c++23\n";
        return ArgumentResult::ExitFailure;
      }
      continue;
    }
    if (argument == "-O0" || argument == "-O1" || argument == "-O2" ||
        argument == "-O3") {
      options.optimization = parseOptimization(argument);
      continue;
    }
    if (argument.starts_with("-O")) {
      std::cerr << "gti: optimization level must be -O0, -O1, -O2, or -O3\n";
      return ArgumentResult::ExitFailure;
    }
    if (argument == "--keep-cpp") {
      options.keepCpp = true;
      continue;
    }
    if (argument == "--no-keep-cpp") {
      options.keepCpp = false;
      continue;
    }
    if (argument == "-v" || argument == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (argument == "--") {
      std::cerr << "gti: project builds do not accept native arguments after "
                   "--\n";
      return ArgumentResult::ExitFailure;
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown build option '" << argument << "'\n";
      return ArgumentResult::ExitFailure;
    }
    if (options.target) {
      std::cerr << "gti: only one project target may be selected\n";
      return ArgumentResult::ExitFailure;
    }
    options.target = argument;
  }
  return ArgumentResult::Run;
}

std::string_view severityName(lang::DiagnosticSeverity severity) {
  switch (severity) {
  case lang::DiagnosticSeverity::Error:
    return "error";
  case lang::DiagnosticSeverity::Warning:
    return "warning";
  case lang::DiagnosticSeverity::Information:
    return "info";
  case lang::DiagnosticSeverity::Hint:
    return "hint";
  }
  return "error";
}

void reportLocation(const lang::SourceSpan &span, std::string_view level,
                    std::string_view message,
                    const lang::SourceManager &sources) {
  const lang::SourceLocation location = sources.locate(span);
  std::cerr << (span.source.empty() ? "<source>" : span.source) << ':'
            << location.line << ':' << location.column << ": " << level << ": "
            << message << '\n';

  const std::string_view sourceLine = sources.line(span);
  if (sourceLine.empty()) {
    return;
  }

  const std::string lineNumber = std::to_string(location.line);
  std::cerr << ' ' << lineNumber << " | " << sourceLine << '\n';
  std::cerr << std::string(lineNumber.size() + 2, ' ') << "| ";

  const std::size_t start = std::min(span.start, location.lineEnd);
  for (std::size_t index = location.lineStart; index < start; ++index) {
    const std::string *source = sources.find(span.source);
    std::cerr << (source != nullptr && (*source)[index] == '\t' ? '\t' : ' ');
  }
  const std::size_t end =
      std::min(location.lineEnd,
               std::max(span.end, std::min(span.start + 1, location.lineEnd)));
  std::cerr << '^';
  if (end > start + 1) {
    std::cerr << std::string(end - start - 1, '~');
  }
  std::cerr << '\n';
}

void reportDiagnostic(const lang::Diagnostic &diagnostic,
                      const lang::SourceManager &sources) {
  const lang::SourceLocation location = sources.locate(diagnostic.primary);
  std::cerr << (diagnostic.primary.source.empty() ? "<source>"
                                                  : diagnostic.primary.source)
            << ':' << location.line << ':' << location.column << ": "
            << severityName(diagnostic.severity);
  if (!diagnostic.code.empty()) {
    std::cerr << '[' << diagnostic.code << ']';
  }
  std::cerr << ": " << diagnostic.message << '\n';

  const std::string_view sourceLine = sources.line(diagnostic.primary);
  if (!sourceLine.empty()) {
    const std::string lineNumber = std::to_string(location.line);
    std::cerr << ' ' << lineNumber << " | " << sourceLine << '\n';
    std::cerr << std::string(lineNumber.size() + 2, ' ') << "| ";
    const std::string *source = sources.find(diagnostic.primary.source);
    const std::size_t start =
        std::min(diagnostic.primary.start, location.lineEnd);
    for (std::size_t index = location.lineStart; index < start; ++index) {
      std::cerr << (source != nullptr && (*source)[index] == '\t' ? '\t' : ' ');
    }
    const std::size_t end = std::min(
        location.lineEnd,
        std::max(diagnostic.primary.end,
                 std::min(diagnostic.primary.start + 1, location.lineEnd)));
    std::cerr << '^';
    if (end > start + 1) {
      std::cerr << std::string(end - start - 1, '~');
    }
    std::cerr << '\n';
  }

  for (const lang::RelatedDiagnostic &related : diagnostic.related) {
    reportLocation(related.span, "note", related.message, sources);
  }
  for (const std::string &hint : diagnostic.hints) {
    std::cerr << "help: " << hint << '\n';
  }
  for (const lang::FixIt &fix : diagnostic.fixes) {
    std::cerr << "help: " << fix.message << '\n';
  }
}

void reportDiagnostics(const std::vector<lang::Diagnostic> &diagnostics,
                       const lang::SourceManager &sources) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    reportDiagnostic(diagnostic, sources);
  }
}

bool writeFile(const std::filesystem::path &path, std::string_view contents) {
  const lang::driver::ArtifactWriteStatus status =
      lang::driver::writeArtifact(path, contents);
  if (status == lang::driver::ArtifactWriteStatus::Success) {
    return true;
  }
  std::cerr << (status == lang::driver::ArtifactWriteStatus::OpenFailure
                    ? "gti: failed to open output file: "
                    : "gti: failed to write output file: ")
            << path << '\n';
  return false;
}

void reportCapturedOutput(std::string_view output, std::string_view heading) {
  if (output.empty()) {
    return;
  }
  std::cerr << heading << output;
  if (output.back() != '\n') {
    std::cerr << '\n';
  }
}

int reportCompilationFailure(
    const lang::driver::CompilationResult &compilation) {
  if (!compilation.succeeded()) {
    if (compilation.status ==
        lang::driver::CompilationStatus::FrontendFailure) {
      reportDiagnostics(compilation.diagnostics, compilation.sources);
    } else {
      std::cerr << "gti: internal compiler error: MIR verification failed";
      if (!compilation.mirErrors.empty()) {
        std::cerr << ": " << compilation.mirErrors.front().message;
      }
      std::cerr << '\n';
    }
  }
  return exitCode(ExitStatus::Compilation);
}

int reportBuildResult(const lang::driver::ExecutableBuildResult &result,
                      bool verbose) {
  if (verbose && !result.nativeCommand.empty()) {
    std::cerr << lang::driver::renderCommand(result.nativeCommand) << '\n';
  }
  if (result.driverDiagnostic) {
    std::cerr << *result.driverDiagnostic << '\n';
  }
  if (result.nativeProcess && (verbose || !result.nativeProcess->succeeded())) {
    reportCapturedOutput(result.nativeProcess->output,
                         result.nativeProcess->succeeded()
                             ? std::string_view{}
                             : "gti: native C++ compiler diagnostics:\n");
  }

  switch (result.status) {
  case lang::driver::ExecutableBuildStatus::Success:
    return exitCode(ExitStatus::Success);
  case lang::driver::ExecutableBuildStatus::CompilationFailure:
    return reportCompilationFailure(result.compilation);
  case lang::driver::ExecutableBuildStatus::OutputDirectoryFailure:
    return exitCode(ExitStatus::Io);
  case lang::driver::ExecutableBuildStatus::GeneratedArtifactFailure:
    std::cerr << (result.artifactWriteStatus ==
                          lang::driver::ArtifactWriteStatus::OpenFailure
                      ? "gti: failed to open generated C++ file: "
                      : "gti: failed to write generated C++ file: ")
              << result.generatedSource << '\n';
    return exitCode(ExitStatus::Io);
  case lang::driver::ExecutableBuildStatus::ToolchainConfigurationFailure:
    std::cerr
        << (result.resourceError ==
                    lang::driver::ToolchainResourceError::RuntimeFilesMissing
                ? "gti: native runtime files were not found\n"
                : "gti: C++20 expected compatibility header was not "
                  "found\n");
    return exitCode(ExitStatus::ToolchainConfiguration);
  case lang::driver::ExecutableBuildStatus::NativeCompilerFailure:
    std::cerr << "gti: native C++ compiler failed with exit code "
              << result.nativeProcess->exitCode << '\n'
              << "gti: generated C++ retained at "
              << result.generatedSource.string() << '\n';
    return result.nativeProcess->exitCode;
  case lang::driver::ExecutableBuildStatus::ArtifactPublicationFailure:
    std::cerr << "gti: generated C++ retained at "
              << result.generatedSource.string() << '\n';
    return exitCode(ExitStatus::Io);
  }
  return exitCode(ExitStatus::Compilation);
}

int runDirect(const Options &options, const char *driver) {
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);

  if (options.emitCpp) {
    const lang::driver::CompilationResult compilation =
        lang::driver::compileToCpp(lang::driver::CompilationRequest(
            options.input, toolchain.standardLibrary, lang::TargetInfo::host(),
            options.optimization, options.standard));
    if (!compilation.succeeded()) {
      return reportCompilationFailure(compilation);
    }
    const lang::BackendArtifact &artifact = *compilation.artifact;
    if (!writeFile(options.output, artifact.contents)) {
      return exitCode(ExitStatus::Io);
    }
    std::cout << "Emitted " << options.output << '\n';
    return exitCode(ExitStatus::Success);
  }

  const std::filesystem::path generatedSource =
      options.keepCpp
          ? std::filesystem::path(options.output.string() + ".gti.cpp")
          : lang::driver::temporaryCppPath(options.input);
  lang::driver::NativeInputs nativeInputs;
  nativeInputs.trailingArguments = options.compilerArguments;
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              options.input, toolchain.standardLibrary,
              lang::TargetInfo::host(), options.optimization, options.standard),
          toolchain, generatedSource, options.output,
          lang::driver::discoverNativeCompiler(options.cxx),
          std::move(nativeInputs), options.keepCpp, false, options.verbose));
  const int status = reportBuildResult(result, options.verbose);
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }

  std::cout << "Built " << options.output << '\n';
  if (options.keepCpp) {
    std::cout << "Kept C++ " << generatedSource << '\n';
  }
  return exitCode(ExitStatus::Success);
}

int runProject(const ProjectOptions &options, const char *driver) {
  std::error_code error;
  const std::filesystem::path currentDirectory =
      std::filesystem::current_path(error);
  if (error) {
    std::cerr << "gti: failed to resolve the current directory: "
              << error.message() << '\n';
    return exitCode(ExitStatus::Io);
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = options.optimization;
  overrides.cppStandard = options.standard;
  overrides.keepCpp = options.keepCpp;
  lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          currentDirectory, options.target, options.profile,
          lang::TargetInfo::host(), std::move(overrides)));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              plan.entry(), toolchain.standardLibrary, plan.target(),
              plan.optimization(), plan.cppStandard()),
          toolchain, plan.generatedSource(), plan.output(),
          lang::driver::discoverNativeCompiler(options.cxx), {}, plan.keepCpp(),
          true, options.verbose));
  const int status = reportBuildResult(result, options.verbose);
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }

  std::cout << "Built " << plan.output() << '\n';
  if (plan.keepCpp()) {
    std::cout << "Kept C++ " << plan.generatedSource() << '\n';
  }
  return exitCode(ExitStatus::Success);
}

bool isReservedProjectCommand(std::string_view command) {
  return command == "check" || command == "run" || command == "test" ||
         command == "clean" || command == "fetch" || command == "metadata";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc > 1 && std::string_view(argv[1]) == "build") {
    ProjectOptions options;
    const ArgumentResult argumentResult =
        parseProjectArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runProject(options, argv[0]);
  }

  if (argc > 1 && isReservedProjectCommand(argv[1])) {
    std::cerr << "gti: project command '" << argv[1]
              << "' is not implemented yet\n";
    return exitCode(ExitStatus::Usage);
  }
  if (argc > 1 && argv[1][0] != '-' &&
      std::filesystem::path(argv[1]).extension() != ".gti") {
    std::cerr << "gti: unknown command '" << argv[1] << "'\n";
    return exitCode(ExitStatus::Usage);
  }

  Options options;
  const ArgumentResult argumentResult = parseArguments(argc, argv, options);
  if (argumentResult == ArgumentResult::ExitSuccess) {
    return exitCode(ExitStatus::Success);
  }
  if (argumentResult == ArgumentResult::ExitFailure) {
    return exitCode(ExitStatus::Usage);
  }
  return runDirect(options, argv[0]);
}
