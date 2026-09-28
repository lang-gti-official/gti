#include "gti/driver/build_script.h"

#include "build_source_identity.h"
#include "gti/driver/build.h"
#include "gti/driver/process.h"
#include "gti/target.h"

#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

constexpr std::string_view outputFilename = "gti-build-output.toml";
constexpr std::string_view stateFilename = "gti-build-state";
constexpr std::string_view cacheSchema = "gti-build-script-cache-v1";

Diagnostic scriptDiagnostic(const BuildScriptPlan &plan, std::string code,
                            std::string message) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        plan.declaration, std::move(message));
}

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end() && candidatePart != candidate.end();
       ++rootPart, ++candidatePart) {
    if (*rootPart != *candidatePart) {
      return false;
    }
  }
  return rootPart == root.end();
}

bool absoluteOnSupportedHost(std::string_view spelling,
                             const std::filesystem::path &path) {
  const bool windowsDrive =
      spelling.size() >= 2 && (((spelling[0] >= 'A' && spelling[0] <= 'Z') ||
                                (spelling[0] >= 'a' && spelling[0] <= 'z')) &&
                               spelling[1] == ':');
  return path.is_absolute() || windowsDrive || spelling.starts_with('\\');
}

std::optional<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::optional<std::string>
scriptIdentity(const ProjectBuildPlan &project, const BuildScriptPlan &plan,
               const BuildScriptExecutionOptions &options,
               std::vector<Diagnostic> &diagnostics) {
  detail::IdentityBuilder identity;
  identity.add("schema", cacheSchema);
  identity.add("package", plan.packageIdentity);
  identity.add("host", targetTriple(TargetInfo::host()));
  identity.add("target", targetTriple(project.target()));
  identity.add("profile", project.profileName());
  identity.add("cc", options.cCompiler);
  identity.add("cxx", options.cppCompiler);
  if (plan.source) {
    identity.add("gti-compiler", options.compilerIdentity);
    identity.add("host-cxx", options.hostCppCompiler);
  }
  for (const std::string &argument : plan.command) {
    identity.add("argv", argument);
  }

  struct InputEntry {
    std::filesystem::path absolute;
    std::string relative;
    bool directory = false;
  };
  std::vector<InputEntry> entries;
  for (const std::filesystem::path &input : plan.inputs) {
    std::error_code statusError;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(input, statusError);
    if (statusError || std::filesystem::is_symlink(status)) {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2001",
          "Cannot fingerprint build-script input '" + input.string() +
              "': inputs must exist and may not be symbolic links."));
      return std::nullopt;
    }
    const std::string declared =
        std::filesystem::relative(input, plan.packageRoot).generic_string();
    if (std::filesystem::is_regular_file(status)) {
      entries.push_back({input, declared, false});
      continue;
    }
    if (!std::filesystem::is_directory(status)) {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2001",
          "Cannot fingerprint build-script input '" + input.string() +
              "': only regular files and directories are supported."));
      return std::nullopt;
    }
    entries.push_back({input, declared + "/", true});
    std::filesystem::recursive_directory_iterator iterator(input, statusError);
    const std::filesystem::recursive_directory_iterator end;
    while (!statusError && iterator != end) {
      const std::filesystem::path path = iterator->path();
      const std::filesystem::file_status nested =
          iterator->symlink_status(statusError);
      if (statusError) {
        break;
      }
      if (std::filesystem::is_symlink(nested)) {
        diagnostics.push_back(scriptDiagnostic(
            plan, "GTI-B2001",
            "Cannot fingerprint build-script input '" + path.string() +
                "': symbolic links are not supported."));
        return std::nullopt;
      }
      if (!std::filesystem::is_directory(nested) &&
          !std::filesystem::is_regular_file(nested)) {
        diagnostics.push_back(scriptDiagnostic(
            plan, "GTI-B2001",
            "Cannot fingerprint build-script input '" + path.string() +
                "': only regular files and directories are supported."));
        return std::nullopt;
      }
      std::string relative =
          std::filesystem::relative(path, plan.packageRoot).generic_string();
      const bool directory = std::filesystem::is_directory(nested);
      if (directory) {
        relative += "/";
      }
      entries.push_back({path, std::move(relative), directory});
      iterator.increment(statusError);
    }
    if (statusError) {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2001",
          "Cannot enumerate build-script input '" + input.string() +
              "': " + statusError.message() + "."));
      return std::nullopt;
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const InputEntry &left, const InputEntry &right) {
              return left.relative < right.relative;
            });
  std::array<char, 64 * 1024> buffer{};
  for (const InputEntry &entry : entries) {
    identity.add(entry.directory ? "input-directory" : "input-file",
                 entry.relative);
    if (entry.directory) {
      continue;
    }
    std::ifstream input(entry.absolute, std::ios::binary);
    if (!input) {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2001",
          "Cannot read build-script input: " + entry.absolute.string() + "."));
      return std::nullopt;
    }
    Sha256 content;
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize count = input.gcount();
      if (count > 0) {
        content.update(
            std::string_view(buffer.data(), static_cast<std::size_t>(count)));
      }
    }
    if (!input.eof()) {
      diagnostics.push_back(
          scriptDiagnostic(plan, "GTI-B2001",
                           "Cannot finish reading build-script input: " +
                               entry.absolute.string() + "."));
      return std::nullopt;
    }
    identity.add("input-sha256", content.finishHex());
  }
  return identity.finish();
}

bool writeState(const BuildScriptPlan &plan, std::string_view identity,
                std::vector<Diagnostic> &diagnostics) {
  const std::filesystem::path state = plan.outputDirectory / stateFilename;
  const std::filesystem::path temporary = state.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !(output << cacheSchema << '\n' << identity << '\n')) {
      diagnostics.push_back(
          scriptDiagnostic(plan, "GTI-B2003",
                           "Cannot write build-script cache state '" +
                               temporary.string() + "'."));
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, state, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    diagnostics.push_back(
        scriptDiagnostic(plan, "GTI-B2003",
                         "Cannot publish build-script cache state '" +
                             state.string() + "': " + error.message() + "."));
    return false;
  }
  return true;
}

std::vector<std::string> stringArray(const toml::table &table,
                                     std::string_view field, bool &valid) {
  std::vector<std::string> values;
  const toml::node *node = table.get(field);
  if (node == nullptr) {
    return values;
  }
  const toml::array *array = node->as_array();
  if (array == nullptr) {
    valid = false;
    return values;
  }
  for (const toml::node &element : *array) {
    const std::optional<std::string> value = element.value<std::string>();
    if (!value || value->empty() ||
        std::any_of(value->begin(), value->end(), [](unsigned char character) {
          return character < 0x20U || character == 0x7fU;
        })) {
      valid = false;
      continue;
    }
    values.push_back(*value);
  }
  return values;
}

std::optional<std::filesystem::path>
outputPath(const BuildScriptPlan &plan, std::string_view spelling,
           bool directory, std::string_view field,
           std::vector<Diagnostic> &diagnostics) {
  const std::filesystem::path declared(spelling);
  if (declared.empty() || absoluteOnSupportedHost(spelling, declared)) {
    diagnostics.push_back(scriptDiagnostic(
        plan, "GTI-B2005",
        "Build script output field '" + std::string(field) +
            "' must contain paths relative to GTI_BUILD_OUT_DIR."));
    return std::nullopt;
  }
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(plan.outputDirectory, error);
  if (error) {
    diagnostics.push_back(scriptDiagnostic(
        plan, "GTI-B2005",
        "Cannot resolve build script output directory: " + error.message() +
            "."));
    return std::nullopt;
  }
  const std::filesystem::path resolved =
      std::filesystem::weakly_canonical(root / declared, error);
  if (error || !pathIsWithin(root, resolved)) {
    diagnostics.push_back(scriptDiagnostic(
        plan, "GTI-B2005",
        "Build script output field '" + std::string(field) + "' escapes '" +
            root.string() + "': " + std::string(spelling) + "."));
    return std::nullopt;
  }
  const bool matches = directory
                           ? std::filesystem::is_directory(resolved, error)
                           : std::filesystem::is_regular_file(resolved, error);
  if (error || !matches) {
    diagnostics.push_back(scriptDiagnostic(
        plan, "GTI-B2005",
        "Build script output field '" + std::string(field) +
            "' names no existing " + (directory ? "directory: " : "file: ") +
            std::string(spelling) + "."));
    return std::nullopt;
  }
  return resolved;
}

bool validNativeName(std::string_view name) {
  return !name.empty() && name.front() != '-' &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos &&
         std::none_of(name.begin(), name.end(), [](unsigned char character) {
           return character <= 0x20U || character == 0x7fU;
         });
}

struct ScriptNativeOutput {
  std::vector<std::filesystem::path> includeDirectories;
  std::vector<std::filesystem::path> cSources;
  std::vector<std::filesystem::path> cppSources;
  std::vector<std::filesystem::path> libraryDirectories;
  std::vector<std::filesystem::path> linkFiles;
  std::vector<std::string> libraries;
  std::vector<std::string> frameworks;
};

std::optional<ScriptNativeOutput>
parseOutput(const BuildScriptPlan &plan, std::vector<Diagnostic> &diagnostics) {
  const std::filesystem::path manifest = plan.outputDirectory / outputFilename;
  const std::optional<std::string> source = readFile(manifest);
  if (!source) {
    diagnostics.push_back(
        scriptDiagnostic(plan, "GTI-B2004",
                         "Build script for '" + plan.packageIdentity +
                             "' did not write '" + manifest.string() + "'."));
    return std::nullopt;
  }
  toml::parse_result parsed = toml::parse(*source, manifest.string());
  if (!parsed) {
    diagnostics.push_back(
        scriptDiagnostic(plan, "GTI-B2004",
                         "Build script for '" + plan.packageIdentity +
                             "' wrote an invalid TOML output manifest: " +
                             std::string(parsed.error().description()) + "."));
    return std::nullopt;
  }
  const toml::table &table = parsed.table();
  static constexpr std::string_view allowed[] = {
      "version",      "include-dirs", "c-sources", "cpp-sources",
      "library-dirs", "link-files",   "libraries", "frameworks"};
  bool valid = true;
  for (const auto &[key, value] : table) {
    if (std::find(std::begin(allowed), std::end(allowed), key.str()) ==
        std::end(allowed)) {
      diagnostics.push_back(
          scriptDiagnostic(plan, "GTI-B2004",
                           "Build script output contains unknown field '" +
                               std::string(key.str()) + "'."));
      valid = false;
    }
  }
  const std::optional<std::int64_t> version =
      table["version"].value<std::int64_t>();
  if (!version || *version != 1) {
    diagnostics.push_back(scriptDiagnostic(
        plan, "GTI-B2004",
        "Build script output requires integer 'version = 1'."));
    valid = false;
  }

  ScriptNativeOutput output;
  const auto paths = [&](std::string_view field, bool directory,
                         std::vector<std::filesystem::path> &destination) {
    bool arrayValid = true;
    const std::vector<std::string> spellings =
        stringArray(table, field, arrayValid);
    if (!arrayValid) {
      diagnostics.push_back(
          scriptDiagnostic(plan, "GTI-B2004",
                           "Build script output field '" + std::string(field) +
                               "' must be an array of non-empty strings."));
      valid = false;
    }
    for (const std::string &spelling : spellings) {
      if (std::optional<std::filesystem::path> resolved =
              outputPath(plan, spelling, directory, field, diagnostics)) {
        destination.push_back(std::move(*resolved));
      } else {
        valid = false;
      }
    }
  };
  paths("include-dirs", true, output.includeDirectories);
  paths("c-sources", false, output.cSources);
  paths("cpp-sources", false, output.cppSources);
  paths("library-dirs", true, output.libraryDirectories);
  paths("link-files", false, output.linkFiles);
  for (const std::filesystem::path &sourcePath : output.cSources) {
    if (sourcePath.extension() != ".c") {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2005",
          "Build script C source does not use '.c': " + sourcePath.string() +
              "."));
      valid = false;
    }
  }
  for (const std::filesystem::path &sourcePath : output.cppSources) {
    const std::filesystem::path extension = sourcePath.extension();
    if (extension != ".cpp" && extension != ".cc" && extension != ".cxx") {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2005",
          "Build script C++ source must use '.cpp', '.cc', or '.cxx': " +
              sourcePath.string() + "."));
      valid = false;
    }
  }

  const auto names = [&](std::string_view field,
                         std::vector<std::string> &destination) {
    bool arrayValid = true;
    destination = stringArray(table, field, arrayValid);
    if (!arrayValid || std::any_of(destination.begin(), destination.end(),
                                   [](const std::string &name) {
                                     return !validNativeName(name);
                                   })) {
      diagnostics.push_back(scriptDiagnostic(
          plan, "GTI-B2004",
          "Build script output field '" + std::string(field) +
              "' must contain native names without flags, whitespace, or "
              "path separators."));
      valid = false;
    }
  };
  names("libraries", output.libraries);
  names("frameworks", output.frameworks);
  return valid ? std::optional<ScriptNativeOutput>(std::move(output))
               : std::nullopt;
}

void appendOutput(NativeInputs &inputs, const BuildScriptPlan &plan,
                  ScriptNativeOutput output) {
  if (plan.selectedPackage) {
    inputs.includeDirectories.insert(inputs.includeDirectories.end(),
                                     output.includeDirectories.begin(),
                                     output.includeDirectories.end());
    inputs.cSources.insert(inputs.cSources.end(), output.cSources.begin(),
                           output.cSources.end());
    inputs.cppSources.insert(inputs.cppSources.end(), output.cppSources.begin(),
                             output.cppSources.end());
    inputs.libraryDirectories.insert(inputs.libraryDirectories.end(),
                                     output.libraryDirectories.begin(),
                                     output.libraryDirectories.end());
    for (const std::filesystem::path &file : output.linkFiles) {
      inputs.libraryFiles.push_back(file);
      inputs.orderedLinkOperands.push_back(
          {NativeLinkOperandKind::File, file.string()});
    }
    for (const std::string &library : output.libraries) {
      inputs.libraries.push_back(library);
      inputs.orderedLinkOperands.push_back(
          {NativeLinkOperandKind::Library, library});
    }
    for (const std::string &framework : output.frameworks) {
      inputs.frameworks.push_back(framework);
      inputs.orderedLinkOperands.push_back(
          {NativeLinkOperandKind::Framework, framework});
    }
    return;
  }

  const auto found = std::find_if(
      inputs.dependencyGroups.begin(), inputs.dependencyGroups.end(),
      [&plan](const NativeDependencyGroup &group) {
        return group.packageIdentity == plan.packageIdentity;
      });
  if (found == inputs.dependencyGroups.end()) {
    return;
  }
  found->includeDirectories.insert(found->includeDirectories.end(),
                                   output.includeDirectories.begin(),
                                   output.includeDirectories.end());
  found->cSources.insert(found->cSources.end(), output.cSources.begin(),
                         output.cSources.end());
  found->cppSources.insert(found->cppSources.end(), output.cppSources.begin(),
                           output.cppSources.end());
  found->libraryDirectories.insert(found->libraryDirectories.end(),
                                   output.libraryDirectories.begin(),
                                   output.libraryDirectories.end());
  for (const std::filesystem::path &file : output.linkFiles) {
    found->linkOperands.push_back({NativeLinkOperandKind::File, file.string()});
  }
  for (const std::string &library : output.libraries) {
    found->linkOperands.push_back({NativeLinkOperandKind::Library, library});
  }
  for (const std::string &framework : output.frameworks) {
    found->linkOperands.push_back(
        {NativeLinkOperandKind::Framework, framework});
  }
}

std::optional<std::vector<std::string>> buildScriptCommand(
    const ProjectBuildPlan &project, const BuildScriptPlan &script,
    const BuildScriptExecutionOptions &options,
    const std::filesystem::path &output, BuildScriptExecutionResult &result) {
  if (!script.source) {
    return script.command;
  }
  if (!options.hostToolchain || options.hostCppCompiler.empty()) {
    result.diagnostics.push_back(scriptDiagnostic(
        script, "GTI-B2003",
        "Cannot compile GTI build script for '" + script.packageIdentity +
            "': the host GTI toolchain is unavailable."));
    return std::nullopt;
  }

  const std::filesystem::path generated =
      script.outputDirectory / ".build-script.gti.cpp";
  const std::filesystem::path executable =
      script.outputDirectory / "build-script-host";
  const ExecutableBuildResult built = buildExecutable(ExecutableBuildRequest(
      CompilationRequest(*script.source, options.hostToolchain->standardLibrary,
                         TargetInfo::host(), OptimizationLevel::O0,
                         CppStandard::Cpp20),
      *options.hostToolchain, generated, executable, options.hostCppCompiler,
      {}, false, true, options.verbose,
      ManagedOutputPolicy{.trustedRoot = project.workspaceRoot(),
                          .outputRoot =
                              project.workspaceRoot() / "build" / "gti"}));
  if (!built.succeeded()) {
    if (!built.compilation.diagnostics.empty()) {
      result.diagnosticSources = built.compilation.sources;
      result.diagnostics = built.compilation.diagnostics;
    } else {
      Diagnostic diagnostic =
          scriptDiagnostic(script, "GTI-B2003",
                           "Failed to compile GTI build script for '" +
                               script.packageIdentity + "'.");
      if (built.driverDiagnostic) {
        diagnostic.hints.push_back(*built.driverDiagnostic);
      }
      if (built.nativeProcess && !built.nativeProcess->output.empty()) {
        diagnostic.hints.push_back(built.nativeProcess->output);
      }
      result.diagnostics.push_back(std::move(diagnostic));
    }
    return std::nullopt;
  }

  return std::vector<std::string>{
      executable.string(),
      output.string(),
      script.packageRoot.string(),
      script.outputDirectory.string(),
      targetTriple(TargetInfo::host()),
      targetTriple(project.target()),
      project.target().os,
      project.profileName(),
      std::to_string(options.jobs),
      options.cCompiler,
      options.cppCompiler,
  };
}

} // namespace

BuildScriptExecutionResult
executeBuildScripts(const ProjectBuildPlan &plan,
                    const BuildScriptExecutionOptions &options) {
  BuildScriptExecutionResult result;
  NativeInputs inputs = plan.nativeInputs();
  for (const BuildScriptPlan &script : plan.buildScripts()) {
    std::error_code directoryError;
    std::filesystem::create_directories(script.outputDirectory, directoryError);
    if (directoryError) {
      result.diagnostics.push_back(
          scriptDiagnostic(script, "GTI-B2003",
                           "Cannot create build-script output directory '" +
                               script.outputDirectory.string() +
                               "': " + directoryError.message() + "."));
      return result;
    }
    const std::optional<std::string> identity =
        scriptIdentity(plan, script, options, result.diagnostics);
    if (!identity) {
      return result;
    }
    const std::filesystem::path output =
        script.outputDirectory / outputFilename;
    const std::filesystem::path state = script.outputDirectory / stateFilename;
    const std::string expectedState =
        std::string(cacheSchema) + "\n" + *identity + "\n";
    if (readFile(state) == expectedState) {
      std::vector<Diagnostic> staleDiagnostics;
      if (std::optional<ScriptNativeOutput> native =
              parseOutput(script, staleDiagnostics)) {
        appendOutput(inputs, script, std::move(*native));
        result.outcomes.push_back(
            {.packageIdentity = script.packageIdentity, .reused = true});
        continue;
      }
    }
    std::error_code removeError;
    std::filesystem::remove(output, removeError);
    if (removeError) {
      result.diagnostics.push_back(scriptDiagnostic(
          script, "GTI-B2003",
          "Cannot replace prior build-script output '" + output.string() +
              "': " + removeError.message() + "."));
      return result;
    }
    std::filesystem::remove(state, removeError);
    if (removeError) {
      result.diagnostics.push_back(scriptDiagnostic(
          script, "GTI-B2003",
          "Cannot replace prior build-script state '" + state.string() +
              "': " + removeError.message() + "."));
      return result;
    }

    const std::optional<std::vector<std::string>> command =
        buildScriptCommand(plan, script, options, output, result);
    if (!command) {
      return result;
    }
    ProcessInvocationOptions processOptions{
        .outputMode = ProcessOutputMode::Capture,
        .captureSuccessfulOutput = options.verbose,
        .description = "build script for " + script.packageIdentity,
        .workingDirectory = script.packageRoot,
        .environment = {{"GTI_BUILD_PACKAGE_DIR", script.packageRoot.string()},
                        {"GTI_BUILD_OUT_DIR", script.outputDirectory.string()},
                        {"GTI_BUILD_OUTPUT", output.string()},
                        {"GTI_BUILD_HOST", targetTriple(TargetInfo::host())},
                        {"GTI_BUILD_TARGET", targetTriple(plan.target())},
                        {"GTI_BUILD_PROFILE", plan.profileName()},
                        {"GTI_BUILD_JOBS", std::to_string(options.jobs)},
                        {"CC", options.cCompiler},
                        {"CXX", options.cppCompiler}}};
    const ProcessResult process = invokeProcess(*command, processOptions);
    if (!process.succeeded()) {
      Diagnostic diagnostic =
          scriptDiagnostic(script, "GTI-B2003",
                           "Build script for '" + script.packageIdentity +
                               "' failed with exit code " +
                               std::to_string(process.exitCode) + ".");
      if (process.driverDiagnostic) {
        diagnostic.hints.push_back(*process.driverDiagnostic);
      }
      if (!process.output.empty()) {
        constexpr std::size_t limit = 8000;
        diagnostic.hints.push_back(
            process.output.size() <= limit
                ? process.output
                : process.output.substr(process.output.size() - limit));
      }
      result.diagnostics.push_back(std::move(diagnostic));
      return result;
    }
    std::optional<ScriptNativeOutput> native =
        parseOutput(script, result.diagnostics);
    if (!native) {
      return result;
    }
    appendOutput(inputs, script, std::move(*native));
    if (!writeState(script, *identity, result.diagnostics)) {
      return result;
    }
    result.outcomes.push_back({.packageIdentity = script.packageIdentity,
                               .reused = false,
                               .output = process.output});
  }
  result.nativeInputs = std::move(inputs);
  return result;
}

} // namespace lang::driver
