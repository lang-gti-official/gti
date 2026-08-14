#include "gti/diagnostic.h"
#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/driver/project.h"
#include "gti/support.h"
#include "project_presentation.h"

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
  std::optional<std::filesystem::path> timeTrace;
  std::vector<std::string> compilerArguments;
  lang::CppStandard standard = lang::CppStandard::Cpp23;
  lang::OptimizationLevel optimization = lang::OptimizationLevel::O0;
  lang::ExecutionProfile executionProfile =
      lang::ExecutionProfile::SingleThreaded;
  bool executionProfileSelected = false;
  bool emitCpp = false;
  bool emitNativeHeader = false;
  bool keepCpp = false;
  bool verbose = false;
};

enum class ProjectCommand {
  Build,
  Check,
  Run,
  Test,
};

struct ProjectOptions {
  ProjectCommand command = ProjectCommand::Build;
  std::optional<std::string> package;
  std::optional<std::string> target;
  std::string profile = "dev";
  bool profileSelected = false;
  std::optional<std::string> cxx;
  std::optional<std::string> cc;
  std::optional<lang::CppStandard> standard;
  std::optional<lang::OptimizationLevel> optimization;
  std::optional<lang::ExecutionProfile> executionProfile;
  std::optional<bool> keepCpp;
  std::vector<std::string> programArguments;
  bool verbose = false;
  bool useCache = true;
};

struct ScaffoldOptions {
  lang::driver::ProjectScaffoldMode mode =
      lang::driver::ProjectScaffoldMode::NewPackage;
  std::filesystem::path destination;
  std::optional<std::string> packageName;
};

struct MetadataOptions {
  std::optional<std::string> package;
};

struct FormatConfigInitOptions {
  std::filesystem::path destination = ".";
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
         "       gti check [target] [--profile <name> | --release]\n"
         "       gti run [target] [options] [-- <program arguments>]\n"
         "       gti test [target] [options]\n"
         "       gti new <path> [--name <name>]\n"
         "       gti init [path] [--name <name>]\n"
         "       gti clean\n"
         "       gti metadata [--format json] [--package <name>]\n"
         "       gti format init [directory]\n"
         "\n"
         "Direct compiler options:\n"
         "  -o, --output <path>  Set the executable or emitted artifact path.\n"
         "      --emit-cpp       Emit C++ without building an executable.\n"
         "      --emit-native-header\n"
         "                       Emit a C17 and C++20/C++23 compatible C ABI "
         "header.\n"
         "      --keep-cpp       Keep the generated C++ beside the "
         "executable.\n"
         "      --cxx <path>     Select the native C++ compiler.\n"
         "      --std <version>  Select c++20 or c++23 (default: c++23).\n"
         "      --execution-profile <name>\n"
         "                       Select single-threaded (default) or "
         "concurrent semantics.\n"
         "  -O0, -O1, -O2, -O3  Select the optimization level (default: -O0).\n"
         "      --time-trace <path>  Write a compile-time profile as Chrome "
         "Trace JSON.\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "\n"
         "Project build options:\n"
         "      --package <name> Select a package from the active workspace.\n"
         "      --profile <name> Select a manifest profile (default: dev).\n"
         "      --release        Shorthand for --profile release.\n"
         "      --cxx <path>     Override the native C++ compiler.\n"
         "      --cc <path>      Override the native C compiler.\n"
         "      --std <version>  Override c++20 or c++23.\n"
         "      --execution-profile <name>\n"
         "                       Override single-threaded or concurrent "
         "semantics.\n"
         "  -O0, -O1, -O2, -O3  Override the profile optimization level.\n"
         "      --keep-cpp       Retain generated C++ in the intermediate "
         "directory.\n"
         "      --no-keep-cpp    Remove generated C++ after a successful "
         "build.\n"
         "      --no-cache       Build without reading or updating the "
         "project cache.\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "\n"
         "Project commands:\n"
         "  new                  Create a new executable package.\n"
         "  init                 Initialize an existing directory as a "
         "package.\n"
         "  check                Analyze a target without C++ emission or "
         "native compilation.\n"
         "  run                  Build and run a target; arguments after -- "
         "belong to the program.\n"
         "  test                 Build and run all test targets, or one named "
         "test target.\n"
         "  clean                Remove the active workspace's build/gti "
         "subtree.\n"
         "  metadata             Print deterministic project metadata as "
         "JSON.\n"
         "  format init          Create the default .gti-format in an "
         "existing directory.\n"
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

bool parseExecutionProfileOption(std::string_view value,
                                 lang::ExecutionProfile &profile) {
  const std::optional<lang::ExecutionProfile> parsed =
      lang::parseExecutionProfile(value);
  if (!parsed) {
    return false;
  }
  profile = *parsed;
  return true;
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

std::filesystem::path
defaultNativeHeaderPath(const std::filesystem::path &input) {
  return input.parent_path() / (input.stem().string() + ".native.h");
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
    if (argument == "--execution-profile") {
      if (++index >= argc) {
        std::cerr << "gti: missing name after --execution-profile\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.executionProfileSelected) {
        std::cerr << "gti: --execution-profile may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      if (!parseExecutionProfileOption(argv[index], options.executionProfile)) {
        std::cerr << "gti: --execution-profile must be single-threaded or "
                     "concurrent\n";
        return ArgumentResult::ExitFailure;
      }
      options.executionProfileSelected = true;
      continue;
    }
    if (argument == "--emit-cpp") {
      options.emitCpp = true;
      continue;
    }
    if (argument == "--emit-native-header") {
      options.emitNativeHeader = true;
      continue;
    }
    if (argument == "--time-trace") {
      if (++index >= argc) {
        std::cerr << "gti: missing path after --time-trace\n";
        return ArgumentResult::ExitFailure;
      }
      options.timeTrace = argv[index];
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
  if ((options.emitCpp || options.emitNativeHeader) && options.keepCpp) {
    std::cerr << "gti: emission modes and --keep-cpp cannot be used "
                 "together\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.emitCpp && options.emitNativeHeader) {
    std::cerr << "gti: --emit-cpp and --emit-native-header cannot be used "
                 "together\n";
    return ArgumentResult::ExitFailure;
  }
  if ((options.emitCpp || options.emitNativeHeader) &&
      !options.compilerArguments.empty()) {
    std::cerr << "gti: native compiler arguments require executable output\n";
    return ArgumentResult::ExitFailure;
  }
  for (const std::string &argument : options.compilerArguments) {
    if (!lang::driver::isReservedNativeBuildArgument(argument)) {
      continue;
    }
    std::cerr << "gti: native compiler argument '" << argument
              << "' cannot override the resolved language standard, "
                 "optimization, target/data layout, output, response-file "
                 "inputs, or executable build mode\n";
    return ArgumentResult::ExitFailure;
  }

  if (options.output.empty()) {
    if (options.emitCpp) {
      options.output = defaultCppPath(options.input);
    } else if (options.emitNativeHeader) {
      options.output = defaultNativeHeaderPath(options.input);
    } else {
      options.output = defaultExecutablePath(options.input);
    }
  }
  return ArgumentResult::Run;
}

std::string_view projectCommandName(ProjectCommand command) {
  switch (command) {
  case ProjectCommand::Build:
    return "build";
  case ProjectCommand::Check:
    return "check";
  case ProjectCommand::Run:
    return "run";
  case ProjectCommand::Test:
    return "test";
  }
  return "build";
}

bool buildsExecutable(ProjectCommand command) {
  return command == ProjectCommand::Build || command == ProjectCommand::Run ||
         command == ProjectCommand::Test;
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
      if (options.profileSelected) {
        std::cerr << "gti: --profile cannot be combined with --release or "
                     "another --profile\n";
        return ArgumentResult::ExitFailure;
      }
      options.profile = argv[index];
      options.profileSelected = true;
      continue;
    }
    if (argument == "--package") {
      if (++index >= argc) {
        std::cerr << "gti: missing package name after --package\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.package) {
        std::cerr << "gti: --package may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.package = argv[index];
      continue;
    }
    if (argument == "--release") {
      if (options.profileSelected) {
        std::cerr << "gti: --release cannot be combined with --profile or "
                     "another --release\n";
        return ArgumentResult::ExitFailure;
      }
      options.profile = "release";
      options.profileSelected = true;
      continue;
    }
    if (argument == "--cxx") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --cxx is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      if (++index >= argc) {
        std::cerr << "gti: missing compiler path after --cxx\n";
        return ArgumentResult::ExitFailure;
      }
      options.cxx = argv[index];
      continue;
    }
    if (argument == "--cc") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --cc is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      if (++index >= argc) {
        std::cerr << "gti: missing compiler path after --cc\n";
        return ArgumentResult::ExitFailure;
      }
      options.cc = argv[index];
      continue;
    }
    if (argument == "--std") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --std is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
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
    if (argument == "--execution-profile") {
      if (++index >= argc) {
        std::cerr << "gti: missing name after --execution-profile\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.executionProfile) {
        std::cerr << "gti: --execution-profile may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      lang::ExecutionProfile profile;
      if (!parseExecutionProfileOption(argv[index], profile)) {
        std::cerr << "gti: --execution-profile must be single-threaded or "
                     "concurrent\n";
        return ArgumentResult::ExitFailure;
      }
      options.executionProfile = profile;
      continue;
    }
    if (argument == "-O0" || argument == "-O1" || argument == "-O2" ||
        argument == "-O3") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: optimization overrides are not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.optimization = parseOptimization(argument);
      continue;
    }
    if (argument.starts_with("-O")) {
      std::cerr << "gti: optimization level must be -O0, -O1, -O2, or -O3\n";
      return ArgumentResult::ExitFailure;
    }
    if (argument == "--keep-cpp") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --keep-cpp is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.keepCpp = true;
      continue;
    }
    if (argument == "--no-keep-cpp") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --no-keep-cpp is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.keepCpp = false;
      continue;
    }
    if (argument == "--no-cache") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --no-cache is not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.useCache = false;
      continue;
    }
    if (argument == "-v" || argument == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (argument == "--") {
      if (options.command != ProjectCommand::Run) {
        std::cerr << "gti: gti " << projectCommandName(options.command)
                  << " does not accept arguments after --\n";
        return ArgumentResult::ExitFailure;
      }
      for (++index; index < argc; ++index) {
        options.programArguments.emplace_back(argv[index]);
      }
      break;
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown " << projectCommandName(options.command)
                << " option '" << argument << "'\n";
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

ArgumentResult parseCleanArguments(int argc, char *argv[]) {
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    std::cerr << "gti: unknown clean option '" << argument << "'\n";
    return ArgumentResult::ExitFailure;
  }
  return ArgumentResult::Run;
}

ArgumentResult parseMetadataArguments(int argc, char *argv[],
                                      MetadataOptions &options) {
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "--format") {
      if (++index >= argc) {
        std::cerr << "gti: missing format name after --format\n";
        return ArgumentResult::ExitFailure;
      }
      if (std::string_view(argv[index]) != "json") {
        std::cerr << "gti: metadata format must be json\n";
        return ArgumentResult::ExitFailure;
      }
      continue;
    }
    if (argument == "--package") {
      if (++index >= argc) {
        std::cerr << "gti: missing package name after --package\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.package) {
        std::cerr << "gti: --package may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.package = argv[index];
      continue;
    }
    std::cerr << "gti: unknown metadata option '" << argument << "'\n";
    return ArgumentResult::ExitFailure;
  }
  return ArgumentResult::Run;
}

ArgumentResult
parseFormatConfigInitArguments(int argc, char *argv[],
                               FormatConfigInitOptions &options) {
  if (argc < 3) {
    std::cerr << "gti: gti format requires the 'init' subcommand\n";
    return ArgumentResult::ExitFailure;
  }
  const std::string_view subcommand = argv[2];
  if (subcommand == "-h" || subcommand == "--help") {
    printUsage(std::cout);
    return ArgumentResult::ExitSuccess;
  }
  if (subcommand != "init") {
    std::cerr << "gti: unknown format subcommand '" << subcommand << "'\n";
    return ArgumentResult::ExitFailure;
  }

  bool destinationSelected = false;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown format init option '" << argument << "'\n";
      return ArgumentResult::ExitFailure;
    }
    if (destinationSelected) {
      std::cerr << "gti: only one format configuration destination may be "
                   "specified\n";
      return ArgumentResult::ExitFailure;
    }
    options.destination = argument;
    destinationSelected = true;
  }
  return ArgumentResult::Run;
}

ArgumentResult parseScaffoldArguments(int argc, char *argv[],
                                      ScaffoldOptions &options) {
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "--name") {
      if (++index >= argc) {
        std::cerr << "gti: missing package name after --name\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.packageName) {
        std::cerr << "gti: --name may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.packageName = argv[index];
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown "
                << (options.mode ==
                            lang::driver::ProjectScaffoldMode::NewPackage
                        ? "new"
                        : "init")
                << " option '" << argument << "'\n";
      return ArgumentResult::ExitFailure;
    }
    if (!options.destination.empty()) {
      std::cerr << "gti: only one project destination may be specified\n";
      return ArgumentResult::ExitFailure;
    }
    options.destination = argument;
  }

  if (options.mode == lang::driver::ProjectScaffoldMode::NewPackage &&
      options.destination.empty()) {
    std::cerr << "gti: gti new requires a destination path\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.destination.empty()) {
    options.destination = ".";
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
    switch (compilation.status) {
    case lang::driver::CompilationStatus::FrontendFailure:
    case lang::driver::CompilationStatus::BackendFailure:
      reportDiagnostics(compilation.diagnostics, compilation.sources);
      break;
    case lang::driver::CompilationStatus::MirVerificationFailure:
      std::cerr << "gti: internal compiler error: MIR verification failed";
      if (!compilation.mirErrors.empty()) {
        std::cerr << ": " << compilation.mirErrors.front().message;
      }
      std::cerr << '\n';
      break;
    case lang::driver::CompilationStatus::Success:
      break;
    }
  }
  return exitCode(ExitStatus::Compilation);
}

int reportBuildResult(const lang::driver::ExecutableBuildResult &result,
                      bool verbose) {
  if (result.cache.warning) {
    std::cerr << *result.cache.warning << '\n';
  }
  if (verbose &&
      result.cache.status != lang::driver::BuildCacheStatus::NotConfigured) {
    std::cerr << "gti: cache ";
    switch (result.cache.status) {
    case lang::driver::BuildCacheStatus::NotConfigured:
      break;
    case lang::driver::BuildCacheStatus::Hit:
      std::cerr << "hit";
      break;
    case lang::driver::BuildCacheStatus::Miss:
      std::cerr << "miss";
      break;
    case lang::driver::BuildCacheStatus::RecoveredCorruption:
      std::cerr << "recovered";
      break;
    case lang::driver::BuildCacheStatus::Bypassed:
      std::cerr << "bypassed";
      break;
    }
    if (!result.cache.key.empty()) {
      std::cerr << ' ' << result.cache.key;
    }
    if (result.cache.detail) {
      std::cerr << " (" << *result.cache.detail << ')';
    }
    std::cerr << '\n';
  }
  for (const lang::driver::NativeCCompilationResult &compilation :
       result.cCompilations) {
    if (verbose) {
      std::cerr << lang::driver::renderCommand(compilation.command) << '\n';
    }
    if (compilation.process.driverDiagnostic) {
      std::cerr << *compilation.process.driverDiagnostic << '\n';
    }
    if (verbose || !compilation.process.succeeded()) {
      const std::string prefix =
          compilation.process.succeeded()
              ? std::string{}
              : "gti: native C compiler diagnostics for '" +
                    compilation.source.string() + "':\n";
      reportCapturedOutput(compilation.process.output, prefix);
    }
  }
  for (const lang::driver::NativeCppCompilationResult &compilation :
       result.cppCompilations) {
    if (verbose) {
      std::cerr << lang::driver::renderCommand(compilation.command) << '\n';
    }
    if (compilation.process.driverDiagnostic) {
      std::cerr << *compilation.process.driverDiagnostic << '\n';
    }
    if (verbose || !compilation.process.succeeded()) {
      const std::string prefix =
          compilation.process.succeeded()
              ? std::string{}
              : "gti: native C++ compiler diagnostics for '" +
                    compilation.source.string() + "':\n";
      reportCapturedOutput(compilation.process.output, prefix);
    }
  }
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
    if (result.resourceError) {
      std::cerr
          << (*result.resourceError ==
                      lang::driver::ToolchainResourceError::RuntimeFilesMissing
                  ? "gti: native runtime files were not found\n"
                  : "gti: C++20 expected compatibility header was not "
                    "found\n");
    }
    return exitCode(ExitStatus::ToolchainConfiguration);
  case lang::driver::ExecutableBuildStatus::NativeCCompilerFailure: {
    const lang::driver::NativeCCompilationResult &compilation =
        result.cCompilations.back();
    std::cerr << "gti: native C compiler failed for "
              << compilation.source.string() << " with exit code "
              << compilation.process.exitCode << '\n'
              << "gti: generated C++ retained at "
              << result.generatedSource.string() << '\n';
    return compilation.process.exitCode;
  }
  case lang::driver::ExecutableBuildStatus::NativeCppCompilerFailure: {
    const lang::driver::NativeCppCompilationResult &compilation =
        result.cppCompilations.back();
    std::cerr << "gti: native C++ compiler failed for "
              << compilation.source.string() << " with exit code "
              << compilation.process.exitCode << '\n'
              << "gti: generated C++ retained at "
              << result.generatedSource.string() << '\n';
    return compilation.process.exitCode;
  }
  case lang::driver::ExecutableBuildStatus::NativeObjectPublicationFailure:
    std::cerr << "gti: generated C++ retained at "
              << result.generatedSource.string() << '\n';
    return exitCode(ExitStatus::Io);
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
  case lang::driver::ExecutableBuildStatus::ArtifactPathConflict:
    return exitCode(ExitStatus::Usage);
  }
  return exitCode(ExitStatus::Compilation);
}

int runDirect(const Options &options, const char *driver) {
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = options.executionProfile;

  if (options.emitCpp || options.emitNativeHeader) {
    const lang::driver::CompilationRequest request(
        options.input, toolchain.standardLibrary, target, options.optimization,
        options.standard);
    const lang::driver::CompilationResult compilation =
        options.emitNativeHeader ? lang::driver::compileToNativeHeader(request)
                                 : lang::driver::compileToCpp(request);
    if (!compilation.succeeded()) {
      return reportCompilationFailure(compilation);
    }
    const lang::BackendArtifact &artifact = *compilation.artifact;
    if (const std::optional<std::filesystem::path> collision =
            lang::driver::findLoadedSourceCollision(options.output,
                                                    compilation.sources)) {
      std::cerr << "gti: refusing to overwrite loaded source '"
                << collision->string() << "' with emitted "
                << (options.emitNativeHeader ? "native header" : "C++ output")
                << " '" << options.output.string() << "'\n";
      return exitCode(ExitStatus::Usage);
    }
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
              options.input, toolchain.standardLibrary, target,
              options.optimization, options.standard),
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

std::optional<std::filesystem::path> workingDirectory() {
  std::error_code error;
  const std::filesystem::path currentDirectory =
      std::filesystem::current_path(error);
  if (error) {
    std::cerr << "gti: failed to resolve the current directory: "
              << error.message() << '\n';
    return std::nullopt;
  }
  return currentDirectory;
}

void reportProjectPlan(const lang::driver::ProjectBuildPlan &plan,
                       ProjectCommand command) {
  std::cerr << "gti: package " << plan.packageName() << "@"
            << plan.packageVersion() << ", target " << plan.targetName() << " ["
            << plan.profileName() << ", "
            << lang::driver::targetTriple(plan.target()) << "]\n"
            << "gti: configuration -O"
            << lang::cli::optimizationNumber(plan.optimization()) << ", "
            << lang::cli::cppStandardName(plan.cppStandard())
            << ", execution-profile="
            << lang::executionProfileName(plan.target().executionProfile)
            << ", keep-cpp=" << (plan.keepCpp() ? "true" : "false") << '\n';
  if (command == ProjectCommand::Check) {
    std::cerr << "gti: source " << plan.entry().string() << '\n';
  } else {
    std::cerr << "gti: output " << plan.output().string() << '\n';
  }
}

void reportProjectSuccess(std::ostream &stream, std::string_view action,
                          const lang::driver::ProjectBuildPlan &plan) {
  stream << action << ' ' << plan.targetName() << " [" << plan.profileName()
         << ", " << lang::driver::targetTriple(plan.target()) << "] -> "
         << plan.output().string() << '\n';
}

int buildProjectPlan(const lang::driver::ProjectBuildPlan &plan,
                     const ProjectOptions &options,
                     const lang::driver::ToolchainLayout &toolchain) {
  std::optional<std::string> cCompiler;
  if (!plan.nativeInputs().cSources.empty()) {
    cCompiler = lang::driver::discoverCCompiler(options.cc);
  }
  std::optional<lang::driver::BuildCachePolicy> cache;
  if (options.useCache) {
    cache = lang::driver::BuildCachePolicy{
        .root = plan.workspaceRoot() / "build" / "gti" / "cache",
        .sourceRoot = plan.workspaceRoot(),
        .compilerIdentity = std::string(version),
        .projectModelIdentity = plan.projectModelIdentity(),
    };
  } else if (options.verbose) {
    std::cerr << "gti: cache disabled by --no-cache\n";
  }

  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              plan.entry(), toolchain.standardLibrary, plan.target(),
              plan.optimization(), plan.cppStandard(), plan.packageSources()),
          toolchain, plan.generatedSource(), plan.output(),
          lang::driver::discoverNativeCompiler(options.cxx),
          plan.nativeInputs(), plan.keepCpp(), true, options.verbose,
          lang::driver::ManagedOutputPolicy{.trustedRoot = plan.workspaceRoot(),
                                            .outputRoot = plan.workspaceRoot() /
                                                          "build" / "gti"},
          std::move(cCompiler), std::move(cache)));
  return reportBuildResult(result, options.verbose);
}

int runProject(const ProjectOptions &options, const char *driver) {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = options.optimization;
  overrides.cppStandard = options.standard;
  overrides.executionProfile = options.executionProfile;
  overrides.keepCpp = options.keepCpp;
  lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          *currentDirectory, options.target, options.profile,
          lang::TargetInfo::host(), std::move(overrides), options.package));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
  if (options.command == ProjectCommand::Run &&
      plan.targetKind() != lang::driver::ProjectTargetKind::Executable) {
    std::cerr << "gti: target '" << plan.targetName()
              << "' is a test target; use gti test " << plan.targetName()
              << "\n";
    return exitCode(ExitStatus::Usage);
  }
  if (options.verbose) {
    reportProjectPlan(plan, options.command);
  }
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);

  if (options.command == ProjectCommand::Check) {
    const lang::driver::CheckResult result =
        lang::driver::checkCompilation(lang::driver::CompilationRequest(
            plan.entry(), toolchain.standardLibrary, plan.target(),
            plan.optimization(), plan.cppStandard(), plan.packageSources()));
    if (!result.succeeded()) {
      reportDiagnostics(result.diagnostics, result.sources);
      return exitCode(ExitStatus::Compilation);
    }
    std::cout << "Checked " << plan.targetName() << " [" << plan.profileName()
              << ", " << lang::driver::targetTriple(plan.target()) << "] -> "
              << plan.entry().string() << '\n';
    return exitCode(ExitStatus::Success);
  }

  const int status = buildProjectPlan(plan, options, toolchain);
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }

  std::ostream &statusStream =
      options.command == ProjectCommand::Run ? std::cerr : std::cout;
  reportProjectSuccess(statusStream, "Built", plan);
  if (plan.keepCpp()) {
    statusStream << "Kept C++ " << plan.generatedSource().string() << '\n';
  }

  if (options.command == ProjectCommand::Run) {
    std::vector<std::string> command{plan.output().string()};
    command.insert(command.end(), options.programArguments.begin(),
                   options.programArguments.end());
    statusStream << "Running " << plan.output().string() << '\n';
    const lang::driver::ProcessResult process = lang::driver::invokeProcess(
        command, {.outputMode = lang::driver::ProcessOutputMode::Inherit,
                  .captureSuccessfulOutput = false,
                  .description = "program"});
    if (process.driverDiagnostic) {
      std::cerr << *process.driverDiagnostic << '\n';
    }
    return process.exitCode;
  }
  return exitCode(ExitStatus::Success);
}

int runProjectTests(const ProjectOptions &options, const char *driver) {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = options.optimization;
  overrides.cppStandard = options.standard;
  overrides.executionProfile = options.executionProfile;
  overrides.keepCpp = options.keepCpp;
  lang::driver::ProjectTestResolutionResult resolution =
      lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
          *currentDirectory, options.target, options.profile,
          lang::TargetInfo::host(), std::move(overrides), options.package));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  std::size_t passed = 0;
  std::size_t failed = 0;
  int firstFailure = exitCode(ExitStatus::Success);
  for (const lang::driver::ProjectBuildPlan &plan : resolution.plans) {
    if (options.verbose) {
      reportProjectPlan(plan, ProjectCommand::Test);
    }
    std::cerr << "Building test " << plan.targetName() << '\n';
    const int buildStatus = buildProjectPlan(plan, options, toolchain);
    if (buildStatus != exitCode(ExitStatus::Success)) {
      return buildStatus;
    }

    reportProjectSuccess(std::cerr, "Built", plan);
    if (plan.keepCpp()) {
      std::cerr << "Kept C++ " << plan.generatedSource().string() << '\n';
    }
    std::cerr << "Testing " << plan.targetName() << '\n';
    const lang::driver::ProcessResult process = lang::driver::invokeProcess(
        {plan.output().string()},
        {.outputMode = lang::driver::ProcessOutputMode::Inherit,
         .captureSuccessfulOutput = false,
         .description = "test target '" + plan.targetName() + "'"});
    if (process.driverDiagnostic) {
      std::cerr << *process.driverDiagnostic << '\n';
    }
    if (process.succeeded()) {
      ++passed;
      std::cerr << "Passed " << plan.targetName() << '\n';
      continue;
    }

    ++failed;
    if (firstFailure == exitCode(ExitStatus::Success)) {
      firstFailure = process.exitCode;
    }
    std::cerr << "Failed " << plan.targetName() << " (exit code "
              << process.exitCode << ")\n";
  }

  std::cerr << "Test result: " << passed << " passed";
  if (failed != 0) {
    std::cerr << ", " << failed << " failed";
  }
  std::cerr << '\n';
  return firstFailure;
}

int runClean() {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }
  const lang::driver::ProjectCleanResult result =
      lang::driver::cleanProject(*currentDirectory);
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, {});
    return result.status == lang::driver::ProjectCleanStatus::FilesystemFailure
               ? exitCode(ExitStatus::Io)
               : exitCode(ExitStatus::Compilation);
  }
  if (result.removedEntries == 0) {
    std::cout << "Nothing to clean at " << result.buildRoot.string() << '\n';
  } else {
    std::cout << "Cleaned " << result.buildRoot.string() << " ("
              << result.removedEntries << " entries)\n";
  }
  return exitCode(ExitStatus::Success);
}

int runMetadata(const MetadataOptions &options) {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }
  const lang::driver::ProjectMetadataResult result =
      lang::driver::resolveProjectMetadata(
          *currentDirectory, lang::TargetInfo::host(), options.package);
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, result.sources);
    return exitCode(ExitStatus::Compilation);
  }
  lang::cli::writeProjectMetadata(std::cout, *result.metadata);
  return exitCode(ExitStatus::Success);
}

int runScaffold(const ScaffoldOptions &options) {
  const lang::driver::ProjectScaffoldResult result =
      lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
          options.mode, options.destination, options.packageName));
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, {});
    return result.status ==
                   lang::driver::ProjectScaffoldStatus::FilesystemFailure
               ? exitCode(ExitStatus::Io)
               : exitCode(ExitStatus::Usage);
  }

  std::cout << (options.mode == lang::driver::ProjectScaffoldMode::NewPackage
                    ? "Created"
                    : "Initialized")
            << " package '" << result.packageName << "' at "
            << result.packageRoot.string() << '\n';
  return exitCode(ExitStatus::Success);
}

int runFormatConfigInit(const FormatConfigInitOptions &options) {
  const lang::driver::FormatConfigScaffoldResult result =
      lang::driver::scaffoldFormatConfig(options.destination);
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, {});
    return result.status ==
                   lang::driver::FormatConfigScaffoldStatus::FilesystemFailure
               ? exitCode(ExitStatus::Io)
               : exitCode(ExitStatus::Usage);
  }

  std::cout << "Created format configuration at " << result.configPath.string()
            << '\n';
  return exitCode(ExitStatus::Success);
}

bool isReservedProjectCommand(std::string_view command) {
  return command == "fetch";
}

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti");
  const std::string_view command =
      argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  if (command == "build" || command == "check" || command == "run" ||
      command == "test") {
    ProjectOptions options;
    options.command = command == "check"  ? ProjectCommand::Check
                      : command == "run"  ? ProjectCommand::Run
                      : command == "test" ? ProjectCommand::Test
                                          : ProjectCommand::Build;
    const ArgumentResult argumentResult =
        parseProjectArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return options.command == ProjectCommand::Test
               ? runProjectTests(options, argv[0])
               : runProject(options, argv[0]);
  }

  if (command == "clean") {
    const ArgumentResult argumentResult = parseCleanArguments(argc, argv);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runClean();
  }

  if (command == "metadata") {
    MetadataOptions options;
    const ArgumentResult argumentResult =
        parseMetadataArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runMetadata(options);
  }

  if (command == "new" || command == "init") {
    ScaffoldOptions options;
    options.mode = command == "new"
                       ? lang::driver::ProjectScaffoldMode::NewPackage
                       : lang::driver::ProjectScaffoldMode::ExistingDirectory;
    const ArgumentResult argumentResult =
        parseScaffoldArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runScaffold(options);
  }

  if (command == "format") {
    FormatConfigInitOptions options;
    const ArgumentResult argumentResult =
        parseFormatConfigInitArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runFormatConfigInit(options);
  }

  if (argc > 1 && isReservedProjectCommand(command)) {
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
  if (options.timeTrace) {
    if (!lang::timeTraceAvailable()) {
      std::cerr << "gti: --time-trace requires a compiler built with LLVM "
                   "support libraries\n";
      return exitCode(ExitStatus::Usage);
    }
    lang::beginTimeTrace("gti");
    const int status = runDirect(options, argv[0]);
    if (!lang::endTimeTrace(options.timeTrace->string())) {
      std::cerr << "gti: failed to write time trace to '"
                << options.timeTrace->string() << "'\n";
      return status == exitCode(ExitStatus::Success) ? exitCode(ExitStatus::Io)
                                                     : status;
    }
    return status;
  }
  return runDirect(options, argv[0]);
}
