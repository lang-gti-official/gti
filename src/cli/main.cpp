#include "build_progress.h"
#include "gti/bindgen/bindgen.h"
#include "gti/diagnostic.h"
#include "gti/driver/artifact.h"
#include "gti/driver/bindgen_import.h"
#include "gti/driver/build.h"
#include "gti/driver/build_report.h"
#include "gti/driver/build_script.h"
#include "gti/driver/compilation.h"
#include "gti/driver/dependencies.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/driver/project.h"
#include "gti/driver/registry.h"
#include "gti/executable_path.h"
#include "gti/support.h"
#include "gti/target.h"
#include "project_presentation.h"
#include "terminal_style.h"

#include <llvm/Support/ConvertUTF.h>
#include <llvm/Support/Unicode.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

#if !defined(GTI_VERSION)
#define GTI_VERSION "0.1.0"
#endif

constexpr std::string_view version = GTI_VERSION;
lang::cli::TerminalStyle terminalStyle;
using lang::cli::Style;
std::optional<std::string> runtimeColorOverride;
lang::cli::ProgressMode progressMode = lang::cli::ProgressMode::Auto;
bool progressSelected = false;

std::vector<std::pair<std::string, std::string>> programColorEnvironment() {
  if (runtimeColorOverride) {
    return {{"GTI_COLOR", *runtimeColorOverride}};
  }
  return {};
}

// Called only at option boundaries, never for another option's operand or
// arguments following '--'. Explicit CLI policy overrides NO_COLOR.
enum class ColorArgument { NotColor, Accepted, Invalid };
ColorArgument parseColorArgument(int argc, char *argv[], int &index) {
  const std::string_view argument = argv[index];
  std::string_view value;
  if (argument == "--color") {
    if (++index >= argc) {
      std::cerr << "gti: --color requires auto, always, or never\n";
      return ColorArgument::Invalid;
    }
    value = argv[index];
  } else if (argument.starts_with("--color=")) {
    value = argument.substr(8);
  } else {
    return ColorArgument::NotColor;
  }
  if (!terminalStyle.setMode(value)) {
    std::cerr << "gti: invalid --color value '" << value
              << "'; expected auto, always, or never\n";
    return ColorArgument::Invalid;
  }
  runtimeColorOverride = value;
  return ColorArgument::Accepted;
}

ColorArgument parseProgressArgument(int argc, char *argv[], int &index) {
  const std::string_view argument = argv[index];
  std::string_view value;
  if (argument == "--progress") {
    if (++index >= argc) {
      std::cerr << "gti: --progress requires auto, plain, or never\n";
      return ColorArgument::Invalid;
    }
    value = argv[index];
  } else if (argument.starts_with("--progress=")) {
    value = argument.substr(11);
  } else {
    return ColorArgument::NotColor;
  }
  if (value == "auto") {
    progressMode = lang::cli::ProgressMode::Auto;
  } else if (value == "plain") {
    progressMode = lang::cli::ProgressMode::Plain;
  } else if (value == "never") {
    progressMode = lang::cli::ProgressMode::Never;
  } else {
    std::cerr << "gti: invalid --progress value '" << value
              << "'; expected auto, plain, or never\n";
    return ColorArgument::Invalid;
  }
  progressSelected = true;
  return ColorArgument::Accepted;
}

struct Options {
  std::filesystem::path input;
  std::filesystem::path output;
  std::optional<std::string> cxx;
  std::optional<std::filesystem::path> timeTrace;
  std::optional<std::filesystem::path> dumpMirBefore;
  std::optional<std::filesystem::path> dumpMirAfter;
  std::optional<std::filesystem::path> optimizationReport;
  std::optional<std::filesystem::path> buildReport;
  std::vector<std::string> compilerArguments;
  lang::ConfigurationFlags configurationFlags;
  std::vector<std::string> disabledWarnings;
  lang::CppStandard standard = lang::CppStandard::Cpp23;
  lang::OptimizationLevel optimization = lang::OptimizationLevel::O0;
  lang::ExecutionProfile executionProfile =
      lang::ExecutionProfile::SingleThreaded;
  bool executionProfileSelected = false;
  bool emitCpp = false;
  bool emitMir = false;
  bool emitNativeHeader = false;
  bool keepCpp = false;
  bool debugInfo = false;
  bool optimizationRemarks = false;
  bool verbose = false;
};

struct BindgenCliOptions {
  std::filesystem::path header;
  std::filesystem::path output;
  std::optional<std::string> targetTriple;
  std::optional<std::string> clang;
  std::optional<std::string> sysroot;
  std::vector<std::filesystem::path> includeDirectories;
  std::vector<std::string> macroArguments;
  lang::bindgen::BindingOptions bindings;
  bool verbose = false;
};

enum class ProjectCommand {
  Build,
  Check,
  Run,
  Test,
  Verify,
};

struct ProjectOptions {
  ProjectCommand command = ProjectCommand::Build;
  std::optional<std::string> package;
  std::optional<std::string> target;
  // The `--target <triple>` cross-compilation selection, distinct from the
  // manifest build target named by `target`.
  std::optional<std::string> targetTriple;
  std::string profile = "dev";
  bool profileSelected = false;
  std::optional<std::string> cxx;
  std::optional<std::string> cc;
  std::optional<lang::CppStandard> standard;
  std::optional<lang::OptimizationLevel> optimization;
  std::optional<lang::ExecutionProfile> executionProfile;
  std::optional<bool> keepCpp;
  std::optional<bool> debugInfo;
  std::optional<std::filesystem::path> buildReport;
  bool emitMir = false;
  bool buildAll = false;
  std::optional<int> jobs;
  std::vector<std::string> programArguments;
  lang::ConfigurationFlags configurationFlags;
  std::vector<std::string> disabledWarnings;
  bool verbose = false;
  bool useCache = true;
  bool noBuild = false;
  bool offline = false;
  bool locked = false;
};

struct FetchOptions {
  bool offline = false;
  bool locked = false;
};

struct RegistryOptions {
  std::string package;
  std::string requirement;
  std::string registry = "official";
  bool dryRun = false;
};

struct ScaffoldOptions {
  lang::driver::ProjectScaffoldMode mode =
      lang::driver::ProjectScaffoldMode::NewPackage;
  std::filesystem::path destination;
  std::optional<std::string> packageName;
  bool initializeGit = false;
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
  // A rebuild succeeded but no longer matches the recorded provenance. The
  // distinct code lets a script separate "this tree drifted" from a usage,
  // build, or I/O failure.
  Verification = 70,
  Io = 74,
  ToolchainConfiguration = 78,
};

constexpr int exitCode(ExitStatus status) { return static_cast<int>(status); }

void printUsage(std::ostream &stream) {
  stream
      << "Usage: gti <source.gti> [options] [-- <native compiler arguments>]\n"
         "       gti build [target] [options]\n"
         "       gti check [target] [--profile <name> | --release]\n"
         "       gti run [target] [options] [-- <program arguments>]\n"
         "       gti test [target] [options]\n"
         "       gti verify --build-report <path> [options]\n"
         "       gti new <path> [--name <name>]\n"
         "       gti init [path] [--name <name>] [--git]\n"
         "       gti add <package[@requirement]> [--registry <name>]\n"
         "       gti fetch [--offline] [--locked]\n"
         "       gti publish [--registry <name>] [--name <name>] [--dry-run]\n"
         "       gti clean\n"
         "       gti metadata [--format json] [--package <name>]\n"
         "       gti format init [directory]\n"
         "       gti bindgen <header.h> [-o <out.gti>] [--namespace <name>] "
         "[--target <triple>] [--sysroot <path>] [-I <dir>] [-D <macro>] "
         "[-U <macro>] [--allow <pattern>] [--deny <pattern>] "
         "[--rename <from>=<to>] [--array-return <fn>=<count>]\n"
         "\n"
         "Global presentation options:\n"
         "      --color=auto|always|never\n"
         "                       Terminal colour (default auto; honours "
         "NO_COLOR).\n"
         "\n"
         "Direct compiler options:\n"
         "  -o, --output <path>  Set the executable or emitted artifact path.\n"
         "      --emit-cpp       Emit the backend representation without "
         "building an executable.\n"
         "      --emit-mir       Emit the deterministic verified-MIR "
         "serialization.\n"
         "      --emit-native-header\n"
         "                       Emit a C17 and C++20/C++23 compatible C ABI "
         "header.\n"
         "      --keep-cpp       Keep the backend representation beside the "
         "executable.\n"
         "  -g, --debug-info     Emit GTI source line mappings and native "
         "debug symbols.\n"
         "      --no-debug-info  Disable source-level debug info.\n"
         "      --cxx <path>     Select the native compiler.\n"
         "      --std <version>  Select c++20 or c++23 (default: c++23).\n"
         "      --execution-profile <name>\n"
         "                       Select single-threaded (default) or "
         "concurrent semantics.\n"
         "  -O0, -O1, -O2, -O3  Select the optimization level (default: -O0).\n"
         "  -D <name>            Define a valueless configuration flag.\n"
         "  -Wno-<group>         Disable a warning group (for example, "
         "-Wno-unused-mut).\n"
         "      --disable-warning <group>\n"
         "                       Disable a warning group; may be repeated.\n"
         "      --time-trace <path>  Write a compile-time profile as Chrome "
         "Trace JSON.\n"
         "      --dump-mir-before <path>\n"
         "                       Write canonical MIR before GTI optimization.\n"
         "      --dump-mir-after <path>\n"
         "                       Write verified MIR after GTI optimization.\n"
         "      --optimization-remarks\n"
         "                       Print GTI MIR optimization decisions.\n"
         "      --optimization-report <path>\n"
         "                       Write versioned deterministic optimizer "
         "JSON.\n"
         "      --build-report <path>\n"
         "                       Write a versioned executable-build provenance "
         "sidecar.\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "      --progress <mode>\n"
         "                       Build progress: auto (default), plain, "
         "never.\n"
         "\n"
         "Project build options:\n"
         "      --package <name> Select a package from the active workspace.\n"
         "      --profile <name> Select a manifest profile (default: dev).\n"
         "      --target <triple>\n"
         "                       Build for a target triple (default: host).\n"
         "                       A non-host triple requires a matching\n"
         "                       [toolchains.<triple>] manifest table.\n"
         "      --release        Shorthand for --profile release.\n"
         "      --all            Build every declared target (gti build "
         "only).\n"
         "      --jobs <count>   Bound native build-script and --all jobs.\n"
         "      --cxx <path>     Override the native compiler.\n"
         "      --cc <path>      Override the native C compiler.\n"
         "      --std <version>  Override c++20 or c++23.\n"
         "      --execution-profile <name>\n"
         "                       Override single-threaded or concurrent "
         "semantics.\n"
         "  -O0, -O1, -O2, -O3  Override the profile optimization level.\n"
         "  -D <name>            Add a valueless project configuration flag.\n"
         "  -Wno-<group>         Disable an additional warning group.\n"
         "      --disable-warning <group>\n"
         "                       Disable an additional warning group.\n"
         "      --build-report <path>\n"
         "                       Write a build provenance sidecar (gti build); "
         "select the record to reproduce (gti verify).\n"
         "      --emit-mir       Emit the deterministic verified-MIR "
         "serialization instead of building.\n"
         "      --keep-cpp       Retain the backend representation in the "
         "intermediate "
         "directory.\n"
         "      --no-keep-cpp    Remove the backend representation after a "
         "successful "
         "build.\n"
         "  -g, --debug-info     Emit GTI source line mappings and native "
         "debug symbols.\n"
         "      --no-debug-info  Disable the profile's debug-info setting.\n"
         "      --no-cache       Build without reading or updating the "
         "project cache.\n"
         "      --no-build       Run an existing managed executable without "
         "checking or rebuilding it (gti run only).\n"
         "      --offline        Never run git; only already fetched "
         "dependencies resolve.\n"
         "      --locked         Require gti.lock coverage without acquiring "
         "anything.\n"
         "  -v, --verbose        Print the native compiler command and "
         "output.\n"
         "\n"
         "Bindgen options:\n"
         "  -o, --output <path>  Write generated GTI declarations to a file.\n"
         "      --namespace <name>\n"
         "                       Wrap generated source names in a GTI "
         "namespace.\n"
         "      --target <triple>\n"
         "                       Select the C ABI target (default: host).\n"
         "      --clang <path>   Select the Clang preprocessor executable.\n"
         "      --sysroot <path>\n"
         "                       Select the platform SDK the preprocessor\n"
         "                       resolves system includes against.\n"
         "      --allow <name|prefix*>\n"
         "                       Import only matching declarations.\n"
         "      --deny <name|prefix*>\n"
         "                       Never import matching declarations.\n"
         "      --rename <c-name>=<gti-name>\n"
         "                       Select a different GTI source name; renamed\n"
         "                       functions retain their C link symbol.\n"
         "      --array-return <function>=<count>\n"
         "                       Annotate a pointer-plus-count result with\n"
         "                       [[c_array(count)]].\n"
         "  -I <dir>             Add a C header search directory.\n"
         "  -D <macro[=value]>   Define a C preprocessor macro.\n"
         "  -U <macro>           Undefine a C preprocessor macro.\n"
         "  -v, --verbose        Print the Clang preprocessing command.\n"
         "\n"
         "Project commands:\n"
         "  new                  Create a new executable package.\n"
         "  init                 Initialize an existing directory as a "
         "package; --git also creates a repository and .gitignore.\n"
         "  check                Analyze a target without backend emission or "
         "native compilation.\n"
         "  run                  Build and run a target, or use --no-build to "
         "launch its existing artifact; arguments after -- belong to the "
         "program.\n"
         "  test                 Build and run all test targets, or one named "
         "test target.\n"
         "  verify               Rebuild a recorded --build-report and report "
         "every recorded fact that no longer matches.\n"
         "  add                  Add a registry dependency and update the "
         "exact lock.\n"
         "  publish              Publish a validated immutable source "
         "package.\n"
         "  fetch                Acquire declared dependencies and write "
         "gti.lock.\n"
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

std::string_view optimizationLevelName(lang::OptimizationLevel level) {
  switch (level) {
  case lang::OptimizationLevel::O0:
    return "O0";
  case lang::OptimizationLevel::O1:
    return "O1";
  case lang::OptimizationLevel::O2:
    return "O2";
  case lang::OptimizationLevel::O3:
    return "O3";
  }
  return "invalid";
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

bool addDisabledWarning(std::string_view group,
                        std::vector<std::string> &disabledWarnings,
                        std::string_view option) {
  if (group.empty()) {
    std::cerr << "gti: " << option << " requires a warning group\n";
    return false;
  }
  if (!lang::isKnownWarningGroup(group)) {
    std::cerr << "gti: unknown warning group '" << group << "'\n";
    return false;
  }
  disabledWarnings.emplace_back(group);
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

std::filesystem::path defaultMirPath(const std::filesystem::path &input) {
  return input.parent_path() / (input.stem().string() + ".mir");
}

std::filesystem::path
defaultNativeHeaderPath(const std::filesystem::path &input) {
  return input.parent_path() / (input.stem().string() + ".native.h");
}

ArgumentResult parseArguments(int argc, char *argv[], Options &options) {
  for (int index = 1; index < argc; ++index) {
    const auto progress = parseProgressArgument(argc, argv, index);
    if (progress == ColorArgument::Invalid)
      return ArgumentResult::ExitFailure;
    if (progress == ColorArgument::Accepted)
      continue;
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    if (argument == "-D") {
      if (++index >= argc) {
        std::cerr << "gti: missing configuration flag name after -D\n";
        return ArgumentResult::ExitFailure;
      }
      if (!lang::isConfigurationFlagName(argv[index])) {
        std::cerr << "gti: configuration flag '" << argv[index]
                  << "' is not an ordinary GTI identifier\n";
        return ArgumentResult::ExitFailure;
      }
      options.configurationFlags.emplace_back(argv[index]);
      continue;
    }
    if (argument == "--disable-warning") {
      if (++index >= argc) {
        std::cerr << "gti: missing warning group after --disable-warning\n";
        return ArgumentResult::ExitFailure;
      }
      if (!addDisabledWarning(argv[index], options.disabledWarnings,
                              "--disable-warning")) {
        return ArgumentResult::ExitFailure;
      }
      continue;
    }
    if (argument.starts_with("-Wno-")) {
      if (!addDisabledWarning(std::string_view(argument).substr(5),
                              options.disabledWarnings, "-Wno-<group>")) {
        return ArgumentResult::ExitFailure;
      }
      continue;
    }
    if (argument == "--emit-mir") {
      options.emitMir = true;
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
    if (argument == "--dump-mir-before" || argument == "--dump-mir-after" ||
        argument == "--optimization-report" || argument == "--build-report") {
      if (++index >= argc) {
        std::cerr << "gti: missing path after " << argument << '\n';
        return ArgumentResult::ExitFailure;
      }
      std::optional<std::filesystem::path> *destination = nullptr;
      if (argument == "--dump-mir-before") {
        destination = &options.dumpMirBefore;
      } else if (argument == "--dump-mir-after") {
        destination = &options.dumpMirAfter;
      } else if (argument == "--optimization-report") {
        destination = &options.optimizationReport;
      } else {
        destination = &options.buildReport;
      }
      if (*destination) {
        std::cerr << "gti: " << argument << " may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      *destination = std::filesystem::path(argv[index]);
      continue;
    }
    if (argument == "--optimization-remarks") {
      options.optimizationRemarks = true;
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
    if (argument == "-g" || argument == "--debug-info" ||
        argument == "--no-debug-info") {
      options.debugInfo = argument != "--no-debug-info";
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
  if (options.debugInfo && (options.emitMir || options.emitNativeHeader)) {
    std::cerr << "gti: debug info requires executable or backend "
                 "representation output\n";
    return ArgumentResult::ExitFailure;
  }
  if ((options.emitCpp || options.emitNativeHeader || options.emitMir) &&
      options.keepCpp) {
    std::cerr << "gti: emission modes and --keep-cpp cannot be used "
                 "together\n";
    return ArgumentResult::ExitFailure;
  }
  if (static_cast<int>(options.emitCpp) +
          static_cast<int>(options.emitNativeHeader) +
          static_cast<int>(options.emitMir) >
      1) {
    std::cerr << "gti: --emit-cpp, --emit-mir, and --emit-native-header "
                 "cannot be used together\n";
    return ArgumentResult::ExitFailure;
  }
  if ((options.emitCpp || options.emitNativeHeader || options.emitMir) &&
      !options.compilerArguments.empty()) {
    std::cerr << "gti: native compiler arguments require executable output\n";
    return ArgumentResult::ExitFailure;
  }
  if ((options.emitCpp || options.emitNativeHeader || options.emitMir) &&
      options.buildReport) {
    std::cerr << "gti: --build-report requires executable output\n";
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
    } else if (options.emitMir) {
      options.output = defaultMirPath(options.input);
    } else if (options.emitNativeHeader) {
      options.output = defaultNativeHeaderPath(options.input);
    } else {
      options.output = defaultExecutablePath(options.input);
    }
  }
  std::vector<std::pair<std::filesystem::path, std::string_view>> outputPaths{
      {options.output, "primary output"}};
  if (options.dumpMirBefore) {
    outputPaths.emplace_back(*options.dumpMirBefore, "--dump-mir-before");
  }
  if (options.dumpMirAfter) {
    outputPaths.emplace_back(*options.dumpMirAfter, "--dump-mir-after");
  }
  if (options.optimizationReport) {
    outputPaths.emplace_back(*options.optimizationReport,
                             "--optimization-report");
  }
  if (options.buildReport) {
    outputPaths.emplace_back(*options.buildReport, "--build-report");
  }
  for (std::size_t left = 0; left < outputPaths.size(); ++left) {
    std::error_code leftError;
    const std::filesystem::path leftPath =
        std::filesystem::absolute(outputPaths[left].first, leftError)
            .lexically_normal();
    for (std::size_t right = left + 1; right < outputPaths.size(); ++right) {
      std::error_code rightError;
      const std::filesystem::path rightPath =
          std::filesystem::absolute(outputPaths[right].first, rightError)
              .lexically_normal();
      if (!leftError && !rightError && leftPath == rightPath) {
        std::cerr << "gti: " << outputPaths[left].second << " and "
                  << outputPaths[right].second
                  << " must use different output paths\n";
        return ArgumentResult::ExitFailure;
      }
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
  case ProjectCommand::Verify:
    return "verify";
  }
  return "build";
}

bool buildsExecutable(ProjectCommand command) {
  return command == ProjectCommand::Build || command == ProjectCommand::Run ||
         command == ProjectCommand::Test || command == ProjectCommand::Verify;
}

ArgumentResult parseProjectArguments(int argc, char *argv[],
                                     ProjectOptions &options) {
  for (int index = 2; index < argc; ++index) {
    const auto progress = parseProgressArgument(argc, argv, index);
    if (progress == ColorArgument::Invalid)
      return ArgumentResult::ExitFailure;
    if (progress == ColorArgument::Accepted)
      continue;
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    if (argument == "--target") {
      if (++index >= argc) {
        std::cerr << "gti: missing target triple after --target\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.targetTriple) {
        std::cerr << "gti: --target may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.targetTriple = argv[index];
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
    if (argument == "--build-report") {
      if (options.command != ProjectCommand::Build &&
          options.command != ProjectCommand::Verify) {
        std::cerr << "gti: --build-report is not supported by gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      if (++index >= argc) {
        std::cerr << "gti: missing report path after --build-report\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.buildReport) {
        std::cerr << "gti: --build-report may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.buildReport = std::filesystem::path(argv[index]);
      continue;
    }
    if (argument == "--emit-mir") {
      if (options.command != ProjectCommand::Build) {
        std::cerr << "gti: --emit-mir is not supported by gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.emitMir = true;
      continue;
    }
    if (argument == "--all") {
      if (options.command != ProjectCommand::Build) {
        std::cerr << "gti: --all is not supported by gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.buildAll = true;
      continue;
    }
    if (argument == "--jobs") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: --jobs is not supported by gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      if (++index >= argc) {
        std::cerr << "gti: missing job count after --jobs\n";
        return ArgumentResult::ExitFailure;
      }
      const std::string_view value = argv[index];
      int jobs = 0;
      const auto [parsedEnd, parseError] =
          std::from_chars(value.data(), value.data() + value.size(), jobs);
      if (parseError != std::errc{} ||
          parsedEnd != value.data() + value.size() || jobs < 1) {
        std::cerr << "gti: --jobs requires a positive whole number\n";
        return ArgumentResult::ExitFailure;
      }
      options.jobs = jobs;
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
    if (argument == "-D") {
      if (++index >= argc) {
        std::cerr << "gti: missing configuration flag name after -D\n";
        return ArgumentResult::ExitFailure;
      }
      if (!lang::isConfigurationFlagName(argv[index])) {
        std::cerr << "gti: configuration flag '" << argv[index]
                  << "' is not an ordinary GTI identifier\n";
        return ArgumentResult::ExitFailure;
      }
      options.configurationFlags.emplace_back(argv[index]);
      continue;
    }
    if (argument == "--disable-warning") {
      if (++index >= argc) {
        std::cerr << "gti: missing warning group after --disable-warning\n";
        return ArgumentResult::ExitFailure;
      }
      if (!addDisabledWarning(argv[index], options.disabledWarnings,
                              "--disable-warning")) {
        return ArgumentResult::ExitFailure;
      }
      continue;
    }
    if (argument.starts_with("-Wno-")) {
      if (!addDisabledWarning(std::string_view(argument).substr(5),
                              options.disabledWarnings, "-Wno-<group>")) {
        return ArgumentResult::ExitFailure;
      }
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
    if (argument == "-g" || argument == "--debug-info" ||
        argument == "--no-debug-info") {
      if (!buildsExecutable(options.command)) {
        std::cerr << "gti: debug-info overrides are not valid for gti "
                  << projectCommandName(options.command) << '\n';
        return ArgumentResult::ExitFailure;
      }
      options.debugInfo = argument != "--no-debug-info";
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
    if (argument == "--no-build") {
      if (options.command != ProjectCommand::Run) {
        std::cerr << "gti: --no-build is supported only by gti run\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.noBuild) {
        std::cerr << "gti: --no-build may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.noBuild = true;
      continue;
    }
    if (argument == "--offline") {
      options.offline = true;
      continue;
    }
    if (argument == "--locked") {
      options.locked = true;
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
  if (options.emitMir && options.keepCpp.value_or(false)) {
    std::cerr << "gti: --emit-mir and --keep-cpp cannot be used together\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.emitMir && options.debugInfo.value_or(false)) {
    std::cerr << "gti: --emit-mir and debug info cannot be used together\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.buildAll && options.target) {
    std::cerr << "gti: --all cannot be combined with a target selection\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.buildAll && options.emitMir) {
    std::cerr << "gti: --all and --emit-mir cannot be used together\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.buildReport && options.emitMir) {
    std::cerr << "gti: --build-report requires an executable build\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.buildReport && options.buildAll) {
    std::cerr << "gti: --build-report describes one target and cannot be "
                 "combined with --all\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.noBuild &&
      (options.cxx || options.cc || options.standard || options.optimization ||
       options.executionProfile || options.keepCpp || options.debugInfo ||
       options.jobs || !options.configurationFlags.empty() ||
       !options.disabledWarnings.empty() || !options.useCache)) {
    std::cerr << "gti: --no-build cannot be combined with compiler, build, "
                 "cache, or configuration overrides\n";
    return ArgumentResult::ExitFailure;
  }
  if (options.command == ProjectCommand::Verify) {
    if (!options.buildReport) {
      std::cerr << "gti: gti verify requires --build-report <path>\n";
      return ArgumentResult::ExitFailure;
    }
    // The record names the package, target, and profile it describes.
    // Accepting a contradicting selection here would verify a different
    // build than the one that was recorded.
    if (options.target || options.package || options.profileSelected ||
        options.targetTriple) {
      std::cerr << "gti: gti verify takes its package, target, profile, and "
                   "target triple from the recorded report\n";
      return ArgumentResult::ExitFailure;
    }
  }
  return ArgumentResult::Run;
}

ArgumentResult parseFetchArguments(int argc, char *argv[],
                                   FetchOptions &options) {
  for (int index = 2; index < argc; ++index) {
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (argument == "--offline") {
      options.offline = true;
      continue;
    }
    if (argument == "--locked") {
      options.locked = true;
      continue;
    }
    std::cerr << "gti: unknown fetch option '" << argument << "'\n";
    return ArgumentResult::ExitFailure;
  }
  return ArgumentResult::Run;
}

ArgumentResult parseRegistryArguments(int argc, char *argv[],
                                      RegistryOptions &options, bool publish) {
  for (int index = 2; index < argc; ++index) {
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      return ArgumentResult::ExitSuccess;
    }
    if (publish && argument == "--dry-run") {
      options.dryRun = true;
      continue;
    }
    if (argument == "--registry" || (publish && argument == "--name")) {
      if (++index >= argc) {
        std::cerr << "gti: " << argument << " needs a value\n";
        return ArgumentResult::ExitFailure;
      }
      if (argument == "--registry") {
        options.registry = argv[index];
      } else {
        options.package = argv[index];
      }
      continue;
    }
    if (!publish && !argument.starts_with('-') && options.package.empty()) {
      const auto at = argument.find('@');
      options.package = argument.substr(0, at);
      if (at != argument.npos) {
        options.requirement = argument.substr(at + 1);
        if (options.requirement.empty()) {
          std::cerr << "gti: missing package version requirement after '@'\n";
          return ArgumentResult::ExitFailure;
        }
      }
      continue;
    }
    std::cerr << "gti: unknown " << (publish ? "publish" : "add")
              << " argument '" << argument << "'\n";
    return ArgumentResult::ExitFailure;
  }
  if (!publish && options.package.empty()) {
    std::cerr << "gti: add requires one package name\n";
    return ArgumentResult::ExitFailure;
  }
  return ArgumentResult::Run;
}

ArgumentResult parseCleanArguments(int argc, char *argv[]) {
  for (int index = 2; index < argc; ++index) {
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return ArgumentResult::ExitFailure;
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
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
    if (argument == "--git") {
      if (options.mode !=
          lang::driver::ProjectScaffoldMode::ExistingDirectory) {
        std::cerr << "gti: --git is supported only by gti init\n";
        return ArgumentResult::ExitFailure;
      }
      if (options.initializeGit) {
        std::cerr << "gti: --git may be specified only once\n";
        return ArgumentResult::ExitFailure;
      }
      options.initializeGit = true;
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

// Render source bytes as terminal text while retaining the compiler's byte
// spans. Tabs have a fixed four-column stop; control bytes cannot inject ANSI
// commands into a diagnostic. LLVM supplies Unicode column widths.
void reportSourceExcerpt(const lang::SourceSpan &span,
                         const lang::SourceManager &sources, Style style) {
  const auto location = sources.locate(span);
  const auto line = sources.line(span);
  if (line.empty())
    return;
  const auto begin =
      std::clamp(span.start, location.lineStart, location.lineEnd) -
      location.lineStart;
  const auto end = std::clamp(span.end, location.lineStart, location.lineEnd) -
                   location.lineStart;
  std::string display;
  std::size_t columns = 0, markerStart = 0, markerEnd = 0;
  for (std::size_t offset = 0; offset < line.size();) {
    const auto start = offset;
    const auto *cursor =
        reinterpret_cast<const llvm::UTF8 *>(line.data() + offset);
    const auto *limit =
        reinterpret_cast<const llvm::UTF8 *>(line.data() + line.size());
    // On malformed UTF-8, escape one byte at a time. An illegal continuation
    // must not absorb an adjacent ASCII character or misplace the underline.
    offset += std::max(1U, llvm::getUTF8SequenceSize(cursor, limit));
    const auto bytes = line.substr(start, offset - start);
    const auto before = columns;
    if (bytes == "\t") {
      const auto width = 4 - columns % 4;
      display.append(width, ' ');
      columns += width;
    } else {
      const int width = llvm::sys::unicode::columnWidthUTF8(
          llvm::StringRef(bytes.data(), bytes.size()));
      if (width < 0) {
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char byte : bytes) {
          display += "\\x";
          display += hex[byte >> 4];
          display += hex[byte & 15];
          columns += 4;
        }
      } else {
        display.append(bytes);
        columns += static_cast<std::size_t>(width);
      }
    }
    if (offset <= begin)
      markerStart = columns;
    else if (start <= begin)
      markerStart = before;
    if (start < end)
      markerEnd = columns;
  }
  const auto number = std::to_string(location.line);
  std::cerr << ' ' << number << " | " << display << '\n'
            << std::string(number.size() + 2, ' ') << "| "
            << std::string(markerStart, ' ')
            << terminalStyle.code(std::cerr, style) << '^';
  if (markerEnd > markerStart + 1)
    std::cerr << std::string(markerEnd - markerStart - 1, '~');
  std::cerr << terminalStyle.code(std::cerr, Style::Reset) << '\n';
}

void reportLocation(const lang::SourceSpan &span, std::string_view level,
                    std::string_view message,
                    const lang::SourceManager &sources) {
  const lang::SourceLocation location = sources.locate(span);
  std::cerr << (span.source.empty() ? "<source>" : span.source) << ':'
            << location.line << ':' << location.column << ": "
            << terminalStyle.code(std::cerr, Style::Note) << level
            << terminalStyle.code(std::cerr, Style::Reset) << ": " << message
            << '\n';

  reportSourceExcerpt(span, sources, Style::Note);
}

void reportDiagnostic(const lang::Diagnostic &diagnostic,
                      const lang::SourceManager &sources) {
  const Style style = diagnostic.severity == lang::DiagnosticSeverity::Error
                          ? Style::Error
                      : diagnostic.severity == lang::DiagnosticSeverity::Warning
                          ? Style::Warning
                          : Style::Note;
  const lang::SourceLocation location = sources.locate(diagnostic.primary);
  std::cerr << (diagnostic.primary.source.empty() ? "<source>"
                                                  : diagnostic.primary.source)
            << ':' << location.line << ':' << location.column << ": "
            << terminalStyle.code(std::cerr, style)
            << severityName(diagnostic.severity);
  if (!diagnostic.code.empty()) {
    std::cerr << '[' << diagnostic.code << ']';
  }
  std::cerr << terminalStyle.code(std::cerr, Style::Reset) << ": "
            << diagnostic.message << '\n';

  reportSourceExcerpt(diagnostic.primary, sources, style);

  for (const lang::RelatedDiagnostic &related : diagnostic.related) {
    reportLocation(related.span, "note", related.message, sources);
  }
  for (const std::string &hint : diagnostic.hints) {
    std::cerr << terminalStyle.code(std::cerr, Style::Note)
              << "help:" << terminalStyle.code(std::cerr, Style::Reset) << ' '
              << hint << '\n';
  }
  for (const lang::FixIt &fix : diagnostic.fixes) {
    std::cerr << terminalStyle.code(std::cerr, Style::Note)
              << "help:" << terminalStyle.code(std::cerr, Style::Reset) << ' '
              << fix.message << '\n';
  }
}

void reportDiagnostics(const std::vector<lang::Diagnostic> &diagnostics,
                       const lang::SourceManager &sources) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    reportDiagnostic(diagnostic, sources);
  }
}

struct DiagnosticSummary {
  std::size_t errors = 0;
  std::size_t warnings = 0;
};

DiagnosticSummary
summarizeDiagnostics(const std::vector<lang::Diagnostic> &diagnostics) {
  DiagnosticSummary summary;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.severity == lang::DiagnosticSeverity::Error) {
      ++summary.errors;
    } else if (diagnostic.severity == lang::DiagnosticSeverity::Warning) {
      ++summary.warnings;
    }
  }
  return summary;
}

void addDiagnosticSummary(DiagnosticSummary &destination,
                          const DiagnosticSummary &source) {
  destination.errors += source.errors;
  destination.warnings += source.warnings;
}

void reportDiagnosticSummary(std::ostream &stream,
                             const DiagnosticSummary &summary) {
  // Keep the summary payload contiguous: build --all reads child counts.
  const Style style = summary.errors     ? Style::Error
                      : summary.warnings ? Style::Warning
                                         : Style::Success;
  stream << terminalStyle.code(stream, style)
         << "Diagnostics: " << summary.errors
         << (summary.errors == 1 ? " error, " : " errors, ") << summary.warnings
         << (summary.warnings == 1 ? " warning" : " warnings")
         << terminalStyle.code(stream, Style::Reset) << '\n';
}

std::optional<DiagnosticSummary>
parseDiagnosticSummary(std::string_view output) {
  constexpr std::string_view prefix = "Diagnostics: ";
  const std::size_t start = output.rfind(prefix);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view fields = output.substr(start + prefix.size());
  DiagnosticSummary summary;
  const auto parseCount = [&](std::size_t &value) {
    const char *begin = fields.data();
    const char *end = begin + fields.size();
    const auto [next, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || next == begin) {
      return false;
    }
    fields.remove_prefix(static_cast<std::size_t>(next - begin));
    return true;
  };
  if (!parseCount(summary.errors) ||
      !(fields.starts_with(" error, ") || fields.starts_with(" errors, "))) {
    return std::nullopt;
  }
  fields.remove_prefix(fields.starts_with(" errors, ") ? 9 : 8);
  if (!parseCount(summary.warnings) ||
      !(fields.starts_with(" warning") || fields.starts_with(" warnings"))) {
    return std::nullopt;
  }
  return summary;
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

bool writeFileAtomically(const std::filesystem::path &path,
                         std::string_view contents) {
  const std::filesystem::path staged = lang::driver::stagedArtifactPath(path);
  lang::driver::TemporaryArtifact cleanup(staged, true);
  const lang::driver::ArtifactWriteStatus written =
      lang::driver::writeArtifact(staged, contents);
  if (written != lang::driver::ArtifactWriteStatus::Success) {
    std::cerr << "gti: failed to write staged build report '" << staged.string()
              << "'\n";
    return false;
  }
  const lang::driver::ArtifactPublishResult published =
      lang::driver::publishArtifact(staged, path);
  if (!published.succeeded()) {
    std::cerr << "gti: failed to publish build report '" << path.string()
              << "': " << published.error.message() << '\n';
    return false;
  }
  return true;
}

lang::driver::OptimizationOutputRequest
optimizationOutputRequest(const Options &options) {
  return {.capturePreMir = options.dumpMirBefore.has_value(),
          .capturePostMir = options.dumpMirAfter.has_value(),
          .captureReport = options.optimizationRemarks ||
                           options.optimizationReport.has_value()};
}

int publishOptimizationOutputs(
    const Options &options,
    const lang::driver::CompilationResult &compilation) {
  const auto publish = [&](const std::optional<std::filesystem::path> &path,
                           const std::optional<std::string> &contents,
                           std::string_view description) {
    if (!path) {
      return exitCode(ExitStatus::Success);
    }
    if (!contents) {
      std::cerr << "gti: internal compiler error: requested " << description
                << " was not captured\n";
      return exitCode(ExitStatus::Compilation);
    }
    if (const std::optional<std::filesystem::path> collision =
            lang::driver::findLoadedSourceCollision(*path,
                                                    compilation.sources)) {
      std::cerr << "gti: refusing to overwrite loaded source '"
                << collision->string() << "' with " << description << " '"
                << path->string() << "'\n";
      return exitCode(ExitStatus::Usage);
    }
    return writeFile(*path, *contents) ? exitCode(ExitStatus::Success)
                                       : exitCode(ExitStatus::Io);
  };

  int status = publish(options.dumpMirBefore, compilation.preOptimizationMir,
                       "pre-optimization MIR");
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }
  status = publish(options.dumpMirAfter, compilation.postOptimizationMir,
                   "post-optimization MIR");
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }
  if (options.optimizationReport) {
    if (!compilation.optimizationReport) {
      std::cerr << "gti: internal compiler error: requested optimization "
                   "report was not captured\n";
      return exitCode(ExitStatus::Compilation);
    }
    const std::optional<std::string> report = serializeOptimizationReportJson(
        *compilation.optimizationReport,
        optimizationLevelName(options.optimization));
    status = publish(options.optimizationReport, report, "optimization report");
    if (status != exitCode(ExitStatus::Success)) {
      return status;
    }
  }
  if (options.optimizationRemarks) {
    if (!compilation.optimizationReport) {
      std::cerr << "gti: internal compiler error: requested optimization "
                   "remarks were not captured\n";
      return exitCode(ExitStatus::Compilation);
    }
    std::cerr << formatOptimizationRemarks(
        *compilation.optimizationReport,
        optimizationLevelName(options.optimization));
  }
  return exitCode(ExitStatus::Success);
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

void reportBackendDiagnosticLog(const lang::driver::DiagnosticLogResult &log) {
  if (log.succeeded()) {
    std::cerr << "gti: backend diagnostics saved at " << log.path.string()
              << '\n';
  } else {
    std::cerr << "gti: failed to save backend diagnostics: "
              << log.error.message() << '\n';
  }
}

void reportSavedRepresentation(
    const lang::driver::ExecutableBuildResult &result) {
  if (result.generatedSourceRetained) {
    std::cerr << "gti: backend 'c++' representation saved at "
              << result.generatedSource.string() << '\n';
  }
}

int reportCompilationFailure(
    const lang::driver::CompilationResult &compilation, bool verbose,
    const lang::driver::DiagnosticLogResult *savedLog = nullptr) {
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
  if (compilation.backendFailureDetails) {
    const auto log = savedLog == nullptr
                         ? lang::driver::writeDiagnosticLog(
                               *compilation.backendFailureDetails)
                         : *savedLog;
    reportBackendDiagnosticLog(log);
    // If preservation fails, never hide the only remaining developer evidence.
    if (verbose || !log.succeeded()) {
      reportCapturedOutput(*compilation.backendFailureDetails,
                           "gti: backend diagnostics:\n");
    }
  }
  DiagnosticSummary summary = summarizeDiagnostics(compilation.diagnostics);
  summary.errors = std::max<std::size_t>(summary.errors, 1);
  std::cerr.flush();
  reportDiagnosticSummary(std::cout, summary);
  return exitCode(ExitStatus::Compilation);
}

int reportBuildResult(const lang::driver::ExecutableBuildResult &result,
                      bool verbose) {
  const DiagnosticSummary diagnosticSummary =
      summarizeDiagnostics(result.compilation.diagnostics);
  const auto finishFailure = [&](int status) {
    DiagnosticSummary summary = diagnosticSummary;
    summary.errors = std::max<std::size_t>(summary.errors, 1);
    std::cerr.flush();
    reportDiagnosticSummary(std::cout, summary);
    return status;
  };
  if (result.compilation.succeeded() &&
      !result.compilation.diagnostics.empty()) {
    reportDiagnostics(result.compilation.diagnostics,
                      result.compilation.sources);
  }
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
      const std::string prefix = compilation.process.succeeded()
                                     ? std::string{}
                                     : "gti: native source diagnostics for '" +
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
      const std::string prefix = compilation.process.succeeded()
                                     ? std::string{}
                                     : "gti: native source diagnostics for '" +
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
  if (result.nativeProcess &&
      (verbose || (result.backendDiagnosticLog &&
                   !result.backendDiagnosticLog->succeeded()))) {
    reportCapturedOutput(result.nativeProcess->output,
                         result.nativeProcess->succeeded()
                             ? std::string_view{}
                             : "gti: backend diagnostics:\n");
  }

  switch (result.status) {
  case lang::driver::ExecutableBuildStatus::Success:
    return exitCode(ExitStatus::Success);
  case lang::driver::ExecutableBuildStatus::CompilationFailure:
    return reportCompilationFailure(
        result.compilation, verbose,
        result.backendDiagnosticLog ? &*result.backendDiagnosticLog : nullptr);
  case lang::driver::ExecutableBuildStatus::OutputDirectoryFailure:
    return finishFailure(exitCode(ExitStatus::Io));
  case lang::driver::ExecutableBuildStatus::GeneratedArtifactFailure:
    std::cerr << (result.artifactWriteStatus ==
                          lang::driver::ArtifactWriteStatus::OpenFailure
                      ? "gti: failed to open backend representation file: "
                      : "gti: failed to write backend representation file: ")
              << result.generatedSource << '\n';
    return finishFailure(exitCode(ExitStatus::Io));
  case lang::driver::ExecutableBuildStatus::ToolchainConfigurationFailure:
    if (result.resourceError) {
      std::cerr
          << (*result.resourceError ==
                      lang::driver::ToolchainResourceError::RuntimeFilesMissing
                  ? "gti: native runtime files were not found\n"
                  : "gti: backend compatibility header was not "
                    "found\n");
    }
    return finishFailure(exitCode(ExitStatus::ToolchainConfiguration));
  case lang::driver::ExecutableBuildStatus::NativeCCompilerFailure: {
    const lang::driver::NativeCCompilationResult &compilation =
        result.cCompilations.back();
    std::cerr << "gti: native source compiler failed for "
              << compilation.source.string() << " with exit code "
              << compilation.process.exitCode << '\n';
    reportSavedRepresentation(result);
    return finishFailure(compilation.process.exitCode);
  }
  case lang::driver::ExecutableBuildStatus::NativeCppCompilerFailure: {
    const lang::driver::NativeCppCompilationResult &compilation =
        result.cppCompilations.back();
    std::cerr << "gti: native source compiler failed for "
              << compilation.source.string() << " with exit code "
              << compilation.process.exitCode << '\n';
    reportSavedRepresentation(result);
    return finishFailure(compilation.process.exitCode);
  }
  case lang::driver::ExecutableBuildStatus::NativeObjectPublicationFailure:
    reportSavedRepresentation(result);
    return finishFailure(exitCode(ExitStatus::Io));
  case lang::driver::ExecutableBuildStatus::NativeCompilerFailure:
    std::cerr
        << terminalStyle.code(std::cerr, Style::Error)
        << "gti: backend failed while compiling and linking the executable "
        << "(exit code " << result.nativeProcess->exitCode << ')'
        << terminalStyle.code(std::cerr, Style::Reset) << '\n';
    reportSavedRepresentation(result);
    if (result.backendDiagnosticLog) {
      reportBackendDiagnosticLog(*result.backendDiagnosticLog);
    }
    return finishFailure(result.nativeProcess->exitCode);
  case lang::driver::ExecutableBuildStatus::ArtifactPublicationFailure:
    reportSavedRepresentation(result);
    return finishFailure(exitCode(ExitStatus::Io));
  case lang::driver::ExecutableBuildStatus::ArtifactPathConflict:
    return finishFailure(exitCode(ExitStatus::Usage));
  case lang::driver::ExecutableBuildStatus::BuildReportCaptureFailure:
    return finishFailure(exitCode(ExitStatus::Io));
  }
  return finishFailure(exitCode(ExitStatus::Compilation));
}

int runDirect(const Options &options, const char *driver) {
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  const lang::driver::AnalysisConfiguration configuration(
      lang::TargetInfo::host(), options.executionProfile,
      lang::driver::RuntimeProfile::Hosted, options.configurationFlags);
  const lang::TargetInfo &target = configuration.target();

  // Keep emit-only direct invocations silent by default for existing tooling.
  // Explicit --progress also enables observation for these modes.
  lang::cli::BuildProgress progress(
      terminalStyle, progressMode, options.input.filename().string(),
      "direct, " + lang::driver::targetTriple(target),
      lang::cli::optimizationNumber(options.optimization), options.verbose,
      progressSelected ||
          !(options.emitCpp || options.emitMir || options.emitNativeHeader));

  if (options.emitCpp || options.emitNativeHeader || options.emitMir) {
    const lang::driver::CompilationRequest request(
        options.input, toolchain.standardLibrary, target, options.optimization,
        options.standard, {}, configuration.configurationFlags(),
        optimizationOutputRequest(options),
        lang::WarningPolicy(options.disabledWarnings), options.debugInfo);
    const lang::driver::CompilationResult compilation =
        options.emitNativeHeader
            ? lang::driver::compileToNativeHeader(request, progress.observer())
            : (options.emitMir
                   ? lang::driver::compileToMir(request, progress.observer())
                   : lang::driver::compileToCpp(request, progress.observer()));
    progress.suspend();
    if (!compilation.succeeded()) {
      return reportCompilationFailure(compilation, options.verbose);
    }
    reportDiagnostics(compilation.diagnostics, compilation.sources);
    const int optimizationOutputStatus =
        publishOptimizationOutputs(options, compilation);
    if (optimizationOutputStatus != exitCode(ExitStatus::Success)) {
      return optimizationOutputStatus;
    }
    const lang::BackendArtifact &artifact = *compilation.artifact;
    if (const std::optional<std::filesystem::path> collision =
            lang::driver::findLoadedSourceCollision(options.output,
                                                    compilation.sources)) {
      std::cerr << "gti: refusing to overwrite loaded source '"
                << collision->string() << "' with emitted "
                << (options.emitNativeHeader
                        ? "native header"
                        : (options.emitMir ? "MIR output"
                                           : "backend representation output"))
                << " '" << options.output.string() << "'\n";
      return exitCode(ExitStatus::Usage);
    }
    if (!writeFile(options.output, artifact.contents)) {
      return exitCode(ExitStatus::Io);
    }
    std::cout << terminalStyle.code(std::cout, Style::Success) << "Emitted"
              << terminalStyle.code(std::cout, Style::Reset) << ' '
              << options.output << '\n';
    reportDiagnosticSummary(std::cout,
                            summarizeDiagnostics(compilation.diagnostics));
    progress.finish(true);
    return exitCode(ExitStatus::Success);
  }

  const std::filesystem::path generatedSource =
      options.keepCpp
          ? std::filesystem::path(options.output.string() + ".gti.cpp")
          : lang::driver::temporaryCppPath(options.input);
  lang::driver::NativeInputs nativeInputs;
  nativeInputs.trailingArguments = options.compilerArguments;
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(
          lang::driver::ExecutableBuildRequest(
              lang::driver::CompilationRequest(
                  options.input, toolchain.standardLibrary, target,
                  options.optimization, options.standard, {},
                  configuration.configurationFlags(),
                  optimizationOutputRequest(options),
                  lang::WarningPolicy(options.disabledWarnings),
                  options.debugInfo),
              toolchain, generatedSource, options.output,
              lang::driver::discoverNativeCompiler(options.cxx),
              std::move(nativeInputs), options.keepCpp, false, options.verbose,
              std::nullopt, std::nullopt, std::nullopt,
              options.buildReport
                  ? std::optional<lang::driver::BuildReportRequest>(
                        {.output = *options.buildReport,
                         .producerExecutable = lang::executablePath(driver)})
                  : std::nullopt),
          progress.observer());
  progress.suspend();
  const int status = reportBuildResult(result, options.verbose);
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }
  const int optimizationOutputStatus =
      publishOptimizationOutputs(options, result.compilation);
  if (optimizationOutputStatus != exitCode(ExitStatus::Success)) {
    return optimizationOutputStatus;
  }
  if (options.buildReport) {
    if (!result.provenance) {
      std::cerr << "gti: internal compiler error: requested build report was "
                   "not captured\n";
      return exitCode(ExitStatus::Compilation);
    }
    if (!writeFileAtomically(
            *options.buildReport,
            lang::driver::serializeBuildProvenanceJson(*result.provenance))) {
      return exitCode(ExitStatus::Io);
    }
  }

  std::cout << terminalStyle.code(std::cout, Style::Success) << "Built"
            << terminalStyle.code(std::cout, Style::Reset) << ' '
            << options.output << '\n';
  if (options.buildReport) {
    std::cout << "Build report " << *options.buildReport << '\n';
  }
  if (options.keepCpp) {
    std::cout << "Kept backend representation " << generatedSource << '\n';
  }
  reportDiagnosticSummary(std::cout,
                          summarizeDiagnostics(result.compilation.diagnostics));
  progress.finish(true);
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
            << lang::cli::optimizationNumber(plan.optimization())
            << ", execution-profile="
            << lang::executionProfileName(plan.target().executionProfile)
            << ", keep-representation=" << (plan.keepCpp() ? "true" : "false")
            << ", debug-info=" << (plan.debugInfo() ? "true" : "false") << '\n';
  if (!plan.configurationFlags().empty()) {
    std::cerr << "gti: defines";
    for (const std::string &flag : plan.configurationFlags()) {
      std::cerr << ' ' << flag;
    }
    std::cerr << '\n';
  }
  if (command == ProjectCommand::Check) {
    std::cerr << "gti: source " << plan.entry().string() << '\n';
  } else {
    std::cerr << "gti: output " << plan.output().string() << '\n';
  }
}

void reportProjectSuccess(std::ostream &stream, std::string_view action,
                          const lang::driver::ProjectBuildPlan &plan) {
  stream << terminalStyle.code(stream, Style::Success) << action
         << terminalStyle.code(stream, Style::Reset) << ' ' << plan.targetName()
         << " [" << plan.profileName() << ", "
         << lang::driver::targetTriple(plan.target()) << "] -> "
         << plan.output().string() << '\n';
}

int invokeProjectProgram(const lang::driver::ProjectBuildPlan &plan,
                         const ProjectOptions &options, bool skippedBuild) {
  std::vector<std::string> command{plan.output().string()};
  command.insert(command.end(), options.programArguments.begin(),
                 options.programArguments.end());
  std::cerr << terminalStyle.code(std::cerr, Style::Note)
            << (skippedBuild ? "Running existing" : "Running")
            << terminalStyle.code(std::cerr, Style::Reset) << ' '
            << plan.output().string();
  if (skippedBuild) {
    std::cerr << " (--no-build)";
  }
  std::cerr << '\n';
  const lang::driver::ProcessResult process = lang::driver::invokeProcess(
      command, {.outputMode = lang::driver::ProcessOutputMode::Inherit,
                .captureSuccessfulOutput = false,
                .description = "program",
                .environment = programColorEnvironment()});
  if (process.driverDiagnostic) {
    std::cerr << *process.driverDiagnostic << '\n';
  }
  return process.exitCode;
}

// Resolves the project command's selected target. Omitting `--target`
// keeps the build host, so an existing manifest needs no change. An
// explicit triple is parsed with the compiler's own vocabulary; a malformed
// or unsupported spelling is a usage error, and a triple GTI can name but
// cannot lay out is a toolchain-configuration error.
struct ProjectTargetSelection {
  std::optional<lang::TargetInfo> target;
  int failure = 0;
};

ProjectTargetSelection selectProjectTarget(const ProjectOptions &options) {
  if (!options.targetTriple) {
    return {.target = lang::TargetInfo::host()};
  }
  // `run` and `test` execute the produced program on this machine. GTI
  // models no runner or emulator, so a foreign target is refused here
  // rather than left to fail as an exec-format error after a full build.
  if (options.command == ProjectCommand::Run ||
      options.command == ProjectCommand::Test) {
    const lang::TargetTripleParseResult requested =
        lang::parseTargetTripleResult(*options.targetTriple);
    const lang::TargetInfo host = lang::TargetInfo::host();
    if (requested.succeeded() &&
        lang::driver::targetTriple(*requested.target) !=
            lang::driver::targetTriple(host)) {
      std::cerr << "gti: " << projectCommandName(options.command)
                << " executes the built program on this machine and cannot "
                   "select target '"
                << *options.targetTriple << "'\n"
                << "gti: build it with gti build --target "
                << *options.targetTriple << '\n';
      return {.failure = exitCode(ExitStatus::Usage)};
    }
  }
  const lang::TargetTripleParseResult parsed =
      lang::parseTargetTripleResult(*options.targetTriple);
  if (!parsed.succeeded()) {
    std::cerr << "gti: invalid --target '" << *options.targetTriple
              << "': " << lang::targetTripleErrorMessage(parsed.error) << '\n';
    return {.failure = exitCode(ExitStatus::Usage)};
  }
  if (!parsed.target->supported()) {
    std::cerr << "gti: target '" << *options.targetTriple
              << "' has no supported GTI data layout\n";
    return {.failure = exitCode(ExitStatus::ToolchainConfiguration)};
  }
  return {.target = *parsed.target};
}

// `gti check` performs no native compilation, so it analyzes any supported
// target without a declared toolchain. Every command that publishes a native
// artifact fails closed instead of reusing host tools.
lang::driver::ToolchainSelectionPolicy
projectToolchainPolicy(const ProjectOptions &options) {
  return options.command == ProjectCommand::Check
             ? lang::driver::ToolchainSelectionPolicy::HostTools
             : lang::driver::ToolchainSelectionPolicy::DeclaredForNonHostTarget;
}

// The project facts a direct compilation has no concept of. They let a
// recorded report name the exact package, target, profile, and resolved
// project model that produced it.
lang::driver::BuildProjectProvenance
projectProvenance(const lang::driver::ProjectBuildPlan &plan) {
  return {.packageName = plan.packageName(),
          .packageVersion = plan.packageVersion(),
          .targetName = plan.targetName(),
          .targetKind = std::string(
              lang::driver::projectTargetKindName(plan.targetKind())),
          .profileName = plan.profileName(),
          .targetTriple = lang::driver::targetTriple(plan.target()),
          .modelIdentity = plan.projectModelIdentity()};
}

int buildProjectPlan(
    const lang::driver::ProjectBuildPlan &plan, const ProjectOptions &options,
    const lang::driver::ToolchainLayout &toolchain,
    const lang::SourceManager *diagnosticSources = nullptr,
    const char *driver = nullptr,
    std::optional<lang::driver::BuildProvenance> *capturedProvenance = nullptr,
    DiagnosticSummary *capturedDiagnostics = nullptr) {
  lang::cli::BuildProgress progress(
      terminalStyle, progressMode, plan.targetName(),
      plan.profileName() + ", " + lang::driver::targetTriple(plan.target()),
      lang::cli::optimizationNumber(plan.optimization()), options.verbose);
  // Configuration precedence: an explicit command-line option, then the
  // manifest's declared toolchain for the selected target, then the existing
  // environment/default discovery order. A declared toolchain deliberately
  // outranks GTI_CXX/CXX, because those describe the host.
  const lang::driver::ResolvedProjectToolchain &declaredToolchain =
      plan.toolchain();
  const std::optional<std::string> selectedCxx =
      options.cxx ? options.cxx
      : declaredToolchain.declared()
          ? std::optional<std::string>(declaredToolchain.cxx)
          : std::nullopt;
  const std::optional<std::string> selectedCc =
      options.cc ? options.cc
      : declaredToolchain.declared() && !declaredToolchain.cc.empty()
          ? std::optional<std::string>(declaredToolchain.cc)
          : std::nullopt;
  std::optional<std::string> cCompiler;
  const bool dependencyCSources =
      std::any_of(plan.nativeInputs().dependencyGroups.begin(),
                  plan.nativeInputs().dependencyGroups.end(),
                  [](const lang::driver::NativeDependencyGroup &group) {
                    return !group.cSources.empty();
                  });
  if (!plan.buildScripts().empty() || !plan.nativeInputs().cSources.empty() ||
      dependencyCSources) {
    cCompiler = lang::driver::discoverCCompiler(selectedCc);
  }
  const std::string cppCompiler =
      lang::driver::discoverNativeCompiler(selectedCxx);
  lang::driver::NativeInputs nativeInputs = plan.nativeInputs();
  if (!plan.buildScripts().empty()) {
    const unsigned defaultJobs =
        std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    const std::string hostCppCompiler =
        lang::driver::targetTriple(plan.target()) ==
                lang::driver::targetTriple(lang::TargetInfo::host())
            ? cppCompiler
            : lang::driver::discoverNativeCompiler();
    const lang::driver::BuildScriptExecutionResult scripts = [&] {
      const lang::ProgressScope scriptProgress(
          progress.observer(), lang::ProgressPhase::NativeDependency);
      return lang::driver::executeBuildScripts(
          plan, {.cCompiler = cCompiler.value_or("cc"),
                 .cppCompiler = cppCompiler,
                 .hostCppCompiler = hostCppCompiler,
                 .compilerIdentity = std::string(version),
                 .hostToolchain = toolchain,
                 .jobs = options.jobs ? static_cast<unsigned>(*options.jobs)
                                      : defaultJobs,
                 .verbose = options.verbose});
    }();
    if (!scripts.succeeded()) {
      progress.suspend();
      const lang::SourceManager emptySources;
      const lang::SourceManager &sources =
          scripts.diagnosticSources      ? *scripts.diagnosticSources
          : diagnosticSources == nullptr ? emptySources
                                         : *diagnosticSources;
      reportDiagnostics(scripts.diagnostics, sources);
      DiagnosticSummary summary = summarizeDiagnostics(scripts.diagnostics);
      summary.errors = std::max<std::size_t>(summary.errors, 1);
      if (capturedDiagnostics != nullptr) {
        *capturedDiagnostics = summary;
      }
      std::cerr.flush();
      reportDiagnosticSummary(std::cout, summary);
      return exitCode(ExitStatus::Compilation);
    }
    nativeInputs = *scripts.nativeInputs;
    for (const lang::driver::BuildScriptOutcome &outcome : scripts.outcomes) {
      if (options.verbose && !outcome.output.empty()) {
        std::cerr << outcome.output;
      }
      std::cerr << terminalStyle.code(std::cerr, Style::Success)
                << (outcome.reused ? "Reused" : "Built")
                << terminalStyle.code(std::cerr, Style::Reset)
                << " native dependency " << outcome.packageIdentity << '\n';
    }
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

  // Provenance capture is only requested when the caller asked for it; the
  // driver then bypasses whole-program cache reuse so the record always
  // describes a performed native invocation.
  std::optional<lang::driver::BuildReportRequest> buildReport;
  if (options.buildReport && driver != nullptr) {
    buildReport = lang::driver::BuildReportRequest{
        .output = *options.buildReport,
        .producerExecutable = lang::executablePath(driver),
        .project = projectProvenance(plan)};
  }

  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(
          lang::driver::ExecutableBuildRequest(
              lang::driver::CompilationRequest(
                  plan.entry(), toolchain.standardLibrary, plan.target(),
                  plan.optimization(), plan.cppStandard(),
                  plan.packageSources(), plan.configurationFlags(), {},
                  plan.warnings(), plan.debugInfo(), plan.packageWarnings()),
              toolchain, plan.generatedSource(), plan.output(), cppCompiler,
              nativeInputs, plan.keepCpp(), true, options.verbose,
              lang::driver::ManagedOutputPolicy{
                  .trustedRoot = plan.workspaceRoot(),
                  .outputRoot = plan.workspaceRoot() / "build" / "gti"},
              std::move(cCompiler), std::move(cache), std::move(buildReport)),
          progress.observer());
  progress.suspend();
  const int status = reportBuildResult(result, options.verbose);
  if (capturedDiagnostics != nullptr) {
    *capturedDiagnostics = summarizeDiagnostics(result.compilation.diagnostics);
  }
  if (capturedProvenance != nullptr) {
    *capturedProvenance = result.provenance;
  }
  progress.finish(status == exitCode(ExitStatus::Success));
  return status;
}

// Builds every declared target by delegating each one to a child `gti build
// <target>` invocation. Children are isolated whole-program builds, so a
// bounded number may run concurrently; the parent replays each child's
// captured output in deterministic target-name order and reports the first
// failing target's status regardless of scheduling.
int runProjectBuildAll(const ProjectOptions &options, const char *driver) {
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
  overrides.debugInfo = options.debugInfo;
  overrides.configurationFlags = options.configurationFlags;
  overrides.warningPolicy = lang::WarningPolicy(options.disabledWarnings);
  const ProjectTargetSelection selection = selectProjectTarget(options);
  if (!selection.target) {
    return selection.failure;
  }
  const lang::driver::ProjectTargetSetResolutionResult resolution =
      lang::driver::resolveAllProjectTargets(lang::driver::ProjectBuildRequest(
          *currentDirectory, std::nullopt, options.profile, *selection.target,
          std::move(overrides), options.package, {},
          lang::driver::RuntimeProfile::Hosted, projectToolchainPolicy(options),
          {.cCompiler = options.cc.has_value()}));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  const std::size_t targetCount = resolution.plans.size();
  const unsigned hardwareJobs =
      std::max(1u, std::thread::hardware_concurrency());
  const std::size_t boundedJobs = std::min<std::size_t>(
      targetCount,
      options.jobs ? static_cast<std::size_t>(*options.jobs) : hardwareJobs);
  if (options.verbose) {
    std::cerr << "gti: building " << targetCount << " targets with "
              << boundedJobs << " parallel jobs\n";
  }

  const bool childColor =
      terminalStyle.enabled(std::cout) || terminalStyle.enabled(std::cerr);
  lang::cli::BuildProgress progress(terminalStyle, progressMode, "all targets",
                                    options.profile, 0, options.verbose);
  const auto childCommand = [&](const lang::driver::ProjectBuildPlan &plan) {
    std::vector<std::string> command{
        driver,   "build", plan.targetName(), "--profile", options.profile,
        "--jobs", "1"};
    // Children are captured, so resolve auto against the parent's destinations.
    command.emplace_back(childColor ? "--color=always" : "--color=never");
    // Captured child telemetry would not be live and would make replay depend
    // on elapsed times. The parent reports the actual target wait instead.
    command.emplace_back("--progress=never");
    if (options.package) {
      command.emplace_back("--package");
      command.push_back(*options.package);
    }
    if (options.targetTriple) {
      command.emplace_back("--target");
      command.push_back(*options.targetTriple);
    }
    if (options.cxx) {
      command.emplace_back("--cxx");
      command.push_back(*options.cxx);
    }
    if (options.cc) {
      command.emplace_back("--cc");
      command.push_back(*options.cc);
    }
    if (options.standard) {
      command.emplace_back("--std");
      command.emplace_back(lang::cli::cppStandardName(*options.standard));
    }
    if (options.optimization) {
      command.emplace_back("-O" + std::to_string(lang::cli::optimizationNumber(
                                      *options.optimization)));
    }
    if (options.executionProfile) {
      command.emplace_back("--execution-profile");
      command.emplace_back(
          lang::executionProfileName(*options.executionProfile));
    }
    if (options.keepCpp) {
      command.emplace_back(*options.keepCpp ? "--keep-cpp" : "--no-keep-cpp");
    }
    if (options.debugInfo) {
      command.emplace_back(*options.debugInfo ? "--debug-info"
                                              : "--no-debug-info");
    }
    for (const std::string &flag : options.configurationFlags) {
      command.emplace_back("-D");
      command.push_back(flag);
    }
    for (const std::string &group : options.disabledWarnings) {
      command.emplace_back("-Wno-" + group);
    }
    if (!options.useCache) {
      command.emplace_back("--no-cache");
    }
    if (options.verbose) {
      command.emplace_back("--verbose");
    }
    return command;
  };

  std::vector<lang::driver::StartedProcess> running;
  running.reserve(targetCount);
  std::size_t nextToStart = 0;
  const auto startNext = [&]() {
    const lang::driver::ProjectBuildPlan &plan = resolution.plans[nextToStart];
    running.push_back(lang::driver::startProcess(
        childCommand(plan),
        {.captureSuccessfulOutput = true,
         .description = "build of target '" + plan.targetName() + "'"}));
    ++nextToStart;
  };
  while (nextToStart < std::min(boundedJobs, targetCount)) {
    startNext();
  }

  int firstFailure = exitCode(ExitStatus::Success);
  DiagnosticSummary diagnosticSummary;
  for (std::size_t index = 0; index < targetCount; ++index) {
    lang::ProgressScope targetProgress(
        progress.observer(), lang::ProgressPhase::Targets,
        "waiting for " + resolution.plans[index].targetName() + " [" +
            std::to_string(index + 1) + "/" + std::to_string(targetCount) +
            "]");
    const lang::driver::ProcessResult result = running[index].wait();
    targetProgress.finish();
    if (nextToStart < targetCount) {
      startNext();
    }
    const lang::driver::ProjectBuildPlan &plan = resolution.plans[index];
    if (const std::optional<DiagnosticSummary> childSummary =
            parseDiagnosticSummary(result.output)) {
      addDiagnosticSummary(diagnosticSummary, *childSummary);
    } else if (!result.succeeded()) {
      ++diagnosticSummary.errors;
    }
    if (result.succeeded()) {
      std::cout << (!childColor || terminalStyle.enabled(std::cout)
                        ? result.output
                        : lang::cli::withoutStyles(result.output));
      continue;
    }
    std::cerr << (!childColor || terminalStyle.enabled(std::cerr)
                      ? result.output
                      : lang::cli::withoutStyles(result.output));
    if (result.driverDiagnostic) {
      std::cerr << *result.driverDiagnostic << '\n';
    }
    std::cerr << "gti: build --all: target '" << plan.targetName() << "' "
              << terminalStyle.code(std::cerr, Style::Error) << "failed"
              << terminalStyle.code(std::cerr, Style::Reset)
              << " with exit code " << result.exitCode << '\n';
    if (firstFailure == exitCode(ExitStatus::Success)) {
      firstFailure = result.exitCode;
    }
  }
  if (firstFailure == exitCode(ExitStatus::Success)) {
    std::cout << terminalStyle.code(std::cout, Style::Success) << "Built"
              << terminalStyle.code(std::cout, Style::Reset) << ' '
              << targetCount << (targetCount == 1 ? " target" : " targets")
              << " [" << options.profile << "]\n";
  }
  std::cerr.flush();
  reportDiagnosticSummary(std::cout, diagnosticSummary);
  progress.finish(firstFailure == exitCode(ExitStatus::Success));
  return firstFailure;
}

// Generates manifest-declared bindgen imports into the plan's synthetic
// package source roots before source loading. Notes (native preprocessor
// output, importer skip reports) print on failure or under --verbose; the
// generated file itself carries the full skip report.
int generateProjectBindgenImports(const lang::driver::ProjectBuildPlan &plan,
                                  const lang::SourceManager &sources,
                                  bool verbose, std::ostream &statusStream) {
  if (plan.bindgenImports().empty()) {
    return exitCode(ExitStatus::Success);
  }
  const lang::driver::BindgenImportGenerationResult generation =
      lang::driver::generateBindgenImports(
          plan, lang::driver::discoverBindgenClang(), version);
  for (const lang::driver::BindgenImportOutcome &outcome :
       generation.outcomes) {
    if (verbose || !outcome.succeeded()) {
      for (const std::string &note : outcome.notes) {
        std::cerr << note << '\n';
      }
    }
    switch (outcome.status) {
    case lang::driver::BindgenImportStatus::Generated:
      statusStream << "Generated bindings " << outcome.alias << " ("
                   << outcome.importedCount << " imported, "
                   << outcome.skippedCount << " skipped) -> "
                   << outcome.output.string() << '\n';
      break;
    case lang::driver::BindgenImportStatus::Reused:
      if (verbose) {
        statusStream << "Reused bindings " << outcome.alias << " -> "
                     << outcome.output.string() << '\n';
      }
      break;
    case lang::driver::BindgenImportStatus::PreprocessingFailure:
    case lang::driver::BindgenImportStatus::GenerationFailure:
    case lang::driver::BindgenImportStatus::PublicationFailure:
      break;
    }
    reportDiagnostics(outcome.diagnostics, sources);
  }
  return generation.succeeded() ? exitCode(ExitStatus::Success)
                                : exitCode(ExitStatus::Compilation);
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
  overrides.debugInfo = options.debugInfo;
  overrides.configurationFlags = options.configurationFlags;
  overrides.warningPolicy = lang::WarningPolicy(options.disabledWarnings);
  const ProjectTargetSelection selection = selectProjectTarget(options);
  if (!selection.target) {
    return selection.failure;
  }
  lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          *currentDirectory, options.target, options.profile, *selection.target,
          std::move(overrides), options.package,
          {.requireLock = true,
           .allowAcquisition =
               !(options.offline || options.locked || options.noBuild)},
          lang::driver::RuntimeProfile::Hosted, projectToolchainPolicy(options),
          {.cCompiler = options.cc.has_value()}));
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
  if (options.noBuild) {
    const lang::driver::ManagedOutputPolicy managed{
        .trustedRoot = plan.workspaceRoot(),
        .outputRoot = plan.workspaceRoot() / "build" / "gti"};
    if (const std::optional<std::string> diagnostic =
            lang::driver::managedExecutableDiagnostic(managed, plan.output())) {
      std::cerr << *diagnostic << '\n'
                << "gti: build target '" << plan.targetName()
                << "' before using --no-build\n";
      return exitCode(ExitStatus::Io);
    }
    return invokeProjectProgram(plan, options, true);
  }
  {
    std::ostream &bindgenStream =
        options.command == ProjectCommand::Run ? std::cerr : std::cout;
    const int bindgenStatus = generateProjectBindgenImports(
        plan, resolution.sources, options.verbose, bindgenStream);
    if (bindgenStatus != exitCode(ExitStatus::Success)) {
      return bindgenStatus;
    }
  }
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);

  if (options.command == ProjectCommand::Build && options.emitMir) {
    lang::cli::BuildProgress progress(
        terminalStyle, progressMode, plan.targetName(),
        plan.profileName() + ", " + lang::driver::targetTriple(plan.target()),
        lang::cli::optimizationNumber(plan.optimization()), options.verbose);
    const lang::driver::CompilationResult compilation =
        lang::driver::compileToMir(
            lang::driver::CompilationRequest(
                plan.entry(), toolchain.standardLibrary, plan.target(),
                plan.optimization(), plan.cppStandard(), plan.packageSources(),
                plan.configurationFlags(), {}, plan.warnings(), false,
                plan.packageWarnings()),
            progress.observer());
    progress.suspend();
    if (!compilation.succeeded()) {
      return reportCompilationFailure(compilation, options.verbose);
    }
    reportDiagnostics(compilation.diagnostics, compilation.sources);
    std::filesystem::path output = plan.output();
    output += ".mir";
    if (const std::optional<std::filesystem::path> collision =
            lang::driver::findLoadedSourceCollision(output,
                                                    compilation.sources)) {
      std::cerr << "gti: refusing to overwrite loaded source '"
                << collision->string() << "' with emitted MIR output '"
                << output.string() << "'\n";
      return exitCode(ExitStatus::Usage);
    }
    std::error_code directoryError;
    std::filesystem::create_directories(output.parent_path(), directoryError);
    if (directoryError || !writeFile(output, compilation.artifact->contents)) {
      return exitCode(ExitStatus::Io);
    }
    std::cout << terminalStyle.code(std::cout, Style::Success) << "Emitted MIR"
              << terminalStyle.code(std::cout, Style::Reset) << ' '
              << plan.targetName() << " [" << plan.profileName() << ", "
              << lang::driver::targetTriple(plan.target()) << "] -> "
              << output.string() << '\n';
    reportDiagnosticSummary(std::cout,
                            summarizeDiagnostics(compilation.diagnostics));
    progress.finish(true);
    return exitCode(ExitStatus::Success);
  }

  if (options.command == ProjectCommand::Check) {
    lang::cli::BuildProgress progress(
        terminalStyle, progressMode, plan.targetName(),
        plan.profileName() + ", " + lang::driver::targetTriple(plan.target()),
        lang::cli::optimizationNumber(plan.optimization()), options.verbose);
    const lang::driver::CheckResult result = lang::driver::checkCompilation(
        lang::driver::CompilationRequest(
            plan.entry(), toolchain.standardLibrary, plan.target(),
            plan.optimization(), plan.cppStandard(), plan.packageSources(),
            plan.configurationFlags(), {}, plan.warnings(), false,
            plan.packageWarnings())
            .forExecutable(),
        progress.observer());
    progress.suspend();
    if (!result.succeeded()) {
      reportDiagnostics(result.diagnostics, result.sources);
      DiagnosticSummary summary = summarizeDiagnostics(result.diagnostics);
      summary.errors = std::max<std::size_t>(summary.errors, 1);
      std::cerr.flush();
      reportDiagnosticSummary(std::cout, summary);
      return exitCode(ExitStatus::Compilation);
    }
    reportDiagnostics(result.diagnostics, result.sources);
    std::cout << terminalStyle.code(std::cout, Style::Success) << "Checked"
              << terminalStyle.code(std::cout, Style::Reset) << ' '
              << plan.targetName() << " [" << plan.profileName() << ", "
              << lang::driver::targetTriple(plan.target()) << "] -> "
              << plan.entry().string() << '\n';
    reportDiagnosticSummary(std::cout,
                            summarizeDiagnostics(result.diagnostics));
    progress.finish(true);
    return exitCode(ExitStatus::Success);
  }

  std::optional<lang::driver::BuildProvenance> provenance;
  DiagnosticSummary diagnosticSummary;
  const int status =
      buildProjectPlan(plan, options, toolchain, &resolution.sources, driver,
                       &provenance, &diagnosticSummary);
  if (status != exitCode(ExitStatus::Success)) {
    return status;
  }

  std::ostream &statusStream =
      options.command == ProjectCommand::Run ? std::cerr : std::cout;
  reportProjectSuccess(statusStream, "Built", plan);
  if (options.buildReport) {
    if (!provenance) {
      std::cerr << "gti: internal compiler error: requested build report was "
                   "not captured\n";
      return exitCode(ExitStatus::Compilation);
    }
    if (!writeFileAtomically(
            *options.buildReport,
            lang::driver::serializeBuildProvenanceJson(*provenance))) {
      return exitCode(ExitStatus::Io);
    }
    statusStream << "Build report " << options.buildReport->string() << '\n';
  }
  if (plan.keepCpp()) {
    statusStream << "Kept backend representation "
                 << plan.generatedSource().string() << '\n';
  }
  reportDiagnosticSummary(statusStream, diagnosticSummary);

  if (options.command == ProjectCommand::Run) {
    return invokeProjectProgram(plan, options, false);
  }
  return exitCode(ExitStatus::Success);
}

// Re-runs the build a recorded report describes and names every recorded
// fact the current tree no longer reproduces. The recorded report is read
// and never written: verification must not be able to repair its own
// evidence.
int runProjectVerify(const ProjectOptions &options, const char *driver) {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }

  const std::filesystem::path &reportPath = *options.buildReport;
  std::ifstream reportStream(reportPath, std::ios::binary);
  if (!reportStream) {
    std::cerr << "gti: failed to read build report '" << reportPath.string()
              << "'\n";
    return exitCode(ExitStatus::Io);
  }
  const std::string reportText{std::istreambuf_iterator<char>(reportStream),
                               std::istreambuf_iterator<char>()};
  std::string parseError;
  const std::optional<lang::driver::BuildProvenance> recorded =
      lang::driver::parseBuildProvenanceJson(reportText, parseError);
  if (!recorded) {
    std::cerr << "gti: '" << reportPath.string()
              << "' is not a build report this compiler can verify: "
              << parseError << '\n';
    return exitCode(ExitStatus::Usage);
  }
  if (!recorded->project) {
    std::cerr << "gti: '" << reportPath.string()
              << "' records a direct compilation; gti verify reproduces "
                 "project builds recorded by gti build --build-report\n";
    return exitCode(ExitStatus::Usage);
  }

  lang::driver::ProjectBuildOverrides overrides;
  overrides.optimization = options.optimization;
  overrides.cppStandard = options.standard;
  overrides.executionProfile = options.executionProfile;
  overrides.keepCpp = options.keepCpp;
  overrides.debugInfo = options.debugInfo.value_or(recorded->debugInfo);
  overrides.configurationFlags = options.configurationFlags;
  overrides.warningPolicy = lang::WarningPolicy(options.disabledWarnings);
  // The record names the target it was built for. Reproducing it on the
  // build host would verify a different build, so the recorded triple is
  // parsed back and selected exactly as `gti build --target` selected it.
  const lang::TargetTripleParseResult recordedTarget =
      lang::parseTargetTripleResult(recorded->project->targetTriple);
  if (!recordedTarget.succeeded() || !recordedTarget.target->supported()) {
    std::cerr << "gti: '" << reportPath.string() << "' records target triple '"
              << recorded->project->targetTriple
              << "', which this compiler cannot select\n";
    return exitCode(ExitStatus::Usage);
  }
  const bool recordedHostTarget =
      lang::driver::targetTriple(*recordedTarget.target) ==
      lang::driver::targetTriple(lang::TargetInfo::host());
  lang::driver::ProjectResolutionResult resolution =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          *currentDirectory, recorded->project->targetName,
          recorded->project->profileName, *recordedTarget.target,
          std::move(overrides), recorded->project->packageName,
          {.requireLock = true,
           .allowAcquisition = !(options.offline || options.locked)},
          lang::driver::RuntimeProfile::Hosted,
          recordedHostTarget ? lang::driver::ToolchainSelectionPolicy::HostTools
                             : lang::driver::ToolchainSelectionPolicy::
                                   DeclaredForNonHostTarget,
          {.cCompiler = options.cc.has_value()}));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    std::cerr << "gti: cannot resolve the recorded package '"
              << recorded->project->packageName << "' target '"
              << recorded->project->targetName << "' [profile "
              << recorded->project->profileName << "] from "
              << currentDirectory->string() << '\n';
    return exitCode(ExitStatus::Compilation);
  }

  const lang::driver::ProjectBuildPlan &plan = *resolution.plan;
  if (options.verbose) {
    reportProjectPlan(plan, options.command);
  }
  {
    const int bindgenStatus = generateProjectBindgenImports(
        plan, resolution.sources, options.verbose, std::cout);
    if (bindgenStatus != exitCode(ExitStatus::Success)) {
      return bindgenStatus;
    }
  }
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  std::optional<lang::driver::BuildProvenance> rebuilt;
  const int status = buildProjectPlan(plan, options, toolchain,
                                      &resolution.sources, driver, &rebuilt);
  if (status != exitCode(ExitStatus::Success)) {
    std::cerr << "gti: the recorded build no longer completes; '"
              << reportPath.string() << "' cannot be reproduced\n";
    return status;
  }
  if (!rebuilt) {
    std::cerr << "gti: internal compiler error: verification build captured "
                 "no provenance\n";
    return exitCode(ExitStatus::Compilation);
  }

  const std::vector<lang::driver::BuildProvenanceDifference> differences =
      lang::driver::compareBuildProvenance(*recorded, *rebuilt);
  if (!differences.empty()) {
    std::cerr << "gti: build report '" << reportPath.string()
              << "' does not describe this tree\n";
    for (const lang::driver::BuildProvenanceDifference &difference :
         differences) {
      std::cerr << "gti:   " << difference.subject << ": " << difference.field
                << "\ngti:     recorded: " << difference.expected
                << "\ngti:     rebuilt:  " << difference.actual << '\n';
    }
    std::cerr << "gti: " << differences.size()
              << (differences.size() == 1 ? " difference\n" : " differences\n");
    return exitCode(ExitStatus::Verification);
  }

  reportProjectSuccess(std::cout, "Verified", plan);
  std::cout << "Build report " << reportPath.string()
            << " reproduced: " << recorded->source.units.size()
            << " source units, " << recorded->runtime.headers.size()
            << " runtime headers, the runtime archive, the native compiler "
               "identity, the native command, and the produced artifact\n";
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
  overrides.debugInfo = options.debugInfo;
  overrides.configurationFlags = options.configurationFlags;
  overrides.warningPolicy = lang::WarningPolicy(options.disabledWarnings);
  const ProjectTargetSelection selection = selectProjectTarget(options);
  if (!selection.target) {
    return selection.failure;
  }
  lang::driver::ProjectTestResolutionResult resolution =
      lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
          *currentDirectory, options.target, options.profile, *selection.target,
          std::move(overrides), options.package,
          {.requireLock = true,
           .allowAcquisition = !(options.offline || options.locked)},
          lang::driver::RuntimeProfile::Hosted, projectToolchainPolicy(options),
          {.cCompiler = options.cc.has_value()}));
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  {
    const int bindgenStatus = generateProjectBindgenImports(
        resolution.plans.front(), resolution.sources, options.verbose,
        std::cout);
    if (bindgenStatus != exitCode(ExitStatus::Success)) {
      return bindgenStatus;
    }
  }
  const lang::driver::ToolchainLayout toolchain =
      lang::driver::discoverToolchainLayout(driver);
  std::size_t passed = 0;
  std::size_t failed = 0;
  int firstFailure = exitCode(ExitStatus::Success);
  DiagnosticSummary diagnosticSummary;
  for (const lang::driver::ProjectBuildPlan &plan : resolution.plans) {
    if (options.verbose) {
      reportProjectPlan(plan, ProjectCommand::Test);
    }
    std::cerr << terminalStyle.code(std::cerr, Style::Note) << "Building test"
              << terminalStyle.code(std::cerr, Style::Reset) << ' '
              << plan.targetName() << '\n';
    DiagnosticSummary buildSummary;
    const int buildStatus =
        buildProjectPlan(plan, options, toolchain, &resolution.sources, nullptr,
                         nullptr, &buildSummary);
    if (buildStatus != exitCode(ExitStatus::Success)) {
      return buildStatus;
    }
    addDiagnosticSummary(diagnosticSummary, buildSummary);

    reportProjectSuccess(std::cerr, "Built", plan);
    if (plan.keepCpp()) {
      std::cerr << "Kept backend representation "
                << plan.generatedSource().string() << '\n';
    }
    std::cerr << terminalStyle.code(std::cerr, Style::Note) << "Testing"
              << terminalStyle.code(std::cerr, Style::Reset) << ' '
              << plan.targetName() << '\n';
    const lang::driver::ProcessResult process = lang::driver::invokeProcess(
        {plan.output().string()},
        {.outputMode = lang::driver::ProcessOutputMode::Inherit,
         .captureSuccessfulOutput = false,
         .description = "test target '" + plan.targetName() + "'",
         .environment = programColorEnvironment()});
    if (process.driverDiagnostic) {
      std::cerr << *process.driverDiagnostic << '\n';
    }
    if (process.succeeded()) {
      ++passed;
      std::cerr << terminalStyle.code(std::cerr, Style::Success) << "Passed"
                << terminalStyle.code(std::cerr, Style::Reset) << ' '
                << plan.targetName() << '\n';
      continue;
    }

    ++failed;
    if (firstFailure == exitCode(ExitStatus::Success)) {
      firstFailure = process.exitCode;
    }
    std::cerr << terminalStyle.code(std::cerr, Style::Error) << "Failed"
              << terminalStyle.code(std::cerr, Style::Reset) << ' '
              << plan.targetName() << " (exit code " << process.exitCode
              << ")\n";
  }

  std::cerr << "Test result: " << terminalStyle.code(std::cerr, Style::Success)
            << passed << " passed"
            << terminalStyle.code(std::cerr, Style::Reset);
  if (failed != 0) {
    std::cerr << ", " << terminalStyle.code(std::cerr, Style::Error) << failed
              << " failed" << terminalStyle.code(std::cerr, Style::Reset);
  }
  std::cerr << '\n';
  reportDiagnosticSummary(std::cerr, diagnosticSummary);
  return firstFailure;
}

int runFetch(const FetchOptions &options) {
  const std::optional<std::filesystem::path> currentDirectory =
      workingDirectory();
  if (!currentDirectory) {
    return exitCode(ExitStatus::Io);
  }

  // Fetch is the lock's only writer: it resolves the manifest closure
  // without requiring gti.lock, materializes every pinned git source (or
  // verifies the store when offline), and records the result.
  lang::driver::WorkspaceResolutionResult resolution =
      lang::driver::resolveProjectWorkspace(
          *currentDirectory, std::nullopt,
          {.requireLock = options.locked,
           .allowAcquisition = !options.offline,
           .acquireRegistry = !options.offline,
           .materializeRegistry = true});
  if (!resolution.succeeded()) {
    reportDiagnostics(resolution.diagnostics, resolution.sources);
    return exitCode(ExitStatus::Compilation);
  }

  const lang::driver::ProjectWorkspace &workspace = *resolution.workspace;
  const lang::driver::DependencyLock lock =
      lang::driver::lockFromWorkspace(workspace);
  const std::filesystem::path lockPath =
      lang::driver::dependencyLockPath(workspace.root());

  std::error_code existsError;
  const bool lockExists =
      std::filesystem::is_regular_file(lockPath, existsError) && !existsError;
  const auto packageCount = lock.packages.size() + lock.registryPackages.size();
  if (packageCount == 0 && !lockExists) {
    std::cout << "No git dependencies to lock\n";
    return exitCode(ExitStatus::Success);
  }

  for (const lang::driver::LockedGitPackage &package : lock.packages) {
    std::cout << "Locked " << package.name << '@' << package.version << " git+"
              << package.url << '#' << package.revision << '\n';
  }
  for (const auto &package : lock.registryPackages) {
    std::cout << "Locked " << package.package << '@' << package.version
              << " registry " << package.registry << '\n';
  }
  const std::string dependencyDescription =
      lock.registryPackages.empty() ? " git dependenc" : " dependenc";

  const std::string rendered = lang::driver::renderDependencyLock(lock);
  if (lockExists) {
    std::ifstream existing(lockPath, std::ios::binary);
    const std::string current{std::istreambuf_iterator<char>(existing),
                              std::istreambuf_iterator<char>()};
    if (existing && current == rendered) {
      std::cout << "gti.lock is up to date (" << packageCount
                << dependencyDescription << (packageCount == 1 ? "y" : "ies")
                << ")\n";
      return exitCode(ExitStatus::Success);
    }
  }
  if (options.locked) {
    std::cerr << "gti: --locked refuses to change gti.lock; run `gti fetch` "
                 "without --locked to reconcile it\n";
    return exitCode(ExitStatus::Compilation);
  }
  std::string writeError;
  if (!lang::driver::writeDependencyLock(workspace.root(), lock, writeError)) {
    std::cerr << "gti: failed to write gti.lock: " << writeError << '\n';
    return exitCode(ExitStatus::Io);
  }
  std::cout << "Wrote " << lockPath.string() << " (" << packageCount
            << dependencyDescription << (packageCount == 1 ? "y" : "ies")
            << ")\n";
  return exitCode(ExitStatus::Success);
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
          *currentDirectory, lang::TargetInfo::host(), options.package,
          lang::driver::RuntimeProfile::Hosted);
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, result.sources);
    return exitCode(ExitStatus::Compilation);
  }
  lang::cli::writeProjectMetadata(std::cout, *result.metadata);
  return exitCode(ExitStatus::Success);
}

int runScaffold(const ScaffoldOptions &options) {
  std::optional<lang::driver::ProjectScaffoldGit> git;
  if (options.initializeGit) {
    git = lang::driver::ProjectScaffoldGit{
        .executable = lang::driver::discoverGitExecutable()};
  }
  const lang::driver::ProjectScaffoldResult result =
      lang::driver::scaffoldProject(lang::driver::ProjectScaffoldRequest(
          options.mode, options.destination, options.packageName,
          std::move(git)));
  if (!result.succeeded()) {
    reportDiagnostics(result.diagnostics, {});
    if (result.status ==
        lang::driver::ProjectScaffoldStatus::FilesystemFailure) {
      return exitCode(ExitStatus::Io);
    }
    if (result.status == lang::driver::ProjectScaffoldStatus::ToolFailure) {
      return exitCode(ExitStatus::ToolchainConfiguration);
    }
    return exitCode(ExitStatus::Usage);
  }

  std::cout << (options.mode == lang::driver::ProjectScaffoldMode::NewPackage
                    ? "Created"
                    : "Initialized")
            << " package '" << result.packageName << "' at "
            << result.packageRoot.string() << '\n';
  if (result.initializedGit) {
    std::cout << "Initialized Git repository and wrote .gitignore\n";
  }
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

int runBindgen(int argc, char *argv[]) {
  BindgenCliOptions options;
  for (int index = 2; index < argc; ++index) {
    const auto color = parseColorArgument(argc, argv, index);
    if (color == ColorArgument::Invalid) {
      return exitCode(ExitStatus::Usage);
    }
    if (color == ColorArgument::Accepted) {
      continue;
    }
    const std::string_view argument = argv[index];
    if (argument == "-o" || argument == "--output") {
      if (index + 1 >= argc) {
        std::cerr << "gti: " << argument << " requires a path\n";
        return exitCode(ExitStatus::Usage);
      }
      options.output = argv[++index];
      continue;
    }
    if (argument == "--namespace") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --namespace requires a name\n";
        return exitCode(ExitStatus::Usage);
      }
      options.bindings.namespaceName = argv[++index];
      continue;
    }
    if (argument == "--allow" || argument == "--deny") {
      if (index + 1 >= argc) {
        std::cerr << "gti: " << argument
                  << " requires a name or 'prefix*' pattern\n";
        return exitCode(ExitStatus::Usage);
      }
      const std::string pattern = argv[++index];
      if (pattern.empty()) {
        std::cerr << "gti: " << argument << " requires a non-empty pattern\n";
        return exitCode(ExitStatus::Usage);
      }
      (argument == "--allow" ? options.bindings.allow : options.bindings.deny)
          .push_back({pattern});
      continue;
    }
    if (argument == "--rename") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --rename requires <c-name>=<gti-name>\n";
        return exitCode(ExitStatus::Usage);
      }
      const std::string spelling = argv[++index];
      const std::size_t separator = spelling.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == spelling.size()) {
        std::cerr << "gti: --rename requires <c-name>=<gti-name>\n";
        return exitCode(ExitStatus::Usage);
      }
      options.bindings.renames.emplace(spelling.substr(0, separator),
                                       spelling.substr(separator + 1));
      continue;
    }
    if (argument == "--array-return") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --array-return requires <function>=<count>\n";
        return exitCode(ExitStatus::Usage);
      }
      const std::string spelling = argv[++index];
      const std::size_t separator = spelling.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == spelling.size()) {
        std::cerr << "gti: --array-return requires <function>=<count>\n";
        return exitCode(ExitStatus::Usage);
      }
      options.bindings.arrayReturns.push_back(
          {spelling.substr(0, separator), spelling.substr(separator + 1)});
      continue;
    }
    if (argument == "--target") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --target requires a target triple\n";
        return exitCode(ExitStatus::Usage);
      }
      options.targetTriple = argv[++index];
      continue;
    }
    if (argument == "--clang") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --clang requires an executable path\n";
        return exitCode(ExitStatus::Usage);
      }
      options.clang = argv[++index];
      continue;
    }
    if (argument == "--sysroot") {
      if (index + 1 >= argc) {
        std::cerr << "gti: --sysroot requires a directory\n";
        return exitCode(ExitStatus::Usage);
      }
      options.sysroot = argv[++index];
      continue;
    }
    if (argument == "-I") {
      if (index + 1 >= argc) {
        std::cerr << "gti: -I requires a directory\n";
        return exitCode(ExitStatus::Usage);
      }
      options.includeDirectories.emplace_back(argv[++index]);
      continue;
    }
    if (argument.starts_with("-I") && argument.size() > 2) {
      options.includeDirectories.emplace_back(argument.substr(2));
      continue;
    }
    if (argument == "-D" || argument == "-U") {
      if (index + 1 >= argc) {
        std::cerr << "gti: " << argument << " requires a macro spelling\n";
        return exitCode(ExitStatus::Usage);
      }
      options.macroArguments.emplace_back(std::string(argument) +
                                          argv[++index]);
      continue;
    }
    if ((argument.starts_with("-D") || argument.starts_with("-U")) &&
        argument.size() > 2) {
      options.macroArguments.emplace_back(argument);
      continue;
    }
    if (argument == "--verbose" || argument == "-v") {
      options.verbose = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout);
      return exitCode(ExitStatus::Success);
    }
    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "gti: unknown bindgen option '" << argument << "'\n";
      return exitCode(ExitStatus::Usage);
    }
    if (!options.header.empty()) {
      std::cerr << "gti: bindgen accepts exactly one header\n";
      return exitCode(ExitStatus::Usage);
    }
    options.header = argument;
  }
  if (options.header.empty()) {
    std::cerr << "gti: bindgen requires a C header path\n";
    return exitCode(ExitStatus::Usage);
  }
  std::ifstream headerStream(options.header, std::ios::binary);
  if (!headerStream) {
    std::cerr << "gti: failed to open header: " << options.header << '\n';
    return exitCode(ExitStatus::Io);
  }
  headerStream.close();

  lang::TargetInfo target = lang::TargetInfo::host();
  if (options.targetTriple) {
    lang::TargetTripleParseResult parsed =
        lang::parseTargetTripleResult(*options.targetTriple);
    if (!parsed.succeeded()) {
      std::cerr << "gti: invalid bindgen target '" << *options.targetTriple
                << "': " << lang::targetTripleErrorMessage(parsed.error)
                << '\n';
      return exitCode(ExitStatus::Usage);
    }
    target = std::move(*parsed.target);
  }
  if (!target.supported()) {
    std::cerr << "gti: bindgen requires a supported 64-bit host or an "
                 "explicit --target triple\n";
    return exitCode(ExitStatus::ToolchainConfiguration);
  }
  const lang::bindgen::CTargetProfile profile(
      target.os == "windows" ? lang::bindgen::CDataModel::Llp64
                             : lang::bindgen::CDataModel::Lp64);

  std::filesystem::path preprocessed;
  std::filesystem::path dependencies;
  try {
    preprocessed =
        lang::driver::temporaryArtifactPath(options.header, ".bindgen.i");
    dependencies =
        lang::driver::temporaryArtifactPath(options.header, ".bindgen.d");
  } catch (const std::filesystem::filesystem_error &error) {
    std::cerr << "gti: failed to create bindgen temporary paths: "
              << error.what() << '\n';
    return exitCode(ExitStatus::Io);
  }
  lang::driver::TemporaryArtifact preprocessedCleanup(preprocessed, true);
  lang::driver::TemporaryArtifact dependencyCleanup(dependencies, true);

  // System includes expand with line markers preserved (C-05): the reader
  // maps every declaration back to its original file, applies the import
  // policy by provenance, and resolves system types like `time_t`
  // canonically. `-dD` keeps macro definitions for constant import.
  std::vector<std::string> preprocessorArguments{"-x", "c", "-dD"};
  if (options.targetTriple) {
    preprocessorArguments.emplace_back("--target=" + *options.targetTriple);
  }
  if (options.sysroot) {
    preprocessorArguments.emplace_back("-isysroot");
    preprocessorArguments.emplace_back(*options.sysroot);
  }
  preprocessorArguments.insert(preprocessorArguments.end(),
                               options.macroArguments.begin(),
                               options.macroArguments.end());
  const lang::driver::NativeCCompileRequest preprocessRequest(
      options.clang.value_or("clang"), options.header, preprocessed,
      lang::driver::CStandard::C17, lang::OptimizationLevel::O0,
      options.includeDirectories, std::move(preprocessorArguments));
  const lang::driver::NativePreprocessingResult preprocessing =
      lang::driver::NativeToolchain().preprocess(
          preprocessRequest, dependencies, {.captureSuccessfulOutput = true});
  if (options.verbose) {
    std::cerr << lang::driver::renderCommand(preprocessing.command) << '\n';
  }
  if (preprocessing.process.driverDiagnostic) {
    std::cerr << *preprocessing.process.driverDiagnostic << '\n';
  }
  if (!preprocessing.process.output.empty()) {
    reportCapturedOutput(preprocessing.process.output,
                         preprocessing.succeeded()
                             ? "gti: C preprocessor diagnostics:\n"
                             : "gti: C preprocessing failed:\n");
  }
  if (!preprocessing.succeeded()) {
    return preprocessing.process.driverDiagnostic
               ? exitCode(ExitStatus::ToolchainConfiguration)
               : exitCode(ExitStatus::Compilation);
  }

  std::ifstream stream(preprocessed, std::ios::binary);
  if (!stream) {
    std::cerr << "gti: Clang did not produce a preprocessed translation unit\n";
    return exitCode(ExitStatus::Io);
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  const lang::bindgen::BindgenResult result = lang::bindgen::runCBindgen(
      contents.str(), options.header.filename().string(),
      std::move(options.bindings), profile);
  // Origins are original (line-marker) locations: the root header by
  // default, the consumed file's own path otherwise.
  const auto originLabel =
      [&result, &options](const lang::bindgen::SourceOrigin &origin) {
        if (origin.file != 0 && origin.file <= result.plan.sourceFiles.size()) {
          return result.plan.sourceFiles[origin.file - 1].path + ":" +
                 std::to_string(origin.line);
        }
        return options.header.string() + ":" + std::to_string(origin.line);
      };
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : result.diagnostics) {
    std::cerr << originLabel(diagnostic.origin) << ": "
              << (diagnostic.severity == lang::bindgen::ReaderSeverity::Error
                      ? "error: "
                      : "note: ")
              << diagnostic.message << '\n';
  }
  for (const lang::bindgen::BindingItem &item : result.plan.items) {
    if (item.kind == lang::bindgen::BindingItemKind::Skipped) {
      std::cerr << originLabel(item.origin) << ": note: skipped '" << item.name
                << "': " << item.reason << '\n';
    }
  }
  if (!result.usable()) {
    return exitCode(ExitStatus::Compilation);
  }
  if (options.output.empty()) {
    std::cout << result.gtiSource;
    return exitCode(ExitStatus::Success);
  }
  return writeFile(options.output, result.gtiSource)
             ? exitCode(ExitStatus::Success)
             : exitCode(ExitStatus::Io);
}

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti");
  // Permit presentation options before a subcommand, without scanning through
  // operands (e.g. an output file literally named --progress=never).
  int first = 1;
  for (; first < argc; ++first) {
    const auto progress = parseProgressArgument(argc, argv, first);
    if (progress == ColorArgument::Invalid) {
      return exitCode(ExitStatus::Usage);
    }
    if (progress == ColorArgument::Accepted) {
      continue;
    }
    const auto color = parseColorArgument(argc, argv, first);
    if (color == ColorArgument::Invalid) {
      return exitCode(ExitStatus::Usage);
    }
    if (color == ColorArgument::NotColor) {
      break;
    }
  }
  std::vector<char *> arguments{argv[0]};
  arguments.insert(arguments.end(), argv + first, argv + argc);
  arguments.push_back(nullptr);
  argc = static_cast<int>(arguments.size()) - 1;
  argv = arguments.data();
  const std::string_view command =
      argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  if (command == "build" || command == "check" || command == "run" ||
      command == "test" || command == "verify") {
    ProjectOptions options;
    options.command = command == "check"    ? ProjectCommand::Check
                      : command == "run"    ? ProjectCommand::Run
                      : command == "test"   ? ProjectCommand::Test
                      : command == "verify" ? ProjectCommand::Verify
                                            : ProjectCommand::Build;
    const ArgumentResult argumentResult =
        parseProjectArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    if (options.command == ProjectCommand::Verify) {
      return runProjectVerify(options, argv[0]);
    }
    if (options.command == ProjectCommand::Test) {
      return runProjectTests(options, argv[0]);
    }
    if (options.buildAll) {
      return runProjectBuildAll(options, argv[0]);
    }
    return runProject(options, argv[0]);
  }

  if (command == "add" || command == "publish") {
    RegistryOptions options;
    const auto parsed =
        parseRegistryArguments(argc, argv, options, command == "publish");
    if (parsed == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (parsed == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    const auto directory = workingDirectory();
    if (!directory) {
      return exitCode(ExitStatus::Io);
    }
    const auto result =
        command == "add"
            ? lang::driver::addRegistryDependency(*directory, options.package,
                                                  options.requirement,
                                                  options.registry)
            : lang::driver::publishRegistryPackage(*directory, options.registry,
                                                   options.package,
                                                   options.dryRun);
    if (!result.succeeded()) {
      reportDiagnostics(result.diagnostics, result.sources);
      return exitCode(ExitStatus::Compilation);
    }
    std::cout << result.summary << '\n';
    return exitCode(ExitStatus::Success);
  }

  if (command == "fetch") {
    FetchOptions options;
    const ArgumentResult argumentResult =
        parseFetchArguments(argc, argv, options);
    if (argumentResult == ArgumentResult::ExitSuccess) {
      return exitCode(ExitStatus::Success);
    }
    if (argumentResult == ArgumentResult::ExitFailure) {
      return exitCode(ExitStatus::Usage);
    }
    return runFetch(options);
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

  if (command == "bindgen") {
    return runBindgen(argc, argv);
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
