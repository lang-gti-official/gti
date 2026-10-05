#include "semantic_diagnostics.h"

#include "semantic_spelling.h"

#include <functional>

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
      "Use literals, earlier constexpr bindings, supported operators and "
      "conversions, fixed-array or structural-field observation, or calls "
      "to available constexpr functions and constructors.";
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
              "nullptr_t values, scoped enums, concrete fixed arrays, "
              "structural aggregates, and checked-integer expected results; "
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
  const auto applicationSpelling = [&](const SemanticConstraintContext &item) {
    std::string spelling = item.name + "<";
    for (std::size_t index = 0; index < item.arguments.size(); ++index) {
      if (index != 0)
        spelling += ", ";
      spelling += typeSpelling(item.arguments[index]);
    }
    return spelling + ">";
  };
  const auto appendMemberReason = [&](Diagnostic &diagnostic,
                                      const SemanticConstraintFailure &leaf) {
    if (!leaf.memberFailure)
      return;
    const auto &detail = *leaf.memberFailure;
    std::string reason;
    switch (detail.kind) {
    case MemberRequirementFailureKind::Missing:
      reason = "Type '" + typeSpelling(leaf.argument) + "' has no member '" +
               leaf.memberName + "()'.";
      break;
    case MemberRequirementFailureKind::Shape:
      if (leaf.leafApplication && leaf.leafApplication->memberRequirement &&
          leaf.leafApplication->memberRequirement->parameters().empty() &&
          !leaf.leafApplication->memberRequirement->receiver()
               .referenceMutability)
        reason = "Member '" + leaf.memberName +
                 "' is not a read-only, nongeneric instance method taking no "
                 "arguments.";
      else
        reason = "Member '" + leaf.memberName +
                 "' does not match the required nongeneric instance signature "
                 "and receiver access.";
      break;
    case MemberRequirementFailureKind::Ambiguous:
      reason = "More than one member matches the required '" + leaf.memberName +
               "()' call.";
      break;
    case MemberRequirementFailureKind::Inaccessible:
      reason = "Member '" + leaf.memberName + "()' is not publicly accessible.";
      break;
    case MemberRequirementFailureKind::Unsafe:
      reason = "Member '" + leaf.memberName + "()' requires unsafe access.";
      break;
    case MemberRequirementFailureKind::Result:
      reason = "Member '" + leaf.memberName + "()' returns '" +
               typeSpelling(detail.actualResult) +
               "', but the required exact "
               "result is '" +
               typeSpelling(detail.requiredResult) + "'.";
      break;
    case MemberRequirementFailureKind::BorrowedResult:
      reason = "Member '" + leaf.memberName +
               "()' returns a reference or tracked borrowed state; this "
               "requirement needs an owned result.";
      break;
    case MemberRequirementFailureKind::Constraints:
      reason = "Member '" + leaf.memberName +
               "()' has unsatisfied requirements of its own.";
      break;
    case MemberRequirementFailureKind::Recursive:
      reason = "Checking member '" + leaf.memberName +
               "()' reaches a recursive requirement or the requirement depth "
               "limit.";
      break;
    case MemberRequirementFailureKind::Unproven:
      reason = "The enclosing generic requirements do not prove member '" +
               leaf.memberName + "()' with the required exact result.";
      break;
    case MemberRequirementFailureKind::None:
      return;
    }
    const Token *location = detail.declaration ? &*detail.declaration : nullptr;
    if (location == nullptr && leaf.leafApplication != nullptr &&
        leaf.leafApplication->memberRequirement != nullptr) {
      location = &leaf.leafApplication->memberRequirement->member();
    }
    if (location != nullptr)
      diagnostic.related.push_back({tokenSpan(*location), std::move(reason)});
    else
      diagnostic.hints.push_back(std::move(reason));
  };
  const auto publish = [&](Diagnostic diagnostic) {
    if (failure.callArgument && failure.callArgumentIndex) {
      diagnostic.primary = tokenSpan(*failure.callArgument);
      std::string prefix =
          "Argument " + std::to_string(*failure.callArgumentIndex + 1) +
          " has type '" + typeSpelling(failure.callArgumentType) + "'";
      prefix +=
          failure.callArgumentType == failure.argument
              ? ", which "
              : "; its inferred type '" + typeSpelling(failure.argument) + "' ";
      if (!failure.context.empty()) {
        diagnostic.message = prefix + "does not satisfy concept '" +
                             applicationSpelling(failure.context.front()) +
                             "'.";
      } else if (failure.structural == StructuralConstraintKind::ReadMember) {
        diagnostic.message = prefix + "does not satisfy required member '" +
                             failure.memberName + "()'.";
      } else {
        diagnostic.message =
            prefix + "does not satisfy generic constraint '" +
            (failure.constraintName
                 ? semantic_spelling::pathSpelling(*failure.constraintName)
                 : "the declared constraint") +
            "'.";
      }
    }
    for (const auto &context : failure.context) {
      diagnostic.related.push_back(
          {tokenSpan(context.location),
           "Required application '" + applicationSpelling(context) + "'."});
    }
    appendMemberReason(diagnostic, failure);
    diagnostics.emplace_back(std::move(diagnostic));
  };
  if (!failure.alternatives.empty()) {
    const std::string constraint =
        failure.constraintName
            ? semantic_spelling::pathSpelling(*failure.constraintName)
            : std::string{};
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2029", DiagnosticPhase::Semantics, site,
        constraint.empty()
            ? "Generic arguments do not satisfy any alternative in the "
              "requires clause."
            : "Generic arguments do not satisfy any alternative in concept '" +
                  constraint + "'.");
    if (failure.disjunction != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(*failure.disjunction),
           "Every branch joined by this disjunction was unsatisfied."});
    }
    std::function<void(const SemanticConstraintFailure &)> appendAlternatives =
        [&](const SemanticConstraintFailure &alternative) {
          if (!alternative.alternatives.empty()) {
            for (const SemanticConstraintFailure &nested :
                 alternative.alternatives) {
              appendAlternatives(nested);
            }
            return;
          }
          const std::string alternativeName =
              alternative.constraintName
                  ? semantic_spelling::pathSpelling(*alternative.constraintName)
                  : "the declared constraint";
          const Token *location = alternative.application == nullptr
                                      ? nullptr
                                      : &alternative.application->name.last();
          if (location == nullptr && alternative.constraintName) {
            location = &alternative.constraintName->last();
          }
          if (location == nullptr) {
            return;
          }
          diagnostic.related.push_back(
              {tokenSpan(*location),
               alternative.structural != StructuralConstraintKind::None
                   ? "Alternative concept '" + alternativeName +
                         "' lacks its required structural capability."
                   : "Alternative concept '" + alternativeName +
                         "' is not satisfied by type '" +
                         typeSpelling(alternative.argument) + "'."});
          for (const auto &context : alternative.context) {
            diagnostic.related.push_back(
                {tokenSpan(context.location), "Unsatisfied alternative '" +
                                                  applicationSpelling(context) +
                                                  "'."});
          }
          appendMemberReason(diagnostic, alternative);
        };
    for (const SemanticConstraintFailure &alternative : failure.alternatives) {
      appendAlternatives(alternative);
    }
    diagnostic.hints.emplace_back(
        "At least one complete '||' branch must be satisfied; capabilities "
        "from different alternatives are not combined.");
    publish(std::move(diagnostic));
    return;
  }
  if (failure.structural != StructuralConstraintKind::None) {
    const MemberRequirement *memberQuery =
        failure.leafApplication == nullptr
            ? nullptr
            : failure.leafApplication->memberRequirement.get();
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
        arguments += semantic_spelling::typeRefSpelling(
            failure.application->arguments[index]);
      }
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2029", DiagnosticPhase::Semantics, site,
        memberQuery != nullptr && !failure.constraintName
            ? "Generic arguments do not satisfy required member '" +
                  memberQuery->member().lexeme + "()'."
            : "Generic arguments do not satisfy concept '" + constraint + "'.");
    if (memberQuery != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(memberQuery->member()),
           "Required here as '" + memberQuery->member().lexeme + "() -> " +
               semantic_spelling::typeRefSpelling(memberQuery->result()) +
               "' on '" +
               semantic_spelling::typeRefSpelling(memberQuery->receiver()) +
               "'."});
    } else if (failure.application != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(failure.application->name.last()),
           "Required here as '" + constraint + "<" + arguments + ">'."});
    }
    switch (failure.structural) {
    case StructuralConstraintKind::ReadMember:
      if (memberQuery && memberQuery->parameters().empty() &&
          !memberQuery->receiver().referenceMutability)
        diagnostic.hints.emplace_back(
            "Provide a public, safe, read-only '" + failure.memberName +
            "()' member with the exact required owned result type.");
      else
        diagnostic.hints.emplace_back(
            "Provide a public, safe '" + failure.memberName +
            "' member with the required receiver access, exact parameter types "
            "and exact owned result type.");
      break;
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
    case StructuralConstraintKind::DereferencesTo:
      diagnostic.hints.emplace_back(
          "The iterator's read-only dereference must return a checked "
          "reference whose referent is exactly the required type.");
      break;
    case StructuralConstraintKind::WritableIterator:
      diagnostic.hints.emplace_back(
          "The iterator must provide public mut operator*() returning a "
          "mutable checked reference and public void operator++() mut.");
      break;
    case StructuralConstraintKind::WritesTo:
      diagnostic.hints.emplace_back(
          "The iterator's mut operator*() must return a mutable checked "
          "reference whose referent is exactly the required type.");
      break;
    case StructuralConstraintKind::BidirectionalIterator:
      diagnostic.hints.emplace_back(
          "The iterator must provide public void operator--() mut.");
      break;
    case StructuralConstraintKind::RandomAccessIterator:
      diagnostic.hints.emplace_back(
          "The iterator must provide public void operator+=(std::ptrdiff_t) "
          "mut and public void operator-=(std::ptrdiff_t) mut.");
      break;
    case StructuralConstraintKind::SubscriptsToMut:
      diagnostic.hints.emplace_back(
          "The iterator's mut operator[](std::ptrdiff_t) must return a "
          "mutable checked reference whose referent is exactly the required "
          "type.");
      break;
    case StructuralConstraintKind::SizedSentinelFor:
      diagnostic.hints.emplace_back(
          "The iterator must provide public read-only "
          "std::ptrdiff_t operator-(S&) for the exact sentinel type, "
          "returning the signed distance from the iterator to the sentinel.");
      break;
    case StructuralConstraintKind::InputRange:
      diagnostic.hints.emplace_back(
          "The range must provide exact public read-only begin()/end(); its "
          "returned iterator and sentinel must satisfy the input-iterator "
          "and exact-sentinel contracts.");
      break;
    case StructuralConstraintKind::RangeOf:
      diagnostic.hints.emplace_back(
          "The complete read-only range must dereference to a checked "
          "reference whose referent is exactly the required element type.");
      break;
    case StructuralConstraintKind::MutableRangeOf:
      diagnostic.hints.emplace_back(
          "The mutable range must provide exact public begin_mut()/end(); "
          "mutable dereference must return the required exact element "
          "reference and prefix increment must return void.");
      break;
    case StructuralConstraintKind::SizedRange:
      diagnostic.hints.emplace_back(
          "The range must provide one exact public read-only size() returning "
          "std::size_t.");
      break;
    case StructuralConstraintKind::Adds:
      diagnostic.hints.emplace_back(
          "The domain type must provide a public read-only "
          "'T operator+(T&)' returning an owned value of its own type.");
      break;
    case StructuralConstraintKind::Subtracts:
      diagnostic.hints.emplace_back(
          "The domain type must provide a public read-only "
          "'T operator-(T&)' returning an owned value of its own type.");
      break;
    case StructuralConstraintKind::Negates:
      diagnostic.hints.emplace_back(
          "The domain type must provide a public read-only unary "
          "'T operator-()' returning an owned value of its own type.");
      break;
    case StructuralConstraintKind::ScalesBy:
      diagnostic.hints.emplace_back(
          "The domain type must provide a public read-only "
          "'T operator*(Scalar)' returning an owned value of its own type "
          "for the exact scalar type.");
      break;
    case StructuralConstraintKind::ReadCallable:
    case StructuralConstraintKind::MutCallable:
    case StructuralConstraintKind::OnceCallable:
      diagnostic.hints.emplace_back(
          "The callable must provide one exact public unary operator() with "
          "the declared owned result and argument taken by value or "
          "read-only reference; its receiver capability must satisfy the "
          "declared read, mut, or once use site.");
      break;
    case StructuralConstraintKind::ReadBinaryCallable:
    case StructuralConstraintKind::MutBinaryCallable:
    case StructuralConstraintKind::OnceBinaryCallable:
      diagnostic.hints.emplace_back(
          "The callable must provide one exact public binary operator() with "
          "the declared owned result and arguments taken by value or "
          "read-only reference; its receiver capability must satisfy the "
          "declared read, mut, or once use site.");
      break;
    case StructuralConstraintKind::OnceAction:
      diagnostic.hints.emplace_back(
          "The action must provide one exact public void operator()() usable "
          "through a consuming receiver; read-only and mutable reusable "
          "targets also satisfy this once-only use site.");
      break;
    case StructuralConstraintKind::IndirectPredicate:
      diagnostic.hints.emplace_back(
          "The iterator's read-only dereference result must be accepted by "
          "one exact repeatable public callable returning bool.");
      break;
    case StructuralConstraintKind::IndirectOperation:
      diagnostic.hints.emplace_back(
          "The iterator's read-only dereference result must be accepted by "
          "one exact repeatable public callable returning the required owned "
          "result type.");
      break;
    case StructuralConstraintKind::RangePredicate:
      diagnostic.hints.emplace_back(
          "The complete range's read-only element must be accepted by one "
          "exact repeatable public callable returning bool.");
      break;
    case StructuralConstraintKind::RangeOperation:
      diagnostic.hints.emplace_back(
          "The complete range's read-only element must be accepted by one "
          "exact repeatable public callable returning the required owned "
          "result type.");
      break;
    case StructuralConstraintKind::None:
      break;
    }
    publish(std::move(diagnostic));
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
  case GenericConstraintKind::CopyConstructible:
    diagnostic.hints.emplace_back(
        "The required concept needs an available copy constructor; copy "
        "assignment is not required.");
    break;
  case GenericConstraintKind::MoveConstructible:
    diagnostic.hints.emplace_back(
        "The required concept needs available move construction; move "
        "assignment is not required.");
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
  case GenericConstraintKind::Relocatable:
    diagnostic.hints.emplace_back(
        "Relocation requires available structural movement and no stored "
        "borrows or address-stable host state; it does not require "
        "assignment.");
    break;
  case GenericConstraintKind::BitwiseRelocatable:
    diagnostic.hints.emplace_back(
        "Bytewise eligibility requires passive scalar/array state or a "
        "non-polymorphic class with no bases, declared cleanup, or "
        "non-bitwise fields.");
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
  case GenericConstraintKind::AtomicScalar:
    diagnostic.hints.emplace_back(
        "The atomic scalar domain contains bool and the fixed-width signed "
        "and unsigned integer types only.");
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
  publish(std::move(diagnostic));
}

void SemanticDiagnosticReporter::reportConstexprConditionFailure(
    const IfStmt &statement, ConstantEvaluationFailure failure,
    const std::string &failureDetail, const Token &location) {
  std::string message =
      "The condition of 'if constexpr' must be a constant bool expression.";
  std::string hint =
      "Use literals, earlier constexpr bindings, supported aggregate "
      "observation, or calls to available constexpr functions.";
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
      "queries, aggregate observation, or calls to available constexpr "
      "functions.";
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
