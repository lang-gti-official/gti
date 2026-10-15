// Prefix `?` through the standard `std::tryable` protocol concept.
//
// Built-in `expected` keeps its own propagation rule. Any other operand must
// satisfy `std::tryable`, which the prelude declares; the compiler binds `?`
// to that concept by trusted declaration identity and knows nothing about the
// types that conform to it. The selected conformance block's functions run as
// ordinary calls, and the residual initializes the enclosing return type as
// `R result = residual;` would.
#include "semantic_analyzer_impl.h"

#include <algorithm>

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
      candidate.declaration->hasFunctionAttribute("borrowed_from")) {
    const auto contract = genericReturnBorrowContract(
        *candidate.declaration, call.returnType, call.parameterTypes, 0, false);
    origin = {.origin = contract.origin,
              .parameter = contract.parameter,
              .access = contract.access};
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
// whose sole parameter is the residual's exact type. Returns false after
// reporting when neither holds.
auto SemanticVisitor::Impl::selectResidualReturn(
    const Unary &expr, const SemanticType &returnType,
    const SemanticType &residualType,
    std::optional<ResolvedConstructionInfo> &construction) -> bool {
  construction.reset();
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
  if (owner == nullptr) {
    return reject(returned + ", which cannot be initialized from it.",
                  "Return a type that accepts the residual, or handle the "
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
  if (matches.size() != 1) {
    return reject(returned + ", which declares " +
                      (matches.empty() ? "no" : "more than one") +
                      " implicit constructor taking exactly that residual.",
                  "Declare one 'implicit " + owner->name.lexeme + "(" +
                      typeSpelling(residualType) +
                      " residual)' constructor without further parameters.");
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
  if (lambdaDepth == 0 && currentFunctionDeclaration != nullptr &&
      currentFunctionDeclaration->isConstexpr()) {
    return reject("Error propagation through 'std::tryable' is not yet "
                  "available in a constexpr function.",
                  "Test the value explicitly in constant code.");
  }

  // The operand is passed like a by-value argument: a temporary or an
  // explicit std::move moves, and a named place is copied.
  const ExpressionInfo *operandInfo =
      semanticModel.findExpression(*expr.right());
  const bool consumes =
      operandInfo != nullptr && operandInfo->category == ValueCategory::Value;
  const SemanticTypeTraits traits = typeTraits(operandType);
  if (traits.containsBorrowedState &&
      !storedReferencePath(operandType).has_value()) {
    return reject("Error propagation through '" + operand +
                      "' needs an exact stored-dependency path.",
                  "Propagate an owning type or a checked single-dependency "
                  "carrier.");
  }
  if (consumes ? !traits.movable : !traits.copyable) {
    return reject(std::string("Error propagation passes its operand like a "
                              "by-value argument, so it cannot ") +
                      (consumes ? "move" : "copy") + " '" + operand + "'.",
                  consumes ? "Propagate a movable type."
                           : "Apply '?' to std::move(name) to consume a "
                             "named move-only operand explicitly.");
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
  // Both nominal records and sums stage their one selected concrete value.
  // Tag selection and payload ownership remain the ordinary enum rules;
  // propagation itself only owns the three verified protocol calls.
  if (operandType.kind != SemanticType::Class &&
      operandType.kind != SemanticType::Enum) {
    return reject(
        "Error propagation through 'std::tryable' currently needs "
        "a class, struct, or enum operand, but received '" +
            operand + "'.",
        "Wrap the value in a class, struct, or enum and give that type "
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
  const SemanticType resultType =
      borrowedResult && output->returnType.arguments.size() == 1
          ? output->returnType.arguments.front()
          : output->returnType;
  const SemanticType residualType = residual->returnType;
  if ((borrowedResult && (output->borrowOrigin != BorrowOriginKind::Argument ||
                          output->borrowArgument != 0 ||
                          !storedReferencePath(operandType).has_value())) ||
      typeTraits(resultType).containsBorrowedState ||
      residualType.kind == SemanticType::Reference ||
      residualType == SemanticType::Void ||
      typeTraits(residualType).containsBorrowedState) {
    return reject("Error propagation through '" + operand +
                      "' needs an owned value or a reference transferred "
                      "from its operand's exact external dependency, and a "
                      "non-void owned residual without stored borrows.",
                  "Declare a checked borrowed_from contract on take_value "
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
  if (!selectResidualReturn(expr, returnType, residualType, construction)) {
    return SemanticType::Unknown;
  }
  semanticModel.recordTryablePropagation(
      expr, {.operandType = operandType,
             .resultType = resultType,
             .residualType = residualType,
             .returnType = returnType,
             .consumesOperand = consumes,
             .test = std::move(*test),
             .output = std::move(*output),
             .residual = std::move(*residual),
             .returnConstruction = std::move(construction)});
  return resultType;
}

} // namespace lang
