#include "gti/driver/compiled_scalar_library.h"
#include "compilation_pipeline.h"
#include "compiled_scalar_support.h"
#include "file_digest.h"
#include "gti/compiled_interface_types.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/compiled_template_body.h"
#include "gti/driver/project.h"
#include "gti/persistent_declaration.h"
#include "gti/source_loader.h"
#include "sha256.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
namespace lang::driver {
namespace {
std::string readCompiledLibraryFileBounded(const std::filesystem::path &path,
                                           std::size_t limit) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    throw std::invalid_argument(
        "compiled library input must be a regular file");
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > limit)
    throw std::invalid_argument(
        "compiled library input exceeds the bundle size limit");
  std::ifstream file(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      file.peek() != std::char_traits<char>::eof() || file.bad())
    throw std::invalid_argument(
        "compiled library input changed or could not be read completely");
  return bytes;
}
} // namespace
std::string readCompiledLibraryFile(const std::filesystem::path &path) {
  return readCompiledLibraryFileBounded(path, libraryBundleByteLimit);
}
void CompiledLibrarySnapshots::retain(
    const std::filesystem::path &path, CompiledLibrarySnapshot snapshot,
    std::vector<std::filesystem::path> sourceFiles) {
  if (path.empty() || !snapshot || snapshot->size() > libraryBundleByteLimit)
    throw std::invalid_argument("invalid compiled library snapshot");
  const auto key = std::filesystem::absolute(path).lexically_normal();
  if (const auto found = artifacts.find(key); found != artifacts.end()) {
    if (found->second.bytes != snapshot && *found->second.bytes != *snapshot)
      throw std::invalid_argument(
          "compiled library selection changed within one build");
    if (!sourceFiles.empty())
      found->second.sourceFiles = std::move(sourceFiles);
    return;
  }
  if (artifacts.size() >= 64 ||
      snapshot->size() > libraryBundleByteLimit - bytes)
    throw std::invalid_argument(
        "compiled library snapshots exceed build limits");
  bytes += snapshot->size();
  artifacts.emplace(
      key, ProjectLibrarySnapshot{std::move(snapshot), std::move(sourceFiles)});
}
std::vector<CompiledLibraryImport> CompiledLibrarySnapshots::select(
    std::span<const CompiledLibraryImport> inputs) {
  if (inputs.size() > 64)
    throw std::invalid_argument(
        "at most 64 compiled libraries may be imported");
  std::vector<CompiledLibraryImport> selected(inputs.begin(), inputs.end());
  for (auto &input : selected) {
    if (input.artifact.empty())
      throw std::invalid_argument(
          "compiled library snapshot needs an artifact path");
    const auto key =
        std::filesystem::absolute(input.artifact).lexically_normal();
    if (input.snapshot) {
      retain(key, input.snapshot, input.sourceFiles);
    } else if (!artifacts.contains(key)) {
      if (artifacts.size() >= 64)
        throw std::invalid_argument(
            "compiled library snapshots exceed build limits");
      std::error_code error;
      const auto size = std::filesystem::file_size(input.artifact, error);
      if (error)
        throw std::invalid_argument("compiled dependency artifact is missing; "
                                    "build its library target first");
      if (size > libraryBundleByteLimit - bytes)
        throw std::invalid_argument(
            "combined compiled libraries exceed the bundle size limit");
      retain(key,
             std::make_shared<const std::string>(readCompiledLibraryFileBounded(
                 input.artifact, libraryBundleByteLimit - bytes)));
    }
    input.snapshot = artifacts.at(key).bytes;
    input.sourceFiles = artifacts.at(key).sourceFiles;
  }
  return selected;
}
std::optional<std::string>
compiledScalarProducerInputDiagnostic(const NativeInputs &n) {
  if (!n.cSources.empty() || !n.cppSources.empty() || !n.libraryFiles.empty() ||
      !n.finalLibraryFiles.empty() || !n.libraries.empty() ||
      !n.frameworks.empty() || !n.compilerArguments.empty() ||
      !n.cCompilerArguments.empty() || !n.linkerArguments.empty() ||
      !n.trailingArguments.empty() || !n.orderedLinkOperands.empty() ||
      std::any_of(n.dependencyGroups.begin(), n.dependencyGroups.end(),
                  [](const auto &group) {
                    return !group.cSources.empty() ||
                           !group.cppSources.empty() ||
                           !group.linkOperands.empty() ||
                           !group.includeDirectories.empty() ||
                           !group.libraryDirectories.empty();
                  }) ||
      !n.includeDirectories.empty() || !n.libraryDirectories.empty())
    return "producer must be self-contained and cannot declare native inputs "
           "in this profile";
  return std::nullopt;
}
namespace {
std::string hash(std::string_view bytes) {
  Sha256 h;
  h.update(bytes);
  return h.finishHex();
}
std::string hashFile(const std::filesystem::path &path) {
  std::string error;
  auto digest = digestFile(path, error);
  if (!digest)
    throw std::invalid_argument("cannot hash compatibility input: " + error);
  return digest->hash;
}
std::vector<std::string> runtimeImports(const std::filesystem::path &path,
                                        const TargetInfo &target,
                                        const std::string &expectedDigest) {
  const auto bytes = readCompiledLibraryFile(path);
  if (hash(bytes) != expectedDigest)
    throw std::invalid_argument(
        "compiled library runtime changed during admission");
  return compiledLibrarySupportSymbols(bytes, target);
}
void field(std::string &out, std::string_view bytes) {
  out += std::to_string(bytes.size());
  out += ':';
  out += bytes;
}
std::vector<CompiledScalarSignature>
legacySignatures(const std::vector<std::string> &keys) {
  if (keys.empty() || keys.size() > 256)
    throw std::invalid_argument("invalid scalar interface count");
  std::size_t total = 0;
  for (const auto &key : keys) {
    if (key.size() > 16384 || key.size() > 1024U * 1024U - total)
      throw std::invalid_argument("scalar interface exceeds metadata limit");
    total += key.size();
  }
  std::vector<CompiledScalarSignature> signatures;
  for (const auto &key : keys)
    signatures.push_back({key, SemanticType::Int32});
  return signatures;
}
std::string recipe(const CompiledScalarSignature &signature) {
  return signature.resultRecipe.empty()
             ? writePersistentType(signature.result, PersistentTypeRegistry{})
             : signature.resultRecipe;
}
PersistentFunctionKey decodedKey(const CompiledScalarSignature &signature) {
  return readPersistentFunctionKey(
      signature.key,
      compiledInterfaceSyntheticRegistry(signature.typeMetadata));
}
std::string interface(const std::vector<CompiledScalarSignature> &signatures) {
  if (signatures.empty() || signatures.size() > 256)
    throw std::invalid_argument("invalid callable interface count");
  std::size_t total = 0;
  for (const auto &signature : signatures) {
    for (const auto *value :
         {&signature.key, &signature.typeMetadata, &signature.resultRecipe}) {
      if (value->size() > 1024U * 1024U - total)
        throw std::invalid_argument(
            "callable interface exceeds metadata limit");
      total += value->size();
    }
    if (signature.key.size() > 16384)
      throw std::invalid_argument("callable key exceeds metadata limit");
  }
  auto sorted = signatures;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto &a, const auto &b) { return a.key < b.key; });
  std::string out = "gti.callable-interface.4\n", previous;
  for (const auto &signature : sorted) {
    if (signature.key == previous)
      throw std::invalid_argument("duplicate callable interface declaration");
    previous = signature.key;
    (void)compiledInterfaceCallableSource(signature);
    field(out, signature.key);
    field(out, recipe(signature));
    field(out, signature.typeMetadata);
    field(out, signature.borrow.origin == BorrowOriginKind::None ? "none"
                                                                 : "argument");
    field(out, std::to_string(signature.borrow.parameter));
    field(out,
          signature.borrow.access == AccessMode::ReadOnly ? "read" : "mut");
    field(out, signature.codegenAttributes.inlining == FunctionInlining::Default
                   ? "default"
               : signature.codegenAttributes.inlining == FunctionInlining::Hint
                   ? "hint"
                   : "never");
    field(out, signature.codegenAttributes.cold ? "1" : "0");
    out += '\n';
    if (out.size() > 1024U * 1024U)
      throw std::invalid_argument("callable interface exceeds metadata limit");
  }
  return out;
}
std::string_view takeField(std::string_view &bytes) {
  const auto colon = bytes.find(':');
  std::size_t size = 0;
  if (colon == bytes.npos || colon == 0 || colon > 7)
    throw std::invalid_argument("malformed callable interface field");
  const auto [end, error] =
      std::from_chars(bytes.data(), bytes.data() + colon, size);
  if (error != std::errc{} || end != bytes.data() + colon ||
      size > 1024U * 1024U || size > bytes.size() - colon - 1)
    throw std::invalid_argument("malformed callable interface field");
  bytes.remove_prefix(colon + 1);
  const auto result = bytes.substr(0, size);
  bytes.remove_prefix(size);
  return result;
}
std::vector<CompiledScalarSignature> readInterface(std::string_view bytes) {
  constexpr std::string_view header = "gti.callable-interface.4\n";
  if (bytes.size() > 1024U * 1024U || !bytes.starts_with(header))
    throw std::invalid_argument("unsupported compiled interface profile");
  auto rest = bytes.substr(header.size());
  std::vector<CompiledScalarSignature> signatures;
  while (!rest.empty()) {
    if (signatures.size() >= 256)
      throw std::invalid_argument("invalid callable interface count");
    CompiledScalarSignature signature;
    signature.key = takeField(rest);
    signature.resultRecipe = takeField(rest);
    signature.typeMetadata = takeField(rest);
    signature.result = readPersistentType(
        signature.resultRecipe,
        compiledInterfaceSyntheticRegistry(signature.typeMetadata));
    const auto origin = takeField(rest);
    if (origin != "none" && origin != "argument")
      throw std::invalid_argument("invalid compiled borrow origin");
    signature.borrow.origin =
        origin == "none" ? BorrowOriginKind::None : BorrowOriginKind::Argument;
    const auto parameter = takeField(rest);
    const auto [end, error] =
        std::from_chars(parameter.data(), parameter.data() + parameter.size(),
                        signature.borrow.parameter);
    if (error != std::errc{} || end != parameter.data() + parameter.size() ||
        signature.borrow.parameter > 256)
      throw std::invalid_argument("invalid compiled borrow parameter");
    const auto access = takeField(rest);
    if (access != "read" && access != "mut")
      throw std::invalid_argument("invalid compiled borrow access");
    signature.borrow.access =
        access == "read" ? AccessMode::ReadOnly : AccessMode::Mutable;
    const auto inlining = takeField(rest);
    if (inlining != "default" && inlining != "hint" && inlining != "never")
      throw std::invalid_argument("invalid compiled inlining policy");
    signature.codegenAttributes.inlining =
        inlining == "default" ? FunctionInlining::Default
        : inlining == "hint"  ? FunctionInlining::Hint
                              : FunctionInlining::Never;
    const auto cold = takeField(rest);
    if (cold != "0" && cold != "1")
      throw std::invalid_argument("invalid compiled cold policy");
    signature.codegenAttributes.cold = cold == "1";
    if (rest.empty() || rest.front() != '\n')
      throw std::invalid_argument("malformed callable interface");
    rest.remove_prefix(1);
    signatures.push_back(std::move(signature));
  }
  if (interface(signatures) != bytes)
    throw std::invalid_argument("noncanonical callable interface");
  return signatures;
}
std::string
libraryIdentity(const std::vector<CompiledScalarSignature> &signatures) {
  const auto owner = decodedKey(signatures.front()).owner;
  for (const auto &signature : signatures) {
    const auto candidate = decodedKey(signature).owner;
    if (candidate.package != owner.package || candidate.target != owner.target)
      throw std::invalid_argument(
          "compiled library keys must belong to one package target");
  }
  std::string identity = "gti.scalar-library.1";
  field(identity, owner.package);
  field(identity, owner.target);
  return hash(identity);
}
struct InstalledModule {
  std::string_view target;
  std::string_view entry;
  std::string_view facade;
};
constexpr InstalledModule installedModules[] = {
#include "gti/installed_stdlib_modules.inc"
};
enum class LibraryProfile { Project, InstalledStdlib };
LibraryProfile producerProfile(const SourceGraph &graph,
                               const CompiledLibraryTarget *target) {
  const auto *root = graph.findUnit(graph.entryUnit());
  if (!target || target->package != "gti-std" || !root ||
      root->role != SourceUnitRole::StandardLibrary)
    return LibraryProfile::Project;
  // Installed modules can be shared across unrelated application defines only
  // when the entire producer closure neither reads nor writes such flags.
  for (const auto &unit : graph.sourceUnits()) {
    if (unit.role == SourceUnitRole::Application ||
        std::ranges::any_of(unit.configurationTokens, [](const auto &token) {
          return token.kind == ConfigurationTokenKind::Flag;
        }))
      throw std::invalid_argument(
          "an installed standard library must have a toolchain-owned, "
          "flag-independent source closure");
  }
  return LibraryProfile::InstalledStdlib;
}
LlvmBackendOptions libraryBackendOptions(const CompilationRequest &request,
                                         LibraryProfile profile) {
  auto options = llvmBackendOptions(request);
  options.distributedLibrary = profile == LibraryProfile::InstalledStdlib;
  return options;
}
LibraryBundleIdentity
environment(const CompilationRequest &request,
            const std::filesystem::path &runtime,
            LibraryProfile profile = LibraryProfile::Project) {
  if (request.backend() != BackendKind::Llvm || request.debugInfo() ||
      request.llvmInstrumentation() != LlvmInstrumentationMode::None ||
      request.program().kind != ProgramKind::Executable ||
      request.program().testSuite)
    throw std::invalid_argument(
        "unsupported compiled scalar build configuration");
  std::string error;
  auto backend = queryLlvmBackendIdentity(
      request.target(), libraryBackendOptions(request, profile), error);
  if (!backend)
    throw std::invalid_argument(error);
  LibraryBundleIdentity id;
  id.target = backend->triple;
  id.toolchain = hashFile(backend->compilerExecutable);
  id.runtime = hashFile(runtime);
  std::string config;
  for (const auto &s : {backend->schema, backend->llvmVersion, backend->triple,
                        backend->dataLayout, backend->cpu, backend->tuneCpu,
                        backend->features, backend->instrumentation})
    field(config, s);
  for (auto value : {backend->privateAbi, backend->relocation,
                     backend->codeModel, backend->codegenOptimization})
    field(config, std::to_string(value));
  field(config, std::to_string(
                    static_cast<unsigned>(request.target().executionProfile)));
  if (profile == LibraryProfile::InstalledStdlib) {
    field(config, "gti.flag-independent-stdlib.1");
  } else {
    field(config, std::to_string(request.configurationFlags().size()));
    for (const auto &flag : request.configurationFlags())
      field(config, flag);
    field(config, std::to_string(request.declaredConfigurationFlags().size()));
    for (const auto &flag : request.declaredConfigurationFlags())
      field(config, flag);
  }
  field(config, std::to_string(static_cast<unsigned>(request.optimization())));
  field(config, request.target().runtimeCapabilities.threads ? "threads"
                                                             : "no-threads");
  id.configuration = hash(config);
  return id;
}
struct ScalarSelection {
  std::vector<CompiledInterfaceUnit> units;
  std::map<SourceUnitId, std::vector<SourceSpan>> declarations;
  std::vector<std::string> functions;
  bool needsDependencySource = false;
};
class ScalarBackend final : public Backend {
public:
  ScalarBackend(std::vector<CompiledScalarSignature> signatures,
                SourceUnitId root, Backend &backend,
                std::optional<ScalarSelection> selection = {},
                std::vector<CompiledScalarImport> imported = {},
                CompiledMirBodies *mirOutput = nullptr)
      : signatures(std::move(signatures)), imports(std::move(imported)),
        root(root), producer(true), backend(backend),
        selection(std::move(selection)), mirOutput(mirOutput) {}
  ScalarBackend(std::vector<CompiledScalarImport> imports, Backend &backend)
      : imports(std::move(imports)), backend(backend) {}
  bool interfaceNeedsSource() const { return needsSource; }
  const std::vector<CompiledScalarSignature> &selectedSignatures() const {
    return signatures;
  }
  std::string_view name() const override { return "llvm-compiled-scalar"; }
  BackendArtifact generate(LoweredProgram program) override {
    std::optional<LoweredProgram> bound;
    try {
      if (producer) {
        std::vector<CompiledScalarImport> exports;
        if (selection) {
          try {
            if (selection->needsDependencySource)
              throw std::invalid_argument(
                  "public library or its implementation requires a dependency "
                  "source interface");
            for (const auto &unit : selection->units) {
              const auto &surface = selection->declarations.at(unit.sourceUnit);
              if (surface.empty() && selection->functions.empty())
                continue;
              auto group = CompiledScalarUnitBuilder().selectExports(
                  program, unit.sourceUnit, unit.owner, selection->functions,
                  imports, surface, selection->units);
              for (auto &signature : group) {
                exports.push_back({signature, unit.sourceUnit});
                signatures.push_back(std::move(signature));
              }
            }
            if (exports.empty())
              throw std::invalid_argument("public interface requires source: "
                                          "no representable callables");
          } catch (const std::invalid_argument &) {
            needsSource = selection->functions.empty();
            throw;
          }
        } else {
          for (const auto &signature : signatures)
            exports.push_back({signature, root});
        }
        (void)interface(signatures);
        (void)libraryIdentity(signatures);
        bound = CompiledScalarUnitBuilder().bindScoped(std::move(program),
                                                       exports, imports);
        if (mirOutput) {
          for (const auto &binding : bound->compiledScalarBindings()) {
            if (binding.imported)
              continue;
            const auto *function =
                bound->mir().findFunctionInstance(binding.function);
            if (auto body = projectCompiledMir(bound->mir(), *function))
              mirOutput->emplace(binding.symbol, std::move(body));
          }
        }
      } else {
        bound = CompiledScalarUnitBuilder().bindImports(std::move(program),
                                                        imports);
      }
    } catch (const std::invalid_argument &error) {
      throw BackendUnsupportedError(error.what());
    }
    return backend.generate(std::move(*bound));
  }

private:
  std::vector<CompiledScalarSignature> signatures;
  std::vector<CompiledScalarImport> imports;
  SourceUnitId root = 0;
  bool producer = false;
  bool needsSource = false;
  Backend &backend;
  std::optional<ScalarSelection> selection;
  CompiledMirBodies *mirOutput = nullptr;
};
std::string templateLibraryIdentity(const CompiledLibraryTarget &target) {
  std::string identity = "gti.generic-library.1";
  field(identity, target.package);
  field(identity, target.target);
  return hash(identity);
}

std::optional<ScalarSelection>
producerSelection(const FrontendResult &frontend,
                  const CompiledLibraryTarget *target, LibraryProfile profile) {
  if (!target || !frontend.sourceValid)
    return std::nullopt;
  const auto &graph = frontend.sourceGraph;
  const auto *entry = graph.findUnit(graph.entryUnit());
  if (!entry || !entry->moduleDeclaration)
    throw std::invalid_argument(
        "compiled library root must declare a module interface");
  if (!frontend.canGenerateCode())
    return std::nullopt;
  ScalarSelection selection;
  selection.functions = target->functions;
  if (selection.functions.empty()) {
    for (const auto &unit : graph.sourceUnits())
      if (unit.packageIdentity &&
          unit.packageIdentity != entry->packageIdentity &&
          unit.role == SourceUnitRole::Application &&
          !unit.compiledScalarInterface)
        selection.needsDependencySource = true;
  }
  std::vector<SourceUnitId> pending{entry->id};
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (selection.declarations.contains(id))
      continue;
    const auto &unit = *graph.findUnit(id);
    if (unit.packageIdentity != entry->packageIdentity ||
        unit.compiledScalarInterface ||
        (unit.role == SourceUnitRole::StandardLibrary &&
         profile != LibraryProfile::InstalledStdlib) ||
        !unit.moduleDeclaration) {
      selection.needsDependencySource = true;
      continue;
    }
    selection.declarations.emplace(id, unit.exportedDeclarations);
    selection.units.push_back({id,
                               {target->package, target->target,
                                unit.moduleDeclaration->lexeme, ""}});
    if (selection.functions.empty())
      for (const auto &edge : graph.dependencyEdges())
        if (edge.source == id &&
            edge.kind == SourceDependencyKind::ExportImport)
          pending.push_back(edge.target);
  }
  return selection;
}

enum class StandardLibraryInput { Source, Installed };
CompiledLibraryInputs
loadExecutableInputs(const CompilationRequest &, const std::filesystem::path &,
                     ProgressObserver *,
                     std::unordered_map<std::string, std::string> overlays);
CompiledLibraryInputs prepareScalarConsumer(
    const CompilationRequest &, std::span<const CompiledLibraryImport>,
    const std::filesystem::path &, ProgressObserver *progress = nullptr,
    StandardLibraryInput standardLibrary = StandardLibraryInput::Source);
std::vector<std::filesystem::path>
producerImplementations(const SourceGraph &graph) {
  std::vector<std::filesystem::path> result;
  for (const auto &unit : graph.sourceUnits())
    if (unit.role == SourceUnitRole::Application &&
        !unit.compiledScalarInterface)
      result.push_back(unit.path);
  return result;
}
std::string producerSources(const CompilationInputs &inputs,
                            const CompiledLibraryTarget *target) {
  std::string sources = "gti.compiled-source-graph.2";
  const auto &graph = inputs.sourceGraph;
  field(sources, graph.sourceOwners()
                     ? graph.sourceOwners()->resolutionIdentity()
                     : std::string{});
  auto names = inputs.sources.names();
  std::sort(names.begin(), names.end());
  field(sources, std::to_string(names.size()));
  for (const auto &name : names) {
    field(sources, name);
    field(sources, *inputs.sources.find(name));
    const auto *unit = graph.findUnit(graph.sourceUnitForPath(name));
    field(sources, unit ? unit->packageIdentity.value_or("") : "");
    field(sources,
          unit ? std::to_string(static_cast<unsigned>(unit->role)) : "");
    field(sources,
          unit && unit->compiledScalarInterface ? "compiled" : "source");
    field(sources, unit && unit->entry ? "entry" : "dependency");
    field(sources, unit && unit->prelude ? "prelude" : "ordinary");
    // Include the actual configured token stream, not a lossy cache digest.
    field(sources,
          std::to_string(unit && unit->tokens ? unit->tokens->size() : 0));
    if (unit && unit->tokens)
      for (const auto &token : *unit->tokens) {
        field(sources, std::to_string(static_cast<unsigned>(token.kind)));
        field(sources, token.lexeme);
      }
  }
  field(sources, std::to_string(target ? target->functions.size() : 0));
  if (target) {
    auto selected = target->functions;
    std::sort(selected.begin(), selected.end());
    for (const auto &name : selected)
      field(sources, name);
  }
  return hash(sources);
}
CompiledScalarLibraryResult buildScalarLibrary(
    const CompilationRequest &request, const std::vector<std::string> &keys,
    const std::filesystem::path &runtime, const CompiledLibraryTarget *target,
    std::span<const CompiledLibraryImport> imports = {}) {
  auto signatures =
      target ? std::vector<CompiledScalarSignature>{} : legacySignatures(keys);
  if (!target) {
    (void)interface(signatures);
    (void)libraryIdentity(signatures);
  }
  auto prepared = imports.empty()
                      ? CompiledLibraryInputs{loadCompilationInputs(request)}
                      : prepareScalarConsumer(request, imports, runtime);
  auto &inputs = prepared.compilation;
  const auto profile = producerProfile(inputs.sourceGraph, target);
  auto id = environment(request, runtime, profile);
  auto allowedImports = runtimeImports(runtime, request.target(), id.runtime);
  id.sources = producerSources(inputs, target);
  const auto producerRequest = request.withApplicationBodies();
  auto frontend =
      detail::analyzeForArtifact(producerRequest, std::move(inputs), nullptr);
  if (target && profile == LibraryProfile::InstalledStdlib &&
      target->functions.empty() &&
      std::ranges::any_of(installedModules,
                          [&](const auto &module) {
                            return module.target == target->target &&
                                   module.facade.empty();
                          }) &&
      frontend.canGenerateCode()) {
    // Only the staged toolchain can export a trusted generic module. Projects
    // continue to import data through the checked, unprivileged interface.
    const auto &graph = std::as_const(frontend.sourceGraph);
    const auto *entry = graph.findUnit(graph.entryUnit());
    if (!entry || !entry->moduleDeclaration || !prepared.dependencies.empty())
      throw std::invalid_argument("invalid installed generic library root");
    auto declarations =
        std::span(frontend.program.declarations())
            .subspan(entry->declarationStart, entry->declarationCount);
    const auto *source = frontend.sources.find(entry->path.string());
    if (!source)
      throw std::invalid_argument("missing generic library source snapshot");
    auto recipe = compileTemplateModule(declarations, *source);
    auto metadata = encodeTemplateModule(recipe);
    id.library = templateLibraryIdentity(*target);
    auto current = loadCompilationInputs(request);
    if (!current.succeeded() ||
        producerSources(current, target) != id.sources ||
        hashFile(runtime) != id.runtime)
      throw std::invalid_argument(
          "generic library inputs changed during compilation");
    CompiledScalarLibraryResult result;
    result.sourceFiles = producerImplementations(graph);
    result.bundle = encodeLibraryBundle(
        {.identity = id, .interfaceMetadata = metadata, .templateOnly = true});
    result.compilation.status = CompilationStatus::Success;
    result.compilation.sources = std::move(frontend.sources);
    result.compilation.diagnostics = std::move(frontend.diagnostics);
    result.compilation.artifact =
        BackendArtifact{.kind = BackendArtifactKind::CompiledInterface,
                        .contents = std::move(metadata)};
    return result;
  }
  auto selection = producerSelection(frontend, target, profile);
  const auto options = libraryBackendOptions(request, profile);
  LlvmBackend native(options);
  CompiledMirBodies optimizationBodies;
  const bool optimizeImports =
      options.optimization != LlvmCodegenOptimization::None &&
      !options.debugInfo &&
      options.instrumentation == LlvmInstrumentationMode::None;
  ScalarBackend backend(std::move(signatures), frontend.sourceGraph.entryUnit(),
                        native, std::move(selection),
                        std::move(prepared.imports),
                        optimizeImports ? &optimizationBodies : nullptr);
  CompiledScalarLibraryResult result;
  result.sourceFiles = producerImplementations(frontend.sourceGraph);
  result.compilation = detail::generateArtifact(
      producerRequest, std::move(frontend), backend, nullptr);
  result.interfaceNeedsSource = backend.interfaceNeedsSource();
  if (result.compilation.succeeded()) {
    auto metadata = interface(backend.selectedSignatures());
    id.library = libraryIdentity(backend.selectedSignatures());
    std::vector<std::string> symbols;
    for (const auto &key : backend.selectedSignatures())
      symbols.push_back(compiledScalarSymbol(key));
    allowedImports.insert(allowedImports.end(), prepared.symbols.begin(),
                          prepared.symbols.end());
    verifyCompiledScalarObject(result.compilation.artifact->singleObject(),
                               request.target(), symbols, allowedImports);
    if (hashFile(runtime) != id.runtime)
      throw std::invalid_argument(
          "compiled library runtime changed during compilation");
    // Publication must still describe the input snapshot that produced code.
    auto current = imports.empty()
                       ? CompiledLibraryInputs{loadCompilationInputs(request)}
                       : prepareScalarConsumer(request, imports, runtime);
    auto sortedDependencies = [](auto dependencies) {
      std::sort(
          dependencies.begin(), dependencies.end(),
          [](const auto &a, const auto &b) { return a.library < b.library; });
      return dependencies;
    };
    if (!current.compilation.succeeded() ||
        producerSources(current.compilation, target) != id.sources ||
        sortedDependencies(current.dependencies) !=
            sortedDependencies(prepared.dependencies))
      throw std::invalid_argument(
          "compiled library inputs changed during compilation");
    result.bundle = encodeLibraryBundle(
        {id,
         std::move(prepared.dependencies),
         {{"unit.o", result.compilation.artifact->singleObject()}},
         metadata,
         encodeCompiledMir(optimizationBodies)});
  }
  return result;
}
} // namespace
std::vector<CompiledScalarSignature>
decodeCompiledLibraryInterface(std::string_view bytes) {
  return readInterface(bytes);
}
std::string encodeCompiledLibraryInterface(
    const std::vector<CompiledScalarSignature> &signatures) {
  return interface(signatures);
}
CompiledScalarLibraryResult
buildCompiledScalarLibrary(const CompilationRequest &request,
                           const std::vector<std::string> &keys,
                           const std::filesystem::path &runtime) {
  return buildScalarLibrary(request, keys, runtime, nullptr);
}
CompiledScalarLibraryResult buildCompiledScalarLibraryTarget(
    const CompilationRequest &request, const CompiledLibraryTarget &target,
    const std::filesystem::path &runtime,
    std::span<const CompiledLibraryImport> imports) {
  return buildScalarLibrary(request, {}, runtime, &target, imports);
}
namespace {
struct ScalarBundleInput {
  LibraryBundle bundle;
  std::filesystem::path path;
  std::optional<CompiledLibraryImport> selected;
  std::string digest;
};
std::string quotedInterfacePath(const std::filesystem::path &path) {
  std::string result = "\"";
  for (unsigned char c : path.generic_string()) {
    switch (c) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (c < 32 || c == 127)
        throw std::invalid_argument("compiled interface path contains an "
                                    "unsupported control character");
      result += static_cast<char>(c);
    }
  }
  return result + "\"";
}
CompiledMirBodies checkedOptimizationBodies(
    const LibraryBundle &bundle,
    const std::vector<CompiledScalarSignature> &signatures) {
  auto bodies = decodeCompiledMir(bundle.optimizationBodies);
  if (bodies.empty())
    return bodies;
  std::size_t matched = 0;
  for (const auto &signature : signatures) {
    auto found = bodies.find(compiledScalarSymbol(signature));
    if (found == bodies.end())
      continue;
    const auto parameters = decodedKey(signature).parameters;
    if (signature.codegenAttributes.inlining == FunctionInlining::Never ||
        signature.codegenAttributes.cold ||
        !verifyCompiledMir(
            *found->second, parameters, signature.result,
            compiledMirInterfaceFields(signature.typeMetadata, parameters)))
      throw std::invalid_argument(
          "invalid compiled optimization MIR for exported signature");
    ++matched;
  }
  if (matched != bodies.size())
    throw std::invalid_argument(
        "compiled optimization MIR has an unknown callable");
  return bodies;
}
struct LibraryFacade {
  std::string path;
  std::string source;
  std::vector<CompiledScalarSignature> signatures;
};
struct AdmittedLibraries {
  std::vector<PackageSourceRoot> packageSources;
  std::vector<LibraryFacade> facades;
  std::vector<LibraryBundleMember> objects;
  std::vector<LibraryBundleDependency> dependencies;
  std::vector<std::string> symbols;

  CompiledMirBodies optimizationBodies;

  void bind(CompiledLibraryInputs &inputs) && {
    auto &graph = inputs.compilation.sourceGraph;
    for (auto &facade : facades) {
      const auto unit = graph.sourceUnitForPath(facade.path);
      if (!unit)
        continue;
      graph.markCompiledScalarInterface(unit);
      for (auto &signature : facade.signatures) {
        auto found = optimizationBodies.find(compiledScalarSymbol(signature));
        inputs.imports.push_back(
            {std::move(signature), unit,
             found == optimizationBodies.end() ? nullptr : found->second});
      }
    }
    std::ranges::move(objects, std::back_inserter(inputs.objects));
    std::ranges::move(dependencies, std::back_inserter(inputs.dependencies));
    std::ranges::move(symbols, std::back_inserter(inputs.symbols));
  }
};

struct AdmissionEnvironment {
  LibraryBundleIdentity identity;
  std::vector<std::string> runtimeSymbols;
};

AdmissionEnvironment admissionEnvironment(const CompilationRequest &request,
                                          const std::filesystem::path &runtime,
                                          LibraryProfile profile) {
  auto identity = environment(request, runtime, profile);
  auto symbols = runtimeImports(runtime, request.target(), identity.runtime);
  return {std::move(identity), std::move(symbols)};
}

AdmittedLibraries admitScalarLibraries(const CompilationRequest &request,
                                       std::vector<ScalarBundleInput> &bundles,
                                       const AdmissionEnvironment &system) {
  if (bundles.empty() || bundles.size() > 64)
    throw std::invalid_argument("compiled consumer requires 1 to 64 libraries");
  struct Admission {
    std::vector<CompiledScalarSignature> signatures;
    std::vector<std::string> symbols;
    std::string library;
    PersistentDeclarationOwner owner;
  };
  std::vector<Admission> admitted;
  std::map<std::string, std::size_t> libraries;
  std::size_t declarationCount = 0;
  CompiledMirBodies optimizationBodies;
  for (auto &input : bundles) {
    auto signatures = readInterface(input.bundle.interfaceMetadata);
    declarationCount += signatures.size();
    if (declarationCount > 256)
      throw std::invalid_argument(
          "combined compiled interface exceeds 256 declarations");
    auto bodies = checkedOptimizationBodies(input.bundle, signatures);
    optimizationBodies.merge(bodies);
    auto identity = libraryIdentity(signatures);
    if (!libraries.emplace(identity, admitted.size()).second)
      throw std::invalid_argument("duplicate compiled library package/target");
    if (input.selected) {
      const auto owner = decodedKey(signatures.front()).owner;
      if (owner.package != input.selected->package ||
          owner.target != input.selected->target)
        throw std::invalid_argument("compiled library package/target does not "
                                    "match the selected artifact");
    }
    std::vector<std::string> symbols;
    for (const auto &signature : signatures)
      symbols.push_back(compiledScalarSymbol(signature));
    const auto owner = decodedKey(signatures.front()).owner;
    admitted.push_back({std::move(signatures), std::move(symbols),
                        std::move(identity), owner});
  }
  std::vector<unsigned> state(bundles.size());
  std::vector<std::set<std::size_t>> dependencyClosure(bundles.size());
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    if (state[index] == 1)
      throw std::invalid_argument("compiled artifact dependency cycle");
    if (state[index] == 2)
      return;
    state[index] = 1;
    for (const auto &dependency : bundles[index].bundle.dependencies) {
      const auto found = libraries.find(dependency.library);
      if (found == libraries.end())
        throw std::invalid_argument("missing compiled dependency artifact");
      if (bundles[found->second].digest != dependency.bundle)
        throw std::invalid_argument(
            "conflicting compiled dependency artifact digest");
      visit(found->second);
      dependencyClosure[index].insert(found->second);
      dependencyClosure[index].insert(dependencyClosure[found->second].begin(),
                                      dependencyClosure[found->second].end());
    }
    state[index] = 2;
  };
  for (std::size_t i = 0; i < bundles.size(); ++i)
    visit(i);
  auto sources = request.packageSources();
  const SourceOwnerRegistry owners(sources);
  const auto registered = [&](const std::filesystem::path &path,
                              const std::string &name) {
    const auto *owner = owners.ownerForPath(path);
    return owner && std::any_of(owner->modules.begin(), owner->modules.end(),
                                [&](const auto &module) {
                                  return module.first == name &&
                                         module.second == path;
                                });
  };
  std::vector<std::filesystem::path> interfacePaths;
  std::set<std::string> distinctPaths;
  for (std::size_t i = 0; i < bundles.size(); ++i) {
    const auto &input = bundles[i];
    auto path = input.selected && !input.selected->interfaceRequired &&
                        input.selected->moduleName.empty()
                    ? std::filesystem::absolute(request.entry()).parent_path() /
                          (".gti-compiled-" + admitted[i].library + ".gti")
                    : input.path;
    if (!path.is_absolute() || path.extension() != ".gti" ||
        (std::filesystem::exists(path) &&
         !(input.selected && !input.selected->moduleName.empty() &&
           registered(path, input.selected->moduleName) &&
           owners.ownerForPath(path)->name == input.selected->package)) ||
        path.lexically_normal() != path)
      throw std::invalid_argument("compiled interface requires an absent "
                                  "normalized absolute .gti path");
    path = std::filesystem::weakly_canonical(path);
    if (!distinctPaths.insert(path.string()).second)
      throw std::invalid_argument("duplicate compiled library interface path");
    interfacePaths.push_back(std::move(path));
  }
  struct Facade {
    std::filesystem::path path;
    std::string name;
    std::vector<CompiledScalarSignature> signatures;
  };
  std::vector<std::map<std::string, Facade>> groups(bundles.size());
  std::vector<std::string> rootNames;
  for (std::size_t i = 0; i < bundles.size(); ++i) {
    const auto &selection = bundles[i].selected;
    const bool named = selection && !selection->moduleName.empty();
    const auto &path = interfacePaths[i];
    auto &modules = groups[i];
    for (auto &signature : admitted[i].signatures)
      modules[decodedKey(signature).owner.module].signatures.push_back(
          std::move(signature));
    const auto rootName = named ? selection->moduleName
                                : "compiled_scalar_" + admitted[i].library;
    rootNames.push_back(rootName);
    for (auto &[name, group] : modules) {
      if ((named && name == rootName) || (!named && modules.size() == 1)) {
        group.path = path;
        group.name = rootName;
      } else {
        const auto key = hash(admitted[i].library + "/" + name);
        group.path = path.parent_path() / (".gti-interface-" + key + ".gti");
        group.name = "compiled_scalar_" + key;
        bool sourceModule = false;
        if (named) {
          auto *owner = owners.ownerForPath(path);
          if (owner) {
            for (const auto &module : owner->modules)
              if (module.first == name) {
                group.path = module.second;
                group.name = name;
                sourceModule = true;
                break;
              }
          }
        }
        if ((!sourceModule && std::filesystem::exists(group.path)) ||
            !distinctPaths.insert(group.path.string()).second)
          throw std::invalid_argument(
              "conflicting compiled module interface path");
      }
    }
  }
  const auto importPath = [&](const std::filesystem::path &from,
                              const std::filesystem::path &to) {
    const auto *sourceOwner = owners.ownerForPath(from);
    const auto *targetOwner = owners.ownerForPath(to);
    if (sourceOwner == targetOwner)
      return quotedInterfacePath(to.lexically_relative(from.parent_path()));
    if (sourceOwner && targetOwner) {
      for (const auto &edge : sourceOwner->dependencies)
        if (edge.targetIdentity == targetOwner->identity)
          for (const auto &module : targetOwner->modules)
            if (module.second == to)
              return edge.alias + "." + module.first;
    }
    throw std::invalid_argument(
        "compiled type import has no declared package/module path");
  };
  std::vector<LibraryFacade> facades;
  std::vector<LibraryBundleMember> objects;
  std::vector<LibraryBundleDependency> direct;
  std::vector<std::string> directSymbols;
  for (std::size_t index = 0; index < bundles.size(); ++index) {
    auto &input = bundles[index];
    auto &bundle = input.bundle;
    auto &metadata = admitted[index];
    std::vector<LibraryBundleDependency> expectedDependencies;
    auto allowedSymbols = system.runtimeSymbols;
    for (const auto &dependency : bundle.dependencies) {
      const auto selected = libraries.at(dependency.library);
      expectedDependencies.push_back(
          {admitted[selected].library, bundles[selected].digest});
      allowedSymbols.insert(allowedSymbols.end(),
                            admitted[selected].symbols.begin(),
                            admitted[selected].symbols.end());
    }
    auto expected = system.identity;
    expected.library = metadata.library;
    expected.sources = bundle.identity.sources;
    requireLibraryBundleCompatibility(bundle, expected, expectedDependencies);
    if (bundle.objects.size() != 1 || bundle.objects.front().name != "unit.o")
      throw std::invalid_argument(
          "unsupported compiled scalar object inventory");
    verifyCompiledScalarObject(bundle.objects.front().bytes, request.target(),
                               metadata.symbols, allowedSymbols);
    objects.push_back(std::move(bundle.objects.front()));
    if (!input.selected || input.selected->directDependency) {
      direct.push_back({metadata.library, input.digest});
      directSymbols.insert(directSymbols.end(), metadata.symbols.begin(),
                           metadata.symbols.end());
    }
    auto &modules = groups[index];
    const auto &root = interfacePaths[index];
    std::string forwards;
    for (const auto &[name, group] : modules)
      if (group.path != root)
        forwards += "export import " + importPath(root, group.path) + ";\n";
    bool hasRoot = false;
    for (auto &[name, group] : modules) {
      std::string source = "export module " + group.name + ";\n";
      if (group.path == root) {
        hasRoot = true;
        source += forwards;
      }
      std::set<std::filesystem::path> typePaths;
      for (const auto &signature : group.signatures)
        for (const auto &owner :
             compiledInterfaceDependencies(signature.typeMetadata)) {
          std::optional<std::filesystem::path> defining;
          const auto find = [&](std::size_t candidate) {
            if (admitted[candidate].owner.package != owner.package ||
                admitted[candidate].owner.target != owner.target)
              return;
            const auto found = groups[candidate].find(owner.module);
            if (found != groups[candidate].end())
              defining = found->second.path;
          };
          find(index);
          for (const auto candidate : dependencyClosure[index])
            find(candidate);
          if (!defining)
            throw std::invalid_argument("nominal interface owner is absent "
                                        "from the compiled dependency closure");
          typePaths.insert(*defining);
        }
      for (const auto &dependency : typePaths)
        source += "import " + importPath(group.path, dependency) + ";\n";
      source += compiledInterfaceSource(group.signatures);
      facades.push_back({group.path.string(), std::move(source),
                         std::move(group.signatures)});
      for (auto &owner : sources)
        if (registered(group.path, name) &&
            owners.ownerForPath(group.path)->identity == owner.identity)
          std::erase_if(owner.moduleImplementations, [&](const auto &member) {
            return member.first == name;
          });
    }
    if (!hasRoot)
      facades.push_back({root.string(),
                         "export module " + rootNames[index] + ";\n" + forwards,
                         {}});
  }
  return {std::move(sources),       std::move(facades),
          std::move(objects),       std::move(direct),
          std::move(directSymbols), std::move(optimizationBodies)};
}
CompiledLibraryInputs prepareScalarConsumer(
    const CompilationRequest &request, std::vector<ScalarBundleInput> bundles,
    const std::filesystem::path &runtime, ProgressObserver *progress,
    StandardLibraryInput standardLibrary) {
  auto admitted = admitScalarLibraries(
      request, bundles,
      admissionEnvironment(request, runtime, LibraryProfile::Project));
  std::unordered_map<std::string, std::string> overlays;
  for (auto &facade : admitted.facades)
    if (!overlays.emplace(facade.path, std::move(facade.source)).second)
      throw std::invalid_argument("duplicate compiled module interface path");
  auto effective =
      request.withPackageSources(std::move(admitted.packageSources));
  // A library producer owns its implementation dependencies. A final consumer
  // can link the installed standard library alongside its project libraries.
  auto loaded = standardLibrary == StandardLibraryInput::Installed
                    ? loadExecutableInputs(effective, runtime, progress,
                                           std::move(overlays))
                    : CompiledLibraryInputs{
                          loadCompilationInputs(effective, progress, overlays)};
  CompiledLibraryInputs inputs{std::move(loaded.compilation)};
  std::move(admitted).bind(inputs);
  std::ranges::move(loaded.imports, std::back_inserter(inputs.imports));
  std::ranges::move(loaded.objects, std::back_inserter(inputs.objects));
  // A private module may also be imported directly by the consumer. One
  // live definition must own its storage in both the cold and warm build.
  const auto &consumerGraph = inputs.compilation.sourceGraph;
  for (const auto &bundle : bundles) {
    if (!bundle.selected)
      continue;
    const auto &selection = *bundle.selected;
    for (const auto &source : selection.sourceFiles) {
      const auto *unit = consumerGraph.findUnit(
          consumerGraph.sourceUnitForPath(source.string()));
      if (unit && !unit->compiledScalarInterface) {
        const auto &owners = consumerGraph.sourceOwners();
        const auto *owner =
            owners ? owners->ownerForPath(selection.interfacePath) : nullptr;
        throw CompiledLibrarySourceConflict(
            source, owner && selection.automatic
                        ? owner->packageRoot / "gti.toml"
                        : std::filesystem::path{});
      }
    }
  }
  return inputs;
}
CompiledLibraryInputs
prepareScalarConsumer(const CompilationRequest &request,
                      std::span<const CompiledLibraryImport> imports,
                      const std::filesystem::path &runtime,
                      ProgressObserver *progress,
                      StandardLibraryInput standardLibrary) {
  if (imports.empty() || imports.size() > 64)
    throw std::invalid_argument("compiled consumer requires 1 to 64 libraries");
  std::size_t total = 0;
  std::vector<ScalarBundleInput> bundles;
  for (const auto &selected : imports) {
    auto snapshot = selected.snapshot;
    if (!snapshot) {
      std::error_code error;
      const auto size = std::filesystem::file_size(selected.artifact, error);
      if (error)
        throw std::invalid_argument("compiled dependency artifact is missing; "
                                    "build its library target first");
      if (size > libraryBundleByteLimit - total)
        throw std::invalid_argument(
            "combined compiled libraries exceed the bundle size limit");
      snapshot =
          std::make_shared<const std::string>(readCompiledLibraryFileBounded(
              selected.artifact, libraryBundleByteLimit - total));
    }
    const auto &bytes = *snapshot;
    // Recheck against the actual read: another process may replace the file
    // between the first stat and the bounded reader's own stat.
    if (bytes.size() > libraryBundleByteLimit - total)
      throw std::invalid_argument(
          "combined compiled libraries exceed the bundle size limit");
    total += bytes.size();
    bundles.push_back({decodeLibraryBundle(bytes), selected.interfacePath,
                       selected, hash(bytes)});
  }
  return prepareScalarConsumer(request, std::move(bundles), runtime, progress,
                               standardLibrary);
}

// This sink checks the same bound lowered contract as executable backends. Its
// empty handoff remains internal; the public query returns only CheckResult.
class ScalarCheckBackend final : public Backend {
public:
  std::string_view name() const override { return "compiled-scalar-check"; }
  BackendArtifact generate(LoweredProgram program) override {
    std::vector<LoweredProgramIssue> issues;
    if (!verifiedLoweredProgram(std::move(program), issues))
      throw std::logic_error(
          "compiled library check requires a verified lowered program");
    return {};
  }
};

CompiledLibraryInputs
loadExecutableInputs(const CompilationRequest &request,
                     const std::filesystem::path &runtime,
                     ProgressObserver *progress,
                     std::unordered_map<std::string, std::string> overlays) {
  const auto source = [&] {
    return CompiledLibraryInputs{
        loadCompilationInputs(request, progress, overlays)};
  };
  const auto &target = request.target();
  if (request.backend() != BackendKind::Llvm || request.debugInfo() ||
      request.llvmInstrumentation() != LlvmInstrumentationMode::None ||
      request.program().kind != ProgramKind::Executable ||
      request.program().testSuite ||
      request.rawPointerAudit() != RawPointerAuditMode::Off ||
      request.optimizationOutputs().requested() ||
      target.executionProfile != ExecutionProfile::SingleThreaded ||
      !target.runtimeCapabilities.threads)
    return source();
  const char *profile;
  if (request.optimization() == OptimizationLevel::O0)
    profile = "dev";
  else if (request.optimization() == OptimizationLevel::O3)
    profile = "release";
  else
    return source();

  const auto root =
      std::filesystem::weakly_canonical(request.standardLibrary().root);
  const auto directory = root.parent_path() / "precompiled-stdlib" /
                         targetTriple(target) / profile;
  if (!std::filesystem::is_directory(directory))
    return source();

  std::optional<AdmissionEnvironment> system;
  struct LoadedModule {
    std::string digest;
    AdmittedLibraries admitted;
    std::filesystem::path entry;
    std::shared_ptr<const CompiledTemplateModule> templateModule;
  };
  std::map<std::string, LoadedModule> loaded;
  std::size_t totalBytes = 0;
  std::size_t declarations = 0;
  SourceLoader loader;
  // Recipe modules expose their own declaration and import skeleton at their
  // public path. The ordinary source loader still resolves its dependencies.
  for (const auto &module : installedModules) {
    const std::string name(module.target);
    if (module.facade.empty()) {
      const auto entry = root / module.entry;
      if (overlays.contains(entry.string()))
        throw std::invalid_argument(
            "project interface overlaps the installed standard library");
      loader.deferInstalledModule(entry.string(), [&, name, entry] {
        const auto artifact = directory / (name + ".gtilib");
        const auto bytes = readCompiledLibraryFileBounded(
            artifact, libraryBundleByteLimit - totalBytes);
        totalBytes += bytes.size();
        const auto templateDigest = hash(bytes);
        // This is executable compiler input belonging to the installed sysroot.
        // Require the installer's independent receipt before reading
        // declarations, including imports and trusted standard-library
        // operations.
        if (readCompiledLibraryFileBounded(artifact.string() + ".sha256", 65) !=
            templateDigest + "\n")
          throw std::invalid_argument(
              "installed generic library receipt mismatch");
        const auto bundle = decodeLibraryBundle(bytes);
        if (!bundle.templateOnly)
          throw std::invalid_argument("expected generic library envelope");
        if (!system)
          system = admissionEnvironment(request, runtime,
                                        LibraryProfile::InstalledStdlib);
        auto expected = system->identity;
        expected.library = templateLibraryIdentity({"gti-std", name, {}});
        expected.sources = bundle.identity.sources;
        requireLibraryBundleCompatibility(bundle, expected, {});
        auto templateModule = std::make_shared<const CompiledTemplateModule>(
            decodeTemplateModule(bundle.interfaceMetadata));
        loaded.emplace(name,
                       LoadedModule{templateDigest, {}, entry, templateModule});
        return templateModule->declarations + "\n// compiled template " +
               templateDigest + "\n";
      });
      continue;
    }
    const auto path = root / (".gti-compiled-" + name + ".gti");
    if (overlays.contains(path.string()) ||
        !overlays.emplace((root / module.entry).string(), module.facade).second)
      throw std::invalid_argument(
          "project interface overlaps the installed standard library");
    // Only the actual import graph triggers artifact reads. Share one identity
    // and runtime symbol inventory across every module reached in this build.
    loader.deferSource(path.string(), [&, name, path] {
      if (!system)
        system = admissionEnvironment(request, runtime,
                                      LibraryProfile::InstalledStdlib);
      const CompiledLibraryImport selection{
          "gti-std", name, directory / (name + ".gtilib"), path};
      auto bytes = readCompiledLibraryFileBounded(
          selection.artifact, libraryBundleByteLimit - totalBytes);
      totalBytes += bytes.size();
      auto digest = hash(bytes);
      std::vector<ScalarBundleInput> bundles;
      bundles.push_back({decodeLibraryBundle(bytes), path, selection, digest});
      auto admitted = admitScalarLibraries(request, bundles, *system);
      if (admitted.facades.size() != 1 ||
          admitted.facades.front().path != path.string())
        throw std::invalid_argument("invalid installed " + name +
                                    " module interface");
      auto &facade = admitted.facades.front();
      declarations += facade.signatures.size();
      if (declarations > 256)
        throw std::invalid_argument(
            "combined compiled interface exceeds 256 declarations");
      auto interface = std::move(facade.source);
      loaded.emplace(name,
                     LoadedModule{std::move(digest), std::move(admitted)});
      return interface;
    });
  }
  CompiledLibraryInputs inputs{
      loadCompilationInputs(request, loader, progress, overlays)};
  if (!loaded.empty()) {
    std::string receipts;
    for (auto &[name, module] : loaded) {
      field(receipts, name);
      field(receipts, module.digest);
      if (module.templateModule) {
        auto &graph = inputs.compilation.sourceGraph;
        graph.attachCompiledTemplate(
            graph.sourceUnitForPath(module.entry.string()),
            std::move(module.templateModule));
      } else {
        std::move(module.admitted).bind(inputs);
      }
    }
    inputs.compilation.standardLibraryBundleDigest = hash(receipts);
  }
  return inputs;
}

} // namespace

CompiledLibraryInputs
loadStandardLibraryInputs(const CompilationRequest &request,
                          const std::filesystem::path &runtime,
                          ProgressObserver *progress) {
  return loadExecutableInputs(request, runtime, progress, {});
}

CompilationResult compileWithLibraryImports(
    const CompilationRequest &request, CompilationInputs inputs,
    std::vector<CompiledScalarImport> imports, ProgressObserver *progress,
    LlvmCodegenUnitCache *unitCache) {
  auto options = llvmBackendOptions(request);
  options.unitCache = unitCache;
  LlvmBackend native(options);
  ScalarBackend backend(std::move(imports), native);
  return compileWithBackend(request.withDefinitionClosure(), std::move(inputs),
                            backend, progress);
}

CheckResult compiledScalarCheckResult(CompilationResult result) {
  CheckResult check;
  check.status = result.succeeded() ? CheckStatus::Success
                 : result.status == CompilationStatus::FrontendFailure
                     ? CheckStatus::FrontendFailure
                     : CheckStatus::ValidationFailure;
  check.sources = std::move(result.sources);
  check.diagnostics = std::move(result.diagnostics);
  if (result.status == CompilationStatus::MirVerificationFailure) {
    check.driverDiagnostic =
        "gti: internal compiler error: MIR verification failed";
    if (!result.mirErrors.empty())
      *check.driverDiagnostic += ": " + result.mirErrors.front().message;
  }
  if (result.backendFailureDetails)
    check.driverDiagnostic =
        "gti: compiled library check: " + *result.backendFailureDetails;
  return check;
}

CheckResult checkWithStandardLibraries(const CompilationRequest &request,
                                       const std::filesystem::path &runtime,
                                       ProgressObserver *progress) {
  try {
    auto inputs = loadStandardLibraryInputs(request, runtime, progress);
    return checkCompilation(request, std::move(inputs.compilation), progress);
  } catch (const std::exception &error) {
    CheckResult result;
    result.status = CheckStatus::ArtifactFailure;
    result.driverDiagnostic =
        "gti: compiled standard library: " + std::string(error.what());
    return result;
  }
}

CompiledScalarConsumerResult compileCompiledScalarConsumer(
    const CompilationRequest &request, std::string_view bytes,
    const std::filesystem::path &path, const std::filesystem::path &runtime,
    const CompiledLibraryImport *selected) {
  std::vector<ScalarBundleInput> bundles;
  bundles.push_back({decodeLibraryBundle(bytes), path,
                     selected ? std::optional(*selected) : std::nullopt,
                     hash(bytes)});
  auto prepared =
      prepareScalarConsumer(request, std::move(bundles), runtime, nullptr,
                            StandardLibraryInput::Installed);
  LlvmBackend native(llvmBackendOptions(request));
  ScalarBackend backend(std::move(prepared.imports), native);
  CompiledScalarConsumerResult result;
  result.compilation = compileWithBackend(
      request.forExecutable(), std::move(prepared.compilation), backend);
  if (result.compilation.succeeded())
    result.objects = std::move(prepared.objects);
  return result;
}

CompiledScalarConsumerResult
compileCompiledScalarConsumer(const CompilationRequest &request,
                              std::span<const CompiledLibraryImport> imports,
                              const std::filesystem::path &runtime) {
  auto prepared = prepareScalarConsumer(request, imports, runtime, nullptr,
                                        StandardLibraryInput::Installed);
  LlvmBackend native(llvmBackendOptions(request));
  ScalarBackend backend(std::move(prepared.imports), native);
  CompiledScalarConsumerResult result;
  result.compilation = compileWithBackend(
      request.forExecutable(), std::move(prepared.compilation), backend);
  if (result.compilation.succeeded())
    result.objects = std::move(prepared.objects);
  return result;
}

CompiledLibrarySnapshot reusableCompiledScalarLibrarySnapshot(
    const CompilationRequest &request, const CompiledLibraryTarget &target,
    std::span<const CompiledLibraryImport> imports,
    const std::filesystem::path &runtime, const std::filesystem::path &artifact,
    std::vector<std::filesystem::path> *sourceFiles) {
  try {
    auto bytes =
        std::make_shared<const std::string>(readCompiledLibraryFile(artifact));
    auto bundle = decodeLibraryBundle(*bytes);
    if (bundle.templateOnly) {
      if (!imports.empty() || !target.functions.empty())
        return {};
      auto inputs = loadCompilationInputs(request);
      if (!inputs.succeeded() || producerProfile(inputs.sourceGraph, &target) !=
                                     LibraryProfile::InstalledStdlib)
        return {};
      auto expected =
          environment(request, runtime, LibraryProfile::InstalledStdlib);
      expected.library = templateLibraryIdentity(target);
      expected.sources = producerSources(inputs, &target);
      requireLibraryBundleCompatibility(bundle, expected, {});
      (void)decodeTemplateModule(bundle.interfaceMetadata);
      if (sourceFiles)
        *sourceFiles = producerImplementations(inputs.sourceGraph);
      return bytes;
    }
    auto signatures = readInterface(bundle.interfaceMetadata);
    (void)checkedOptimizationBodies(bundle, signatures);
    const auto owner = decodedKey(signatures.front()).owner;
    if (owner.package != target.package || owner.target != target.target)
      return {};
    auto prepared = imports.empty()
                        ? CompiledLibraryInputs{loadCompilationInputs(request)}
                        : prepareScalarConsumer(request, imports, runtime);
    if (!prepared.compilation.succeeded())
      return {};
    auto expected =
        environment(request, runtime,
                    producerProfile(prepared.compilation.sourceGraph, &target));
    auto allowedImports =
        runtimeImports(runtime, request.target(), expected.runtime);
    allowedImports.insert(allowedImports.end(), prepared.symbols.begin(),
                          prepared.symbols.end());
    expected.library = libraryIdentity(signatures);
    expected.sources = producerSources(prepared.compilation, &target);
    requireLibraryBundleCompatibility(bundle, expected, prepared.dependencies);
    if (bundle.objects.size() != 1 || bundle.objects.front().name != "unit.o")
      return {};
    std::vector<std::string> symbols;
    for (const auto &signature : signatures)
      symbols.push_back(compiledScalarSymbol(signature));
    verifyCompiledScalarObject(bundle.objects.front().bytes, request.target(),
                               symbols, allowedImports);
    if (sourceFiles)
      *sourceFiles = producerImplementations(prepared.compilation.sourceGraph);
    return bytes;
  } catch (const std::exception &) {
    return {};
  }
}

bool reusableCompiledScalarLibraryTarget(
    const CompilationRequest &request, const CompiledLibraryTarget &target,
    std::span<const CompiledLibraryImport> imports,
    const std::filesystem::path &runtime,
    const std::filesystem::path &artifact) {
  return bool(reusableCompiledScalarLibrarySnapshot(request, target, imports,
                                                    runtime, artifact));
}

CheckResult checkCompiledScalarProject(const ProjectBuildPlan &plan,
                                       const ToolchainLayout &toolchain,
                                       ProgressObserver *progress) {
  try {
    const auto producer = plan.compiledLibrary();
    const auto &consumers = plan.compiledImports();
    if (!producer && consumers.empty())
      throw std::invalid_argument(
          "checking requires exactly one compiled library producer or import");
    if (!plan.bindgenImports().empty())
      throw std::invalid_argument("compiled-library checking does not run "
                                  "bindgen imports in this profile");
    if (plan.keepCpp())
      throw std::invalid_argument(
          "compiled-library checking does not support retained C++");
    if (producer) {
      if (!plan.buildScripts().empty())
        throw std::invalid_argument("compiled library producers do not support "
                                    "native build scripts in this profile");
      if (const auto diagnostic =
              compiledScalarProducerInputDiagnostic(plan.nativeInputs()))
        throw std::invalid_argument(*diagnostic);
    }
    const auto request =
        CompilationRequest(
            plan.entry(), toolchain.standardLibrary, plan.target(),
            plan.optimization(), plan.cppStandard(), plan.packageSources(),
            plan.configurationFlags(), {}, plan.warnings(), plan.debugInfo(),
            plan.packageWarnings(), plan.rawPointerAudit(),
            plan.programRequest(), plan.declaredConfigurationFlags(),
            plan.rawPointerAuditLibraryFindings())
            .withBackend(plan.backend())
            .withCompileLimits(plan.compileLimits());
    ScalarCheckBackend sink;
    if (producer) {
      auto prepared =
          consumers.empty()
              ? CompiledLibraryInputs{loadCompilationInputs(request, progress)}
              : prepareScalarConsumer(request, consumers,
                                      toolchain.runtimeLibrary, progress);
      auto &inputs = prepared.compilation;
      const auto profile = producerProfile(inputs.sourceGraph, &*producer);
      (void)environment(request, toolchain.runtimeLibrary, profile);
      const auto producerRequest = request.withApplicationBodies();
      auto frontend = detail::analyzeForArtifact(producerRequest,
                                                 std::move(inputs), progress);
      auto selection = producerSelection(frontend, &*producer, profile);
      ScalarBackend backend({}, frontend.sourceGraph.entryUnit(), sink,
                            std::move(selection), std::move(prepared.imports));
      return compiledScalarCheckResult(detail::generateArtifact(
          producerRequest, std::move(frontend), backend, progress));
    }
    auto prepared =
        prepareScalarConsumer(request, consumers, toolchain.runtimeLibrary,
                              progress, StandardLibraryInput::Installed);
    // Object admission is complete. Checking never links these bytes, so do
    // not keep them alive while analyzing the consumer.
    prepared.objects.clear();
    ScalarBackend backend(std::move(prepared.imports), sink);
    return compiledScalarCheckResult(
        compileWithBackend(request.forExecutable(),
                           std::move(prepared.compilation), backend, progress));
  } catch (const std::exception &error) {
    CheckResult result;
    result.status = CheckStatus::ArtifactFailure;
    result.driverDiagnostic =
        "gti: compiled library: " + std::string(error.what());
    return result;
  }
}

} // namespace lang::driver
