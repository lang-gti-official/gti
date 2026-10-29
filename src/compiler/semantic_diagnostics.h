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
#include "gti/semantic_model.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lang {

// Diagnostic-only witnesses, owned by the current semantic analysis. Joins
// retain a bounded, deterministic sample, not a second ownership analysis.
struct SemanticMoveOrigins {
  std::vector<SourceSpan> sites;
  std::size_t omittedLowerBound = 0;

  SemanticMoveOrigins() = default;
  SemanticMoveOrigins(SourceSpan origin) : sites{std::move(origin)} {}
  void clear() {
    sites.clear();
    omittedLowerBound = 0;
  }
  static SemanticMoveOrigins merge(const SemanticMoveOrigins &left,
                                   const SemanticMoveOrigins &right);
  void appendTo(Diagnostic &diagnostic) const;
};

// Identity is the shared record, not its message or position in a mutable
// diagnostics vector. Only diagnosed failures receive one; an incomplete or
// dependent Unknown type is not itself evidence of an error.
struct SemanticRecoveryCause {
  SourceSpan origin;
  std::string code;
};
using SemanticRecoveryIdentity = std::shared_ptr<const SemanticRecoveryCause>;

enum class MemberRequirementFailureKind {
  None,
  Unproven,
  Recursive,
  Missing,
  Shape,
  Ambiguous,
  Inaccessible,
  CompilerPrivate,
  Unsafe,
  Result,
  BorrowedResult,
  Constraints,
};

struct SemanticConstraintFailure;

// A representative leaf, not a second proof tree. Its AST references remain
// inside the owning semantic snapshot, just like the enclosing failure.
struct SemanticMemberConstraintCause {
  std::shared_ptr<const SemanticConstraintFailure> leaf;
  std::size_t omittedMemberSteps = 0;
  std::size_t omittedAlternatives = 0;
  std::size_t omittedApplications = 0;

  static SemanticMemberConstraintCause
  retain(SemanticConstraintFailure failure);
};

struct SemanticMemberRequirementFailure {
  MemberRequirementFailureKind kind = MemberRequirementFailureKind::None;
  std::optional<Token> declaration;
  SemanticType actualResult = SemanticType::Unknown;
  SemanticType requiredResult = SemanticType::Unknown;
  std::optional<SemanticMemberConstraintCause> constraintCause;
};

struct SemanticConstraintContext {
  std::string name;
  std::vector<SemanticType> arguments;
  Token location;
};

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
  const Token *disjunction = nullptr;
  std::vector<SemanticConstraintFailure> alternatives;
  std::string memberName;
  // Outermost source contract first; preserve its concrete application while
  // retaining the leaf that actually failed. This is not a second evaluator.
  std::vector<SemanticConstraintContext> context;
  const ConceptApplication *leafApplication = nullptr;
  std::optional<std::size_t> applicationArgumentIndex;
  GenericParameterId parameterId = 0;
  std::optional<SemanticMemberRequirementFailure> memberFailure;
  std::optional<std::size_t> callArgumentIndex;
  std::optional<Token> callArgument;
  SemanticType callArgumentType = SemanticType::Unknown;
  std::optional<Token> candidate;
  // A failed unevaluated expression or constexpr predicate, diagnosed by the
  // ordinary frontend in an isolated query rather than by native compilation.
  std::optional<std::string> expressionFailure;
  // A concept with a body that no conformance block supplies for these
  // arguments.
  bool missingConformance = false;
  std::vector<SemanticType> conformanceArguments;
  // A matching block exists, but a concrete reference result does not retain
  // the protocol's declaration-relative origin contract.
  std::optional<Token> conformanceBorrowOperation;
  // What satisfies the failed concept, from the outermost concept in the
  // failure that declares `[[unsatisfied("...")]]`.
  std::optional<std::string> conceptExplanation;
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
                              const Token &location,
                              const std::optional<Token> &libraryLocation = {});

  // A failed 'if constexpr' condition evaluation.
  void reportConstexprConditionFailure(const IfStmt &statement,
                                       ConstantEvaluationFailure failure,
                                       const std::string &failureDetail,
                                       const Token &location);

  // A well-typed static assertion whose condition cannot be evaluated by the
  // bounded GTI constexpr evaluator.
  void reportStaticAssertEvaluationFailure(const StaticAssertDecl &declaration,
                                           ConstantEvaluationFailure failure,
                                           const std::string &failureDetail,
                                           const Token &location);

  // A generic argument that fails its declared constraint: concept,
  // structural, or built-in constraint kinds each carry their exact
  // message and hint set.
  void reportConstraintFailure(
      const Token &site, const SemanticConstraintFailure &failure,
      const std::function<std::string(const SemanticType &)> &spelling = {});

private:
  [[nodiscard]] std::string typeSpelling(const SemanticType &type) const;

  const SemanticModel &semantics;
  std::vector<Diagnostic> &diagnostics;
};

} // namespace lang
