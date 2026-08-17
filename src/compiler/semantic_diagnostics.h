// Construction of semantic diagnostics: the exact codes, messages, spans,
// related entries, and hints the analyzer publishes. The reporter is pure
// output — it binds the diagnostic sink and the semantic model for type
// spelling, and every analysis fact it quotes arrives as a parameter, so
// no analysis state or decision lives here.
#pragma once

#include "gti/ast.h"
#include "gti/constant_evaluator.h"
#include "gti/diagnostic.h"
#include "gti/generic_constraint.h"
#include "gti/semantic_analyzer.h"

#include <string>
#include <vector>

namespace lang {

// The facts of one generic-constraint failure, assembled by overload and
// requirement analysis and quoted verbatim by the reporter.
struct SemanticConstraintFailure {
  Token parameter;
  SemanticType argument = SemanticType::Unknown;
  GenericConstraintSet constraints = 0;
  GenericConstraintKind failed = GenericConstraintKind::None;
  std::optional<NamePath> constraintName;
  StructuralConstraintKind structural = StructuralConstraintKind::None;
  const ConceptApplication *application = nullptr;
};

class SemanticDiagnosticReporter {
public:
  SemanticDiagnosticReporter(const SemanticModel &semantics,
                             std::vector<Diagnostic> &diagnostics)
      : semantics(semantics), diagnostics(diagnostics) {}

  void report(const Token &token, std::string message,
              std::string code = "GTI-S2000");

  // A failed constexpr binding initializer: the failure kind selects the
  // exact message and hint, and the declaration anchors the related entry.
  void reportConstexprFailure(const VariableDecl &declaration,
                              const SemanticType &type,
                              ConstantEvaluationFailure failure,
                              const std::string &failureDetail,
                              const Token &location);

  // A failed 'if constexpr' condition evaluation.
  void reportConstexprConditionFailure(const IfStmt &statement,
                                       ConstantEvaluationFailure failure,
                                       const std::string &failureDetail,
                                       const Token &location);

  // A generic argument that fails its declared constraint: concept,
  // structural, or built-in constraint kinds each carry their exact
  // message and hint set.
  void reportConstraintFailure(const Token &site,
                               const SemanticConstraintFailure &failure);

private:
  [[nodiscard]] std::string typeSpelling(const SemanticType &type) const;

  const SemanticModel &semantics;
  std::vector<Diagnostic> &diagnostics;
};

} // namespace lang
