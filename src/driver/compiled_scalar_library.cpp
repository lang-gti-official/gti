#include "gti/driver/compiled_scalar_library.h"
#include "compiled_scalar_support.h"
#include "gti/compiled_interface_types.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/driver/project.h"
#include "gti/persistent_declaration.h"
#include "sha256.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <functional>
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
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot read compatibility input");
  Sha256 h;
  char bytes[65536];
  while (file.read(bytes, sizeof(bytes)) || file.gcount())
    h.update(std::string_view(bytes, file.gcount()));
  if (!file.eof())
    throw std::invalid_argument("cannot hash compatibility input");
  return h.finishHex();
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
  std::string out = "gti.callable-interface.3\n", previous;
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
  constexpr std::string_view header = "gti.callable-interface.3\n";
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
    if (candidate != owner)
      throw std::invalid_argument(
          "compiled library keys must belong to one package target module");
  }
  std::string identity = "gti.scalar-library.1";
  field(identity, owner.package);
  field(identity, owner.target);
  return hash(identity);
}
LibraryBundleIdentity environment(const CompilationRequest &request,
                                  const std::filesystem::path &runtime) {
  if (request.backend() != BackendKind::Llvm || request.debugInfo() ||
      request.llvmInstrumentation() != LlvmInstrumentationMode::None ||
      request.program().kind != ProgramKind::Executable ||
      request.program().testSuite)
    throw std::invalid_argument(
        "unsupported compiled scalar build configuration");
  std::string error;
  auto backend = queryLlvmBackendIdentity(request.target(),
                                          llvmBackendOptions(request), error);
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
  field(config, std::to_string(request.configurationFlags().size()));
  for (const auto &flag : request.configurationFlags())
    field(config, flag);
  field(config, std::to_string(request.declaredConfigurationFlags().size()));
  for (const auto &flag : request.declaredConfigurationFlags())
    field(config, flag);
  field(config, std::to_string(static_cast<unsigned>(request.optimization())));
  field(config, request.target().runtimeCapabilities.threads ? "threads"
                                                             : "no-threads");
  id.configuration = hash(config);
  return id;
}
struct ScalarSelection {
  PersistentDeclarationOwner owner;
  SourceUnitId entry = 0;
  std::vector<std::string> functions;
};
class ScalarBackend final : public Backend {
public:
  ScalarBackend(std::vector<CompiledScalarSignature> signatures,
                SourceUnitId root, Backend &backend,
                std::optional<ScalarSelection> selection = {},
                std::vector<CompiledScalarImport> imported = {})
      : signatures(std::move(signatures)), imports(std::move(imported)),
        root(root), producer(true), backend(backend),
        selection(std::move(selection)) {}
  ScalarBackend(std::vector<CompiledScalarImport> imports, Backend &backend)
      : imports(std::move(imports)), backend(backend) {}
  const std::vector<CompiledScalarSignature> &selectedSignatures() const {
    return signatures;
  }
  std::string_view name() const override { return "llvm-compiled-scalar"; }
  BackendArtifact generate(LoweredProgram program) override {
    std::optional<LoweredProgram> bound;
    try {
      if (producer) {
        if (selection)
          signatures = CompiledScalarUnitBuilder().selectExports(
              program, selection->entry, selection->owner, selection->functions,
              imports);
        (void)interface(signatures);
        (void)libraryIdentity(signatures);
        bound = CompiledScalarUnitBuilder().bindCombined(
            std::move(program), root, signatures, imports);
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
  Backend &backend;
  std::optional<ScalarSelection> selection;
};
std::optional<ScalarSelection>
producerSelection(const CompilationInputs &inputs,
                  const CompiledLibraryTarget *target) {
  std::optional<ScalarSelection> selection;
  if (target && inputs.succeeded()) {
    const SourceGraph &graph = inputs.sourceGraph;
    const auto *entry = graph.findUnit(graph.entryUnit());
    if (!entry || !entry->moduleDeclaration)
      throw std::invalid_argument(
          "compiled library root must declare a module interface");
    selection = ScalarSelection{
        {target->package, target->target, entry->moduleDeclaration->lexeme, ""},
        entry->id,
        target->functions};
  }
  return selection;
}

struct ScalarConsumerInputs {
  CompilationInputs compilation;
  std::vector<CompiledScalarImport> imports;
  std::vector<LibraryBundleMember> objects;
  std::vector<LibraryBundleDependency> dependencies;
  std::vector<std::string> symbols;
};
ScalarConsumerInputs prepareScalarConsumer(
    const CompilationRequest &, std::span<const CompiledLibraryImport>,
    const std::filesystem::path &, ProgressObserver *progress = nullptr);
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
  auto id = environment(request, runtime);
  auto prepared = imports.empty()
                      ? ScalarConsumerInputs{loadCompilationInputs(request)}
                      : prepareScalarConsumer(request, imports, runtime);
  auto &inputs = prepared.compilation;
  auto selection = producerSelection(inputs, target);
  id.sources = producerSources(inputs, target);
  LlvmBackend native(llvmBackendOptions(request));
  ScalarBackend backend(std::move(signatures), inputs.sourceGraph.entryUnit(),
                        native, std::move(selection),
                        std::move(prepared.imports));
  CompiledScalarLibraryResult result;
  result.compilation = compileWithBackend(request.withApplicationBodies(),
                                          std::move(inputs), backend);
  if (result.compilation.succeeded()) {
    auto metadata = interface(backend.selectedSignatures());
    id.library = libraryIdentity(backend.selectedSignatures());
    std::vector<std::string> symbols;
    for (const auto &key : backend.selectedSignatures())
      symbols.push_back(compiledScalarSymbol(key));
    verifyCompiledScalarObject(result.compilation.artifact->contents,
                               request.target(), symbols, prepared.symbols);
    // Publication must still describe the input snapshot that produced code.
    auto current = imports.empty()
                       ? ScalarConsumerInputs{loadCompilationInputs(request)}
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
         {{"unit.o", result.compilation.artifact->contents}},
         metadata});
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
ScalarConsumerInputs
prepareScalarConsumer(const CompilationRequest &request,
                      std::vector<ScalarBundleInput> bundles,
                      const std::filesystem::path &runtime,
                      ProgressObserver *progress = nullptr) {
  if (bundles.empty() || bundles.size() > 64)
    throw std::invalid_argument("compiled consumer requires 1 to 64 libraries");
  const auto environmentIdentity = environment(request, runtime);
  std::unordered_map<std::string, std::string> overlays;
  struct Admission {
    std::vector<CompiledScalarSignature> signatures;
    std::vector<std::string> symbols;
    std::string library;
    PersistentDeclarationOwner owner;
  };
  std::vector<Admission> admitted;
  std::map<std::string, std::size_t> libraries;
  std::size_t declarationCount = 0;
  for (auto &input : bundles) {
    auto signatures = readInterface(input.bundle.interfaceMetadata);
    declarationCount += signatures.size();
    if (declarationCount > 256)
      throw std::invalid_argument(
          "combined compiled interface exceeds 256 declarations");
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
  std::vector<std::filesystem::path> interfacePaths;
  std::set<std::string> distinctPaths;
  for (std::size_t i = 0; i < bundles.size(); ++i) {
    const auto &input = bundles[i];
    auto path = input.selected && !input.selected->interfaceRequired
                    ? std::filesystem::absolute(request.entry()).parent_path() /
                          (".gti-compiled-" + admitted[i].library + ".gti")
                    : input.path;
    if (!path.is_absolute() || path.extension() != ".gti" ||
        std::filesystem::exists(path) || path.lexically_normal() != path)
      throw std::invalid_argument("compiled interface requires an absent "
                                  "normalized absolute .gti path");
    path = std::filesystem::weakly_canonical(path);
    if (!distinctPaths.insert(path.string()).second)
      throw std::invalid_argument("duplicate compiled library interface path");
    interfacePaths.push_back(std::move(path));
  }
  struct Facade {
    std::string path;
    std::vector<CompiledScalarSignature> signatures;
  };
  std::vector<Facade> facades;
  std::vector<LibraryBundleMember> objects;
  std::vector<LibraryBundleDependency> direct;
  std::vector<std::string> directSymbols;
  for (std::size_t index = 0; index < bundles.size(); ++index) {
    auto &input = bundles[index];
    auto &bundle = input.bundle;
    auto &metadata = admitted[index];
    std::vector<LibraryBundleDependency> expectedDependencies;
    std::vector<std::string> allowedSymbols;
    for (const auto &dependency : bundle.dependencies) {
      const auto selected = libraries.at(dependency.library);
      expectedDependencies.push_back(
          {admitted[selected].library, bundles[selected].digest});
      allowedSymbols.insert(allowedSymbols.end(),
                            admitted[selected].symbols.begin(),
                            admitted[selected].symbols.end());
    }
    auto expected = environmentIdentity;
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
    const auto canonical = interfacePaths[index].string();
    std::string source =
        "export module compiled_scalar_" + metadata.library + ";\n";
    std::set<std::size_t> typeDependencies;
    for (const auto &signature : metadata.signatures)
      for (const auto &owner :
           compiledInterfaceDependencies(signature.typeMetadata)) {
        std::optional<std::size_t> defining;
        for (const auto candidate : dependencyClosure[index])
          if (admitted[candidate].owner == owner)
            defining = candidate;
        if (!defining)
          throw std::invalid_argument("nominal interface owner is absent from "
                                      "the compiled dependency closure");
        typeDependencies.insert(*defining);
      }
    for (const auto dependency : typeDependencies)
      source +=
          "import " +
          quotedInterfacePath(interfacePaths[dependency].lexically_relative(
              interfacePaths[index].parent_path())) +
          ";\n";
    source += compiledInterfaceSource(metadata.signatures);
    if (!overlays.emplace(canonical, std::move(source)).second)
      throw std::invalid_argument("duplicate compiled library interface path");
    facades.push_back({canonical, std::move(metadata.signatures)});
  }
  auto inputs = loadCompilationInputs(request, progress, overlays);
  std::vector<CompiledScalarImport> imports;
  for (auto &facade : facades) {
    const auto unit = inputs.sourceGraph.sourceUnitForPath(facade.path);
    if (!unit)
      continue;
    inputs.sourceGraph.markCompiledScalarInterface(unit);
    for (auto &signature : facade.signatures)
      imports.push_back({std::move(signature), unit});
  }
  return {std::move(inputs), std::move(imports), std::move(objects),
          std::move(direct), std::move(directSymbols)};
}
ScalarConsumerInputs
prepareScalarConsumer(const CompilationRequest &request,
                      std::span<const CompiledLibraryImport> imports,
                      const std::filesystem::path &runtime,
                      ProgressObserver *progress) {
  if (imports.empty() || imports.size() > 64)
    throw std::invalid_argument("compiled consumer requires 1 to 64 libraries");
  std::size_t total = 0;
  std::vector<ScalarBundleInput> bundles;
  for (const auto &selected : imports) {
    std::error_code error;
    const auto size = std::filesystem::file_size(selected.artifact, error);
    if (error)
      throw std::invalid_argument("compiled dependency artifact is missing; "
                                  "build its library target first");
    if (size > libraryBundleByteLimit - total)
      throw std::invalid_argument(
          "combined compiled libraries exceed the bundle size limit");
    const auto bytes = readCompiledLibraryFileBounded(
        selected.artifact, libraryBundleByteLimit - total);
    // Recheck against the actual read: another process may replace the file
    // between the first stat and the bounded reader's own stat.
    if (bytes.size() > libraryBundleByteLimit - total)
      throw std::invalid_argument(
          "combined compiled libraries exceed the bundle size limit");
    total += bytes.size();
    bundles.push_back({decodeLibraryBundle(bytes), selected.interfacePath,
                       selected, hash(bytes)});
  }
  return prepareScalarConsumer(request, std::move(bundles), runtime, progress);
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

} // namespace

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

CompiledScalarConsumerResult compileCompiledScalarConsumer(
    const CompilationRequest &request, std::string_view bytes,
    const std::filesystem::path &path, const std::filesystem::path &runtime,
    const CompiledLibraryImport *selected) {
  std::vector<ScalarBundleInput> bundles;
  bundles.push_back({decodeLibraryBundle(bytes), path,
                     selected ? std::optional(*selected) : std::nullopt,
                     hash(bytes)});
  auto prepared = prepareScalarConsumer(request, std::move(bundles), runtime);
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
  auto prepared = prepareScalarConsumer(request, imports, runtime);
  LlvmBackend native(llvmBackendOptions(request));
  ScalarBackend backend(std::move(prepared.imports), native);
  CompiledScalarConsumerResult result;
  result.compilation = compileWithBackend(
      request.forExecutable(), std::move(prepared.compilation), backend);
  if (result.compilation.succeeded())
    result.objects = std::move(prepared.objects);
  return result;
}

bool reusableCompiledScalarLibraryTarget(
    const CompilationRequest &request, const CompiledLibraryTarget &target,
    std::span<const CompiledLibraryImport> imports,
    const std::filesystem::path &runtime,
    const std::filesystem::path &artifact) {
  try {
    auto bundle = decodeLibraryBundle(readCompiledLibraryFile(artifact));
    auto signatures = readInterface(bundle.interfaceMetadata);
    const auto owner = decodedKey(signatures.front()).owner;
    if (owner.package != target.package || owner.target != target.target)
      return false;
    auto prepared = imports.empty()
                        ? ScalarConsumerInputs{loadCompilationInputs(request)}
                        : prepareScalarConsumer(request, imports, runtime);
    if (!prepared.compilation.succeeded())
      return false;
    auto expected = environment(request, runtime);
    expected.library = libraryIdentity(signatures);
    expected.sources = producerSources(prepared.compilation, &target);
    requireLibraryBundleCompatibility(bundle, expected, prepared.dependencies);
    if (bundle.objects.size() != 1 || bundle.objects.front().name != "unit.o")
      return false;
    std::vector<std::string> symbols;
    for (const auto &signature : signatures)
      symbols.push_back(compiledScalarSymbol(signature));
    verifyCompiledScalarObject(bundle.objects.front().bytes, request.target(),
                               symbols, prepared.symbols);
    return true;
  } catch (const std::exception &) {
    return false;
  }
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
            .withBackend(plan.backend());
    ScalarCheckBackend sink;
    if (producer) {
      (void)environment(request, toolchain.runtimeLibrary);
      auto prepared =
          consumers.empty()
              ? ScalarConsumerInputs{loadCompilationInputs(request, progress)}
              : prepareScalarConsumer(request, consumers,
                                      toolchain.runtimeLibrary, progress);
      auto &inputs = prepared.compilation;
      auto selection = producerSelection(inputs, &*producer);
      ScalarBackend backend({}, inputs.sourceGraph.entryUnit(), sink,
                            std::move(selection), std::move(prepared.imports));
      return compiledScalarCheckResult(
          compileWithBackend(request.withApplicationBodies(), std::move(inputs),
                             backend, progress));
    }
    auto prepared = prepareScalarConsumer(request, consumers,
                                          toolchain.runtimeLibrary, progress);
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
