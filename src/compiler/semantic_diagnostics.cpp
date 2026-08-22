#include "semantic_diagnostics.h"

#include "semantic_spelling.h"

namespace lang {

std::string
SemanticDiagnosticReporter::typeSpelling(const SemanticType &type) const {
  return semantic_spelling::typeSpelling(semantics, type);
}

void SemanticDiagnosticReporter::report(const Token &token, std::string message,
                                        std::string code) {
  diagnostics.push_back(makeDiagnostic(
      std::move(code), DiagnosticPhase::Semantics, token, std::move(message)));
}

void SemanticDiagnosticReporter::reportConstexprFailure(
    const VariableDecl &declaration, const SemanticType &type,
    ConstantEvaluationFailure failure, const std::string &failureDetail,
    const Token &location) {
  std::string message = "The initializer for constexpr binding '" +
                        declaration.name().lexeme +
                        "' is not a constant expression.";
  std::string hint =
      "Use literals, earlier constexpr bindings, scalar operators, and "
      "explicit numeric conversions in this constexpr initializer.";
  switch (failure) {
  case ConstantEvaluationFailure::NonConstantReference:
    message = "The initializer for constexpr binding '" +
              declaration.name().lexeme +
              "' reads a binding that is not constexpr.";
    break;
  case ConstantEvaluationFailure::ResourceLimit:
    message = "The constexpr initializer exceeded the compiler's bounded "
              "evaluation limit.";
    hint = "Reduce constexpr recursion or loop work, or split the "
           "calculation into smaller constexpr bindings.";
    break;
  case ConstantEvaluationFailure::IntegerOverflow:
    message = "Integer overflow occurred while evaluating constexpr binding '" +
              declaration.name().lexeme + "'.";
    hint = "Use a wider explicit integer type or keep the result within its "
           "declared range.";
    break;
  case ConstantEvaluationFailure::DivisionByZero:
    message = "Division by zero occurred in a constexpr initializer.";
    break;
  case ConstantEvaluationFailure::ModuloByZero:
    message = "Modulo by zero occurred in a constexpr initializer.";
    break;
  case ConstantEvaluationFailure::NegativeShiftCount:
    message = "A constexpr shift count cannot be negative.";
    break;
  case ConstantEvaluationFailure::ShiftCountOutOfRange:
    message = "A constexpr shift count must be smaller than the width of "
              "the shifted integer type.";
    break;
  case ConstantEvaluationFailure::ConversionOutOfRange:
    message = "A constexpr integer conversion is outside the target type's "
              "range.";
    break;
  case ConstantEvaluationFailure::UnsupportedType:
    message = "The bounded constexpr evaluator supports fixed-width "
              "integers, float, double, bool, char, string_view, and "
              "nullptr_t values, plus checked-integer expected results; "
              "type '" +
              typeSpelling(type) + "' is not supported yet.";
    break;
  case ConstantEvaluationFailure::UnsupportedExpression:
    message = "This expression is not supported by the bounded "
              "constexpr evaluator.";
    break;
  case ConstantEvaluationFailure::InvalidOperands:
    message = "The constexpr evaluator cannot apply this operation to "
              "these operand types.";
    break;
  case ConstantEvaluationFailure::None:
    break;
  }
  if (!failureDetail.empty()) {
    message += " " + failureDetail;
  }

  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2057", DiagnosticPhase::Semantics,
                     location.lexeme.empty() ? declaration.name() : location,
                     std::move(message));
  diagnostic.related.push_back({tokenSpan(*declaration.constexprKeyword()),
                                "constexpr binding declared here."});
  diagnostic.hints.emplace_back(std::move(hint));
  diagnostics.emplace_back(std::move(diagnostic));
}

void SemanticDiagnosticReporter::reportConstraintFailure(
    const Token &site, const SemanticConstraintFailure &failure) {
  if (failure.structural != StructuralConstraintKind::None) {
    const std::string constraint =
        failure.constraintName
            ? semantic_spelling::pathSpelling(*failure.constraintName)
            : "the declared requirement";
    std::string arguments;
    if (failure.application != nullptr) {
      for (std::size_t index = 0; index < failure.application->arguments.size();
           ++index) {
        if (index != 0) {
          arguments += ", ";
        }
        arguments += failure.application->arguments[index].lexeme;
      }
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2029", DiagnosticPhase::Semantics, site,
        "Generic arguments do not satisfy concept '" + constraint + "'.");
    if (failure.application != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(failure.application->name.last()),
           "Required here as '" + constraint + "<" + arguments + ">'."});
    }
    switch (failure.structural) {
    case StructuralConstraintKind::InputIterator:
      diagnostic.hints.emplace_back(
          "The iterator must provide public read-only operator*() returning "
          "a checked reference and public void operator++() mut.");
      break;
    case StructuralConstraintKind::SentinelFor:
      diagnostic.hints.emplace_back(
          "The iterator must provide public read-only bool operator!=(S&) "
          "for the exact sentinel type.");
      break;
    case StructuralConstraintKind::AccumulatesInto:
      diagnostic.hints.emplace_back(
          "The iterator's read-only dereference must return a checked "
          "reference whose referent is exactly the accumulator type.");
      break;
    case StructuralConstraintKind::None:
      break;
    }
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  const std::string constraint =
      failure.constraintName
          ? semantic_spelling::pathSpelling(*failure.constraintName)
          : "the declared constraint";
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2029", DiagnosticPhase::Semantics, site,
      "Type '" + typeSpelling(failure.argument) +
          "' does not satisfy generic constraint '" + constraint +
          "' for parameter '" + failure.parameter.lexeme + "'.");
  if (failure.constraintName) {
    diagnostic.related.push_back({tokenSpan(failure.constraintName->last()),
                                  "Constraint on generic parameter '" +
                                      failure.parameter.lexeme +
                                      "' is declared here."});
  }
  switch (failure.failed) {
  case GenericConstraintKind::EqualityComparable:
    diagnostic.hints.emplace_back(
        "Class types must provide public, read-only bool operator==(" +
        typeSpelling(failure.argument) + "& other) and operator!= overloads.");
    break;
  case GenericConstraintKind::RelationallyOrdered:
    diagnostic.hints.emplace_back(
        "Class types must provide exact public, read-only bool overloads "
        "for <, <=, >, and >=.");
    break;
  case GenericConstraintKind::Copyable:
    diagnostic.hints.emplace_back(
        "The required concept needs available copy construction and "
        "assignment.");
    break;
  case GenericConstraintKind::Movable:
    diagnostic.hints.emplace_back(
        "The required concept needs available move construction and "
        "assignment.");
    break;
  case GenericConstraintKind::Transferable:
    diagnostic.hints.emplace_back(
        "The required concept needs state and cleanup that can safely move "
        "between threads.");
    break;
  case GenericConstraintKind::Shareable:
    diagnostic.hints.emplace_back(
        "The required concept needs read-only shared access that is safe "
        "across threads.");
    break;
  case GenericConstraintKind::DefaultInitializable:
    diagnostic.hints.emplace_back(
        "The required concept needs a public zero-argument constructor.");
    break;
  case GenericConstraintKind::None:
  case GenericConstraintKind::Invalid:
  case GenericConstraintKind::Count:
  case GenericConstraintKind::Numeric:
  case GenericConstraintKind::SignedNumeric:
  case GenericConstraintKind::Integral:
  case GenericConstraintKind::SignedIntegral:
  case GenericConstraintKind::UnsignedIntegral:
  case GenericConstraintKind::FloatingPoint:
    break;
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

void SemanticDiagnosticReporter::reportConstexprConditionFailure(
    const IfStmt &statement, ConstantEvaluationFailure failure,
    const std::string &failureDetail, const Token &location) {
  std::string message =
      "The condition of 'if constexpr' must be a constant bool expression.";
  std::string hint =
      "Use literals, earlier constexpr bindings, or calls to available "
      "constexpr functions.";
  switch (failure) {
  case ConstantEvaluationFailure::NonConstantReference:
    message = "The condition of 'if constexpr' reads a binding that is not "
              "constexpr.";
    break;
  case ConstantEvaluationFailure::ResourceLimit:
    message = "The condition of 'if constexpr' exceeded the compiler's "
              "bounded evaluation limit.";
    hint = "Reduce constexpr recursion or loop work in the condition.";
    break;
  case ConstantEvaluationFailure::IntegerOverflow:
    message = "Integer overflow occurred while evaluating an 'if "
              "constexpr' condition.";
    break;
  case ConstantEvaluationFailure::DivisionByZero:
    message = "Division by zero occurred in an 'if constexpr' condition.";
    break;
  case ConstantEvaluationFailure::ModuloByZero:
    message = "Modulo by zero occurred in an 'if constexpr' condition.";
    break;
  case ConstantEvaluationFailure::NegativeShiftCount:
    message = "An 'if constexpr' shift count cannot be negative.";
    break;
  case ConstantEvaluationFailure::ShiftCountOutOfRange:
    message = "An 'if constexpr' shift count exceeds the integer width.";
    break;
  case ConstantEvaluationFailure::ConversionOutOfRange:
    message = "An integer conversion in an 'if constexpr' condition is "
              "outside the target type's range.";
    break;
  case ConstantEvaluationFailure::UnsupportedType:
    message = "The 'if constexpr' condition uses a type that the bounded "
              "constexpr evaluator does not support.";
    break;
  case ConstantEvaluationFailure::UnsupportedExpression:
    break;
  case ConstantEvaluationFailure::InvalidOperands:
    message = "The constexpr evaluator cannot apply an operation in this "
              "'if constexpr' condition.";
    break;
  case ConstantEvaluationFailure::None:
    break;
  }
  if (!failureDetail.empty()) {
    message += " " + failureDetail;
  }
  const Token &keyword = *statement.constexprKeyword();
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2057", DiagnosticPhase::Semantics,
      location.lexeme.empty() ? keyword : location, std::move(message));
  diagnostic.related.push_back(
      {tokenSpan(keyword), "Compile-time branch declared here."});
  diagnostic.hints.emplace_back(std::move(hint));
  diagnostics.emplace_back(std::move(diagnostic));
}

void SemanticDiagnosticReporter::reportStaticAssertEvaluationFailure(
    const StaticAssertDecl &declaration, ConstantEvaluationFailure failure,
    const std::string &failureDetail, const Token &location) {
  std::string message =
      "The condition of 'static_assert' must be a constant bool expression.";
  std::string hint =
      "Use literals, earlier constexpr bindings, supported target layout "
      "queries, or calls to available constexpr functions.";
  switch (failure) {
  case ConstantEvaluationFailure::NonConstantReference:
    message = "The condition of 'static_assert' reads a binding that is not "
              "constexpr.";
    break;
  case ConstantEvaluationFailure::ResourceLimit:
    message = "The condition of 'static_assert' exceeded the compiler's "
              "bounded evaluation limit.";
    hint = "Reduce constexpr recursion or loop work in the condition.";
    break;
  case ConstantEvaluationFailure::IntegerOverflow:
    message = "Integer overflow occurred while evaluating a 'static_assert' "
              "condition.";
    break;
  case ConstantEvaluationFailure::DivisionByZero:
    message = "Division by zero occurred in a 'static_assert' condition.";
    break;
  case ConstantEvaluationFailure::ModuloByZero:
    message = "Modulo by zero occurred in a 'static_assert' condition.";
    break;
  case ConstantEvaluationFailure::NegativeShiftCount:
    message = "A 'static_assert' shift count cannot be negative.";
    break;
  case ConstantEvaluationFailure::ShiftCountOutOfRange:
    message = "A 'static_assert' shift count exceeds the integer width.";
    break;
  case ConstantEvaluationFailure::ConversionOutOfRange:
    message = "An integer conversion in a 'static_assert' condition is "
              "outside the target type's range.";
    break;
  case ConstantEvaluationFailure::UnsupportedType:
    message = "The 'static_assert' condition uses a type that the bounded "
              "constexpr evaluator does not support.";
    break;
  case ConstantEvaluationFailure::InvalidOperands:
    message = "The constexpr evaluator cannot apply an operation in this "
              "'static_assert' condition.";
    break;
  case ConstantEvaluationFailure::UnsupportedExpression:
  case ConstantEvaluationFailure::None:
    break;
  }
  if (!failureDetail.empty()) {
    message += " " + failureDetail;
  }
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2057", DiagnosticPhase::Semantics,
                     location.lexeme.empty() ? declaration.keyword() : location,
                     std::move(message));
  diagnostic.related.push_back(
      {tokenSpan(declaration.keyword()), "Static assertion declared here."});
  diagnostic.hints.emplace_back(std::move(hint));
  diagnostics.emplace_back(std::move(diagnostic));
}

} // namespace lang
