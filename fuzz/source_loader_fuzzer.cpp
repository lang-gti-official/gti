// Bounded 2-3 unit source-loader harness (Q-FUZZ-01B).
//
// The harness uses Frontend's production sourceOverrides seam: no files are
// written and no second import parser exists. One input bit selects a two- or
// three-unit quoted-import chain; the remaining arbitrary bytes are the leaf
// unit. Two fresh snapshots must agree on diagnostics, phase flags, and the
// complete source graph, and every diagnostic range must belong to loaded
// source text.

#include "fuzz_support.h"

#include "gti/frontend.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

void appendString(std::ostringstream &output, std::string_view value) {
  output << value.size() << ':' << value << ';';
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
    gti_fuzz::fail("source-loader", "diagnostic range escapes loaded source");
  }
}

std::string snapshot(const lang::FrontendResult &frontend) {
  std::ostringstream output;
  output << frontend.sourceValid << frontend.syntaxValid
         << frontend.semanticValid << frontend.hirValid
         << frontend.failureMetadataValid << frontend.mirValid << ';'
         << frontend.diagnostics.size() << ';';
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    checkSpan(diagnostic.primary, frontend.sources);
    appendString(output, diagnostic.code);
    output << static_cast<int>(diagnostic.phase) << ','
           << static_cast<int>(diagnostic.severity) << ';';
    appendSpan(output, diagnostic.primary);
    appendString(output, diagnostic.message);
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      checkSpan(related.span, frontend.sources);
      appendSpan(output, related.span);
      appendString(output, related.message);
    }
    for (const lang::FixIt &fix : diagnostic.fixes) {
      checkSpan(fix.span, frontend.sources);
      appendSpan(output, fix.span);
      appendString(output, fix.replacement);
      appendString(output, fix.message);
    }
  }

  const lang::SourceGraph &graph = frontend.sourceGraph;
  output << graph.entryUnit() << ',' << graph.sourceUnits().size() << ','
         << graph.dependencyEdges().size() << ';';
  for (const lang::SourceUnit &unit : graph.sourceUnits()) {
    output << unit.id << ',' << unit.sourceDigest << ',' << unit.syntaxDigest
           << ',' << unit.declarationStart << ',' << unit.declarationCount
           << ',' << unit.entry << ';';
    appendString(output, unit.path.generic_string());
    output << unit.tokens.size() << ';';
    for (const lang::Token &token : unit.tokens) {
      output << static_cast<int>(token.kind) << ';';
      appendString(output, token.lexeme);
    }
  }
  for (const lang::SourceDependency &edge : graph.dependencyEdges()) {
    output << edge.source << ',' << edge.target << ','
           << static_cast<int>(edge.kind) << ',' << edge.includeOccurrence
           << ';';
    appendString(output, edge.includeSpelling);
  }
  for (const lang::SourceUnitId unit : graph.compilationOrder()) {
    output << unit << ',';
  }
  return output.str();
}

struct SourceSet {
  std::filesystem::path entry;
  std::string entrySource;
  std::unordered_map<std::string, std::string> overrides;
  std::size_t expectedUnits = 2;
};

SourceSet makeSources(const std::uint8_t *data, std::size_t size) {
  std::error_code error;
  std::filesystem::path root = std::filesystem::temp_directory_path(error);
  if (error) {
    root = std::filesystem::current_path(error);
  }
  const std::filesystem::path canonicalRoot =
      std::filesystem::weakly_canonical(root, error);
  if (!error) {
    root = canonicalRoot;
  }
  root /= "gti-fuzz-source-loader";
  const std::filesystem::path entry = (root / "main.gti").lexically_normal();
  const std::filesystem::path first = (root / "a.gti").lexically_normal();
  const std::filesystem::path second = (root / "b.gti").lexically_normal();
  const bool threeUnits = size > 0 && (data[0] & 1) != 0;
  const std::size_t payloadStart = size == 0 ? 0 : 1;
  const std::string payload(reinterpret_cast<const char *>(data + payloadStart),
                            size - payloadStart);

  SourceSet result;
  result.entry = entry;
  result.entrySource = "import \"a.gti\";\nint main() { return 0; }\n";
  if (threeUnits) {
    result.overrides.emplace(first.string(),
                             "export module a;\nimport \"b.gti\";\n");
    result.overrides.emplace(second.string(), "export module b;\n" + payload);
    result.expectedUnits = 3;
  } else {
    result.overrides.emplace(first.string(), "export module a;\n" + payload);
    result.expectedUnits = 2;
  }
  return result;
}

lang::FrontendResult analyze(const SourceSet &sources) {
  return lang::Frontend().analyze(sources.entry, sources.entrySource, {},
                                  sources.overrides);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxPipelineInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);
  const SourceSet sources = makeSources(data, size);
  const lang::FrontendResult first = analyze(sources);
  const lang::FrontendResult second = analyze(sources);
  if (first.sourceGraph.sourceUnits().size() != sources.expectedUnits ||
      second.sourceGraph.sourceUnits().size() != sources.expectedUnits) {
    gti_fuzz::fail("source-loader", "bounded import chain was not retained");
  }
  const std::string firstSnapshot = snapshot(first);
  std::string secondSnapshot = snapshot(second);
  if (gti_fuzz::injectProperty(data, size, "source-loader-snapshot")) {
    secondSnapshot += "injected";
  }
  if (firstSnapshot != secondSnapshot) {
    gti_fuzz::fail("source-loader",
                   "fresh source snapshots are not deterministic");
  }
  return 0;
}
