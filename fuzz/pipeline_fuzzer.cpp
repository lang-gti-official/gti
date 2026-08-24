// libFuzzer harness for the whole GTI analysis-to-backend pipeline
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// Every bounded input is analyzed twice through fresh production snapshots.
// Acceptance, complete diagnostics, cross-phase facts, optimization state,
// structured rejection, and MIR/C++ artifacts must be deterministic. Every
// diagnostic range is checked against the SourceManager that owns it. An
// accepted input must either produce coherent nonempty MIR and C++ artifacts
// or carry an explicit lowered-program rejection with at least one issue.
// Native compilation and execution remain out of process.

#include "fuzz_support.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_backend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/report.h"
#include "gti/optimizer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
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
  std::string cppArtifact;
};

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
           << unit.prelude << ';' << unit.tokens.size() << ';';
    for (const lang::Token &token : unit.tokens) {
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

Observation observe(std::string_view source, lang::OptimizationLevel level,
                    bool injectRange) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("fuzz.gti", std::string(source));
  Observation result;
  result.accepted = frontend.canGenerateCode();
  result.diagnosticCount = frontend.diagnostics.size();
  result.diagnostics = diagnosticsSnapshot(frontend);
  result.phaseFacts = frontendPhaseSnapshot(frontend);
  if (injectRange) {
    checkInjectedRange(frontend);
  }

  if (!result.accepted) {
    result.disposition = "frontend-rejected";
    return result;
  }

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir, level);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .compatibility = &compatibility});
  if (!optimized.valid()) {
    gti_fuzz::fail("pipeline", "optimizer produced an incoherent program");
  }
  appendString(result.phaseFacts,
               lang::MirPrinter().print(optimized.sourceMir));
  appendString(result.phaseFacts, lang::MirPrinter().print(optimized.mir));
  appendString(result.phaseFacts,
               lang::serializeOptimizationReportJson(
                   optimized.report,
                   level == lang::OptimizationLevel::O0 ? "O0" : "O2"));

  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations,
      lang::TargetInfo::host());
  if (!lowered.valid()) {
    if (lowered.issues.empty()) {
      gti_fuzz::fail("pipeline",
                     "lowered-program rejection has no classified issue");
    }
    result.disposition = "lowered-rejected";
    result.rejection = loweredIssueSnapshot(lowered.issues);
    return result;
  }

  // A backend reports an internal error by throwing, and the driver turns
  // that into GTI-B0001. Letting it escape here would abort the process with
  // no diagnostic, so every ICE would look like a memory-safety crash and
  // the first one would end the run. Catch it, print what the user would
  // have seen, and then fail deliberately — an ICE is still a bug.
  result.disposition = "emitted";
  try {
    result.mirArtifact = lang::MirBackend().generate(*lowered.program).contents;
    result.cppArtifact = lang::CppBackend(lang::CppStandard::Cpp23)
                             .generate(*lowered.program)
                             .contents;
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "gti-fuzz(pipeline): backend internal error: %s\n",
                 exception.what());
    gti_fuzz::fail("pipeline", "backend raised an internal error");
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
  if (observation.disposition == "lowered-rejected") {
    if (observation.rejection.empty()) {
      gti_fuzz::fail("pipeline", "lowered rejection has no stable evidence");
    }
    return;
  }
  if (observation.disposition != "emitted") {
    gti_fuzz::fail("pipeline", "accepted input has no classified outcome");
  }
  if (observation.mirArtifact.empty() || observation.cppArtifact.empty()) {
    gti_fuzz::fail("pipeline", "accepted input produced an empty artifact");
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxPipelineInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  const std::string_view source(reinterpret_cast<const char *>(data), size);
  const lang::OptimizationLevel level = (size > 0 && (data[0] & 1) != 0)
                                            ? lang::OptimizationLevel::O2
                                            : lang::OptimizationLevel::O0;
  Observation first = observe(source, level, false);
  Observation second = observe(
      source, level,
      gti_fuzz::injectProperty(data, size, "pipeline-diagnostic-range"));

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
    second.cppArtifact.clear();
  }
  if (gti_fuzz::injectProperty(data, size, "pipeline-artifact")) {
    second.cppArtifact += "injected";
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
      first.cppArtifact != second.cppArtifact) {
    gti_fuzz::fail("pipeline", "backend artifacts are not deterministic");
  }
  return 0;
}
