#pragma once

#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/mir.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

struct FrontendOptions {
  TargetInfo target = TargetInfo::host();
  bool analyzeRecoveredProgram = false;
  std::optional<std::size_t> completionOffset;
};

struct FrontendResult {
  Program program;
  SemanticModel semantics;
  HirProgram hir;
  MirProgram mir;
  SourceGraph sourceGraph;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  bool sourceValid = false;
  bool syntaxValid = false;
  bool semanticValid = false;
  bool hirValid = false;
  bool mirValid = false;

  [[nodiscard]] bool canGenerateCode() const {
    return sourceValid && syntaxValid && semanticValid && hirValid && mirValid;
  }
};

class Frontend {
public:
  explicit Frontend(FrontendOptions options = {})
      : options(std::move(options)) {}

  [[nodiscard]] FrontendResult analyze(
      const std::filesystem::path &entryPath,
      std::optional<std::string> entrySource = std::nullopt,
      const std::vector<std::filesystem::path> &preludePaths = {},
      const std::unordered_map<std::string, std::string> &sourceOverrides = {},
      const std::vector<std::filesystem::path> &standardLibraryRoots = {})
      const {
    FrontendResult result;
    SourceLoader sourceLoader;
    result.sourceGraph = sourceLoader.load(
        entryPath, std::move(entrySource), preludePaths, sourceOverrides,
        standardLibraryRoots, options.completionOffset);
    result.sources = sourceLoader.sources();
    append(result.diagnostics, sourceLoader.errors());
    result.sourceValid = !sourceLoader.hadError();
    if (!result.sourceValid) {
      return result;
    }

    StmtList declarations;
    bool syntaxValid = true;
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
      SourceUnit *unit = result.sourceGraph.findUnit(unitId);
      if (unit == nullptr) {
        continue;
      }
      Parser parser(std::move(unit->tokens));
      Program parsed = parser.parse();
      appendParserDiagnostics(result.diagnostics, parser.errors(),
                              result.sourceGraph, unitId);
      syntaxValid = syntaxValid && !parser.hadError();

      unit->declarationStart = declarations.size();
      StmtList unitDeclarations = parsed.takeDeclarations();
      unit->declarationCount = unitDeclarations.size();
      declarations.insert(declarations.end(),
                          std::make_move_iterator(unitDeclarations.begin()),
                          std::make_move_iterator(unitDeclarations.end()));
    }
    result.program = Program(std::move(declarations));
    result.syntaxValid = syntaxValid;
    if (!result.syntaxValid && !options.analyzeRecoveredProgram) {
      return result;
    }

    SemanticVisitor semantic(options.target, &result.sourceGraph);
    result.semanticValid = semantic.check(result.program);
    result.semantics = semantic.model();
    append(result.diagnostics, semantic.errors());
    if (!result.semanticValid || options.completionOffset) {
      return result;
    }

    HirLoweringResult hir =
        HirLowerer(options.target).lower(result.program, semantic);
    result.hirValid = hir.valid();
    result.hir = std::move(hir.program);
    append(result.diagnostics, hir.diagnostics);
    if (!result.hirValid) {
      return result;
    }

    MirLoweringResult mir = MirLowerer().lower(result.hir);
    result.mirValid = mir.valid();
    result.mir = std::move(mir.program);
    if (!result.mirValid) {
      const SourceUnit *entry =
          result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
      const MirVerificationResult verification = verifyMirProgram(result.mir);
      std::string message =
          "Internal compiler error: failed to construct valid MIR.";
      const auto detail = std::find_if(
          verification.errors.begin(), verification.errors.end(),
          [](const MirVerificationError &error) {
            return error.message != "MIR program is marked invalid";
          });
      if (detail != verification.errors.end()) {
        message += " " + detail->message + " (body owner " +
                   std::to_string(detail->owner) + ", block " +
                   std::to_string(detail->block) + ", instruction " +
                   std::to_string(detail->instruction) + ").";
      }
      result.diagnostics.push_back(
          makeDiagnostic("GTI-B0001", DiagnosticPhase::Backend,
                         SourceSpan{entry == nullptr ? entryPath.string()
                                                     : entry->path.string(),
                                    0, 0, 1},
                         std::move(message)));
    }
    return result;
  }

private:
  static void
  appendParserDiagnostics(std::vector<Diagnostic> &destination,
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
  static void append(std::vector<Diagnostic> &destination,
                     const std::vector<DiagnosticType> &source) {
    destination.insert(destination.end(), source.begin(), source.end());
  }

  FrontendOptions options;
};

} // namespace lang
