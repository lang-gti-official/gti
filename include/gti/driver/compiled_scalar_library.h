#pragma once
#include "gti/compiled_scalar_signature.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/driver/compilation.h"
#include "gti/driver/compiled_library.h"
#include "gti/driver/library_bundle.h"
#include <span>
namespace lang::driver {
// Canonical development interface codec, shared by admission and tooling.
[[nodiscard]] std::vector<CompiledScalarSignature>
decodeCompiledLibraryInterface(std::string_view bytes);
[[nodiscard]] std::string encodeCompiledLibraryInterface(
    const std::vector<CompiledScalarSignature> &signatures);
struct CompiledLibraryInputs {
  CompilationInputs compilation;
  std::vector<CompiledScalarImport> imports;
  std::vector<LibraryBundleMember> objects;
  std::vector<LibraryBundleDependency> dependencies;
  std::vector<std::string> symbols;
  std::vector<CompiledModuleImport> moduleImports;
};

// Select installed, compatible standard modules reached by this compilation.
[[nodiscard]] CompiledLibraryInputs
loadStandardLibraryInputs(const CompilationRequest &,
                          const std::filesystem::path &runtime,
                          ProgressObserver *progress = nullptr);
[[nodiscard]] CompilationResult
compileWithLibraryImports(const CompilationRequest &, CompilationInputs,
                          std::vector<CompiledScalarImport>,
                          ProgressObserver *progress = nullptr,
                          std::vector<CompiledModuleImport> moduleImports = {});
[[nodiscard]] CheckResult
checkWithStandardLibraries(const CompilationRequest &,
                           const std::filesystem::path &runtime,
                           ProgressObserver *);
class ProjectBuildPlan;
struct ToolchainLayout;
// Read-only artifact-aware checking: validates metadata, compatibility, imports
// and lowered scalar bindings without emitting objects or invoking native
// tools.
[[nodiscard]] CheckResult
checkCompiledScalarProject(const ProjectBuildPlan &, const ToolchainLayout &,
                           ProgressObserver *progress = nullptr);
// Development-only separate compilation with compiler-owned value type and
// borrow contracts. Producers may own verified passive zero/constant storage;
// generic APIs, producer entry points and executable storage lifetimes reject.
struct CompiledScalarLibraryResult {
  bool interfaceNeedsSource = false;
  CompilationResult compilation;
  std::string bundle;
  std::vector<std::filesystem::path> sourceFiles;
};
class CompiledLibrarySourceConflict final : public std::runtime_error {
public:
  CompiledLibrarySourceConflict(const std::filesystem::path &source,
                                std::filesystem::path manifest)
      : std::runtime_error(
            "compiled library implementation also imported from source: " +
            source.string()),
        sourceManifest(std::move(manifest)) {}
  std::filesystem::path sourceManifest;
};
[[nodiscard]] CompiledScalarLibraryResult
buildCompiledScalarLibrary(const CompilationRequest &,
                           const std::vector<std::string> &keys,
                           const std::filesystem::path &runtimeArchive);
[[nodiscard]] CompiledScalarLibraryResult buildCompiledScalarLibraryTarget(
    const CompilationRequest &, const CompiledLibraryTarget &,
    const std::filesystem::path &runtimeArchive,
    std::span<const CompiledLibraryImport> imports = {});
// Conservative output reuse: complete source/configuration/dependency checks,
// without semantic analysis or native emission. Invalid/stale artifacts miss.
[[nodiscard]] bool
reusableCompiledScalarLibraryTarget(const CompilationRequest &,
                                    const CompiledLibraryTarget &,
                                    std::span<const CompiledLibraryImport>,
                                    const std::filesystem::path &runtime,
                                    const std::filesystem::path &artifact);
// Same validation as the boolean query, retaining the bytes it actually read.
[[nodiscard]] CompiledLibrarySnapshot reusableCompiledScalarLibrarySnapshot(
    const CompilationRequest &, const CompiledLibraryTarget &,
    std::span<const CompiledLibraryImport>,
    const std::filesystem::path &runtime, const std::filesystem::path &artifact,
    std::vector<std::filesystem::path> *sourceFiles = nullptr);
struct CompiledScalarConsumerResult {
  CompilationResult compilation;
  std::vector<LibraryBundleMember> objects;
};
// interfacePath must be an absent, absolute virtual .gti path imported by the
// consumer. Metadata is projected into ordinary GTI declarations and checked
// by the same frontend. Implementation source is never requested.
[[nodiscard]] CompiledScalarConsumerResult
compileCompiledScalarConsumer(const CompilationRequest &,
                              std::string_view bundle,
                              const std::filesystem::path &interfacePath,
                              const std::filesystem::path &runtimeArchive,
                              const CompiledLibraryImport *expected = nullptr);
// Load a bounded dependency closure in one frontend transaction.
[[nodiscard]] CompiledScalarConsumerResult
compileCompiledScalarConsumer(const CompilationRequest &,
                              std::span<const CompiledLibraryImport>,
                              const std::filesystem::path &runtimeArchive);
} // namespace lang::driver
