#include "../src/driver/compiled_scalar_support.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/driver/compiled_scalar_library.h"
#include "gti/driver/process.h"
#include "gti/driver/project.h"
#include "gti/lowered_program_printer.h"
#include "gti/persistent_declaration.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace lang;
using namespace lang::driver;
namespace lang {
struct LoweredProgramTestAccess {
  static auto &bindings(LoweredProgram &p) { return p.compiledScalarBindings_; }
  static auto &units(LoweredProgram &p) { return p.compiledInterfaceUnits_; }
};
} // namespace lang
namespace {
void expect(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void rejects(const std::function<void()> &action, const std::string &why) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  } catch (const LibraryBundleError &) {
    return;
  }
  throw std::runtime_error("accepted " + why);
}
void write(const std::filesystem::path &path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), text.size());
  expect(bool(out), "write failed: " + path.string());
}
std::string read(const std::filesystem::path &path) {
  expect(std::filesystem::file_size(path) <= libraryBundleByteLimit,
         "oversized test input");
  std::ifstream in(path, std::ios::binary);
  expect(bool(in), "read failed: " + path.string());
  return std::string(std::istreambuf_iterator<char>(in), {});
}
void compiled(const CompilationResult &result) {
  if (result.succeeded())
    return;
  for (const auto &d : result.diagnostics)
    std::cerr << d.message << '\n';
  throw std::runtime_error(
      result.backendFailureDetails.value_or("compilation failed"));
}
struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::weakly_canonical(
          std::filesystem::temp_directory_path()) /
      ("gti-separate-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code e;
    std::filesystem::remove_all(path, e);
  }
};
std::vector<std::string> keys() {
  FunctionInfo f;
  f.qualifiedName = "choose";
  f.parameterTypes = {SemanticType::Int32};
  return {
      persistentFunctionKey(f, {"example/numbers", "numbers", "numbers", ""})};
}
CompilationRequest request(const std::filesystem::path &root,
                           const std::filesystem::path &source,
                           OptimizationLevel level) {
  return CompilationRequest(source, standardLibraryLayout(root / "stdlib"),
                            TargetInfo::host(), level, CppStandard::Cpp23)
      .withBackend(BackendKind::Llvm);
}
ProcessResult process(const std::vector<std::string> &args) {
  ProcessInvocationOptions options;
  options.timeout = std::chrono::seconds(60);
  options.captureSuccessfulOutput = true;
  return invokeProcess(args, options);
}
void linkAndRun(const std::filesystem::path &root,
                const std::filesystem::path &runtime,
                const CompiledScalarConsumerResult &client) {
  compiled(client.compilation);
  expect(client.objects.size() == 1, "missing library object");
  for (const auto &name : client.compilation.sources.names())
    expect(name.find("/producer/") == std::string::npos,
           "consumer loaded implementation");
  write(root / "consumer.o", client.compilation.artifact->contents);
  write(root / "library.o", client.objects.front().bytes);
  std::vector<std::string> args{"c++", (root / "consumer.o").string(),
                                (root / "library.o").string(), runtime.string(),
                                "-pthread"};
#if defined(__linux__)
  args.push_back("-ldl");
#endif
  args.insert(args.end(), {"-o", (root / "app").string()});
  const auto linked = process(args);
  expect(linked.succeeded(), "link failed: " + linked.output);
  const auto ran = process({(root / "app").string()});
  expect(ran.succeeded(), "execution failed: " + ran.output);
}
class Capture final : public Backend {
public:
  std::optional<LoweredProgram> program;
  std::string_view name() const override { return "capture-scalar-unit"; }
  BackendArtifact generate(LoweredProgram p) override {
    program = std::move(p);
    return {BackendArtifactKind::Object, "captured", ".o", {}};
  }
};
void projectCheckTests(const std::filesystem::path &root,
                       const std::filesystem::path &runtime) {
  // Reach the error adapter without relying on a frontend bug to manufacture
  // invalid MIR. Internal verification failures must retain their explanation.
  CompilationResult invalidMir;
  invalidMir.status = CompilationStatus::MirVerificationFailure;
  invalidMir.mirErrors.push_back(
      {.message = "test verifier failure: missing block"});
  const auto invalidCheck = compiledScalarCheckResult(std::move(invalidMir));
  expect(
      invalidCheck.status == CheckStatus::ValidationFailure &&
          invalidCheck.driverDiagnostic &&
          invalidCheck.driverDiagnostic->find(
              "test verifier failure: missing block") != std::string::npos,
      "artifact checking must not discard the MIR verifier's failure details");
  write(root / "stdlib/prelude.gti", "");
  const auto source = root / "library.gti";
  const std::string valid =
      "export module numbers; export int choose(int x) { return x; }\n";
  write(source, valid);
  write(root / "gti.toml",
        "manifest-version = 1\n[package]\nname = \"numbers\"\nversion = "
        "\"0.1.0\"\n"
        "[targets.numbers]\nkind = \"gti-library\"\nroot = \"library.gti\"\n"
        "functions = [\"choose\"]\n");
  auto project = resolveProjectBuild(
      ProjectBuildRequest(root, "numbers", "release", TargetInfo::host()));
  expect(project.succeeded(), "check project failed to resolve");
  ToolchainLayout toolchain{.standardLibrary =
                                standardLibraryLayout(root / "stdlib"),
                            .runtimeLibrary = runtime};
  auto result = checkCompiledScalarProject(*project.plan, toolchain);
  expect(result.succeeded() && !result.driverDiagnostic &&
             !std::filesystem::exists(root / "build"),
         "successful library query must not publish native artifacts");
  write(
      source,
      "export module numbers; export int choose(int x) { return unknown; }\n");
  result = checkCompiledScalarProject(*project.plan, toolchain);
  expect(result.status == CheckStatus::FrontendFailure &&
             !result.diagnostics.empty(),
         "artifact check must retain frontend failure classification");
  expect(
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [&](const auto &d) {
                    return d.phase == DiagnosticPhase::Semantics &&
                           d.primary.source == source.string() &&
                           result.sources.find(d.primary.source) != nullptr &&
                           d.fixes.empty();
                  }),
      "artifact check must retain semantic diagnostics and their owned source");
  write(source, "export module numbers; int choose(int x) { return x; }\n");
  result = checkCompiledScalarProject(*project.plan, toolchain);
  expect(result.status == CheckStatus::ValidationFailure &&
             std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-B0003"; }),
         "private export selection must retain the lowered-profile diagnostic");
  write(source, valid);
  toolchain.runtimeLibrary = root / "missing-runtime.a";
  result = checkCompiledScalarProject(*project.plan, toolchain);
  expect(
      result.status == CheckStatus::ArtifactFailure &&
          result.driverDiagnostic && !std::filesystem::exists(root / "build"),
      "compatibility input failure must return a read-only artifact failure");
}
void negativeTests(const std::filesystem::path &root,
                   const std::filesystem::path &runtime,
                   const std::string &bytes) {
  const auto consumer = root / "consumer/main.gti";
  const auto facade = root / "consumer/api.gti";
  auto req = request(root, consumer, OptimizationLevel::O3);
  auto consume = [&](std::string_view data) {
    return compileCompiledScalarConsumer(req, data, facade, runtime);
  };
  rejects(
      [&] {
        (void)buildCompiledScalarLibrary(req, {std::string(16385, 'x')},
                                         runtime);
      },
      "oversized producer key");
  FunctionInfo foreign;
  foreign.qualifiedName = "foreign";
  foreign.parameterTypes = {SemanticType::Int32};
  auto mixed = keys();
  mixed.push_back(persistentFunctionKey(
      foreign, {"different/package", "numbers", "numbers", ""}));
  rejects([&] { (void)buildCompiledScalarLibrary(req, mixed, runtime); },
          "mixed package-target keys");
  auto bundle = decodeLibraryBundle(bytes);
  // Envelope integrity is insufficient: a changed result recipe must not
  // redirect a call to an object exported under a different callable ABI.
  auto forgedResult = bundle;
  auto signatures = decodeCompiledLibraryInterface(bundle.interfaceMetadata);
  signatures.front().result = SemanticType::Bool;
  signatures.front().resultRecipe =
      writePersistentType(SemanticType::Bool, PersistentTypeRegistry{});
  forgedResult.interfaceMetadata = encodeCompiledLibraryInterface(signatures);
  rejects([&] { (void)consume(encodeLibraryBundle(forgedResult)); },
          "result recipe inconsistent with native export identity");
  auto duplicateResult = bundle;
  rejects(
      [&] {
        auto duplicated =
            decodeCompiledLibraryInterface(bundle.interfaceMetadata);
        duplicated.push_back(signatures.front());
        duplicateResult.interfaceMetadata =
            encodeCompiledLibraryInterface(duplicated);
        (void)consume(encodeLibraryBundle(duplicateResult));
      },
      "duplicate overload identity with a different result");
  for (int field = 0; field < 4; ++field) {
    auto bad = bundle;
    std::string *fields[] = {&bad.identity.toolchain,
                             &bad.identity.configuration, &bad.identity.runtime,
                             &bad.identity.target};
    *fields[field] = field == 3 ? "x86_64-other-unknown" : std::string(64, '0');
    rejects([&] { (void)consume(encodeLibraryBundle(bad)); },
            "incompatible artifact");
  }
  for (const auto &metadata : {std::string("unsupported-profile\n"),
                               std::string("gti.scalar-interface.1\n"),
                               bundle.interfaceMetadata + keys()[0] + "\n"}) {
    auto malformed = bundle;
    malformed.interfaceMetadata = metadata;
    rejects([&] { (void)consume(encodeLibraryBundle(malformed)); },
            "malformed interface with valid envelope integrity");
  }
  auto bad = bytes;
  bad.back() ^= 1;
  rejects([&] { (void)consume(bad); }, "checksum damage");
  rejects(
      [&] {
        (void)consume(std::string_view(bytes).substr(0, bytes.size() / 2));
      },
      "truncation");
  auto broken = bundle;
  broken.objects[0].bytes = "not an object";
  rejects([&] { (void)consume(encodeLibraryBundle(broken)); },
          "invalid native object");
  auto wrongKey = keys()[0];
  wrongKey.replace(wrongKey.find("choose"), 6, "absent");
  rejects(
      [&] {
        verifyCompiledScalarObject(bundle.objects[0].bytes, req.target(),
                                   {compiledScalarSymbol(wrongKey)});
      },
      "missing object export");
  rejects(
      [&] {
        (void)compileCompiledScalarConsumer(
            request(root, consumer, OptimizationLevel::O0), bytes, facade,
            runtime);
      },
      "optimization mismatch");
  const auto fakeRuntime = root / "other-runtime.a";
  write(fakeRuntime, "different runtime");
  rejects(
      [&] {
        (void)compileCompiledScalarConsumer(req, bytes, facade, fakeRuntime);
      },
      "different runtime bytes");
  write(consumer, "import \"api.gti\"; int missing(int x); int main() { return "
                  "missing(choose(7)); }\n");
  const auto unresolved = consume(bytes);
  expect(!unresolved.compilation.succeeded(),
         "ordinary missing declaration was admitted");
  bool missingDiagnostic = false;
  for (const auto &d : unresolved.compilation.diagnostics)
    missingDiagnostic |=
        d.message.find("has no definition") != std::string::npos;
  expect(missingDiagnostic, "missing-definition diagnostic was lost");
  write(consumer,
        "import \"api.gti\"; int main() { return choose_impl(7); }\n");
  expect(!consume(bytes).compilation.succeeded(),
         "nonpublished library declaration leaked");
  write(consumer, "import \"api.gti\"; int main() { return choose(1, 2); }\n");
  expect(!consume(bytes).compilation.succeeded(),
         "wrong imported arity accepted");
  write(consumer, "import \"api.gti\"; int main() { return 0; }\n");
  linkAndRun(root, runtime, consume(bytes)); // Unused import is valid.
  write(facade, "export module compiled_scalar; export int choose(int x);\n");
  rejects([&] { (void)consume(bytes); }, "existing facade overwrite");
  write(consumer, "import \"api.gti\"; int main() { return choose(7); }\n");
  expect(!compileToSelectedBackend(req).succeeded(),
         "ordinary source forged imported admission");
  std::filesystem::remove(facade);
  auto importedInputs = loadCompilationInputs(
      req, nullptr,
      {{facade.string(),
        "export module compiled_scalar; export int choose(int x);\n"}});
  const auto importedUnit =
      importedInputs.sourceGraph.sourceUnitForPath(facade.string());
  expect(importedUnit != 0, "imported mutation fixture has no source unit");
  importedInputs.sourceGraph.markCompiledScalarInterface(importedUnit);
  Capture importedCapture;
  compiled(compileWithBackend(req, std::move(importedInputs), importedCapture));
  std::vector<CompiledScalarImport> importedSignatures;
  for (const auto &entry :
       decodeCompiledLibraryInterface(bundle.interfaceMetadata))
    importedSignatures.push_back({entry, importedUnit});
  auto importedProgram = CompiledScalarUnitBuilder().bindImports(
      *importedCapture.program, importedSignatures);
  LoweredProgramTestAccess::bindings(importedProgram).clear();
  expect(!verifyCompiledScalarUnit(importedProgram).empty(),
         "deleting every binding must not turn a called import into an open "
         "fragment");
  const auto library = root / "producer/library.gti";
  for (std::string_view source :
       {"export module numbers; int choose(int x) { return x; }",
        "export module numbers; export bool choose(int x) { return true; }",
        "export module numbers; export int choose(int& x) { return x; }",
        "export module numbers; int state = 1; export int choose(int x) { "
        "return state; }",
        "export module numbers; export int choose(int x) { return x; } int "
        "main() { return 0; }",
        "export module numbers; export constexpr int choose(int x) { return x; "
        "}",
        "export module numbers; export int choose(int x = 0) { return x; }"}) {
    write(library, source);
    const auto rejected = buildCompiledScalarLibrary(
        request(root, library, OptimizationLevel::O3), keys(), runtime);
    expect(!rejected.compilation.succeeded() && rejected.bundle.empty(),
           "unsupported producer accepted: " + std::string(source));
  }
  write(library,
        "export module numbers; export int choose(int x) { return x; }");
  auto originalInterface = buildCompiledScalarLibrary(
      request(root, library, OptimizationLevel::O3), keys(), runtime);
  compiled(originalInterface.compilation);
  FunctionInfo extra;
  extra.qualifiedName = "extra";
  auto expandedKeys = keys();
  expandedKeys.push_back(persistentFunctionKey(
      extra, {"example/numbers", "numbers", "numbers", ""}));
  write(library, "export module numbers; export int choose(int x) { return x; "
                 "} export int extra() { return 1; }");
  auto expanded = buildCompiledScalarLibrary(
      request(root, library, OptimizationLevel::O3), expandedKeys, runtime);
  compiled(expanded.compilation);
  expect(decodeLibraryBundle(originalInterface.bundle).identity.library ==
             decodeLibraryBundle(expanded.bundle).identity.library,
         "public interface edits changed logical package-target identity");
  Capture capture;
  compiled(compileWithBackend(
      request(root, library, OptimizationLevel::O3).withApplicationBodies(),
      capture));
  const CompiledScalarSignature signature{keys()[0], SemanticType::Int32};
  rejects(
      [&] {
        (void)CompiledScalarUnitBuilder().bindExports(*capture.program, 999999,
                                                      std::span(&signature, 1));
      },
      "producer root belonging to another source unit");
  auto bound = CompiledScalarUnitBuilder().bind(*capture.program, keys(), true);
  expect(verifyLoweredProgram(bound).empty(),
         "valid compiled plan failed verification");
  expect(LoweredProgramPrinter().print(bound).find(
             "compiled-interface-unit ") != std::string::npos,
         "compiled interface ownership must enter lowered serialization");
  const std::vector<CompiledScalarSignature> duplicates{signature, signature};
  rejects(
      [&] {
        (void)CompiledScalarUnitBuilder().bindExports(
            *capture.program, bound.compiledScalarBindings().front().sourceUnit,
            duplicates);
      },
      "duplicate typed export identity");
  for (int mutation = 0; mutation < 10; ++mutation) {
    auto changed = bound;
    auto &bindings = LoweredProgramTestAccess::bindings(changed);
    if (mutation == 0)
      bindings[0].symbol += "_forged";
    if (mutation == 1)
      bindings[0].function = 999999;
    if (mutation == 2)
      bindings[0].imported = true;
    if (mutation == 3)
      bindings.push_back(bindings.front());
    if (mutation == 4)
      bindings[0].sourceUnit = 999999;
    if (mutation == 5)
      bindings[0].result = SemanticType::Bool;
    auto &units = LoweredProgramTestAccess::units(changed);
    if (mutation == 6)
      units.clear();
    if (mutation == 7)
      units.front().owner.target = "wrong-owner";
    if (mutation == 8)
      units.push_back(units.front());
    if (mutation == 9)
      units.front().sourceUnit = 0;
    expect(!verifyCompiledScalarUnit(changed).empty() &&
               !verifyLoweredProgram(changed).empty(),
           "forged compiled plan accepted");
  }
  std::filesystem::remove_all(library.parent_path());
  write(consumer, "import \"api.gti\"; int main() { if (extra() != 1) { return "
                  "1; } return choose(0); }");
  linkAndRun(
      root, runtime,
      compileCompiledScalarConsumer(req, expanded.bundle, facade, runtime));
  std::cout << "compatibility, visibility, malformed artifact, unsupported "
               "profile and linkage mutation checks passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 6) {
      const std::filesystem::path runtime = argv[2], root = argv[3];
      const auto level = static_cast<OptimizationLevel>(std::stoi(argv[4]));
      const auto bundlePath = root / "numbers.gtilib";
      if (std::string_view(argv[1]) == "--produce") {
        auto producer = buildCompiledScalarLibrary(
            request(root, root / "producer/library.gti", level), keys(),
            runtime);
        compiled(producer.compilation);
        write(bundlePath, producer.bundle);
      } else if (std::string_view(argv[1]) == "--consume") {
        expect(!std::filesystem::exists(root / "producer"),
               "producer sources still exist");
        const auto client = compileCompiledScalarConsumer(
            request(root, root / "consumer/main.gti", level), read(bundlePath),
            root / "consumer/api.gti", runtime);
        linkAndRun(root, runtime, client);
      } else
        throw std::invalid_argument("unknown child mode");
      return 0;
    }
    expect(argc == 3, "expected runtime archive and example directory");
    TemporaryDirectory tmp;
    const std::filesystem::path examples = argv[2];
    projectCheckTests(tmp.path / "check-project", argv[1]);
    write(tmp.path / "stdlib/prelude.gti", "");
    for (auto level : {OptimizationLevel::O0, OptimizationLevel::O1,
                       OptimizationLevel::O3}) {
      for (const auto *file : {"library.gti", "implementation.gti"})
        write(tmp.path / "producer" / file, read(examples / file));
      const auto started = std::chrono::steady_clock::now();
      auto producer =
          process({argv[0], "--produce", argv[1], tmp.path.string(),
                   std::to_string(static_cast<int>(level)), "child"});
      expect(producer.succeeded(),
             "producer process failed: " + producer.output);
      const auto original = read(tmp.path / "numbers.gtilib");
      std::filesystem::remove_all(tmp.path / "producer");
      for (int edit = 0; edit < 2; ++edit) {
        write(tmp.path / "consumer/main.gti",
              edit == 0
                  ? read(examples / "consumer.gti")
                  : "import \"api.gti\"; int main() { return choose(0); }\n");
        auto client =
            process({argv[0], "--consume", argv[1], tmp.path.string(),
                     std::to_string(static_cast<int>(level)), "child"});
        expect(client.succeeded(), "consumer process failed: " + client.output);
        expect(read(tmp.path / "numbers.gtilib") == original,
               "consumer changed library artifact");
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started)
              .count();
      std::cout << "O" << static_cast<int>(level)
                << ": separate-process source-absent consumer and rebuild "
                   "passed; bundle="
                << original.size() << " object="
                << std::filesystem::file_size(tmp.path / "library.o")
                << " consumer="
                << std::filesystem::file_size(tmp.path / "consumer.o")
                << " elapsed_ms=" << elapsed << '\n';
      if (level == OptimizationLevel::O3)
        negativeTests(tmp.path, argv[1], original);
    }
    // Use the shipped prelude too: unrelated trusted declarations must not
    // become unbound imports in the producer's concrete application closure.
    std::filesystem::remove_all(tmp.path / "stdlib");
    std::filesystem::create_directory_symlink(
        std::filesystem::canonical(examples.parent_path().parent_path() /
                                   "stdlib"),
        tmp.path / "stdlib");
    for (const auto *file : {"library.gti", "implementation.gti"})
      write(tmp.path / "producer" / file, read(examples / file));
    auto producer = process(
        {argv[0], "--produce", argv[1], tmp.path.string(), "3", "child"});
    expect(producer.succeeded(),
           "shipped-prelude producer failed: " + producer.output);
    std::filesystem::remove_all(tmp.path / "producer");
    write(tmp.path / "consumer/main.gti", read(examples / "consumer.gti"));
    auto consumer = process(
        {argv[0], "--consume", argv[1], tmp.path.string(), "3", "child"});
    expect(consumer.succeeded(),
           "shipped-prelude consumer failed: " + consumer.output);
    std::cout << "shipped-prelude source-absent producer/consumer passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
