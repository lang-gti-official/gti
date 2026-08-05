#include "gti/cpp_emitter.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#if !defined(GTI_VERSION)
#define GTI_VERSION "0.1.0"
#endif
#if !defined(GTI_BUILD_STDLIB_PATH)
#define GTI_BUILD_STDLIB_PATH ""
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

constexpr std::string_view version = GTI_VERSION;

struct Options {
  std::filesystem::path input;
  std::filesystem::path output;
  std::optional<std::string> cxx;
  std::vector<std::string> compilerArguments;
  bool emitCpp = false;
  bool keepCpp = false;
  bool verbose = false;
};

struct ToolchainPaths {
  std::filesystem::path standardLibrary;
  std::filesystem::path runtimeInclude;
  std::filesystem::path runtimeLibrary;
};

enum class ArgumentResult {
  Run,
  ExitSuccess,
  ExitFailure,
};

std::filesystem::path selectToolchainPath(
    const char *environmentName, const std::filesystem::path &installed,
    const std::filesystem::path &buildPath,
    const std::filesystem::path &requiredChild = {}) {
  if (const char *configured = std::getenv(environmentName);
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  std::error_code error;
  const auto exists = [&](const std::filesystem::path &path) {
    error.clear();
    return !path.empty() && std::filesystem::exists(
                                requiredChild.empty() ? path
                                                      : path / requiredChild,
                                error);
  };
  if (exists(installed)) {
    return installed;
  }
  if (exists(buildPath)) {
    return buildPath;
  }
  return buildPath;
}

ToolchainPaths discoverToolchainPaths(const char *driver) {
  std::error_code error;
  std::filesystem::path executable = std::filesystem::absolute(driver, error);
  if (!error) {
    executable = std::filesystem::weakly_canonical(executable, error);
  }
  const std::filesystem::path prefix = executable.parent_path().parent_path();

  return {
      .standardLibrary = selectToolchainPath(
          "GTI_STDLIB_PATH", prefix / "share/gti/stdlib/prelude.gti",
          GTI_BUILD_STDLIB_PATH),
      .runtimeInclude = selectToolchainPath(
          "GTI_RUNTIME_INCLUDE", prefix / "include",
          GTI_BUILD_RUNTIME_INCLUDE_DIR, "gti/runtime.hpp"),
      .runtimeLibrary = selectToolchainPath(
          "GTI_RUNTIME_LIBRARY", prefix / "lib" / GTI_RUNTIME_LIBRARY_NAME,
          GTI_BUILD_RUNTIME_LIBRARY_PATH),
  };
}

void printUsage(std::ostream &stream) {
  stream << "Usage: gti <source.gti> [options] [-- <c++ compiler arguments>]\n"
            "\n"
            "Options:\n"
            "  -o, --output <path>  Set the executable or emitted C++ path.\n"
            "      --emit-cpp       Emit C++ without building an executable.\n"
            "      --keep-cpp       Keep the generated C++ beside the executable.\n"
            "      --cxx <path>     Select the native C++ compiler.\n"
            "  -v, --verbose        Print the native compiler command.\n"
            "  -h, --help           Show this help text.\n"
            "      --version        Print the GTI compiler version.\n";
}

std::filesystem::path defaultExecutablePath(
    const std::filesystem::path &input) {
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
    if (argument == "--emit-cpp") {
      options.emitCpp = true;
      continue;
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

void reportParseError(const lang::ParseDiagnostic &diagnostic) {
  const lang::Token &token = diagnostic.token;
  if (!token.source.empty()) {
    std::cerr << token.source << ':';
  }
  std::cerr << token.line << ": error";
  if (token.kind == lang::TokenKind::END_OF_FILE) {
    std::cerr << " at end";
  } else {
    std::cerr << " at '" << token.lexeme << "'";
  }
  std::cerr << ": " << diagnostic.message << '\n';
}

void reportSemanticError(const lang::SemanticDiagnostic &diagnostic) {
  if (!diagnostic.token.source.empty()) {
    std::cerr << diagnostic.token.source << ':';
  }
  std::cerr << diagnostic.token.line << ": semantic error";
  if (!diagnostic.token.lexeme.empty()) {
    std::cerr << " at '" << diagnostic.token.lexeme << "'";
  }
  std::cerr << ": " << diagnostic.message << '\n';
}

std::optional<std::string>
lowerToCpp(const std::filesystem::path &input,
           const std::filesystem::path &standardLibrary) {
  lang::SourceLoader sourceLoader;
  std::vector<lang::Token> tokens =
      sourceLoader.load(input, std::nullopt, {standardLibrary});
  if (sourceLoader.hadError()) {
    for (const lang::SourceDiagnostic &diagnostic : sourceLoader.errors()) {
      if (!diagnostic.token.source.empty()) {
        std::cerr << diagnostic.token.source << ':';
      }
      std::cerr << diagnostic.token.line
                << ": source error: " << diagnostic.message << '\n';
    }
    return std::nullopt;
  }

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  if (parser.hadError()) {
    for (const lang::ParseDiagnostic &diagnostic : parser.errors()) {
      reportParseError(diagnostic);
    }
    return std::nullopt;
  }

  lang::SemanticVisitor semantic;
  if (!semantic.check(program)) {
    for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
      reportSemanticError(diagnostic);
    }
    return std::nullopt;
  }

  return lang::CppEmitter().emit(program);
}

bool writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path);
  if (!output) {
    std::cerr << "gti: failed to open output file: " << path << '\n';
    return false;
  }
  output << contents;
  if (!output) {
    std::cerr << "gti: failed to write output file: " << path << '\n';
    return false;
  }
  return true;
}

std::string nativeCompiler(const Options &options) {
  if (options.cxx) {
    return *options.cxx;
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

std::filesystem::path temporaryCppPath(const std::filesystem::path &input) {
  const auto nonce = std::chrono::high_resolution_clock::now()
                         .time_since_epoch()
                         .count();
  return std::filesystem::temp_directory_path() /
         ("gti-" + std::to_string(nonce) + "-" + input.stem().string() +
          ".cpp");
}

void printCommand(const std::vector<std::string> &arguments) {
  std::cerr << '+';
  for (const std::string &argument : arguments) {
    std::cerr << ' ';
    if (argument.find_first_of(" \t\"") == std::string::npos) {
      std::cerr << argument;
    } else {
      std::cerr << '"';
      for (char character : argument) {
        if (character == '"' || character == '\\') {
          std::cerr << '\\';
        }
        std::cerr << character;
      }
      std::cerr << '"';
    }
  }
  std::cerr << '\n';
}

int runProcess(const std::vector<std::string> &arguments) {
  if (arguments.empty()) {
    return 127;
  }

  std::vector<char *> processArguments;
  processArguments.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    processArguments.push_back(const_cast<char *>(argument.c_str()));
  }
  processArguments.push_back(nullptr);

#if defined(_WIN32)
  const intptr_t status =
      _spawnvp(_P_WAIT, processArguments.front(), processArguments.data());
  return status == -1 ? 127 : static_cast<int>(status);
#else
  const pid_t child = fork();
  if (child == -1) {
    std::cerr << "gti: failed to start native compiler: "
              << std::strerror(errno) << '\n';
    return 127;
  }
  if (child == 0) {
    execvp(processArguments.front(), processArguments.data());
    std::cerr << "gti: failed to execute '" << arguments.front()
              << "': " << std::strerror(errno) << '\n';
    _exit(127);
  }

  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR) {
      std::cerr << "gti: failed while waiting for native compiler: "
                << std::strerror(errno) << '\n';
      return 127;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 127;
#endif
}

class TemporaryFile {
public:
  TemporaryFile(std::filesystem::path path, bool removeOnDestruction)
      : path(std::move(path)), removeOnDestruction(removeOnDestruction) {}

  ~TemporaryFile() {
    if (removeOnDestruction) {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  }

private:
  std::filesystem::path path;
  bool removeOnDestruction;
};

} // namespace

int main(int argc, char *argv[]) {
  Options options;
  const ArgumentResult argumentResult = parseArguments(argc, argv, options);
  if (argumentResult == ArgumentResult::ExitSuccess) {
    return 0;
  }
  if (argumentResult == ArgumentResult::ExitFailure) {
    return 64;
  }

  const ToolchainPaths toolchain = discoverToolchainPaths(argv[0]);
  const std::optional<std::string> cpp =
      lowerToCpp(options.input, toolchain.standardLibrary);
  if (!cpp) {
    return 65;
  }

  if (options.emitCpp) {
    if (!writeFile(options.output, *cpp)) {
      return 74;
    }
    std::cout << "Emitted " << options.output << '\n';
    return 0;
  }

  const std::filesystem::path cppPath =
      options.keepCpp
          ? std::filesystem::path(options.output.string() + ".gti.cpp")
          : temporaryCppPath(options.input);
  TemporaryFile temporary(cppPath, !options.keepCpp);
  if (!writeFile(cppPath, *cpp)) {
    return 74;
  }

  std::error_code resourceError;
  if (!std::filesystem::exists(toolchain.runtimeInclude / "gti/runtime.hpp",
                               resourceError) ||
      !std::filesystem::exists(toolchain.runtimeLibrary, resourceError)) {
    std::cerr << "gti: native runtime files were not found\n";
    return 78;
  }

  std::vector<std::string> compilerCommand{
      nativeCompiler(options),
      "-std=c++20",
      "-I" + toolchain.runtimeInclude.string(),
      cppPath.string(),
      toolchain.runtimeLibrary.string(),
      "-o",
      options.output.string()};
  compilerCommand.insert(compilerCommand.end(),
                         options.compilerArguments.begin(),
                         options.compilerArguments.end());

  if (options.verbose) {
    printCommand(compilerCommand);
  }
  const int compilerStatus = runProcess(compilerCommand);
  if (compilerStatus != 0) {
    std::cerr << "gti: native C++ compiler failed with exit code "
              << compilerStatus << '\n';
    return compilerStatus;
  }

  std::cout << "Built " << options.output << '\n';
  if (options.keepCpp) {
    std::cout << "Kept C++ " << cppPath << '\n';
  }
  return 0;
}
