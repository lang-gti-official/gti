#include "semantic_diagnostics.h"

#include "semantic_spelling.h"

#include <algorithm>
#include <functional>
#include <tuple>

namespace lang {

SemanticMoveOrigins
SemanticMoveOrigins::merge(const SemanticMoveOrigins &left,
                           const SemanticMoveOrigins &right) {
  SemanticMoveOrigins result;
  result.sites = left.sites;
  result.sites.insert(result.sites.end(), right.sites.begin(),
                      right.sites.end());
  std::sort(result.sites.begin(), result.sites.end(),
            [](const auto &a, const auto &b) {
              return std::tie(a.source, a.start, a.end) <
                     std::tie(b.source, b.start, b.end);
            });
  result.sites.erase(std::unique(result.sites.begin(), result.sites.end(),
                                 [](const auto &a, const auto &b) {
                                   return a.source == b.source &&
                                          a.start == b.start && a.end == b.end;
                                 }),
                     result.sites.end());
  // Omitted sites can overlap across repeated joins. Summing their counts
  // would invent causes; retain only a provable lower bound on distinct sites.
  const auto known =
      std::max({result.sites.size(), left.sites.size() + left.omittedLowerBound,
                right.sites.size() + right.omittedLowerBound});
  constexpr std::size_t limit = 4;
  if (result.sites.size() > limit)
    result.sites.resize(limit);
  result.omittedLowerBound = known - result.sites.size();
  return result;
}

void SemanticMoveOrigins::appendTo(Diagnostic &diagnostic) const {
  for (const auto &origin : sites)
    diagnostic.related.push_back({origin, "Ownership transferred here."});
  if (sites.size() > 1 || omittedLowerBound != 0)
    diagnostic.hints.emplace_back(
        "These transfer locations are alternatives across control-flow paths, "
        "not a single execution trace.");
  if (omittedLowerBound != 0)
    diagnostic.hints.push_back(
        "At least " + std::to_string(omittedLowerBound) +
        " additional ownership-transfer " +
        (omittedLowerBound == 1 ? "location is" : "locations are") +
        " omitted from this explanation.");
}

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
    const Token &location, const std::optional<Token> &libraryLocation) {
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
  if (libraryLocation)
    diagnostic.related.insert(diagnostic.related.begin(),
                              {tokenSpan(*libraryLocation),
                               "Reported inside this library instance."});
  diagnostics.emplace_back(std::move(diagnostic));
}

SemanticMemberConstraintCause
SemanticMemberConstraintCause::retain(SemanticConstraintFailure failure) {
  SemanticMemberConstraintCause cause;
  // Disjunction admission already proved every branch false. Retain one leaf
  // in source order, explicitly accounting for the branches not displayed.
  while (!failure.alternatives.empty()) {
    cause.omittedAlternatives += failure.alternatives.size() - 1;
    SemanticConstraintFailure first = std::move(failure.alternatives.front());
    failure = std::move(first);
  }
  if (failure.memberFailure && failure.memberFailure->constraintCause) {
    const auto &nested = *failure.memberFailure->constraintCause;
    cause.leaf = nested.leaf;
    cause.omittedMemberSteps = nested.omittedMemberSteps + 1;
    cause.omittedAlternatives += nested.omittedAlternatives;
    cause.omittedApplications = nested.omittedApplications;
    return cause;
  }
  // Keep the outer request and innermost applications. The selected leaf and
  // its exact type/signature facts are never replaced by an elision marker.
  constexpr std::size_t contextLimit = 8;
  if (failure.context.size() > contextLimit) {
    cause.omittedApplications = failure.context.size() - contextLimit;
    failure.context.erase(failure.context.begin() + contextLimit / 2,
                          failure.context.end() - contextLimit / 2);
  }
  failure.callArgument.reset();
  failure.callArgumentIndex.reset();
  failure.candidate.reset();
  cause.leaf =
      std::make_shared<const SemanticConstraintFailure>(std::move(failure));
  return cause;
}

void SemanticDiagnosticReporter::reportConstraintFailure(
    const Token &site, const SemanticConstraintFailure &failure,
    const std::function<std::string(const SemanticType &)> &spelling) {
  const auto typeSpelling = [&](const SemanticType &type) {
    return spelling ? spelling(type) : this->typeSpelling(type);
  };
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
    case MemberRequirementFailureKind::CompilerPrivate:
      reason = "Member '" + leaf.memberName +
               "()' is compiler-private; only a direct requirement written in "
               "compiler-trusted source can select it, never a named concept.";
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
    if (detail.constraintCause && detail.constraintCause->leaf) {
      const auto &cause = *detail.constraintCause;
      if (detail.rejectedConstraintCandidates > 1)
        diagnostic.hints.push_back(
            "Showing the first of " +
            std::to_string(detail.rejectedConstraintCandidates) +
            " member candidates with unsatisfied requirements; their causes "
            "may differ.");
      std::vector<Diagnostic> nested;
      SemanticDiagnosticReporter(semantics, nested)
          .reportConstraintFailure(location ? *location : leaf.parameter,
                                   *cause.leaf, spelling);
      for (const Diagnostic &explanation : nested) {
        diagnostic.related.push_back(
            {explanation.primary,
             "Member requirement failed: " + explanation.message});
        diagnostic.related.insert(diagnostic.related.end(),
                                  explanation.related.begin(),
                                  explanation.related.end());
        for (const auto &hint : explanation.hints)
          diagnostic.hints.push_back("Member requirement: " + hint);
      }
      if (cause.omittedMemberSteps != 0)
        diagnostic.hints.push_back(std::to_string(cause.omittedMemberSteps) +
                                   " intermediate member-requirement steps "
                                   "omitted; the failing leaf is shown.");
      if (cause.omittedAlternatives != 0)
        diagnostic.hints.push_back("At least " +
                                   std::to_string(cause.omittedAlternatives) +
                                   " other failing disjunction branches "
                                   "omitted from the member explanation; "
                                   "the shown branch is representative, not an "
                                   "exhaustive repair list.");
      if (cause.omittedApplications != 0)
        diagnostic.hints.push_back(std::to_string(cause.omittedApplications) +
                                   " intermediate concept applications omitted "
                                   "from the member explanation.");
    }
  };
  const auto publish = [&](Diagnostic diagnostic) {
    if (failure.expressionFailure && !failure.context.empty()) {
      diagnostic.message =
          (failure.applicationArgumentIndex
               ? "Type '" + typeSpelling(failure.argument) +
                     "' does not satisfy concept '"
               : "Generic arguments do not satisfy concept '") +
          applicationSpelling(failure.context.front()) + "'.";
    }
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
    if (failure.expressionFailure) {
      const Token &location = failure.leafApplication
                                  ? failure.leafApplication->name.last()
                                  : failure.parameter;
      diagnostic.related.push_back(
          {tokenSpan(location), *failure.expressionFailure});
    }
    appendMemberReason(diagnostic, failure);
    if (failure.conformanceBorrowOperation) {
      diagnostic.hints.push_back(
          "Operation '" + failure.conformanceBorrowOperation->lexeme +
          "' must preserve the concept's borrowed_from parameter contract "
          "for its concrete reference result.");
      diagnostic.related.push_back(
          {tokenSpan(*failure.conformanceBorrowOperation),
           "The required result-origin contract is declared here."});
    }
    if (failure.conceptExplanation) {
      // The concept's author states what satisfies it. For a concept with a
      // body, conformance blocks are how a type does so, not what a caller
      // must provide, so the statement replaces the block hint.
      diagnostic.hints.insert(diagnostic.hints.begin(),
                              *failure.conceptExplanation);
    } else if (failure.missingConformance) {
      const std::string name =
          failure.constraintName
              ? semantic_spelling::pathSpelling(*failure.constraintName)
              : std::string("the concept");
      std::string arguments;
      for (const SemanticType &argument : failure.conformanceArguments) {
        arguments += (arguments.empty() ? "" : ", ") + typeSpelling(argument);
      }
      if (failure.conformanceArguments.empty()) {
        arguments = typeSpelling(failure.argument);
      }
      diagnostic.hints.push_back(
          "Concept '" + name + "' has a body, so " +
          (failure.conformanceArguments.size() > 1 ? "types satisfy"
                                                   : "a type satisfies") +
          " it only through a matching 'for concept " + name +
          "<...>' block; none matches '" + arguments + "'.");
    }
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
          if (alternative.expressionFailure &&
              !alternative.expressionFailure->empty())
            diagnostic.related.push_back(
                {tokenSpan(*location), *alternative.expressionFailure});
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
  if (failure.expressionFailure &&
      failure.failed == GenericConstraintKind::None) {
    const bool unaryPredicate =
        failure.leafApplication && failure.leafApplication->predicate &&
        failure.leafApplication->arguments.size() == 1 &&
        failure.argument != SemanticType::Unknown;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2029", DiagnosticPhase::Semantics, site,
        unaryPredicate
            ? "Type '" + typeSpelling(failure.argument) +
                  "' does not satisfy the required compile-time predicate."
            : "Generic arguments do not satisfy the required expression or "
              "compile-time predicate.");
    diagnostic.hints.emplace_back(
        "The expression must be valid under its declared access and unsafe "
        "contract, with the exact required result; a predicate must evaluate "
        "to true at compile time.");
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
      std::string signature = memberQuery->member().lexeme + "(";
      for (std::size_t index = 0; index < memberQuery->parameters().size();
           ++index) {
        if (index != 0)
          signature += ", ";
        const TypeRef &parameter = memberQuery->parameters()[index];
        if (parameter.referenceMutability)
          signature += "mut ";
        signature += semantic_spelling::typeRefSpelling(parameter);
      }
      signature += ")";
      if (memberQuery->receiver().referenceMutability)
        signature += " mut";
      if (memberQuery->unsafeKeyword())
        signature = "unsafe { " + signature + " }";
      diagnostic.related.push_back(
          {tokenSpan(memberQuery->member()),
           "Required here as '" + signature + " -> " +
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
    case StructuralConstraintKind::ReadMember: {
      const bool permitsUnsafe = memberQuery && memberQuery->unsafeKeyword();
      const std::string safety = permitsUnsafe ? "public" : "public, safe";
      if (memberQuery && memberQuery->parameters().empty() &&
          !memberQuery->receiver().referenceMutability)
        diagnostic.hints.emplace_back(
            "Provide a " + safety + ", read-only '" + failure.memberName +
            "()' member with the exact required owned result type.");
      else
        diagnostic.hints.emplace_back(
            "Provide a " + safety + " '" + failure.memberName +
            "' member with the required receiver access, exact parameter types "
            "and exact owned result type.");
      if (permitsUnsafe)
        diagnostic.hints.emplace_back(
            "This requirement permits a safe or unsafe member; generic calls "
            "through this requirement still need an explicit unsafe context.");
      break;
    }
    case StructuralConstraintKind::Iteration:
      diagnostic.hints.emplace_back(
          "Provide the operations and checked access required by the range "
          "loop in this source requirement.");
      break;
    case StructuralConstraintKind::EnumUnderlying:
      diagnostic.hints.emplace_back(
          "The first type must be an integral enum and the second its exact "
          "declared underlying integer type.");
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
  // A requirement may constrain a written type expression, such as a
  // callable's result, rather than a generic parameter; name that subject.
  const TypeRef *subject =
      failure.application != nullptr && failure.applicationArgumentIndex &&
              *failure.applicationArgumentIndex <
                  failure.application->arguments.size()
          ? &failure.application->arguments[*failure.applicationArgumentIndex]
          : nullptr;
  const bool expressionSubject =
      subject != nullptr &&
      (subject->name.segments.size() > 1 || !subject->arguments.empty());
  const std::string subjectSpelling =
      expressionSubject
          ? "type '" + semantic_spelling::typeRefSpelling(*subject) + "'"
          : "parameter '" + failure.parameter.lexeme + "'";
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2029", DiagnosticPhase::Semantics, site,
                     "Type '" + typeSpelling(failure.argument) +
                         "' does not satisfy generic constraint '" +
                         constraint + "' for " + subjectSpelling + ".");
  if (failure.constraintName) {
    diagnostic.related.push_back(
        {tokenSpan(failure.constraintName->last()),
         expressionSubject
             ? "Constraint on " + subjectSpelling + " is declared here."
             : "Constraint on generic parameter '" + failure.parameter.lexeme +
                   "' is declared here."});
  }
  switch (failure.failed) {
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
  case GenericConstraintKind::IntegralEnum:
    diagnostic.hints.emplace_back("The type must be an integral enum; payload "
                                  "enums have no underlying integer value.");
    break;
  case GenericConstraintKind::Reflectable:
    diagnostic.hints.emplace_back(
        "The type must be a struct or class whose instance fields are all "
        "public, or one declared [[reflect_private]].");
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
  case GenericConstraintKind::Boolean:
  case GenericConstraintKind::ObjectLayout:
  case GenericConstraintKind::BorrowFree:
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
