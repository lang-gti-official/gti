#include "gti/driver/bindgen_import.h"

#include "gti/bindgen/bindgen.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "sha256.h"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

// Bindgen import generation (BUILD-01). The pipeline mirrors the standalone
// `gti bindgen` subcommand — Clang-driver preprocessing with kept system
// includes and retained user macro definitions, then the in-process importer
// — but the driver owns discovery, deterministic output placement, atomic
// publication, and the reuse key. The generated file has no special status:
// it is loaded as ordinary package source and the frontend's native-interop
// validation remains the ABI authority.

namespace lang::driver {
namespace {

constexpr std::string_view kBindingsFilename = "bindings.gti";
constexpr std::string_view kKeyFilename = "bindings.key";

[[nodiscard]] Diagnostic importDiagnostic(const BindgenImportPlan &import,
                                          std::string code,
                                          std::string message) {
  Diagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.severity = DiagnosticSeverity::Error;
  diagnostic.primary = import.declaration;
  diagnostic.message = std::move(message);
  return diagnostic;
}

[[nodiscard]] std::optional<std::string>
readFileText(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  return std::move(contents).str();
}

[[nodiscard]] bool writeFileText(const std::filesystem::path &path,
                                 std::string_view contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream << contents;
  stream.flush();
  return stream.good();
}

// Publishes `contents` at `destination` through a staged sibling so a
// concurrent reader or a failed write never observes a partial file.
[[nodiscard]] bool publishText(const std::filesystem::path &destination,
                               std::string_view contents) {
  const std::filesystem::path staged = stagedArtifactPath(destination);
  if (!writeFileText(staged, contents)) {
    std::error_code cleanup;
    std::filesystem::remove(staged, cleanup);
    return false;
  }
  return publishArtifact(staged, destination).succeeded();
}

// The reuse key: preprocessed bytes carry every transitive header fact, the
// options and target carry the request, and the tool identity carries the
// in-process importer's policy. Equal keys mean byte-identical output, so
// the published file can be reused without re-running the importer.
[[nodiscard]] std::string reuseKey(const BindgenImportPlan &import,
                                   std::string_view toolIdentity,
                                   std::string_view triple,
                                   std::string_view preprocessed) {
  Sha256 hash;
  hash.update("gti-bindgen-import-v1\n");
  hash.update(toolIdentity);
  hash.update("\ntriple:");
  hash.update(triple);
  hash.update("\nalias:");
  hash.update(import.alias);
  hash.update("\nnamespace:");
  hash.update(import.namespaceName);
  for (const std::string &argument : import.macroArguments) {
    hash.update("\nmacro:");
    hash.update(argument);
  }
  hash.update("\n--\n");
  hash.update(preprocessed);
  return hash.finishHex();
}

void generateImport(const BindgenImportPlan &import,
                    const std::string &clangExecutable,
                    std::string_view toolIdentity, std::string_view triple,
                    BindgenImportOutcome &outcome) {
  outcome.packageName = import.packageName;
  outcome.alias = import.alias;
  outcome.output = import.outputDirectory / kBindingsFilename;

  std::error_code error;
  std::filesystem::create_directories(import.outputDirectory, error);
  if (error) {
    outcome.status = BindgenImportStatus::PublicationFailure;
    outcome.diagnostics.push_back(importDiagnostic(
        import, "GTI-B1805",
        "Failed to create the bindgen output directory '" +
            import.outputDirectory.string() + "': " + error.message() + "."));
    return;
  }

  std::filesystem::path preprocessed;
  std::filesystem::path dependencies;
  try {
    preprocessed = temporaryArtifactPath(import.header, ".bindgen.i");
    dependencies = temporaryArtifactPath(import.header, ".bindgen.d");
  } catch (const std::filesystem::filesystem_error &pathError) {
    outcome.status = BindgenImportStatus::PreprocessingFailure;
    outcome.diagnostics.push_back(importDiagnostic(
        import, "GTI-B1805",
        std::string("Failed to create bindgen temporary paths: ") +
            pathError.what() + "."));
    return;
  }
  const TemporaryArtifact preprocessedCleanup(preprocessed, true);
  const TemporaryArtifact dependencyCleanup(dependencies, true);

  // System includes expand with line markers preserved (C-05): the reader
  // maps declarations to their original files and applies the import policy
  // by provenance. `-dD` keeps macro definitions for constant import; the
  // resolved plan target selects the C data model.
  std::vector<std::string> preprocessorArguments{
      "-x", "c", "-dD", "--target=" + std::string(triple)};
  preprocessorArguments.insert(preprocessorArguments.end(),
                               import.macroArguments.begin(),
                               import.macroArguments.end());
  const NativeCCompileRequest preprocessRequest(
      clangExecutable, import.header, preprocessed, CStandard::C17,
      OptimizationLevel::O0, import.includeDirectories,
      std::move(preprocessorArguments));
  const NativePreprocessingResult preprocessing = NativeToolchain().preprocess(
      preprocessRequest, dependencies, {.captureSuccessfulOutput = true});
  if (preprocessing.process.driverDiagnostic) {
    outcome.notes.push_back(*preprocessing.process.driverDiagnostic);
  }
  if (!preprocessing.process.output.empty()) {
    outcome.notes.push_back(preprocessing.process.output);
  }
  if (!preprocessing.succeeded()) {
    outcome.status = BindgenImportStatus::PreprocessingFailure;
    outcome.diagnostics.push_back(importDiagnostic(
        import, "GTI-B1806",
        "Bindgen import '" + import.alias + "': C preprocessing of '" +
            import.declaredHeader + "' failed."));
    return;
  }

  const std::optional<std::string> text = readFileText(preprocessed);
  if (!text) {
    outcome.status = BindgenImportStatus::PreprocessingFailure;
    outcome.diagnostics.push_back(importDiagnostic(
        import, "GTI-B1806",
        "Bindgen import '" + import.alias +
            "': Clang did not produce a preprocessed translation unit."));
    return;
  }

  const std::string key = reuseKey(import, toolIdentity, triple, *text);
  const std::filesystem::path keyPath = import.outputDirectory / kKeyFilename;
  std::error_code existsError;
  if (std::filesystem::is_regular_file(outcome.output, existsError) &&
      !existsError) {
    if (const std::optional<std::string> storedKey = readFileText(keyPath);
        storedKey && *storedKey == key) {
      outcome.status = BindgenImportStatus::Reused;
      return;
    }
  }

  const lang::bindgen::CTargetProfile profile(
      triple.find("windows") != std::string_view::npos
          ? lang::bindgen::CDataModel::Llp64
          : lang::bindgen::CDataModel::Lp64);
  lang::bindgen::BindingOptions options;
  options.namespaceName = import.namespaceName;
  const lang::bindgen::BindgenResult result = lang::bindgen::runCBindgen(
      *text, import.header.filename().string(), std::move(options), profile);
  const std::string headerName = import.declaredHeader;
  const auto originLabel =
      [&result, &headerName](const lang::bindgen::SourceOrigin &origin) {
        if (origin.file != 0 && origin.file <= result.plan.sourceFiles.size()) {
          return result.plan.sourceFiles[origin.file - 1].path + ":" +
                 std::to_string(origin.line);
        }
        return headerName + ":" + std::to_string(origin.line);
      };
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : result.diagnostics) {
    outcome.notes.push_back(
        originLabel(diagnostic.origin) + ": " +
        (diagnostic.severity == lang::bindgen::ReaderSeverity::Error
             ? "error: "
             : "note: ") +
        diagnostic.message);
  }
  for (const lang::bindgen::BindingItem &item : result.plan.items) {
    if (item.kind == lang::bindgen::BindingItemKind::Skipped) {
      outcome.notes.push_back(originLabel(item.origin) + ": note: skipped '" +
                              item.name + "': " + item.reason);
    }
  }
  if (!result.usable()) {
    outcome.status = BindgenImportStatus::GenerationFailure;
    outcome.diagnostics.push_back(importDiagnostic(
        import, "GTI-B1807",
        "Bindgen import '" + import.alias + "': '" + import.declaredHeader +
            "' could not be imported."));
    return;
  }
  outcome.importedCount = result.plan.importedCount();
  outcome.skippedCount = result.plan.skippedCount();

  if (!publishText(outcome.output, result.gtiSource) ||
      !publishText(keyPath, key)) {
    outcome.status = BindgenImportStatus::PublicationFailure;
    outcome.diagnostics.push_back(
        importDiagnostic(import, "GTI-B1805",
                         "Bindgen import '" + import.alias +
                             "': failed to publish generated bindings at '" +
                             outcome.output.string() + "'."));
    return;
  }
  outcome.status = BindgenImportStatus::Generated;
}

} // namespace

BindgenImportGenerationResult
generateBindgenImports(const ProjectBuildPlan &plan,
                       const std::string &clangExecutable,
                       std::string_view toolIdentity) {
  BindgenImportGenerationResult result;
  const std::string triple = targetTriple(plan.target());
  result.outcomes.reserve(plan.bindgenImports().size());
  for (const BindgenImportPlan &import : plan.bindgenImports()) {
    BindgenImportOutcome outcome;
    generateImport(import, clangExecutable, toolIdentity, triple, outcome);
    result.outcomes.push_back(std::move(outcome));
  }
  return result;
}

} // namespace lang::driver
