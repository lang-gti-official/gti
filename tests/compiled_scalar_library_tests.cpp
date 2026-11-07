#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/session.h"
#include "../src/driver/compiled_scalar_support.h"
#include "gti/compiled_interface_types.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/driver/build.h"
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
  static auto &mir(LoweredProgram &p) { return p.mir_; }
};
struct MirProgramTestAccess {
  static auto &steps(MirProgram &p) { return p.programInitialization.steps; }
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
void passiveStorageTests(const std::filesystem::path &root,
                         const std::filesystem::path &runtime) {
  write(root / "stdlib/prelude.gti", "");
  const auto source = root / "producer/library.gti";
  constexpr std::string_view library = R"(
export module numbers;
mut int total;
mut int calls;
constexpr int bias = 7;
class Counts {
public:
  static constexpr int weight = 2;
};
int read(int& value) { return value; }
export int choose(int x) {
  total += x;
  calls += 1;
  return total + calls + read(bias) + read(Counts::weight);
}
)";
  for (auto level : {OptimizationLevel::O0, OptimizationLevel::O3}) {
    write(source, library);
    auto producer = buildCompiledScalarLibrary(request(root, source, level),
                                               keys(), runtime);
    compiled(producer.compilation);
    if (level == OptimizationLevel::O3) {
      Capture capture;
      compiled(compileWithBackend(
          request(root, source, level).withApplicationBodies(), capture));
      auto bound =
          CompiledScalarUnitBuilder().bind(*capture.program, keys(), true);
      expect(verifyLoweredProgram(bound).empty(),
             "valid passive library failed verification");
      const auto &steps = bound.mir().programInitializationPlan().steps;
      expect(
          steps.size() == 4 &&
              std::count_if(steps.begin(), steps.end(),
                            [](const auto &step) {
                              return step.dataInitialization ==
                                     MirProgramDataInitializationKind::Constant;
                            }) == 2,
          "fixture must exercise both zero and constant object storage");
      for (int mutation = 0; mutation < 7; ++mutation) {
        auto changed = bound;
        auto &step =
            MirProgramTestAccess::steps(LoweredProgramTestAccess::mir(changed))
                .front();
        if (mutation == 0)
          step.role = ProgramInitializationStepRole::Initializer;
        if (mutation == 1)
          step.storageKind = ProgramStorageKind::FunctionLocal;
        if (mutation == 2)
          step.requiresActiveCleanup = true;
        if (mutation == 3)
          step.nativeRetirement = MirDropType{};
        if (mutation == 4)
          step.dataInitialization = MirProgramDataInitializationKind::None;
        if (mutation == 5)
          step.dataInitialization = MirProgramDataInitializationKind::Constant;
        if (mutation == 6)
          step.dataConstant = steps[2].dataConstant;
        expect(!verifyCompiledScalarUnit(changed).empty() &&
                   !verifyLoweredProgram(changed).empty(),
               "forged passive storage plan accepted");
      }
    }
    std::filesystem::remove_all(source.parent_path());
    write(root / "consumer/main.gti", R"(
import "api.gti";
int main() {
  if (choose(2) != 12) { return 1; }
  if (choose(3) != 16) { return 2; }
  if (choose(0) != 17) { return 3; }
  return 0;
}
)");
    linkAndRun(root, runtime,
               compileCompiledScalarConsumer(
                   request(root, root / "consumer/main.gti", level),
                   producer.bundle, root / "consumer/api.gti", runtime));
    // A new executable process starts with fresh object data, not a build-time
    // or process-external counter shared by consumers.
    const auto rerun = process({(root / "app").string()});
    expect(rerun.succeeded(),
           "passive storage did not reset per process: " + rerun.output);
  }
  struct Rejection {
    std::string_view storage, code, message;
  };
  for (const auto &test :
       {Rejection{"int initial() { return 1; } mut int total = initial();",
                  "GTI-B0003", "requires passive program storage"},
        Rejection{"thread_local mut int total;", "GTI-B0003",
                  "requires passive program storage"},
        // Source-owned global cleanup is rejected by semantics before
        // compiled-unit admission. Keep that earlier safety boundary.
        Rejection{"class Guard { public: Guard() = default; ~Guard() {} }; "
                  "Guard guard; mut int total;",
                  "GTI-S2061", "requires source or owning cleanup"}}) {
    write(source, "export module numbers; " + std::string(test.storage) +
                      " export int choose(int x) { return total + x; }");
    const auto rejected = buildCompiledScalarLibrary(
        request(root, source, OptimizationLevel::O3), keys(), runtime);
    const bool storageLimitation = std::any_of(
        rejected.compilation.diagnostics.begin(),
        rejected.compilation.diagnostics.end(), [&](const auto &diagnostic) {
          return diagnostic.code == test.code &&
                 diagnostic.message.find(test.message) != std::string::npos;
        });
    std::string details;
    for (const auto &diagnostic : rejected.compilation.diagnostics)
      details += diagnostic.code + ": " + diagnostic.message + "\n";
    expect(!rejected.compilation.succeeded() && rejected.bundle.empty() &&
               storageLimitation,
           "unsupported storage must fail at its owning boundary: " +
               std::string(test.storage) + ": " + details);
  }
  std::cout << "passive namespace/static/constant storage and source-absent "
               "execution passed at O0/O3\n";
}
void runtimeClosureTests(const std::filesystem::path &root,
                         const std::filesystem::path &runtime) {
  const auto target = TargetInfo::host();
  const auto services = compiledLibrarySupportSymbols(read(runtime), target);
  expect(!services.empty() && std::is_sorted(services.begin(), services.end()),
         "runtime service inventory must be nonempty and canonical");
  rejects(
      [&] { (void)compiledLibrarySupportSymbols("not an archive", target); },
      "malformed runtime archive");
  rejects([&] { (void)compiledLibrarySupportSymbols("!<thin>\n", target); },
          "external thin runtime members");
  rejects([&] { (void)compiledLibrarySupportSymbols("!<arch>\n", target); },
          "runtime without services");
  // These are symbol-verifier fixtures, not calls with an invented runtime ABI:
  // nothing links or executes them. Prefix alone must never authorize an
  // import.
  const auto runtimeService =
      std::find_if(services.begin(), services.end(), [](const auto &name) {
        return name.starts_with("gti_rt_");
      });
  expect(runtimeService != services.end(), "missing actual runtime C export");
  for (const auto &dependency : {*runtimeService, std::string("memcpy"),
                                 std::string("gti_rt_missing_service_v999"),
                                 std::string("arbitrary_native_service")}) {
    const auto source = root / "runtime-probe.cpp";
    const auto object = root / "runtime-probe.o";
    write(source, "extern \"C\" void " + dependency +
                      "(); extern \"C\" void probe() { " + dependency +
                      "(); }\n");
    auto built = process({"c++", "-c", source.string(), "-o", object.string()});
    expect(built.succeeded(), "runtime symbol fixture failed: " + built.output);
    const auto bytes = read(object);
    rejects([&] { verifyCompiledScalarObject(bytes, target, {"probe"}); },
            "unselected runtime dependency");
    if (dependency == *runtimeService || dependency == "memcpy")
      verifyCompiledScalarObject(bytes, target, {"probe"}, services);
    else
      rejects(
          [&] {
            verifyCompiledScalarObject(bytes, target, {"probe"}, services);
          },
          "unprovided native dependency");
  }
}
void checkAnnotationIr(const LoweredProgram &program) {
  std::vector<LoweredProgramIssue> issues;
  const auto verified = verifiedLoweredProgram(program, issues);
  expect(verified.has_value(), "compiled annotation plan is not verified");
  native_llvm::Session session(*verified, {});
  const auto admission = planLlvmAdmission(*verified, session.options());
  expect(admission.admitted(), "annotated compiled unit is not admitted");
  native_llvm::lowerScalarProgram(session, admission);
  const auto check = [](const llvm::Function &function,
                        FunctionCodegenAttributes policy) {
    expect(function.hasFnAttribute(llvm::Attribute::Cold) == policy.cold &&
               function.hasFnAttribute(llvm::Attribute::InlineHint) ==
                   (policy.inlining == FunctionInlining::Hint) &&
               function.hasFnAttribute(llvm::Attribute::NoInline) ==
                   (policy.inlining == FunctionInlining::Never) &&
               !function.hasFnAttribute(llvm::Attribute::AlwaysInline),
           "LLVM compiled boundary lost or strengthened annotation policy");
  };
  for (const auto &binding : program.compiledScalarBindings()) {
    const auto *external = session.module().getFunction(binding.symbol);
    expect(external != nullptr, "compiled boundary missing from LLVM IR");
    check(*external, binding.codegenAttributes);
    if (!binding.imported)
      check(*session.abi().declareFunction(binding.function),
            binding.codegenAttributes);
  }
  session.verify();
}
void annotationTests(const std::filesystem::path &root,
                     const std::filesystem::path &runtime) {
  write(root / "stdlib/prelude.gti", "");
  const auto library = root / "producer/library.gti";
  const auto consumer = root / "consumer/main.gti";
  const auto facade = root / "consumer/api.gti";
  const std::vector<std::string> attributes{
      "",          "[[inline]] ",       "[[noinline]] ",
      "[[cold]] ", "[[cold, inline]] ", "[[cold, noinline]] "};
  std::string source = "export module annotations; export namespace api {\n";
  std::string client = "import \"api.gti\"; int main() {\n";
  std::vector<std::string> names;
  for (std::size_t i = 0; i < attributes.size(); ++i) {
    const auto name = "f" + std::to_string(i);
    names.push_back("api::" + name);
    source += attributes[i] + "int " + name + "(int x) { return x; }\n";
    client += "if (api::" + name + "(7) != 7) { return 1; }\n";
  }
  write(library, source + "}\n");
  write(consumer, client + "return 0; }\n");
  const auto producerRequest = request(root, library, OptimizationLevel::O3);
  Capture producer;
  compiled(
      compileWithBackend(producerRequest.withApplicationBodies(), producer));
  expect(!producer.program->mir().functionInstances().empty(),
         "producer has no functions");
  const auto unit =
      producer.program
          ->findFunctionDeclaration(
              producer.program->mir().functionInstances().front().declaration)
          ->sourceUnit;
  const auto signatures = CompiledScalarUnitBuilder().selectExports(
      *producer.program, unit,
      {"annotations", "annotations", "annotations", ""}, names);
  expect(signatures.size() == attributes.size(),
         "missing annotation overloads");
  const auto bytes = encodeCompiledLibraryInterface(signatures);
  expect(decodeCompiledLibraryInterface(bytes) == signatures,
         "annotation interface did not round-trip exactly");
  for (std::size_t i = 0; i < signatures.size(); ++i) {
    expect(signatures[i].codegenAttributes ==
               FunctionCodegenAttributes{static_cast<FunctionInlining>(i % 3),
                                         i >= 3},
           "export selection erased the source annotation");
  }
  auto bound = CompiledScalarUnitBuilder().bindExports(*producer.program, unit,
                                                       signatures);
  checkAnnotationIr(bound);
  auto altered = signatures;
  altered.front().codegenAttributes.cold = true;
  expect(compiledScalarSymbol(altered.front()) !=
             compiledScalarSymbol(signatures.front()),
         "annotation change did not invalidate the exported contract");
  rejects(
      [&] {
        (void)CompiledScalarUnitBuilder().bindExports(*producer.program, unit,
                                                      altered);
      },
      "producer binding with a policy different from its source");
  altered.front().codegenAttributes.inlining =
      static_cast<FunctionInlining>(99);
  rejects([&] { (void)encodeCompiledLibraryInterface(altered); },
          "unknown inlining policy");
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"7:default", "6:always"},
           {"1:0\n", "1:2\n"},
           {"1:0\n", "2:00\n"},
           {"gti.callable-interface.4", "gti.callable-interface.3"}}) {
    auto malformed = bytes;
    const auto offset = malformed.find(from);
    expect(offset != std::string::npos, "missing wire mutation target");
    malformed.replace(offset, from.size(), to);
    rejects([&] { (void)decodeCompiledLibraryInterface(malformed); },
            "malformed annotation metadata");
  }
  auto tampered = bound;
  auto &binding = LoweredProgramTestAccess::bindings(tampered).front();
  binding.codegenAttributes.cold = true;
  auto changedSignature = signatures.front();
  changedSignature.codegenAttributes = binding.codegenAttributes;
  binding.symbol = compiledScalarSymbol(changedSignature);
  expect(!verifyCompiledScalarUnit(tampered).empty(),
         "changed source policy accepted");
  const auto built = buildCompiledScalarLibraryTarget(
      producerRequest, {"annotations", "annotations", names}, runtime);
  compiled(built.compilation);
  std::filesystem::remove_all(library.parent_path());
  const auto consumerRequest = request(root, consumer, OptimizationLevel::O3);
  auto inputs = loadCompilationInputs(
      consumerRequest, nullptr,
      {{facade.string(),
        "export module annotations;\n" + compiledInterfaceSource(signatures)}});
  const auto importedUnit =
      inputs.sourceGraph.sourceUnitForPath(facade.string());
  inputs.sourceGraph.markCompiledScalarInterface(importedUnit);
  Capture imported;
  compiled(compileWithBackend(consumerRequest, std::move(inputs), imported));
  std::vector<CompiledScalarImport> imports;
  for (const auto &signature : signatures)
    imports.push_back({signature, importedUnit});
  auto importedProgram =
      CompiledScalarUnitBuilder().bindImports(*imported.program, imports);
  checkAnnotationIr(importedProgram);
  const auto serialized = LoweredProgramPrinter().print(importedProgram);
  expect(serialized.find("codegen-inline=2 codegen-cold=1") !=
             std::string::npos,
         "compiled annotation policy missing from sealed serialization");
  LoweredProgramTestAccess::bindings(importedProgram)
      .front()
      .codegenAttributes.cold = true;
  expect(!verifyCompiledScalarUnit(importedProgram).empty(),
         "import binding policy lost symbol verification");
  linkAndRun(root, runtime,
             compileCompiledScalarConsumer(consumerRequest, built.bundle,
                                           facade, runtime));
  auto forged = decodeLibraryBundle(built.bundle);
  auto forgedSignatures =
      decodeCompiledLibraryInterface(forged.interfaceMetadata);
  forgedSignatures.front().codegenAttributes.cold = true;
  forged.interfaceMetadata = encodeCompiledLibraryInterface(forgedSignatures);
  rejects(
      [&] {
        (void)compileCompiledScalarConsumer(
            consumerRequest, encodeLibraryBundle(forged), facade, runtime);
      },
      "rechecksummed interface policy inconsistent with its native object");
  std::cout << "compiled annotation wire, LLVM boundary, source-absent "
               "execution and rejection checks passed\n";
}
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
void snapshotTests(const std::filesystem::path &root,
                   const std::filesystem::path &runtime) {
  write(root / "stdlib/prelude.gti", "");
  const auto source = root / "producer/library.gti";
  const auto artifact = root / "numbers.gtilib";
  const auto consumer = root / "consumer/main.gti";
  const auto facade = root / "consumer/api.gti";
  const CompiledLibraryTarget target{"example/numbers", "numbers", {"choose"}};
  for (const auto level : {OptimizationLevel::O0, OptimizationLevel::O3}) {
    write(source,
          "export module numbers; export int choose(int x) { return x; }");
    const auto producerRequest = request(root, source, level);
    const auto buildRequest = ExecutableBuildRequest(
        producerRequest,
        {.standardLibrary = standardLibraryLayout(root / "stdlib"),
         .runtimeLibrary = runtime},
        root / "producer.o", artifact, "c++", {}, false, true, true, {}, {},
        BuildCachePolicy{.root = root / "cache"}, {}, target);
    auto built = buildExecutable(buildRequest);
    expect(built.succeeded() && built.compiledLibrarySnapshot &&
               *built.compiledLibrarySnapshot == read(artifact),
           "producer must return its exact published artifact bytes");
    auto reused = buildExecutable(buildRequest);
    expect(reused.succeeded() && reused.cache.status == BuildCacheStatus::Hit &&
               reused.compiledLibrarySnapshot &&
               *reused.compiledLibrarySnapshot ==
                   *built.compiledLibrarySnapshot,
           "cache hit must retain the exact validated artifact bytes");
    CompiledLibrarySnapshots first;
    first.retain(artifact, reused.compiledLibrarySnapshot);
    first.retain(artifact, built.compiledLibrarySnapshot);
    // Replace the path with another valid bundle from the same logical
    // library, then delete all producer sources and the selected artifact.
    write(source,
          "export module numbers; export int choose(int x) { return x + 1; }");
    auto replacement = buildExecutable(buildRequest);
    expect(replacement.succeeded() && replacement.compiledLibrarySnapshot &&
               *replacement.compiledLibrarySnapshot !=
                   *reused.compiledLibrarySnapshot,
           "source edit must build a different library snapshot");
    std::vector<CompiledLibraryImport> inputs{
        {"example/numbers", "numbers", artifact, facade}};
    auto oldSelection = first.select(inputs);
    CompiledLibrarySnapshots nextBuild;
    auto newSelection = nextBuild.select(inputs);
    expect(oldSelection.front().snapshot == reused.compiledLibrarySnapshot &&
               *newSelection.front().snapshot ==
                   *replacement.compiledLibrarySnapshot,
           "each build must retain its own artifact selection");
    std::filesystem::remove_all(source.parent_path());
    std::filesystem::remove(artifact);
    expect(first.select(inputs).front().snapshot ==
                   oldSelection.front().snapshot &&
               nextBuild.select(inputs).front().snapshot ==
                   newSelection.front().snapshot,
           "selected dependencies must not be reopened or recopied");
    for (int revision = 0; revision < 2; ++revision) {
      write(consumer, "import \"api.gti\"; int main() { return choose(7) - " +
                          std::to_string(7 + revision) + "; }");
      linkAndRun(root, runtime,
                 compileCompiledScalarConsumer(
                     request(root, consumer, level),
                     revision ? newSelection : oldSelection, runtime));
    }
    rejects(
        [&] { first.retain(artifact, replacement.compiledLibrarySnapshot); },
        "replacing a selection within the same build");
    auto corrupted = oldSelection;
    corrupted.front().snapshot = std::make_shared<const std::string>("broken");
    rejects(
        [&] {
          (void)compileCompiledScalarConsumer(request(root, consumer, level),
                                              corrupted, runtime);
        },
        "in-memory bytes bypassing integrity checks");
    auto incompatible = decodeLibraryBundle(*oldSelection.front().snapshot);
    incompatible.identity.runtime = std::string(64, '0');
    corrupted.front().snapshot =
        std::make_shared<const std::string>(encodeLibraryBundle(incompatible));
    rejects(
        [&] {
          (void)compileCompiledScalarConsumer(request(root, consumer, level),
                                              corrupted, runtime);
        },
        "checksummed in-memory bytes bypassing runtime compatibility");
    auto wrongOwner = oldSelection;
    wrongOwner.front().package = "another/package";
    rejects(
        [&] {
          (void)compileCompiledScalarConsumer(request(root, consumer, level),
                                              wrongOwner, runtime);
        },
        "snapshot bypassing selected package identity");
  }
  CompiledLibrarySnapshots limited;
  const auto small = std::make_shared<const std::string>("x");
  for (int i = 0; i < 64; ++i)
    limited.retain(root / std::to_string(i), small);
  limited.retain(root / "0", small);
  rejects([&] { limited.retain(root / "64", small); },
          "more than 64 selections");
  CompiledLibrarySnapshots byteLimited;
  const auto block = std::make_shared<const std::string>(4 * 1024 * 1024, 'x');
  for (int i = 0; i < 63; ++i)
    byteLimited.retain(root / std::to_string(i), block);
  rejects(
      [&] {
        byteLimited.retain(root / "63", std::make_shared<const std::string>(
                                            8 * 1024 * 1024, 'x'));
      },
      "aggregate snapshot bytes exceeding the bundle limit");
  rejects([&] { byteLimited.retain(root / "null", {}); }, "null snapshot");
  std::cout
      << "published/cache-hit snapshot handoff, replacement/deletion, "
         "source-absent O0/O3 execution and bounded untrusted inputs passed\n";
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
    runtimeClosureTests(tmp.path / "runtime", argv[1]);
    projectCheckTests(tmp.path / "check-project", argv[1]);
    snapshotTests(tmp.path / "snapshots", argv[1]);
    passiveStorageTests(tmp.path / "passive-storage", argv[1]);
    annotationTests(tmp.path / "annotations", argv[1]);
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
