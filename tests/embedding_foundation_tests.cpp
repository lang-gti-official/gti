#include "gti/driver/compilation.h"
#include "gti/driver/manifest.h"
#include "gti/driver/project.h"
#include "gti/driver/project_facts.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"
#include "gti/standard_library.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasCode(const std::vector<lang::Diagnostic> &diagnostics,
             std::string_view code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool hasMessage(const std::vector<lang::Diagnostic> &diagnostics,
                std::string_view text) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

const lang::Diagnostic *
findMessage(const std::vector<lang::Diagnostic> &diagnostics,
            std::string_view text) {
  const auto found =
      std::find_if(diagnostics.begin(), diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.message.find(text) != std::string::npos;
                   });
  return found == diagnostics.end() ? nullptr : &*found;
}

lang::ProgramRequest
libraryRequest(std::vector<std::pair<std::string, std::string>> exports) {
  lang::ProgramRequest request{.kind = lang::ProgramKind::Library};
  for (auto &[function, symbol] : exports) {
    request.exports.push_back(
        {.function = std::move(function), .symbol = std::move(symbol)});
  }
  return request;
}

lang::FrontendResult analyze(std::string_view name, std::string source,
                             lang::ProgramRequest request) {
  return lang::Frontend({.program = std::move(request)})
      .analyze(std::string(name), std::move(source));
}

lang::FrontendResult analyzeWithStandardLibrary(std::string_view name,
                                                std::string source,
                                                lang::ProgramRequest request) {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend({.program = std::move(request)})
      .analyze(std::string(name), std::move(source),
               {repository / "stdlib/prelude.gti"}, {},
               {repository / "stdlib"});
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gti-embedding-foundation-" + std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  const std::filesystem::path &root() const { return path; }

private:
  std::filesystem::path path;
};

bool writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
  return static_cast<bool>(output);
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string embeddingFixtureSource() {
  return readFile(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                  "embedding" / "gameplay.gti");
}

std::string reorderedEmbeddingFixtureSource() {
  return R"([[c_opaque]] struct Engine;

[[c_abi]]
struct Frame {
  mut int32_t frame;
  mut double delta_seconds;
};

namespace game {
int32_t unused(int32_t value) { return value; }

void initialize(Engine* engine) {}

int32_t update(Engine* engine, Frame frame) {
  return add_one(frame.frame);
}

int32_t add_one(int32_t value) { return value + 1; }
}
)";
}

void testSemanticAndLoweringRoots() {
  lang::FrontendResult frontend =
      analyze("embedding.gti", embeddingFixtureSource(),
              libraryRequest({{"game::update", "game_update_v1"},
                              {"game::initialize", "game_initialize_v1"}}));
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  const auto &semantic = frontend.semantics.embeddingPlan();
  const auto &hir = frontend.hir.embeddingPlan();
  const auto &mir = frontend.mir.embeddingPlan();
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "a library source graph without main should complete the frontend");
  expect(frontend.semantics.programKind() == lang::ProgramKind::Library &&
             frontend.hir.programKind() == lang::ProgramKind::Library &&
             frontend.mir.programKind() == lang::ProgramKind::Library,
         "program kind should remain exact through semantic, HIR, and MIR");
  expect(semantic && hir && mir && semantic->exports.size() == 2 &&
             hir->exports.size() == 2 && mir->exports.size() == 2 &&
             semantic->exports[0].symbol == "game_initialize_v1" &&
             semantic->exports[1].symbol == "game_update_v1",
         "export rows should be canonicalized by public symbol");
  expect(
      lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid() &&
          lang::verifyMirProgram(frontend.mir).valid(),
      "verified HIR and MIR should retain exact embedding roots");
  expect(!frontend.failureMetadata.sites().empty() &&
             frontend.failureMetadata == frontend.mir.failureMetadata(),
         "failure metadata should cover the checked exported call graph");
  const auto helper =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "add_one";
                   });
  const bool helperFailurePublished =
      helper != frontend.hir.functionInstances().end() &&
      std::any_of(
          helper->body.values.begin(), helper->body.values.end(),
          [&](const lang::HirValue &value) {
            return std::any_of(
                value.definedFailure.localOrigins.begin(),
                value.definedFailure.localOrigins.end(),
                [&](const lang::DefinedFailureOrigin &origin) {
                  return frontend.failureMetadata.siteFor(origin).has_value();
                });
          });
  expect(helperFailurePublished,
         "the checked helper reached from an export should retain an exact "
         "failure site");

  const auto unusedPublic =
      mir && std::any_of(mir->exports.begin(), mir->exports.end(),
                         [](const lang::MirEmbeddingExport &selected) {
                           return selected.qualifiedName == "game::unused";
                         });
  expect(!unusedPublic,
         "an unused declaration should not become a public ABI root");

  lang::HirProgram corruptedHir = frontend.hir;
  auto &corruptedHirPlan = const_cast<std::optional<lang::HirEmbeddingPlan> &>(
      corruptedHir.embeddingPlan());
  if (corruptedHirPlan) {
    corruptedHirPlan->exports.front().function = 0;
  }
  expect(!lang::verifyHirProgramPlans(frontend.semantics, corruptedHir).valid(),
         "HIR verification should reject a forged export instance");

  lang::MirProgram corruptedMir = frontend.mir;
  auto &corruptedMirPlan = const_cast<std::optional<lang::MirEmbeddingPlan> &>(
      corruptedMir.embeddingPlan());
  if (corruptedMirPlan) {
    corruptedMirPlan->exports.front().symbol = "zz_out_of_order";
  }
  expect(!lang::verifyMirProgram(corruptedMir).valid(),
         "MIR verification should reject a forged export order");

  lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility = pipeline.run(
      frontend.hir, lang::OptimizationLevel::O2, lang::TargetInfo::host());
  lang::OptimizedProgram optimized =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O2,
                    .target = lang::TargetInfo::host(),
                    .compatibility = &compatibility});
  expect(optimized.valid() && optimized.sourceMir.embeddingPlan() ==
                                  optimized.mir.embeddingPlan(),
         "optimization should preserve the immutable embedding plan");
  if (!optimized.valid()) {
    return;
  }
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations,
      lang::TargetInfo::host());
  expect(lowered.valid() && lowered.program->embeddingPlan() &&
             lowered.program->embeddingPlan()->exports.size() == 2 &&
             lang::verifyLoweredProgram(*lowered.program).empty(),
         "the lowered program should expose only verified immutable ABI roots");
  if (lowered.valid()) {
    const std::string serialized =
        lang::LoweredProgramPrinter().print(*lowered.program);
    expect(serialized.starts_with("lowered-program-v17\n") &&
               serialized.find("embedding-export symbol=\"game_update_v1\"") !=
                   std::string::npos &&
               lang::MirPrinter()
                   .print(optimized.mir)
                   .starts_with("mir-v100 valid=1\n"),
           "library-only serialization should retain canonical export facts");
  }

  const lang::FrontendResult reversed =
      analyze("embedding-reversed.gti", embeddingFixtureSource(),
              libraryRequest({{"game::initialize", "game_initialize_v1"},
                              {"game::update", "game_update_v1"}}));
  const auto publicRows = [](const auto &plan) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (plan) {
      for (const auto &selected : plan->exports) {
        rows.emplace_back(selected.symbol, selected.qualifiedName);
      }
    }
    return rows;
  };
  expect(reversed.canGenerateCode() &&
             publicRows(reversed.semantics.embeddingPlan()) ==
                 publicRows(frontend.semantics.embeddingPlan()) &&
             publicRows(reversed.hir.embeddingPlan()) ==
                 publicRows(frontend.hir.embeddingPlan()) &&
             publicRows(reversed.mir.embeddingPlan()) ==
                 publicRows(frontend.mir.embeddingPlan()),
         "public export serialization should not depend on request order");

  const lang::FrontendResult reordered = analyze(
      "embedding-declarations-reordered.gti", reorderedEmbeddingFixtureSource(),
      libraryRequest({{"game::update", "game_update_v1"},
                      {"game::initialize", "game_initialize_v1"}}));
  expect(reordered.canGenerateCode() &&
             publicRows(reordered.semantics.embeddingPlan()) ==
                 publicRows(frontend.semantics.embeddingPlan()) &&
             publicRows(reordered.hir.embeddingPlan()) ==
                 publicRows(frontend.hir.embeddingPlan()) &&
             publicRows(reordered.mir.embeddingPlan()) ==
                 publicRows(frontend.mir.embeddingPlan()),
         "public export serialization should not depend on source declaration "
         "or discovery order");
}

void expectSemanticRejection(std::string_view label, std::string source,
                             lang::ProgramRequest request,
                             std::string_view message = {},
                             bool standardLibrary = false) {
  const lang::FrontendResult result =
      standardLibrary
          ? analyzeWithStandardLibrary(std::string(label) + ".gti",
                                       std::move(source), std::move(request))
          : analyze(std::string(label) + ".gti", std::move(source),
                    std::move(request));
  const bool rejected =
      !result.semanticValid && hasCode(result.diagnostics, "GTI-S2104") &&
      (message.empty() || hasMessage(result.diagnostics, message));
  if (!rejected) {
    std::cerr << "diagnostics for " << label << ":\n";
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                << '\n';
    }
  }
  expect(rejected, std::string("embedding semantics should reject ") +
                       std::string(label));
}

void testSemanticRejections() {
  expectSemanticRejection("empty export list", "int32_t value() { return 0; }",
                          libraryRequest({}), "at least one");
  expectSemanticRejection(
      "hosted main", "int32_t value() { return 0; } int main() { return 0; }",
      libraryRequest({{"value", "value_v1"}}), "cannot define");
  expectSemanticRejection("missing function", "int32_t value() { return 0; }",
                          libraryRequest({{"missing", "missing_v1"}}),
                          "No GTI function");
  expectSemanticRejection("overload set",
                          "int32_t pick(int32_t v) { return v; } "
                          "int64_t pick(int64_t v) { return v; }",
                          libraryRequest({{"pick", "pick_v1"}}), "overloaded");
  expectSemanticRejection(
      "method", "struct Item { int32_t read(int32_t v) { return v; } };",
      libraryRequest({{"Item::read", "item_read_v1"}}), "namespace-scope");
  expectSemanticRejection(
      "generic function", "T identity<T>(T value) { return value; }",
      libraryRequest({{"identity", "identity_v1"}}), "non-generic");
  expectSemanticRejection("declaration without body", "int32_t declared();",
                          libraryRequest({{"declared", "declared_v1"}}),
                          "with a body");
  expectSemanticRejection("ordinary class boundary",
                          "struct Value { int32_t value = 0; }; "
                          "Value read(Value value) { return value; }",
                          libraryRequest({{"read", "read_v1"}}),
                          "type outside");
  expectSemanticRejection(
      "reference boundary", "int32_t read(int32_t& value) { return value; }",
      libraryRequest({{"read", "read_v1"}}), "type outside");
  expectSemanticRejection("mutable parameter boundary",
                          "int32_t read(mut int32_t value) { return value; }",
                          libraryRequest({{"read", "read_v1"}}),
                          "type outside");
  expectSemanticRejection(
      "fixed array boundary",
      "int32_t read(int32_t values[2]) { return values[0]; }",
      libraryRequest({{"read", "read_v1"}}), "type outside");
  expectSemanticRejection("nested-pointer boundary",
                          "[[c_opaque]] struct Handle; "
                          "int32_t read(Handle** value) { return 0; }",
                          libraryRequest({{"read", "read_v1"}}),
                          "type outside");
  expectSemanticRejection(
      "boolean boundary", "bool read(bool value) { return value; }",
      libraryRequest({{"read", "read_v1"}}), "type outside");
  const auto nativeLong = lang::SemanticType::nativeCScalarType(
      lang::NativeCScalarKind::Long, lang::TargetInfo::host());
  const std::string distinctNative =
      nativeLong && nativeLong->nativeCScalar != lang::NativeCScalarKind::None
          ? "c_long"
          : "c_longlong";
  expectSemanticRejection(
      "distinct native scalar boundary",
      distinctNative + " read(" + distinctNative + " value) { return value; }",
      libraryRequest({{"read", "read_v1"}}), "type outside", true);
  expectSemanticRejection(
      "c string boundary", "c_string read(c_string value) { return value; }",
      libraryRequest({{"read", "read_v1"}}), "type outside", true);
  expectSemanticRejection("owning string boundary",
                          "#include <std/string>\n"
                          "int32_t read(std::string value) { return 0; }",
                          libraryRequest({{"read", "read_v1"}}), "type outside",
                          true);
  expectSemanticRejection(
      "container boundary",
      "#include <std/vector>\n"
      "int32_t read(std::vector<int32_t> values) { return 0; }",
      libraryRequest({{"read", "read_v1"}}), "type outside", true);
  expectSemanticRejection(
      "owner boundary",
      "#include <std/memory>\n"
      "int32_t read(std::unique_ptr<int32_t> value) { return 0; }",
      libraryRequest({{"read", "read_v1"}}), "type outside", true);
  expectSemanticRejection(
      "expected boundary",
      "int32_t read(expected<int32_t, int32_t> value) { return 0; }",
      libraryRequest({{"read", "read_v1"}}), "type outside", true);
  expectSemanticRejection("payload enum boundary",
                          "enum class Maybe { none, some(int32_t value) }; "
                          "int32_t read(Maybe value) { return 0; }",
                          libraryRequest({{"read", "read_v1"}}),
                          "type outside");
  expectSemanticRejection("callable boundary",
                          "using Unary = (int32_t) -> int32_t; "
                          "int32_t read(Unary value) { return 0; }",
                          libraryRequest({{"read", "read_v1"}}),
                          "type outside");
  expectSemanticRejection("duplicate symbol",
                          "int32_t a() { return 0; } int32_t b() { return 0; }",
                          libraryRequest({{"a", "same_v1"}, {"b", "same_v1"}}),
                          "selected more than once");
  expectSemanticRejection("duplicate function", "int32_t a() { return 0; }",
                          libraryRequest({{"a", "a_v1"}, {"a", "a_again_v1"}}),
                          "selected for export more than once");
  expectSemanticRejection("reserved symbol", "int32_t a() { return 0; }",
                          libraryRequest({{"a", "gti_context_create_v1"}}),
                          "non-reserved");
  expectSemanticRejection("C keyword symbol", "int32_t a() { return 0; }",
                          libraryRequest({{"a", "restrict"}}), "non-reserved");
  expectSemanticRejection("C++ keyword symbol", "int32_t a() { return 0; }",
                          libraryRequest({{"a", "module"}}), "non-reserved");
  expectSemanticRejection("non-ASCII symbol", "int32_t a() { return 0; }",
                          libraryRequest({{"a", "caf\xC3\xA9"}}),
                          "non-reserved");
  expectSemanticRejection(
      "process-lifetime callback adapter",
      "using Unary = (int32_t) -> int32_t; "
      "int32_t add(int32_t value) { return value + 1; } "
      "int32_t install() { Unary callback = add; return 0; }",
      libraryRequest({{"install", "install_v1"}}), "callback adapters");

  lang::ProgramRequest executable;
  executable.exports.push_back({.function = "a", .symbol = "a_v1"});
  expectSemanticRejection("exports on executable", "int32_t a() { return 0; }",
                          std::move(executable), "Executable programs");

  lang::ProgramRequest anchored = libraryRequest({{"a", "restrict"}});
  anchored.exports.front().declaration = {"gti.toml", 80, 128, 7};
  anchored.exports.front().functionDeclaration = {"gti.toml", 93, 96, 8};
  anchored.exports.front().symbolDeclaration = {"gti.toml", 106, 116, 9};
  const lang::FrontendResult anchoredResult =
      analyze("anchored.gti", "int32_t a() { return 0; }", anchored);
  const lang::Diagnostic *reserved =
      findMessage(anchoredResult.diagnostics, "not a non-reserved");
  expect(reserved != nullptr && reserved->code == "GTI-S2104" &&
             reserved->primary == anchored.exports.front().symbolDeclaration &&
             !reserved->hints.empty() && reserved->fixes.empty(),
         "embedding diagnostics should retain the exact manifest field span, "
         "guidance, and no intent-changing fix-it");
}

std::string manifest(std::string_view target) {
  return "manifest-version = 1\n\n"
         "[package]\n"
         "name = \"gameplay\"\n"
         "version = \"0.1.0\"\n\n" +
         std::string(target);
}

constexpr std::string_view validLibraryTarget = R"(
[targets.gameplay]
kind = "library"
linkage = "shared"
root = "src/gameplay.gti"

[[targets.gameplay.exports]]
function = "game::update"
symbol = "game_update_v1"

[[targets.gameplay.exports]]
function = "game::initialize"
symbol = "game_initialize_v1"
)";

void testDormantDriverModel() {
  TemporaryDirectory temporary;
  const std::filesystem::path manifestPath = temporary.root() / "gti.toml";
  expect(writeFile(temporary.root() / "src/gameplay.gti",
                   embeddingFixtureSource()) &&
             writeFile(manifestPath, manifest(validLibraryTarget)),
         "embedding driver fixture should be created");

  const lang::driver::ManifestLoadResult released =
      lang::driver::loadProjectManifest(manifestPath);
  expect(!released.succeeded() && hasCode(released.diagnostics, "GTI-B2000"),
         "released manifest loading should keep library targets dormant");

  const lang::driver::ManifestFeaturePolicy enabled{.dormantLibraryTargets =
                                                        true};
  const lang::driver::ManifestLoadResult loaded =
      lang::driver::loadProjectManifest(manifestPath, std::nullopt, enabled);
  const lang::driver::ProjectTarget *target =
      loaded.succeeded() ? loaded.manifest->findTarget("gameplay") : nullptr;
  expect(target != nullptr &&
             target->kind == lang::driver::ProjectTargetKind::Library &&
             target->linkage == lang::driver::ProjectTargetLinkage::Shared &&
             target->exports.size() == 2,
         "internal manifest loading should retain linkage and export spans");

  std::string overlayText = manifest(validLibraryTarget);
  overlayText.replace(overlayText.find("game_update_v1"),
                      std::string("game_update_v1").size(),
                      "overlay_update_v1");
  const lang::driver::WorkspaceManifestOverlay overlay{manifestPath,
                                                       overlayText};
  const auto overlayWorkspace = lang::driver::resolveProjectWorkspace(
      temporary.root(), std::nullopt, {.allowAcquisition = false}, overlay,
      enabled);
  const auto *overlayTarget =
      overlayWorkspace.succeeded()
          ? overlayWorkspace.workspace->selectedPackage().manifest.findTarget(
                "gameplay")
          : nullptr;
  expect(overlayTarget != nullptr && overlayTarget->exports.size() == 2 &&
             overlayTarget->exports.front().symbol == "overlay_update_v1" &&
             readFile(manifestPath) == manifest(validLibraryTarget),
         "dormant feature policy and dirty manifest overlays must compose "
         "without disk writes");
  const auto disabledOverlay = lang::driver::resolveProjectWorkspace(
      temporary.root(), std::nullopt, {.allowAcquisition = false}, overlay);
  expect(!disabledOverlay.succeeded() &&
             hasCode(disabledOverlay.diagnostics, "GTI-B2000"),
         "a manifest overlay cannot activate public library targets");

  const lang::driver::ProjectResolutionResult resolved =
      lang::driver::resolveProjectBuild(lang::driver::ProjectBuildRequest(
          temporary.root(), std::optional<std::string>{"gameplay"}, "dev",
          lang::TargetInfo::host(), {}, std::nullopt,
          {.requireLock = true, .allowAcquisition = false},
          lang::driver::RuntimeProfile::None,
          lang::driver::ToolchainSelectionPolicy::HostTools, {}, enabled));
  const lang::driver::ProjectBuildPlan *plan =
      resolved.succeeded() ? &*resolved.plan : nullptr;
  const lang::ProgramRequest program =
      plan == nullptr ? lang::ProgramRequest{} : plan->programRequest();
  expect(plan != nullptr &&
             plan->targetKind() == lang::driver::ProjectTargetKind::Library &&
             plan->targetLinkage() ==
                 lang::driver::ProjectTargetLinkage::Shared &&
             plan->exports().size() == 2 &&
             plan->output().filename().string().starts_with("libgameplay.") &&
             plan->projectModelIdentity().find("program-kind:library") !=
                 std::string::npos &&
             plan->projectModelIdentity().find(
                 "export:game_initialize_v1=game::initialize") !=
                 std::string::npos &&
             program.kind == lang::ProgramKind::Library &&
             program.exports.size() == 2,
         "the build plan and cache identity should retain dormant library "
         "policy");

  const lang::driver::ProjectFactsResult facts =
      lang::driver::resolveProjectFacts(temporary.root(), std::nullopt,
                                        enabled);
  expect(facts.succeeded() && facts.facts->targets().size() == 1 &&
             facts.facts->targets().front().linkage ==
                 lang::driver::ProjectTargetLinkage::Shared &&
             facts.facts->targets().front().exports.size() == 2,
         "read-only project facts should expose the same library request");

  const lang::driver::ProjectTestResolutionResult tests =
      lang::driver::resolveProjectTests(lang::driver::ProjectBuildRequest(
          temporary.root(), std::optional<std::string>{"gameplay"}, "dev",
          lang::TargetInfo::host(), {}, std::nullopt,
          {.requireLock = true, .allowAcquisition = false},
          lang::driver::RuntimeProfile::None,
          lang::driver::ToolchainSelectionPolicy::HostTools, {}, enabled));
  expect(!tests.succeeded() && hasCode(tests.diagnostics, "GTI-B1204"),
         "the test command should reject a library target explicitly");

  if (plan != nullptr) {
    const std::filesystem::path repository =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    const lang::driver::CheckResult checked =
        lang::driver::checkCompilation(lang::driver::CompilationRequest(
            plan->entry(), lang::standardLibraryLayout(repository / "stdlib"),
            plan->target(), plan->optimization(), plan->cppStandard(),
            plan->packageSources(), plan->configurationFlags(), {}, {}, false,
            {}, lang::RawPointerAuditMode::Off, plan->programRequest()));
    expect(checked.succeeded() && checked.diagnostics.empty(),
           "internal gti check should analyze a no-main library plan");
  }

  expect(writeFile(manifestPath, manifest(R"(
[targets.gameplay]
kind = "library"
root = "src/gameplay.gti"
[[targets.gameplay.exports]]
function = "game::update"
symbol = "game_update_v1"
)")) && !lang::driver::loadProjectManifest(manifestPath, std::nullopt, enabled)
                 .succeeded(),
         "a library target without linkage should fail manifest validation");
  expect(writeFile(manifestPath, manifest(R"(
[targets.gameplay]
kind = "library"
linkage = "static"
root = "src/gameplay.gti"
[[targets.gameplay.exports]]
function = "game::update"
symbol = "game_update_v1"
)")) && hasCode(lang::driver::loadProjectManifest(manifestPath, std::nullopt,
                                                  enabled)
                    .diagnostics,
                "GTI-B2000"),
         "unknown library linkage should fail with the embedding diagnostic");
  expect(writeFile(manifestPath, manifest(R"(
[targets.gameplay]
kind = "library"
linkage = "shared"
root = "src/gameplay.gti"
)")) && !lang::driver::loadProjectManifest(manifestPath, std::nullopt, enabled)
                 .succeeded(),
         "a library target without exports should fail manifest validation");
}

} // namespace

int main() {
  testSemanticAndLoweringRoots();
  testSemanticRejections();
  testDormantDriverModel();
  if (failures != 0) {
    std::cerr << failures << " embedding foundation test(s) failed\n";
    return 1;
  }
  std::cout << "embedding foundation tests passed\n";
  return 0;
}
