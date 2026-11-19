// Prefix `?` through the standard `std::tryable` protocol concept.
//
// Every operand, including the prelude's `expected`, must satisfy
// `std::tryable`, which the prelude declares; the compiler binds `?`
// to that concept by trusted declaration identity and knows nothing about the
// types that conform to it. The selected conformance block's functions run as
// ordinary calls. A residual initializes the enclosing return type directly,
// through its exact implicit constructor, or through std::from<Return, Residual>.
#include "semantic_analyzer_impl.h"

#include <algorithm>
#include <array>

namespace lang {

auto SemanticVisitor::Impl::tryableConcept() const -> std::optional<ConceptId> {
  for (std::size_t index = 0; index < concepts.size(); ++index) {
    const RegisteredConcept &conceptInfo = concepts[index];
    if (conceptInfo.protocol && conceptInfo.qualifiedName == "std::tryable" &&
        isPreludeUnit(conceptInfo.sourceUnit)) {
      return index + 1;
    }
  }
  return std::nullopt;
}

// One operation of a selected block as an ordinary call record: the block
// function with the block's binding as its explicit type arguments.
auto SemanticVisitor::Impl::protocolOperationCall(
    const RegisteredConformance &conformance,
    const std::vector<SemanticType> &binding, std::size_t operation) const
    -> std::optional<ResolvedCallInfo> {
  if (operation >= conformance.operations.size() ||
      conformance.operations[operation].overloads.size() != 1) {
    return std::nullopt;
  }
  const CallableCandidate &candidate =
      conformance.operations[operation].overloads.front();
  // The record built below is checked against the operation's body-derived
  // facts; the caller requests the body first (demand before record).
  checkBodyFactRead(candidate.bodyDeclaration(), candidate.id,
                    "a protocol operation call");
  const std::vector<GenericParameterInfo> &parameters =
      candidate.record().genericParameters;
  if (parameters.size() != binding.size()) {
    return std::nullopt;
  }
  GenericSubstitution substitution;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    substitution.types.emplace(parameters[index].id, binding[index]);
  }
  ResolvedCallInfo call{.function = candidate.id,
                        .declaration = candidate.declaration,
                        .returnType =
                            substituteType(candidate.returnType, substitution),
                        .typeArguments = binding};
  for (const SemanticType &parameter : candidate.parameterTypes) {
    call.parameterTypes.push_back(substituteType(parameter, substitution));
  }
  const auto &declaration = candidate.record();
  auto origin = functionReturnBorrowSummary(
      call.returnType, call.parameterTypes, 0, false, IntrinsicKind::None);
  if (candidate.declaration != nullptr &&
      (hasExplicitBorrowContract(*candidate.declaration) ||
       patternResultContract(*candidate.declaration, call.returnType))) {
    const auto contract = genericReturnBorrowContract(
        *candidate.declaration, call.returnType, call.parameterTypes, 0, false);
    origin = {.origin = contract.origin,
              .parameter = contract.parameter,
              .access = contract.access};
  }
  // Use the same elision as ordinary calls and concrete function analysis.
  // A reference-bearing result with no supplying input has static origins;
  // its complete contract can further prove an empty error alternative.
  if (origin.origin == BorrowOriginKind::None &&
      (candidate.declaration == nullptr ||
       !hasExplicitBorrowContract(*candidate.declaration)) &&
      implicitStaticCarrierResult(call.returnType, call.parameterTypes, 0,
                                  false, IntrinsicKind::None)) {
    origin.origin = BorrowOriginKind::Global;
    origin.access = AccessMode::ReadOnly;
  }
  call.borrowOrigin = origin.origin;
  call.borrowArgument = origin.parameter;
  call.borrowAccess = origin.access;
  call.borrowArguments = origin.parameters;
  call.borrowFields = declaration.returnBorrowFields;
  call.neverReturns = declaration.neverReturns;
  call.tracksCaller = declaration.tracksCaller;
  return call;
}

// The residual initializes the enclosing return type as `R result =
// residual;` would: it is R itself, or R has exactly one implicit constructor
// whose sole parameter is the residual's exact type. Otherwise std::from
// selects one explicitly registered conversion.
auto SemanticVisitor::Impl::selectResidualReturn(
    const Unary &expr, const SemanticType &returnType,
    const SemanticType &residualType,
    std::optional<ResolvedConstructionInfo> &construction,
    std::optional<ResolvedCallInfo> &conversion,
    std::optional<SemanticBorrowDependencySet> &conversionBorrow) -> bool {
  construction.reset();
  conversion.reset();
  const Token &site = expr.oper();
  const SemanticType target = selectExactClassSpecialization(returnType);
  if (target == canonicalExactSpecializationType(residualType)) {
    return true;
  }
  const ClassInfo *owner = classInfo(target);
  const auto reject = [&](std::string message, std::string hint) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2086", DiagnosticPhase::Semantics, site, std::move(message));
    if (owner != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(owner->name),
           "The enclosing return type is declared here."});
    }
    diagnostic.hints.emplace_back(std::move(hint));
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  };
  const std::string returned = "Error propagation returns the residual '" +
                               typeSpelling(residualType) + "' as '" +
                               typeSpelling(target) + "'";
  // Preserve direct residual construction. Otherwise an explicit protocol
  // conformance supplies exactly one conversion, with no conversion search.
  const auto convertResidual = [&]() -> std::optional<bool> {
    for (std::size_t index = 0; index < concepts.size(); ++index) {
      const auto &protocol = concepts[index];
      if (!protocol.protocol || protocol.qualifiedName != "std::from" ||
          !isPreludeUnit(protocol.sourceUnit))
        continue;
      const ConceptId id = index + 1;
      const std::vector<SemanticType> arguments{target, residualType};
      const auto match = matchConformance(id, arguments);
      if (!match)
        return std::nullopt;
      if (const auto failure = protocolConformanceFailure(
              {.conceptId = id, .arguments = arguments})) {
        reportConstraintFailure(site, *failure);
        return false;
      }
      const auto &block = conformances[match->conformance - 1];
      if (protocol.operations.size() != 1 ||
          protocol.operations.front().name != "from" ||
          block.operations.size() != 1 ||
          block.operations.front().overloads.size() != 1)
        return reject("The prelude's 'std::from' protocol is incomplete.",
                      "Declare one consuming 'To from(From value)' operation.");
      const auto &candidate = block.operations.front().overloads.front();
      (void)requireBody(candidate.bodyDeclaration());
      requireFunctionDefinition(candidate, site);
      conversion = protocolOperationCall(block, match->binding, 0);
      if (!conversion || conversion->returnType != target ||
          conversion->parameterTypes != std::vector<SemanticType>{residualType})
        return false;
      auto concrete = candidate;
      concrete.returnType = conversion->returnType;
      concrete.parameterTypes = conversion->parameterTypes;
      if (const auto complete = completeFunctionReturnBorrow(
              concrete, conversion->typeArguments, conversion->valueArguments,
              tokenSpan(site))) {
        const std::array<const ExprPtr *, 1> residualInput{nullptr};
        conversionBorrow = instantiateCompleteBorrowArguments(
            *conversion, *complete, residualInput);
      }
      return true;
    }
    return std::nullopt;
  };
  if (owner == nullptr) {
    if (const auto converted = convertResidual())
      return *converted;
    return reject(returned + ", which cannot be initialized from it.",
                  "Declare a 'std::from' conformance for the return type and "
                  "residual, return a type that accepts it, or handle the "
                  "failure explicitly instead of applying '?'.");
  }
  const GenericSubstitution substitution = classSubstitution(target);
  std::vector<const ConstructorInfo *> matches;
  for (const ConstructorInfo &constructor : owner->constructors) {
    if (!constructor.implicitConstruction || constructor.parameterPack ||
        !constructor.genericParameters.empty() ||
        constructor.kind != ConstructorKind::Ordinary ||
        constructor.parameterTypes.size() != 1 ||
        constructor.declaration == nullptr) {
      continue;
    }
    if (substituteType(constructor.parameterTypes.front(), substitution) ==
        residualType) {
      matches.push_back(&constructor);
    }
  }
  if (matches.empty())
    if (const auto converted = convertResidual())
      return *converted;
  if (matches.size() != 1) {
    // A trusted library type cannot gain a constructor from this program;
    // the failure itself must change before it can propagate here.
    return reject(returned + ", which declares " +
                      (matches.empty() ? "no" : "more than one") +
                      " implicit constructor taking exactly that residual.",
                  isCompilerTrustedUnit(owner->sourceUnit)
                      ? "Convert the operand's failure to a residual '" +
                            typeSpelling(target) +
                            "' accepts, or handle the failure explicitly "
                            "instead of applying '?'."
                      : "Declare one 'implicit " + owner->name.lexeme + "(" +
                            typeSpelling(residualType) +
                            " residual)' constructor without further "
                            "parameters.");
  }
  const ConstructorInfo &selected = *matches.front();
  if (!selected.requirements.empty() || selected.requiresUnsafe) {
    return reject("Error propagation cannot construct '" +
                      typeSpelling(target) +
                      "' through a constrained or unsafe implicit "
                      "constructor.",
                  "Declare the residual constructor without a requires "
                  "clause or 'unsafe'.");
  }
  if (!canAccessMember(selected.access, owner->id, target)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, site,
        "The residual constructor of '" + owner->name.lexeme + "' is " +
            std::string(accessModifierSpelling(selected.access)) + ".");
    diagnostic.related.push_back({tokenSpan(selected.declaration->name()),
                                  "Constructor declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  construction = ResolvedConstructionInfo{.constructor = selected.id,
                                          .declaration = selected.declaration,
                                          .constructedType = target,
                                          .parameterTypes = {residualType},
                                          .kind = ConstructorKind::Ordinary};
  return true;
}

auto SemanticVisitor::Impl::analyzeTryablePropagation(
    const Unary &expr, const SemanticType &operandType) -> SemanticType {
  const Token &site = expr.oper();
  const std::string operand = typeSpelling(operandType);
  const auto reject = [&](std::string message, std::string hint) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2086", DiagnosticPhase::Semantics, site, std::move(message));
    diagnostic.hints.emplace_back(std::move(hint));
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType(SemanticType::Unknown);
  };
  const std::string blockHint =
      "Apply '?' to an 'expected' value, or declare a 'for concept "
      "std::tryable<...>' block for the operand's type.";
  const std::optional<ConceptId> tryable = tryableConcept();
  if (!tryable) {
    return reject("Error propagation requires an 'expected<T, E>' operand or "
                  "a 'std::tryable' type, but received '" +
                      operand + "'.",
                  blockHint);
  }
  const RegisteredConcept &conceptInfo = concepts[*tryable - 1];
  const auto associated = [&](std::string_view name) {
    const auto found = std::find(conceptInfo.associatedTypes.begin(),
                                 conceptInfo.associatedTypes.end(), name);
    return found == conceptInfo.associatedTypes.end()
               ? std::optional<std::uint32_t>{}
               : static_cast<std::uint32_t>(
                     found - conceptInfo.associatedTypes.begin());
  };
  const auto operation = [&](std::string_view name) {
    const auto found = std::find_if(
        conceptInfo.operations.begin(), conceptInfo.operations.end(),
        [&](const auto &candidate) { return candidate.name == name; });
    return found == conceptInfo.operations.end()
               ? std::optional<std::size_t>{}
               : static_cast<std::size_t>(found -
                                          conceptInfo.operations.begin());
  };
  const std::optional<std::uint32_t> valueMember = associated("value_type");
  const std::optional<std::size_t> hasValue = operation("has_value");
  const std::optional<std::size_t> takeValue = operation("take_value");
  const std::optional<std::size_t> takeResidual = operation("take_residual");
  if (!valueMember || !associated("residual_type") || !hasValue || !takeValue ||
      !takeResidual) {
    return reject("The prelude's 'std::tryable' concept lacks a member that "
                  "'?' uses.",
                  "Restore the standard library's prelude.");
  }
  // A constant invocation uses the same selected calls, branch, residual
  // construction and cleanup as runtime. The staged HIR closure admits only
  // available constexpr definitions, and verified MIR executes those calls;
  // the protocol has no separate constant-evaluation semantics.

  // The operand is passed like a by-value argument: a temporary or an
  // explicit std::move moves, and a named place is copied.
  const ExpressionInfo *operandInfo =
      semanticModel.findExpression(*expr.right());
  const bool consumes =
      operandInfo != nullptr && operandInfo->category == ValueCategory::Value;
  const SemanticTypeTraits traits = typeTraits(operandType);
  const auto operandInventory = storedReferenceInventory(operandType);
  if (traits.containsBorrowedState &&
      (!operandInventory || operandInventory->empty())) {
    return reject("Error propagation through '" + operand +
                      "' needs a complete checked stored-dependency inventory.",
                  "Propagate an owning type or a carrier whose complete "
                  "external dependencies can be checked.");
  }
  if (consumes ? !traits.movable : !traits.copyable) {
    const Expr *named = expr.right().get();
    while (const auto *grouping = exprAs<Grouping>(named))
      named = grouping->expression().get();
    const auto *variable = exprAs<Variable>(named);
    const std::string spelling =
        variable != nullptr ? variable->name().lexeme : std::string("name");
    return reject(std::string("Error propagation passes its operand like a "
                              "by-value argument, so it cannot ") +
                      (consumes ? "move" : "copy") + " '" + operand + "'.",
                  consumes ? std::string("Propagate a movable type.")
                           : "Apply '?' to std::move(" + spelling +
                                 ") to consume a named move-only operand "
                                 "explicitly.");
  }

  const std::vector<SemanticType> arguments{operandType};
  if (containsOpenTypeParameter(operandType)) {
    // Each concrete instance resolves the block and checks the return.
    if (firstConceptApplicationFailure(
            {.conceptId = *tryable, .arguments = arguments})) {
      return reject("Error propagation on '" + operand +
                        "' requires 'requires std::tryable<" + operand +
                        ">' on the enclosing declaration.",
                    "Add the requirement, or propagate a concrete type.");
    }
    return associatedTypeProjection(*tryable, *valueMember, arguments);
  }
  const std::optional<ConformanceMatch> match =
      matchConformance(*tryable, arguments);
  if (!match) {
    return reject("Error propagation requires an 'expected<T, E>' operand or "
                  "a 'std::tryable' type; no 'for concept std::tryable<" +
                      operand + ">' block is declared.",
                  blockHint);
  }
  // The propagation plan stages a class, struct or payload-enum operand's
  // temporary or place. A temporary is borrowed for the test exactly as an
  // ordinary call borrows one; other operand kinds are a later slice.
  if (operandType.kind != SemanticType::Class &&
      !payloadEnumType(operandType)) {
    return reject("Error propagation through 'std::tryable' currently needs "
                  "a class, struct or payload-enum operand, but received '" +
                      operand + "'.",
                  "Wrap the value in a class or struct and give that type "
                  "the 'std::tryable' block.");
  }
  if (const std::optional<ConstraintFailure> failure =
          protocolConformanceFailure(
              {.conceptId = *tryable, .arguments = arguments})) {
    reportConstraintFailure(site, *failure);
    return SemanticType::Unknown;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  // Demand before record: the operations are selected from the conformance
  // block, not through call resolution, so their bodies are requested here
  // as ordinary call resolution requests a callee's.
  for (const std::optional<std::size_t> &index :
       {hasValue, takeValue, takeResidual})
    if (*index < conformance.operations.size() &&
        conformance.operations[*index].overloads.size() == 1)
      (void)requireBody(
          conformance.operations[*index].overloads.front().bodyDeclaration());
  std::optional<ResolvedCallInfo> test =
      protocolOperationCall(conformance, match->binding, *hasValue);
  std::optional<ResolvedCallInfo> output =
      protocolOperationCall(conformance, match->binding, *takeValue);
  std::optional<ResolvedCallInfo> residual =
      protocolOperationCall(conformance, match->binding, *takeResidual);
  if (!test || !output || !residual) {
    // The incomplete block has already been reported.
    return SemanticType::Unknown;
  }
  for (const std::size_t index : {*hasValue, *takeValue, *takeResidual}) {
    requireFunctionDefinition(conformance.operations[index].overloads.front(),
                              site);
  }
  const bool borrowedResult =
      output->returnType.kind == SemanticType::Reference;
  std::optional<SemanticBorrowDependencySet> outputDependencies;
  if (borrowedResult) {
    auto candidate = conformance.operations[*takeValue].overloads.front();
    candidate.returnType = output->returnType;
    candidate.parameterTypes = output->parameterTypes;
    const auto complete =
        completeFunctionReturnBorrow(candidate, output->typeArguments,
                                     output->valueArguments, tokenSpan(site));
    const std::vector<const ExprPtr *> actualArguments{&expr.right()};
    if (complete)
      outputDependencies = instantiateCompleteBorrowArguments(
          *output, *complete, actualArguments);
  }
  const SemanticType resultType =
      borrowedResult && output->returnType.arguments.size() == 1
          ? output->returnType.arguments.front()
          : output->returnType;
  const SemanticType residualType = residual->returnType;
  if ((borrowedResult && (output->borrowOrigin != BorrowOriginKind::Argument ||
                          output->borrowArgument != 0 ||
                          (!outputDependencies &&
                           !storedReferencePath(operandType).has_value()))) ||
      typeTraits(resultType).containsBorrowedState ||
      residualType.kind == SemanticType::Reference ||
      residualType == SemanticType::Void ||
      typeTraits(residualType).containsBorrowedState) {
    return reject("Error propagation through '" + operand +
                      "' needs an owned value or a reference transferred "
                      "from its operand's exact external dependency, and a "
                      "non-void owned residual without stored borrows.",
                  "Declare take_value's result region from its operand, as in "
                  "'T&<a> take_value<lifetime a>(... &<a> self)', "
                  "when returning a reference, and keep the residual owned.");
  }
  const SemanticType returnType = currentReturnType;
  if (returnType == SemanticType::Unknown || returnType == SemanticType::Void) {
    return reject("Error propagation through '" + operand +
                      "' returns the residual '" + typeSpelling(residualType) +
                      "', so it needs an enclosing function or lambda whose "
                      "declared return type accepts it.",
                  "Return a type that the residual initializes, such as "
                  "another instance of the operand's type.");
  }
  if (containsOpenTypeParameter(returnType)) {
    // A generic body's concrete instances check and record the return.
    return resultType;
  }
  std::optional<ResolvedConstructionInfo> construction;
  std::optional<ResolvedCallInfo> conversion;
  std::optional<SemanticBorrowDependencySet> residualReturn;
  if (!selectResidualReturn(expr, returnType, residualType, construction,
                            conversion, residualReturn)) {
    return SemanticType::Unknown;
  }
  // This path returns from the enclosing callable even though its source is
  // an expression, not a ReturnStmt. The residual is owned and holds no
  // stored borrow (checked above), so a selected residual construction
  // publishes exactly the alternatives of its complete contract for a
  // dependency-free input. Without that contract the return is unknown.
  if (construction)
    if (const auto summary =
            completeConstructorBorrow(*construction, tokenSpan(site))) {
      ResolvedCallInfo publication;
      publication.returnType = construction->constructedType;
      publication.parameterTypes = construction->parameterTypes;
      const std::array<const ExprPtr *, 1> residualInput{nullptr};
      residualReturn = instantiateCompleteBorrowArguments(publication, *summary,
                                                          residualInput);
    }
  if (residualReturn && residualReturn->dependencies.empty())
    bodyFlow.implicitResultReturns.push_back(std::move(*residualReturn));
  else
    bodyFlow.unknownImplicitResultReturn = true;
  if (outputDependencies)
    semanticModel.recordBorrowDependencySet(expr, *outputDependencies);
  semanticModel.recordTryablePropagation(
      expr, {.operandType = operandType,
             .resultType = resultType,
             .residualType = residualType,
             .returnType = returnType,
             .consumesOperand = consumes,
             .test = std::move(*test),
             .output = std::move(*output),
             .residual = std::move(*residual),
             .returnConstruction = std::move(construction),
             .returnConversion = std::move(conversion)});
  return resultType;
}

} // namespace lang
