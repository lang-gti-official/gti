#include "gti/frontend.h"
#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/support.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {
namespace {

std::string cacheContext(const FrontendOptions &options) {
  ConfigurationFlags flags = options.configurationFlags;
  normalizeConfigurationFlags(flags);
  std::string result =
      options.target.arch + '\n' + options.target.vendor + '\n' +
      options.target.os + '\n' +
      std::string(executionProfileName(options.target.executionProfile)) +
      '\n' +
      (options.target.runtimeCapabilities.threads ? "threads:1" : "threads:0") +
      '\n' +
      std::string(
          targetEndiannessName(options.target.dataLayout.endianness())) +
      '\n' + std::to_string(options.target.dataLayout.pointerWidthBits());
  for (const std::string &flag : flags) {
    result += '\n';
    result += flag;
  }
  result += '\n';
  result += options.target.dataLayout.supported() ? "layout:supported"
                                                  : "layout:unsupported";
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    result += '\n';
    const auto layout = options.target.dataLayout.scalarLayout(
        static_cast<TargetScalarKind>(index));
    if (!layout) {
      result += '-';
      continue;
    }
    result += std::to_string(layout->sizeBytes) + ':' +
              std::to_string(layout->abiAlignmentBytes) + ':' +
              std::to_string(layout->preferredAlignmentBytes);
  }
  result += options.program.kind == ProgramKind::Library
                ? "\nprogram:library"
                : "\nprogram:executable";
  std::vector<std::pair<std::string, std::string>> exports;
  exports.reserve(options.program.exports.size());
  for (const EmbeddingExportRequest &exportRequest : options.program.exports) {
    exports.emplace_back(exportRequest.symbol, exportRequest.function);
  }
  std::sort(exports.begin(), exports.end());
  for (const auto &[symbol, function] : exports) {
    result += "\nexport:" + symbol + '=' + function;
  }
  return result;
}

std::uint64_t sourceGeneration(const FrontendOptions &options,
                               std::string_view path) {
  const auto found = options.sourceGenerations.find(std::string(path));
  return found == options.sourceGenerations.end() ? 0 : found->second;
}

void digestInteger(std::uint64_t &digest, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    digest ^= static_cast<unsigned char>(value >> shift);
    digest *= 1099511628211ULL;
  }
}

void digestText(std::uint64_t &digest, std::string_view value) {
  digestInteger(digest, value.size());
  for (const unsigned char byte : value) {
    digest ^= byte;
    digest *= 1099511628211ULL;
  }
}

class DependencyDigestBuilder final {
public:
  DependencyDigestBuilder(const SourceGraph &graph,
                          const FrontendOptions &options)
      : graph(graph), options(options),
        dependenciesBySource(graph.sourceUnits().size() + 1) {
    for (const SourceDependency &dependency : graph.dependencyEdges()) {
      if (dependency.source < dependenciesBySource.size()) {
        dependenciesBySource[dependency.source].push_back(&dependency);
      }
    }
  }

  [[nodiscard]] std::unordered_map<SourceUnitId, std::uint64_t> build() {
    for (const SourceUnit &unit : graph.sourceUnits()) {
      static_cast<void>(digestFor(unit.id));
    }
    return std::move(memoized);
  }

private:
  [[nodiscard]] std::uint64_t digestFor(SourceUnitId source) {
    if (const auto found = memoized.find(source); found != memoized.end()) {
      return found->second;
    }

    std::uint64_t result = 14695981039346656037ULL;
    if (!visiting.insert(source).second) {
      // SourceLoader diagnoses include cycles. Retain a deterministic marker
      // so a recovered graph cannot recurse indefinitely while computing a
      // cache identity.
      digestText(result, "dependency-cycle");
      digestInteger(result, source);
      return result;
    }

    if (source >= dependenciesBySource.size()) {
      visiting.erase(source);
      memoized.emplace(source, result);
      return result;
    }
    for (const SourceDependency *dependency : dependenciesBySource[source]) {
      const SourceUnit *target = graph.findUnit(dependency->target);
      if (target == nullptr) {
        continue;
      }
      digestInteger(result, static_cast<std::uint64_t>(dependency->kind));
      digestText(result, dependency->includeSpelling);
      digestInteger(result, dependency->includeOccurrence);
      digestText(result, target->path.string());
      digestInteger(result, sourceGeneration(options, target->path.string()));
      digestInteger(result, target->sourceDigest);
      digestInteger(result, target->syntaxDigest);
      digestInteger(result, digestFor(target->id));
    }

    visiting.erase(source);
    // Zero is reserved as the identity of a raw lexical cache row.
    if (result == 0) {
      result = 1;
    }
    memoized.emplace(source, result);
    return result;
  }

  const SourceGraph &graph;
  const FrontendOptions &options;
  std::unordered_map<SourceUnitId, std::uint64_t> memoized;
  std::unordered_set<SourceUnitId> visiting;
  std::vector<std::vector<const SourceDependency *>> dependenciesBySource;
};

FrontendUnitCacheKey unitCacheKey(const FrontendOptions &options,
                                  const SourceUnit &unit,
                                  std::string_view context,
                                  std::uint64_t dependencyDigest) {
  return {.canonicalPath = unit.path.string(),
          .sourceGeneration = sourceGeneration(options, unit.path.string()),
          .sourceDigest = unit.sourceDigest,
          .syntaxDigest = unit.syntaxDigest,
          .dependencyDigest = dependencyDigest,
          .context = std::string(context),
          .completionOffset =
              unit.entry ? options.completionOffset : std::nullopt};
}

std::size_t estimatedTokenBytes(const std::vector<Token> &tokens) {
  std::size_t result = tokens.size() * sizeof(Token);
  for (const Token &token : tokens) {
    result += token.lexeme.size() + token.source.size();
    if (const auto *text = std::get_if<std::string>(&token.literal)) {
      result += text->size();
    }
    if (token.leadingDocumentationMarkdown) {
      result += token.leadingDocumentationMarkdown->size();
    }
  }
  return result;
}

} // namespace

Frontend::Frontend(FrontendOptions options) : options(std::move(options)) {}

FrontendResult Frontend::analyze(
    const std::filesystem::path &entryPath,
    std::optional<std::string> entrySource,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    const std::vector<PackageSourceRoot> &packageSourceRoots) const {
  FrontendResult result;
  if (options.workMetrics != nullptr) {
    options.workMetrics->clear();
  }
  const std::string frontendCacheContext = cacheContext(options);
  SourceLoader sourceLoader(options.workMetrics, options.cache,
                            &options.sourceGenerations, &options.sourceDigests,
                            frontendCacheContext);
  const auto sourceLoadStart = std::chrono::steady_clock::now();
  {
    const PhaseTimeScope timeScope("gti-source-load");
    result.sourceGraph = sourceLoader.load(
        entryPath, std::move(entrySource), preludePaths, sourceOverrides,
        standardLibraryRoots, options.completionOffset, packageSourceRoots,
        options.target, options.configurationFlags);
  }
  if (options.workMetrics != nullptr) {
    options.workMetrics->sourceLoadMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - sourceLoadStart)
            .count());
  }
  result.sources = sourceLoader.sources();
  append(result.diagnostics, sourceLoader.errors());
  result.sourceValid = !sourceLoader.hadError();
  return finishAnalysis(entryPath, std::move(result));
}

// Continues the canonical frontend pipeline from a graph produced by the
// compiler's SourceLoader. Build orchestration uses this entry point after
// computing a content identity for the exact loaded inputs; it does not
// maintain a second include parser or semantic representation.
FrontendResult
Frontend::analyzeLoaded(const std::filesystem::path &entryPath,
                        SourceGraph sourceGraph, SourceManager sources,
                        std::vector<Diagnostic> sourceDiagnostics,
                        bool sourceValid) const {
  FrontendResult result;
  result.sourceGraph = std::move(sourceGraph);
  result.sources = std::move(sources);
  result.diagnostics = std::move(sourceDiagnostics);
  result.sourceValid = sourceValid;
  return finishAnalysis(entryPath, std::move(result));
}

FrontendResult Frontend::finishAnalysis(const std::filesystem::path &entryPath,
                                        FrontendResult result) const {
  if (!result.sourceValid) {
    return result;
  }

  if (!options.target.supported()) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2062", DiagnosticPhase::Semantics,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        "Selected target '" + options.target.arch + "-" +
            options.target.vendor + "-" + options.target.os +
            "' has no supported GTI data layout.");
    diagnostic.hints.emplace_back(
        "Select a 64-bit little-endian arm64 or x86_64 target for macOS, "
        "Linux, or Windows.");
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  }

  StmtList declarations;
  bool syntaxValid = true;
  const std::string frontendCacheContext = cacheContext(options);
  const std::unordered_map<SourceUnitId, std::uint64_t> dependencyDigests =
      DependencyDigestBuilder(result.sourceGraph, options).build();
  const auto parseStart = std::chrono::steady_clock::now();
  {
    const PhaseTimeScope timeScope("gti-parse");
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
      SourceUnit *unit = result.sourceGraph.findUnit(unitId);
      if (unit == nullptr) {
        continue;
      }
      unit->declarationStart = declarations.size();
      StmtList unitDeclarations;
      const auto dependencyDigest = dependencyDigests.find(unitId);
      const FrontendUnitCacheKey key =
          unitCacheKey(options, *unit, frontendCacheContext,
                       dependencyDigest == dependencyDigests.end()
                           ? 0
                           : dependencyDigest->second);
      std::optional<CachedParsedUnit> cached =
          options.cache ? options.cache->lookupParsed(key) : std::nullopt;
      if (cached) {
        unitDeclarations = std::move(cached->declarations);
        appendParserDiagnostics(result.diagnostics, cached->diagnostics,
                                result.sourceGraph, unitId);
        syntaxValid = syntaxValid && !cached->hadError;
        if (options.workMetrics != nullptr) {
          options.workMetrics->parseCacheHits.push_back(unit->path.string());
        }
      } else {
        if (options.workMetrics != nullptr) {
          options.workMetrics->parsedUnits.push_back(unit->path.string());
        }
        std::size_t estimatedInputBytes = estimatedTokenBytes(unit->tokens);
        if (const std::string *source =
                result.sources.find(unit->path.string())) {
          estimatedInputBytes += source->size();
        }
        Parser parser(std::move(unit->tokens));
        Program parsed = parser.parse();
        appendParserDiagnostics(result.diagnostics, parser.errors(),
                                result.sourceGraph, unitId);
        syntaxValid = syntaxValid && !parser.hadError();
        unitDeclarations = parsed.takeDeclarations();
        if (options.cache) {
          options.cache->storeParsed(key, estimatedInputBytes,
                                     {.declarations = unitDeclarations,
                                      .diagnostics = parser.errors(),
                                      .hadError = parser.hadError()});
        }
      }
      unit->declarationCount = unitDeclarations.size();
      declarations.insert(declarations.end(), unitDeclarations.begin(),
                          unitDeclarations.end());
    }
  }
  if (options.workMetrics != nullptr) {
    options.workMetrics->parseMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - parseStart)
            .count());
  }
  result.program = Program(std::move(declarations));
  result.syntaxValid = syntaxValid;
  if (!result.syntaxValid && !options.analyzeRecoveredProgram) {
    return result;
  }

  SemanticVisitor semantic(options.target, &result.sourceGraph,
                           options.toolingOccurrences, options.workMetrics);
  if (options.workMetrics != nullptr) {
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
      if (const SourceUnit *unit = result.sourceGraph.findUnit(unitId)) {
        options.workMetrics->semanticallyAnalyzedUnits.push_back(
            unit->path.string());
      }
    }
  }
  const auto semanticStart = std::chrono::steady_clock::now();
  {
    const PhaseTimeScope timeScope("gti-semantics");
    result.semanticValid = semantic.check(result.program);
    if (result.semanticValid) {
      result.semanticValid =
          semantic.applyProgramRequest(result.program, options.program);
    }
  }
  if (options.workMetrics != nullptr) {
    options.workMetrics->semanticMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - semanticStart)
            .count());
  }
  append(result.diagnostics, semantic.errors());
  if (!result.semanticValid || options.completionOffset ||
      options.stopAfter == FrontendPhase::Semantics) {
    result.semantics = semantic.takeModel();
    return result;
  }

  const auto hirStart = std::chrono::steady_clock::now();
  HirLoweringResult hir = [&] {
    const PhaseTimeScope timeScope("gti-hir-lowering");
    return HirLowerer(options.target)
        .lower(result.program, semantic.model(), semantic);
  }();
  if (options.workMetrics != nullptr) {
    options.workMetrics->hirMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - hirStart)
            .count());
  }
  result.hirValid = hir.valid();
  result.hir = std::move(hir.program);
  append(result.diagnostics, hir.diagnostics);
  // HIR instance reanalysis is the last reader of the semantic visitor;
  // move the model into the result instead of deep-copying it.
  result.semantics = semantic.takeModel();
  if (!result.hirValid || options.stopAfter == FrontendPhase::Hir) {
    return result;
  }

  FailureMetadataBuildResult failureMetadata = [&] {
    const PhaseTimeScope timeScope("gti-failure-metadata");
    return FailureMetadataBuilder().build(result.sourceGraph, result.sources,
                                          result.hir, entryPath);
  }();
  result.failureMetadataValid = failureMetadata.valid();
  result.failureMetadata = std::move(failureMetadata.metadata);
  if (!result.failureMetadataValid) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    std::string message =
        "Internal compiler error: failed to construct failure metadata.";
    if (!failureMetadata.errors.empty()) {
      message += " " + failureMetadata.errors.front() + ".";
    }
    result.diagnostics.push_back(makeDiagnostic(
        "GTI-B0002", DiagnosticPhase::Backend,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        std::move(message)));
    return result;
  }

  const PhaseTimeScope mirTimeScope("gti-mir-lowering");
  MirLoweringResult mir =
      MirLowerer().lower(result.hir, result.failureMetadata);
  result.mirValid = mir.valid();
  result.mir = std::move(mir.program);
  if (!result.mirValid) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    const MirVerificationResult verification = verifyMirProgram(result.mir);
    std::string message =
        "Internal compiler error: failed to construct valid MIR.";
    // One internal failure often reports several verifier errors, and the
    // first is not always the owning cause. Report every distinct error so an
    // internal failure names its real origin instead of a later symptom.
    constexpr std::size_t maximumReportedDetails = 5;
    std::vector<std::string> details;
    for (const MirVerificationError &error : verification.errors) {
      if (error.message == "MIR program is marked invalid") {
        continue;
      }
      std::string detail = error.message + " (body owner " +
                           std::to_string(error.owner) + ", block " +
                           std::to_string(error.block) + ", instruction " +
                           std::to_string(error.instruction) + ")";
      if (std::find(details.begin(), details.end(), detail) != details.end()) {
        continue;
      }
      details.push_back(std::move(detail));
      if (details.size() == maximumReportedDetails) {
        break;
      }
    }
    for (const std::string &detail : details) {
      message += " " + detail + ".";
    }
    result.diagnostics.push_back(makeDiagnostic(
        "GTI-B0001", DiagnosticPhase::Backend,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        std::move(message)));
  }
  return result;
}

void Frontend::appendParserDiagnostics(
    std::vector<Diagnostic> &destination,
    const std::vector<ParseDiagnostic> &diagnostics,
    const SourceGraph &sourceGraph, SourceUnitId unit) {
  for (const ParseDiagnostic &source : diagnostics) {
    Diagnostic diagnostic = source;
    for (const SourceDependency &dependency : sourceGraph.dependencyEdges()) {
      if (dependency.target == unit && dependency.directive) {
        diagnostic.related.push_back(
            {*dependency.directive, "Included from here."});
      }
    }
    destination.push_back(std::move(diagnostic));
  }
}

template <typename DiagnosticType>
void Frontend::append(std::vector<Diagnostic> &destination,
                      const std::vector<DiagnosticType> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace lang
