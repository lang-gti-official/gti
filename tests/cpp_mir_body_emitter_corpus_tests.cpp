#include "cpp_mir_body_emitter_test_fixture.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>

namespace {

// Optional evidence only: admission, both sweeps and their assertions remain
// unchanged. Flush at bounded checkpoints so a CTest timeout retains the tail.
class CorpusProgress {
public:
  CorpusProgress(const std::filesystem::path &source, std::string_view sweep)
      : source_(source.filename().string()), sweep_(sweep),
        enabled_(std::getenv("GTI_CPP_MIR_CORPUS_PROGRESS") != nullptr),
        started_(std::chrono::steady_clock::now()) {
    checkpoint("start");
  }

  void checkpoint(std::string_view stage, std::size_t bodies = 0,
                  std::size_t programVerifications = 0) const {
    if (!enabled_)
      return;
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started_)
                               .count();
    std::cerr << "GTI_CPP_MIR_CORPUS_PROGRESS_V1 source="
              << std::quoted(source_) << " sweep=" << sweep_
              << " stage=" << stage << " seconds=" << seconds
              << " bodies=" << bodies
              << " program_verifications=" << programVerifications << std::endl;
  }

private:
  std::string source_;
  std::string_view sweep_;
  bool enabled_;
  std::chrono::steady_clock::time_point started_;
};

bool sameAnalysis(const lang::CppMirBodyEmissionAnalysis &left,
                  const lang::CppMirBodyEmissionAnalysis &right) {
  return left.body == right.body && left.readiness == right.readiness &&
         left.issues.size() == right.issues.size() &&
         std::equal(left.issues.begin(), left.issues.end(),
                    right.issues.begin(),
                    [](const lang::CppMirBodyEmissionIssue &left,
                       const lang::CppMirBodyEmissionIssue &right) {
                      return left.kind == right.kind &&
                             left.body == right.body &&
                             left.block == right.block &&
                             left.instruction == right.instruction &&
                             left.detail == right.detail;
                    });
}

void testExampleCorpusEmissionReadiness() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::vector<std::filesystem::path> sources = exampleCorpusSources();
  expect(!sources.empty(), "the example corpus should be discoverable");

  std::size_t rejected = 0;
  std::size_t bodies = 0;
  std::size_t ready = 0;
  std::size_t incoherent = 0;
  std::size_t invalidIssues = 0;
  for (const std::filesystem::path &source : sources) {
    const CorpusProgress progress(source, "readiness");
    std::ifstream input(source);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::filesystem::path standardLibrary = repository / "stdlib";
    lang::TargetInfo target = lang::TargetInfo::host();
    if (source.filename() == "066-managed-threads.gti" ||
        source.filename() == "076-scalar-atomics.gti") {
      target.executionProfile = lang::ExecutionProfile::Concurrent;
      target.runtimeCapabilities.threads = true;
    }
    const lang::FrontendResult frontend =
        lang::Frontend({.target = target})
            // Preserve source identity so quoted sibling includes resolve
            // exactly as they do when compiling this example from the CLI.
            .analyze(source.string(), buffer.str(),
                     {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
    progress.checkpoint("frontend");
    if (!frontend.canGenerateCode()) {
      ++rejected;
      std::cerr << "corpus source no longer reaches MIR: "
                << source.filename().string() << '\n';
      for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
        std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                  << '\n';
      }
      continue;
    }
    lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
    progress.checkpoint("complete-rows");
    const lang::CppMirBodyEmissionMapRows productionRows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23, target);
    progress.checkpoint("production-rows");
    const lang::CppMirBodyEmissionMap productionMap(productionRows);
    const auto productionPlan =
        lang::CppMirBodyEmitter(frontend.mir, productionMap).buildProgramPlan();
    progress.checkpoint("production-plan");
    for (const auto &body : productionPlan.analysis().bodies) {
      if (body.ready())
        continue;
      for (const auto &issue : body.issues) {
        std::cerr << "production plan issue in " << source.filename().string()
                  << " kind=" << static_cast<int>(issue.body.kind)
                  << " owner=" << issue.body.owner << ": " << issue.detail
                  << '\n';
      }
    }
    rows.enums = productionRows.enums;
    for (const lang::CppMirEnumRepresentation &enumeration : rows.enums) {
      addType(rows, enumeration.underlyingType);
      for (const lang::CppMirPayloadVariantRepresentation &variant :
           enumeration.payloadVariants) {
        for (const lang::SemanticType &field : variant.fieldTypes) {
          addType(rows, field);
        }
      }
    }
    const lang::CppMirBodyEmissionMap map(std::move(rows));
    lang::CppMirBodyEmissionMetrics metrics;
    const lang::CppMirBodyEmitter emitter(frontend.mir, map, &metrics);
    const auto addresses = lang::enumerateMirBodyAddresses(frontend.mir);
    const lang::CppMirProgramEmissionAnalysis batch =
        emitter.analyzeProgramStructure();
    progress.checkpoint("batch-analysis", batch.bodies.size(),
                        metrics.programVerifications);
    expect(batch.issues.empty(),
           "corpus structural batch must retain every global rejection");
    expect(batch.bodies.size() == addresses.size(),
           "structural batch must cover the exact complete body census");
    for (const auto &issue : batch.issues) {
      std::cerr << "global structural batch issue in "
                << source.filename().string() << ": " << issue.detail << '\n';
    }
    std::size_t sourceBodies = 0;
    for (const lang::CppMirBodyEmissionAnalysis &analysis : batch.bodies) {
      expect(sourceBodies < addresses.size() &&
                 analysis.body == addresses[sourceBodies],
             "structural batch must retain exact body identities and order");
      ++sourceBodies;
      const lang::MirBodyAddress address = analysis.body;
      if (std::getenv("GTI_CPP_MIR_CORPUS_COMPARE_BATCH") != nullptr) {
        expect(sameAnalysis(analysis, emitter.analyze(address)),
               "structural batch must exactly match standalone readiness and "
               "every issue identity/detail");
      }
      ++bodies;
      if (bodies % 100 == 0) {
        progress.checkpoint("body-analysis", bodies,
                            metrics.programVerifications);
      }
      if (analysis.ready()) {
        ++ready;
      } else {
        std::cerr << "not-ready body in " << source.filename().string()
                  << " kind=" << static_cast<int>(address.kind)
                  << " owner=" << address.owner;
        if (analysis.issues.empty()) {
          std::cerr << ": <no issue>\n";
        } else {
          std::cerr << ":\n";
          for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
            std::cerr << "  " << static_cast<int>(issue.kind) << ": "
                      << issue.detail << '\n';
          }
        }
      }
      if (analysis.readiness == lang::CppMirBodyEmissionReadiness::Incoherent) {
        ++incoherent;
        std::cerr << "incoherent body in " << source.filename().string()
                  << " kind=" << static_cast<int>(address.kind)
                  << " owner=" << address.owner << ": "
                  << (analysis.issues.empty() ? std::string("<no issue>")
                                              : analysis.issues.front().detail)
                  << '\n';
      }
      for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
        switch (issue.kind) {
        case lang::CppMirBodyEmissionIssueKind::InvalidMirProgram:
        case lang::CppMirBodyEmissionIssueKind::InvalidBodyAddress:
        case lang::CppMirBodyEmissionIssueKind::InvalidRepresentationEnum:
        case lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow:
        case lang::CppMirBodyEmissionIssueKind::
            DuplicateContainedConstructorRepresentation:
        case lang::CppMirBodyEmissionIssueKind::InvalidBodyKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidInstructionKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidOperation:
        case lang::CppMirBodyEmissionIssueKind::InvalidOperandKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidPlaceRootKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidProjectionKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidTerminatorKind:
          ++invalidIssues;
          std::cerr << "invalid-shape issue in " << source.filename().string()
                    << ": " << issue.detail << '\n';
          break;
        default:
          break;
        }
      }
    }
    progress.checkpoint("complete", bodies, metrics.programVerifications);
  }

  std::cout << "GTI_CPP_MIR_CORPUS_READINESS_V1 sources=" << sources.size()
            << " rejected=" << rejected << " bodies=" << bodies
            << " ready=" << ready << " incoherent=" << incoherent
            << " invalid=" << invalidIssues << '\n';

  expect(rejected == 0,
         "every shipped example should still reach verified MIR; a rejected "
         "source silently inflates every readiness figure");
  expect(incoherent == 0,
         "no frontend-produced body should be structurally incoherent to the "
         "generic emission gate");
  expect(invalidIssues == 0,
         "frontend-produced MIR should raise no invalid-shape emission issue");
  if (!corpusSource.has_value()) {
    // A floor rather than an exact count: the gate protects against regression
    // without forcing an update on every representation improvement.
    expect(ready >= 1900,
           "generic emission readiness across the example corpus should not "
           "regress below its recorded floor");
  }
}

// ADR 016 phases 4-5 agreement gate: the production rows builder plus the
// general text step must reproduce, byte for byte, every plain and
// failure-propagating scalar-cfg body production publishes, for every shipped
// example. The artifact's own markers select the bodies, so this gate tracks
// production admission without modeling the selectors. It also pins the
// complete direct-CFG representation across that corpus.

void testGeneralTextStepMatchesProductionEmission() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::vector<std::filesystem::path> sources = exampleCorpusSources();
  expect(!sources.empty(), "the example corpus should be discoverable");

  const std::string_view markerPrefix = "// GTI verified-MIR body: ";
  std::size_t generalBodies = 0;
  std::size_t matchedBodies = 0;
  bool deterministic = true;
  bool intrinsicBodies = false;
  bool checkedIntrinsicBodies = false;
  bool loanBodies = false;
  bool directControlFlowBodies = true;
  for (const std::filesystem::path &source : sources) {
    const CorpusProgress progress(source, "agreement");
    std::ifstream input(source);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::filesystem::path standardLibrary = repository / "stdlib";
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        source.string(), buffer.str(), {standardLibrary / "prelude.gti"}, {},
        {standardLibrary});
    progress.checkpoint("frontend");
    if (!frontend.canGenerateCode()) {
      continue;
    }
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(frontend.hir,
                                         lang::OptimizationLevel::O0);
    progress.checkpoint("optimization");
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
    progress.checkpoint("production-artifact");

    lang::CppMirBodyEmissionMapRows rows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    progress.checkpoint("production-rows-first");
    const lang::CppMirBodyEmissionMapRows again =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    progress.checkpoint("production-rows-again");
    deterministic =
        deterministic && rows.types == again.types &&
        rows.bodies == again.bodies && rows.symbols == again.symbols &&
        rows.enums == again.enums && rows.capabilities == again.capabilities;

    const lang::CppMirBodyEmissionMap map(std::move(rows));
    lang::CppMirBodyEmissionMetrics metrics;
    const lang::CppMirBodyEmitter emitter(frontend.mir, map, &metrics);
    const std::string &generated = artifact.contents;
    const lang::CppMirProgramEmissionPlan batchPlan =
        emitter.buildProgramPlan();
    progress.checkpoint("batch-text-plan", batchPlan.bodies().size(),
                        metrics.programVerifications);
    // This detached map intentionally selects production scalar-CFG markers;
    // unrelated callable-template rows need not have complete text admission.
    // Every selected body still requires exact admission/text matching below.
    expect(batchPlan.issues().empty(),
           "agreement text plan must retain global validation; each selected "
           "body must retain exact admission and text matching");
    for (std::size_t position = generated.find(markerPrefix);
         position != std::string::npos;
         position = generated.find(markerPrefix, position + 1)) {
      const std::size_t labelStart = position + markerPrefix.size();
      const std::size_t labelEnd = generated.find(' ', labelStart);
      if (labelEnd == std::string::npos) {
        continue;
      }
      const std::string label =
          generated.substr(labelStart, labelEnd - labelStart);
      const bool failureForm = label == "scalar-cfg-failure-v1";
      if (label != "scalar-cfg-v1" && !failureForm) {
        continue;
      }
      const std::string_view instanceMarker = "function-instance ";
      const std::size_t instanceStart =
          generated.find(instanceMarker, labelEnd);
      const std::size_t lineEnd = generated.find('\n', labelEnd);
      if (instanceStart == std::string::npos || lineEnd == std::string::npos ||
          instanceStart > lineEnd) {
        continue;
      }
      ++generalBodies;
      const std::size_t instance = static_cast<std::size_t>(std::stoull(
          generated.substr(instanceStart + instanceMarker.size(),
                           lineEnd - instanceStart - instanceMarker.size())));
      const std::size_t lineStart = generated.rfind('\n', position) + 1;
      const std::size_t markerIndent = position - lineStart;
      if (markerIndent < 2 || markerIndent % 2 != 0) {
        continue;
      }
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = instance};
      const lang::CppMirBodyEmissionPlan *plan = batchPlan.find(address);
      expect(plan != nullptr,
             "agreement must retain every production-selected body identity");
      const lang::CppMirBodyEmissionText emission =
          plan == nullptr
              ? lang::CppMirBodyEmissionText{}
              : (failureForm ? emitter.emitFailureBodyText(*plan, label,
                                                           markerIndent / 2 - 1)
                             : emitter.emitBodyText(*plan, label,
                                                    markerIndent / 2 - 1));
      if (std::getenv("GTI_CPP_MIR_CORPUS_COMPARE_BATCH") != nullptr) {
        const lang::CppMirBodyEmissionText standalone =
            failureForm
                ? emitter.emitFailureBodyText(address, label,
                                              markerIndent / 2 - 1)
                : emitter.emitBodyText(address, label, markerIndent / 2 - 1);
        expect(emission.text == standalone.text &&
                   sameAnalysis(emission.analysis, standalone.analysis),
               "immutable planned text must exactly match standalone text and "
               "every admission issue");
      }
      if (generalBodies % 100 == 0) {
        progress.checkpoint("general-text", generalBodies,
                            metrics.programVerifications);
      }
      if (emission.text.find("::gti_internal::backend::wrapping_add(") !=
              std::string::npos ||
          emission.text.find("::gti_internal::backend::saturating_sub(") !=
              std::string::npos) {
        intrinsicBodies = true;
      }
      if (emission.text.find("__gti_mir_loan_") != std::string::npos &&
          (emission.text.find("return *__gti_mir_loan_") != std::string::npos ||
           emission.text.find("__gti_mir_out_result = __gti_mir_loan_") !=
               std::string::npos)) {
        loanBodies = true;
      }
      if (emission.text.find("checked_add") != std::string::npos ||
          emission.text.find("checked_sub") != std::string::npos ||
          emission.text.find("checked_mul") != std::string::npos) {
        checkedIntrinsicBodies = true;
      }
      directControlFlowBodies =
          directControlFlowBodies &&
          emission.text.find("switch (__gti_mir_bb)") == std::string::npos &&
          emission.text.find("std::size_t __gti_mir_bb") == std::string::npos &&
          emission.text.find("__gti_mir_bb =") == std::string::npos &&
          emission.text.find("goto __gti_mir_bb_") != std::string::npos &&
          emission.text.find("__gti_mir_bb_1: {") != std::string::npos;
      if (emission.emitted() &&
          generated.find(emission.text) != std::string::npos) {
        ++matchedBodies;
      } else {
        std::cerr << "general text step disagreed for " << label
                  << " function-instance " << instance << " in "
                  << source.filename().string() << '\n';
      }
    }
    progress.checkpoint("complete", generalBodies,
                        metrics.programVerifications);
  }

  std::cout << "GTI_CPP_MIR_CORPUS_AGREEMENT_V1 sources=" << sources.size()
            << " general=" << generalBodies << " matched=" << matchedBodies
            << " deterministic=" << deterministic
            << " intrinsic=" << intrinsicBodies
            << " checked_intrinsic=" << checkedIntrinsicBodies
            << " loan=" << loanBodies << " direct=" << directControlFlowBodies
            << '\n';
  expect(deterministic,
         "the production rows builder should be run-to-run deterministic");
  expect(directControlFlowBodies,
         "every production MIR body should use verified direct labels and "
         "gotos without a mutable block dispatcher");
  expect(matchedBodies == generalBodies,
         "every production scalar-cfg body must byte-match the "
         "general text step's emission");
  if (!corpusSource.has_value()) {
    expect(loanBodies,
           "the corpus should emit at least one loan-erasure body (ADR 018): "
           "a pointer loan local, an address-of borrow, and a dereferenced "
           "loan return");
    expect(intrinsicBodies,
           "the corpus should emit at least one wrapping/saturating intrinsic "
           "helper call from verified MIR");
    expect(checkedIntrinsicBodies,
           "the corpus should emit at least one checked-result intrinsic "
           "helper call from verified MIR now that Expected sums joined the "
           "signature boundary (0.183.0)");
    expect(generalBodies >= 30,
           "the corpus should keep exercising the migrated per-body families");
  }
}

// The production rows now carry namespace-global storage names,
// constructor/destructor body names, and the sealed lifetime-slot helper,
// so every non-entry scalar function body of the class-default-cleanup
// fixture is analysis-Ready; its constructor and destructor bodies stay
// gated by the structural rollback/double-failure proofs, and the text
// vocabulary still declines Construct/Drop, so production emission of the
// family is unchanged until those land.

constexpr NamedTest kTests[] = {
    {"testExampleCorpusEmissionReadiness", testExampleCorpusEmissionReadiness},
    {"testGeneralTextStepMatchesProductionEmission",
     testGeneralTextStepMatchesProductionEmission},
};

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--corpus-source") {
    corpusSource = std::filesystem::path(argv[2]);
    for (const NamedTest &test : kTests) {
      test.run();
    }
    return failures == 0 ? 0 : 1;
  }
  return runNamedTests(argc, argv, kTests, "corpus");
}
