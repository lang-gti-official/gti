// libFuzzer harness for the whole GTI analysis-to-LLVM pipeline
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// The pipeline mirrors the driver's LLVM path (detail::generateArtifact):
// frontend, MIR optimizer, lowered-program builder, then the LLVM backend at
// the codegen level the driver maps from the GTI optimization level. The first
// input byte selects -O0..-O3.
//
// Every bounded input is analyzed twice through fresh production snapshots.
// Acceptance, complete diagnostics, cross-phase facts, optimization state,
// classified backend limitations, the `--emit-mir` artifact, the
// `--emit-llvm` textual module, and the native object must be deterministic.
// Every diagnostic range is checked against the SourceManager that owns it.
// An accepted input must either produce a nonempty MIR artifact, textual LLVM
// IR, and one nonempty object, or carry a classified bounded LLVM limitation
// (the driver's GTI-B0003). A lowered-program construction failure or any
// other backend exception is the driver's internal compiler error GTI-B0001
// and therefore a fuzz failure. The textual module is re-parsed in a fresh
// LLVMContext and must pass the LLVM verifier, as an `--emit-llvm` consumer
// would see it. Linking and execution remain out of process.

#include "fuzz_support.h"

#include "gti/backend.h"
#include "gti/frontend.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_backend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/report.h"
#include "gti/optimizer.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Observation {
  bool accepted = false;
  std::size_t diagnosticCount = 0;
  std::string diagnostics;
  std::string phaseFacts;
  std::string disposition;
  std::string rejection;
  std::string mirArtifact;
  std::string llvmIr;
  std::string llvmObject;
};

// A syntactically valid function whose use precedes its definition: it parses,
// so the negative perturbation reaches the verifier rather than the parser.
constexpr std::string_view kInjectedInvalidIr =
    "\ndefine i32 @gti_fuzz_injected_verifier() {\n"
    "entry:\n"
    "  %use = add i32 %def, 1\n"
    "  %def = add i32 1, 1\n"
    "  ret i32 %use\n"
    "}\n";

std::string_view levelName(lang::OptimizationLevel level) {
  switch (level) {
  case lang::OptimizationLevel::O0:
    return "O0";
  case lang::OptimizationLevel::O1:
    return "O1";
  case lang::OptimizationLevel::O2:
    return "O2";
  case lang::OptimizationLevel::O3:
    return "O3";
  }
  return "invalid";
}

// The driver's mapping (llvmCodegenOptimization in src/driver/compilation.cpp).
lang::LlvmCodegenOptimization codegenLevel(lang::OptimizationLevel level) {
  switch (level) {
  case lang::OptimizationLevel::O0:
    return lang::LlvmCodegenOptimization::None;
  case lang::OptimizationLevel::O1:
    return lang::LlvmCodegenOptimization::Less;
  case lang::OptimizationLevel::O2:
    return lang::LlvmCodegenOptimization::Default;
  case lang::OptimizationLevel::O3:
    return lang::LlvmCodegenOptimization::Aggressive;
  }
  gti_fuzz::fail("pipeline", "unknown optimization level");
}

void verifyTextualIr(std::string ir, bool injectInvalid) {
  if (injectInvalid) {
    ir += kInjectedInvalidIr;
  }
  llvm::LLVMContext context;
  llvm::SMDiagnostic parseError;
  std::unique_ptr<llvm::Module> module =
      llvm::parseAssemblyString(ir, parseError, context);
  if (!module) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    parseError.print("fuzz.ll", stream);
    std::fprintf(stderr, "gti-fuzz(pipeline): %s\n", message.c_str());
    gti_fuzz::fail("pipeline", "LLVM IR artifact does not parse");
  }
  std::string message;
  llvm::raw_string_ostream stream(message);
  if (llvm::verifyModule(*module, &stream)) {
    std::fprintf(stderr, "gti-fuzz(pipeline): %s\n", message.c_str());
    gti_fuzz::fail("pipeline", "LLVM IR artifact fails the LLVM verifier");
  }
}

// Units without post-directive tokens snapshot as an empty sequence.
const std::vector<lang::Token> &tokensOf(const lang::SourceUnit &unit) {
  static const std::vector<lang::Token> empty;
  return unit.tokens ? *unit.tokens : empty;
}

void appendString(std::ostringstream &output, std::string_view value) {
  output << value.size() << ':' << value << ';';
}

void appendString(std::string &output, std::string_view value) {
  output += std::to_string(value.size());
  output += ':';
  output += value;
  output += ';';
}

void appendSpan(std::ostringstream &output, const lang::SourceSpan &span) {
  appendString(output, span.source);
  output << span.start << ',' << span.end << ',' << span.line << ';';
}

void checkSpan(const lang::SourceSpan &span,
               const lang::SourceManager &sources) {
  const std::string *source = sources.find(span.source);
  if (source == nullptr || span.start > span.end || span.end > source->size() ||
      span.line < 1) {
    gti_fuzz::fail("pipeline", "diagnostic range is outside its source");
  }
}

std::string diagnosticsSnapshot(const lang::FrontendResult &frontend) {
  std::ostringstream output;
  output << frontend.diagnostics.size() << ';';
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    checkSpan(diagnostic.primary, frontend.sources);
    appendString(output, diagnostic.code);
    output << static_cast<int>(diagnostic.phase) << ','
           << static_cast<int>(diagnostic.severity) << ';';
    appendSpan(output, diagnostic.primary);
    appendString(output, diagnostic.message);
    output << diagnostic.related.size() << ';';
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      checkSpan(related.span, frontend.sources);
      appendSpan(output, related.span);
      appendString(output, related.message);
    }
    output << diagnostic.fixes.size() << ';';
    for (const lang::FixIt &fix : diagnostic.fixes) {
      checkSpan(fix.span, frontend.sources);
      appendSpan(output, fix.span);
      appendString(output, fix.replacement);
      appendString(output, fix.message);
    }
    output << diagnostic.hints.size() << ';';
    for (const std::string &hint : diagnostic.hints) {
      appendString(output, hint);
    }
  }
  return output.str();
}

std::string sourceGraphSnapshot(const lang::SourceGraph &graph) {
  std::ostringstream output;
  output << graph.entryUnit() << ';' << graph.sourceUnits().size() << ';'
         << graph.dependencyEdges().size() << ';';
  for (const lang::SourceUnit &unit : graph.sourceUnits()) {
    output << unit.id << ';';
    appendString(output, unit.path.generic_string());
    output << unit.sourceDigest << ',' << unit.syntaxDigest << ','
           << unit.declarationStart << ',' << unit.declarationCount << ','
           << static_cast<int>(unit.role) << ',' << unit.entry << ','
           << unit.prelude << ';' << tokensOf(unit).size() << ';';
    for (const lang::Token &token : tokensOf(unit)) {
      output << static_cast<int>(token.kind) << ';';
      appendString(output, token.lexeme);
    }
    output << unit.inactiveSpans.size() << ','
           << unit.configurationTokens.size() << ';';
  }
  for (const lang::SourceDependency &edge : graph.dependencyEdges()) {
    output << edge.source << ',' << edge.target << ','
           << static_cast<int>(edge.kind) << ',' << edge.includeOccurrence
           << ';';
    appendString(output, edge.includeSpelling);
  }
  const std::vector<lang::SourceUnitId> order = graph.compilationOrder();
  output << order.size() << ';';
  for (const lang::SourceUnitId unit : order) {
    output << unit << ',';
  }
  return output.str();
}

std::string frontendPhaseSnapshot(const lang::FrontendResult &frontend) {
  std::ostringstream output;
  output << frontend.sourceValid << frontend.syntaxValid
         << frontend.semanticValid << frontend.hirValid
         << frontend.failureMetadataValid << frontend.mirValid << ';';
  appendString(output, sourceGraphSnapshot(frontend.sourceGraph));
  output << frontend.program.declarations().size() << ','
         << frontend.semantics.expressionCount() << ','
         << frontend.semantics.ownershipEventCount() << ','
         << frontend.semantics.fullExpressions().size() << ';';
  output << frontend.hir.valid() << ',' << frontend.hir.classInstances().size()
         << ',' << frontend.hir.enumDeclarations().size() << ','
         << frontend.hir.functionInstances().size() << ','
         << frontend.hir.constructorInstances().size() << ','
         << frontend.hir.destructorInstances().size() << ','
         << frontend.hir.lambdaInstances().size() << ','
         << frontend.hir.valueCount() << ',' << frontend.hir.statementCount()
         << ',' << frontend.hir.programInitializationPlan().steps.size() << ','
         << frontend.hir.hostedProgramEntryPlan().has_value() << ';';
  if (frontend.failureMetadataValid) {
    appendString(output, frontend.failureMetadata.artifactIdentity().hex());
    output << frontend.failureMetadata.descriptorBytes().size() << ','
           << frontend.failureMetadata.sites().size() << ','
           << frontend.failureMetadata.assignments().size() << ';';
  }
  if (frontend.mirValid) {
    appendString(output, lang::MirPrinter().print(frontend.mir));
  }
  return output.str();
}

std::string
loweredIssueSnapshot(const std::vector<lang::LoweredProgramIssue> &issues) {
  std::ostringstream output;
  output << issues.size() << ';';
  for (const lang::LoweredProgramIssue &issue : issues) {
    output << static_cast<int>(issue.kind) << ';';
    appendString(output, issue.detail);
    if (issue.body) {
      output << 'b' << static_cast<int>(issue.body->kind) << ','
             << issue.body->owner << ';';
    } else {
      output << "b-;";
    }
    output << (issue.declaration ? *issue.declaration : 0) << ';';
    if (issue.generatedItem) {
      output << 'g' << static_cast<int>(issue.generatedItem->kind) << ','
             << issue.generatedItem->owner << ','
             << issue.generatedItem->ordinal << ';';
    } else {
      output << "g-;";
    }
  }
  return output.str();
}

void checkInjectedRange(const lang::FrontendResult &frontend) {
  const lang::SourceUnit *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  if (entry == nullptr) {
    gti_fuzz::fail("pipeline", "injected range has no entry source");
  }
  const std::string name = entry->path.string();
  const std::string *source = frontend.sources.find(name);
  if (source == nullptr) {
    gti_fuzz::fail("pipeline", "injected range has no source text");
  }
  checkSpan({.source = name,
             .start = source->size(),
             .end = source->size() + 1,
             .line = 1},
            frontend.sources);
}

struct Injections {
  bool diagnosticRange = false;
  bool loweredRejection = false;
  bool invalidIr = false;
};

Observation observe(std::string_view source, lang::OptimizationLevel level,
                    bool verifyIr, const Injections &inject) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("fuzz.gti", std::string(source));
  Observation result;
  result.accepted = frontend.canGenerateCode();
  result.diagnosticCount = frontend.diagnostics.size();
  result.diagnostics = diagnosticsSnapshot(frontend);
  result.phaseFacts = frontendPhaseSnapshot(frontend);
  if (inject.diagnosticRange) {
    checkInjectedRange(frontend);
  }

  if (!result.accepted) {
    result.disposition = "frontend-rejected";
    return result;
  }

  const lang::TargetInfo target = lang::TargetInfo::host();
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir, level, target);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .target = target,
                                        .compatibility = &compatibility});
  if (!optimized.valid()) {
    gti_fuzz::fail("pipeline", "optimizer produced an incoherent program");
  }
  appendString(result.phaseFacts,
               lang::MirPrinter().print(optimized.sourceMir));
  appendString(result.phaseFacts, lang::MirPrinter().print(optimized.mir));
  appendString(result.phaseFacts, lang::serializeOptimizationReportJson(
                                      optimized.report, levelName(level)));

  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations, target);
  if (!lowered.valid() || inject.loweredRejection) {
    // The driver reports this as internal compiler error GTI-B0001: every
    // issue kind names a broken construction invariant, not a limitation.
    std::fprintf(stderr, "gti-fuzz(pipeline): lowered-program issues: %s\n",
                 loweredIssueSnapshot(lowered.issues).c_str());
    gti_fuzz::fail("pipeline", "lowered-program construction failed");
  }

  // A classified LLVM capability gap (GTI-B0003) is a deterministic,
  // inspectable outcome. Every other backend exception maps to GTI-B0001 at
  // the driver boundary and remains a fuzz failure.
  const lang::LlvmBackendOptions objectOptions{.optimization =
                                                   codegenLevel(level)};
  lang::LlvmBackendOptions irOptions = objectOptions;
  irOptions.textualIr = true;
  result.disposition = "emitted";
  try {
    result.mirArtifact = lang::MirBackend().generate(*lowered.program).contents;
    try {
      result.llvmIr =
          lang::LlvmBackend(irOptions).generate(*lowered.program).contents;
    } catch (const lang::BackendUnsupportedError &exception) {
      result.disposition = "backend-unsupported";
      result.rejection = exception.what();
      return result;
    }
    const lang::BackendArtifact object =
        lang::LlvmBackend(objectOptions).generate(*lowered.program);
    if (object.kind != lang::BackendArtifactKind::Object ||
        object.objects.size() != 1) {
      gti_fuzz::fail("pipeline", "LLVM object artifact is not one object");
    }
    result.llvmObject = object.objects.front();
  } catch (const lang::BackendUnsupportedError &exception) {
    // Textual IR and object emission share admission and the module
    // pipeline; only one of them declining is an inconsistency.
    std::fprintf(stderr, "gti-fuzz(pipeline): backend limitation: %s\n",
                 exception.what());
    gti_fuzz::fail("pipeline",
                   "MIR, LLVM IR and object emission disagree on support");
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "gti-fuzz(pipeline): backend internal error: %s\n",
                 exception.what());
    gti_fuzz::fail("pipeline", "backend raised an internal error");
  }
  if (verifyIr) {
    verifyTextualIr(result.llvmIr, inject.invalidIr);
  }
  return result;
}

void requireClassified(const Observation &observation) {
  if (!observation.accepted) {
    if (observation.disposition != "frontend-rejected" ||
        observation.diagnosticCount == 0) {
      gti_fuzz::fail("pipeline",
                     "frontend rejection is not explicit and diagnostic");
    }
    return;
  }
  if (observation.disposition == "backend-unsupported") {
    if (observation.rejection.empty() || observation.mirArtifact.empty() ||
        !observation.llvmIr.empty() || !observation.llvmObject.empty()) {
      gti_fuzz::fail("pipeline",
                     "backend limitation has incomplete classification");
    }
    return;
  }
  if (observation.disposition != "emitted") {
    gti_fuzz::fail("pipeline", "accepted input has no classified outcome");
  }
  if (observation.mirArtifact.empty() || observation.llvmIr.empty() ||
      observation.llvmObject.empty()) {
    gti_fuzz::fail("pipeline", "accepted input produced an empty artifact");
  }
}

} // namespace

// The LLVM libraries this harness links (system or bundled) are never built
// with ASan, while GTI's own LLVM passes are. Under libc++'s container
// annotations, a std::vector shared across that boundary, such as the pass
// list GTI's registered callbacks append to inside
// PassBuilder::buildModuleOptimizationPipeline at -O1..-O3, is reported as a
// container-overflow without any out-of-bounds access. Only this harness
// links LLVM, so only it turns that one check off; the parser, formatter and
// source-loader harnesses keep it. An explicit ASAN_OPTIONS still overrides.
extern "C" const char *__asan_default_options() {
  return "detect_container_overflow=0";
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxPipelineInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  const std::string_view source(reinterpret_cast<const char *>(data), size);
  // Two low bits of the first byte select -O0..-O3, so every seed reaches
  // every GTI and LLVM pipeline level under mutation.
  constexpr lang::OptimizationLevel levels[] = {
      lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
      lang::OptimizationLevel::O2, lang::OptimizationLevel::O3};
  const lang::OptimizationLevel level = levels[size > 0 ? data[0] & 3 : 0];
  // Re-parsing and verifying the textual module once per input is enough:
  // the second observation must reproduce the same bytes.
  Observation first = observe(source, level, true,
                              {.loweredRejection = gti_fuzz::injectProperty(
                                   data, size, "pipeline-lowered-rejection"),
                               .invalidIr = gti_fuzz::injectProperty(
                                   data, size, "pipeline-llvm-verifier")});
  Observation second = observe(source, level, false,
                               {.diagnosticRange = gti_fuzz::injectProperty(
                                    data, size, "pipeline-diagnostic-range")});

  if (gti_fuzz::injectProperty(data, size, "pipeline-acceptance")) {
    second.accepted = !second.accepted;
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-diagnostics")) {
    second.diagnostics += "injected";
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-phase-facts")) {
    second.phaseFacts += "injected";
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-unclassified")) {
    second.disposition.clear();
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-empty-artifact")) {
    second.llvmObject.clear();
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-artifact")) {
    second.llvmIr += "injected";
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-object")) {
    second.llvmObject += "injected";
  }

  if (first.accepted != second.accepted) {
    gti_fuzz::fail("pipeline", "acceptance is not deterministic");
  }
  if (first.diagnosticCount != second.diagnosticCount ||
      first.diagnostics != second.diagnostics) {
    gti_fuzz::fail("pipeline", "diagnostics are not deterministic");
  }
  if (first.phaseFacts != second.phaseFacts) {
    gti_fuzz::fail("pipeline", "phase facts are not deterministic");
  }
  requireClassified(first);
  requireClassified(second);
  if (first.disposition != second.disposition ||
      first.rejection != second.rejection) {
    gti_fuzz::fail("pipeline", "classified outcome is not deterministic");
  }
  if (first.mirArtifact != second.mirArtifact ||
      first.llvmIr != second.llvmIr) {
    gti_fuzz::fail("pipeline", "MIR/LLVM IR artifacts are not deterministic");
  }
  if (first.llvmObject != second.llvmObject) {
    gti_fuzz::fail("pipeline", "LLVM object artifacts are not deterministic");
  }
  return 0;
}
