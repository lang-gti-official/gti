#pragma once

#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

struct FrontendOptions {
  TargetInfo target = TargetInfo::host();
  bool analyzeRecoveredProgram = false;
};

struct FrontendResult {
  Program program;
  SemanticModel semantics;
  HirProgram hir;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  bool sourceValid = false;
  bool syntaxValid = false;
  bool semanticValid = false;
  bool hirValid = false;

  [[nodiscard]] bool canGenerateCode() const {
    return sourceValid && syntaxValid && semanticValid && hirValid;
  }
};

class Frontend {
public:
  explicit Frontend(FrontendOptions options = {})
      : options(std::move(options)) {}

  [[nodiscard]] FrontendResult
  analyze(const std::filesystem::path &entryPath,
          std::optional<std::string> entrySource = std::nullopt,
          const std::vector<std::filesystem::path> &preludePaths = {},
          const std::unordered_map<std::string, std::string> &sourceOverrides =
              {}) const {
    FrontendResult result;
    SourceLoader sourceLoader;
    std::vector<Token> tokens = sourceLoader.load(
        entryPath, std::move(entrySource), preludePaths, sourceOverrides);
    result.sources = sourceLoader.sources();
    append(result.diagnostics, sourceLoader.errors());
    result.sourceValid = !sourceLoader.hadError();
    if (!result.sourceValid) {
      return result;
    }

    Parser parser(std::move(tokens));
    result.program = parser.parse();
    append(result.diagnostics, parser.errors());
    result.syntaxValid = !parser.hadError();
    if (!result.syntaxValid && !options.analyzeRecoveredProgram) {
      return result;
    }

    SemanticVisitor semantic(options.target);
    result.semanticValid = semantic.check(result.program);
    result.semantics = semantic.model();
    append(result.diagnostics, semantic.errors());
    if (!result.semanticValid) {
      return result;
    }

    HirLoweringResult hir =
        HirLowerer(options.target).lower(result.program, semantic);
    result.hirValid = hir.valid();
    result.hir = std::move(hir.program);
    append(result.diagnostics, hir.diagnostics);
    return result;
  }

private:
  template <typename DiagnosticType>
  static void append(std::vector<Diagnostic> &destination,
                     const std::vector<DiagnosticType> &source) {
    destination.insert(destination.end(), source.begin(), source.end());
  }

  FrontendOptions options;
};

} // namespace lang
