#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::isContextualArrayArgument(
    const ExprPtr &expression) const -> bool {
  return expression != nullptr &&
         exprAs<ArrayInitializer>(expression.get()) != nullptr;
}

auto SemanticVisitor::Impl::nativeFunctionCandidateIsEligible(
    const CallableCandidate &candidate) -> bool {
  if (candidate.ownerClass != 0 || candidate.declaration == nullptr ||
      candidate.id == 0 || !candidate.record().genericParameters.empty() ||
      candidate.record().parameterPack || !candidate.requirements.empty() ||
      candidate.virtualMethod || candidate.pureVirtual ||
      candidate.overrideMethod ||
      candidate.record().intrinsic != IntrinsicKind::None) {
    return false;
  }
  const FunctionDecl &declaration = *candidate.declaration;
  return declaration.body() != nullptr && !declaration.runtimeBinding() &&
         !declaration.hasCLinkage() && !declaration.operatorName();
}

auto SemanticVisitor::Impl::nativeFunctionCandidateMatches(
    const CallableCandidate &candidate, const SemanticType &expectedType) const
    -> bool {
  if (!nativeFunctionCandidateIsEligible(candidate) ||
      !expectedType.hasNativeFunctionShape()) {
    return false;
  }
  const FunctionInfo *function = semanticModel.findFunction(candidate.id);
  if (function == nullptr && instanceBaseModel != nullptr) {
    function = instanceBaseModel->findFunction(candidate.id);
  }
  if (function == nullptr || function->entryPoint ||
      function->linkage != LanguageLinkage::Gti) {
    return false;
  }
  const SemanticType *returnType = expectedType.nativeFunctionReturnType();
  const std::span<const SemanticType> parameterTypes =
      expectedType.nativeFunctionParameterTypes();
  return returnType != nullptr && candidate.returnType == *returnType &&
         candidate.parameterTypes.size() == parameterTypes.size() &&
         std::equal(candidate.parameterTypes.begin(),
                    candidate.parameterTypes.end(), parameterTypes.begin());
}

auto SemanticVisitor::Impl::matchingNativeFunctionCandidates(
    const Symbol &symbol, const SemanticType &expectedType) const
    -> std::vector<const CallableCandidate *> {
  std::vector<const CallableCandidate *> matches;
  for (const CallableCandidate &candidate : symbol.overloads) {
    if (nativeFunctionCandidateMatches(candidate, expectedType)) {
      matches.push_back(&candidate);
    }
  }
  return matches;
}

auto SemanticVisitor::Impl::analyzeNativeFunctionItem(
    const Expr &expression, const Symbol &symbol,
    const SemanticType &expectedType, const Token &location) -> SemanticType {
  if (target.executionProfile == ExecutionProfile::Concurrent) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2076", DiagnosticPhase::Semantics, location,
        "GTI function callbacks are not available in the concurrent "
        "execution profile yet.");
    diagnostic.hints.emplace_back(
        "Use the single-threaded execution profile until callback thread "
        "entry and synchronization semantics are defined.");
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }

  const std::vector<const CallableCandidate *> matches =
      matchingNativeFunctionCandidates(symbol, expectedType);
  if (matches.size() != 1) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2076", DiagnosticPhase::Semantics, location,
        matches.empty()
            ? "No eligible free GTI function exactly matches native "
              "function type '" +
                  typeSpelling(expectedType) + "'."
            : "Function conversion is ambiguous; " +
                  std::to_string(matches.size()) +
                  " overloads exactly match native function type '" +
                  typeSpelling(expectedType) + "'.");
    diagnostic.hints.emplace_back(
        "Callback targets must be non-generic namespace-scope GTI "
        "functions with bodies and an exact parameter and return type "
        "match.");
    for (const CallableCandidate &candidate : symbol.overloads) {
      if (candidate.declaration != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(candidate.declaration->name()),
             "Candidate: " + functionSignatureSpelling(candidate)});
      }
    }
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }

  const CallableCandidate &selected = *matches.front();
  semanticModel.recordNativeFunctionConversion(
      expression,
      NativeFunctionConversionInfo{.function = selected.id,
                                   .declaration = selected.declaration,
                                   .type = expectedType});
  return expectedType;
}

auto SemanticVisitor::Impl::isContextualNativeFunctionArgument(
    const ExprPtr &expression) const -> bool {
  if (!expression) {
    return false;
  }
  const Symbol *symbol = nullptr;
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    symbol = resolve(variable->name());
  } else if (const auto *qualified = exprAs<QualifiedName>(expression.get())) {
    symbol = resolveQualified(qualified->name());
  }
  return symbol != nullptr && symbol->type == SemanticType::Function;
}

auto SemanticVisitor::Impl::isContextualNativeOutPointerArgument(
    const ExprPtr &expression) const -> bool {
  const auto *address =
      expression == nullptr ? nullptr : exprAs<Unary>(expression.get());
  if (address == nullptr || address->oper().kind != TokenKind::AMPERSAND) {
    return false;
  }
  const std::optional<Symbol> symbol =
      resolveExpressionSymbol(address->right());
  return symbol && symbol->type.kind == SemanticType::RawPointer;
}

auto SemanticVisitor::Impl::analyzeContextualCallArguments(
    const ExprList &arguments) -> ContextualCallArguments {
  ContextualCallArguments result;
  result.types.reserve(arguments.size());
  result.deferred.reserve(arguments.size());
  bool deferred = false;
  for (const ExprPtr &argument : arguments) {
    deferred = deferred || isContextualArrayArgument(argument) ||
               isContextualNativeFunctionArgument(argument) ||
               isContextualNativeOutPointerArgument(argument);
    result.deferred.emplace_back(deferred);
    result.types.emplace_back(deferred ? SemanticType::Unknown
                                       : analyze(argument));
  }
  return result;
}

auto SemanticVisitor::Impl::contextualCallArgumentPotentiallyMatches(
    const SemanticType &parameter, const SemanticType &argument,
    const ExprPtr &expression, bool deferred) const -> bool {
  if (!deferred) {
    return callArgumentMatches(parameter, argument, expression);
  }
  if (isContextualArrayArgument(expression)) {
    const auto *initializer = exprAs<ArrayInitializer>(expression.get());
    return parameter.kind == SemanticType::Array &&
           parameter.arrayLengthParameterId == 0 &&
           (initializer == nullptr || initializer->elements().empty() ||
            initializer->elements().size() == parameter.arrayLength);
  }
  if (isContextualNativeFunctionArgument(expression)) {
    if (parameter.kind != SemanticType::NativeFunction) {
      return false;
    }
    const std::optional<Symbol> symbol = resolveExpressionSymbol(expression);
    return symbol &&
           matchingNativeFunctionCandidates(*symbol, parameter).size() == 1;
  }
  if (isContextualNativeOutPointerArgument(expression)) {
    const auto *address = exprAs<Unary>(expression.get());
    const std::optional<Symbol> symbol =
        address == nullptr ? std::nullopt
                           : resolveExpressionSymbol(address->right());
    return symbol && symbol->type.kind == SemanticType::RawPointer &&
           isCAbiOpaqueHandleOutPointer(parameter);
  }
  return true;
}

auto SemanticVisitor::Impl::finishContextualCallArguments(
    const std::vector<SemanticType> &parameterTypes, const ExprList &arguments,
    ContextualCallArguments &analysis, const Token &location,
    std::string_view callable) -> bool {
  if (parameterTypes.size() < arguments.size() ||
      analysis.types.size() != arguments.size() ||
      analysis.deferred.size() != arguments.size()) {
    return false;
  }
  bool valid = true;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!analysis.deferred[index]) {
      continue;
    }
    const SemanticType &parameter = parameterTypes[index];
    if (isContextualArrayArgument(arguments[index])) {
      if (parameter.kind != SemanticType::Array ||
          parameter.arrayLengthParameterId != 0) {
        report(expressionToken(arguments[index]),
               "A brace argument requires one exact fixed-array parameter "
               "type from the selected callable.",
               "GTI-S2015");
        analysis.types[index] = SemanticType::Unknown;
        valid = false;
        continue;
      }
      analysis.types[index] = analyzeInitializer(arguments[index], parameter);
    } else if (isContextualNativeFunctionArgument(arguments[index])) {
      if (parameter.kind != SemanticType::NativeFunction) {
        report(expressionToken(arguments[index]),
               "A function item argument requires one exact named native "
               "function parameter type from the selected callable.",
               "GTI-S2076");
        analysis.types[index] = SemanticType::Unknown;
        valid = false;
        continue;
      }
      analysis.types[index] = analyzeInitializer(arguments[index], parameter);
    } else if (isContextualNativeOutPointerArgument(arguments[index])) {
      const std::optional<SemanticType> enclosingType =
          contextualNativeOutPointerType;
      contextualNativeOutPointerType = parameter;
      analysis.types[index] = analyze(arguments[index]);
      contextualNativeOutPointerType = enclosingType;
    } else {
      analysis.types[index] = analyze(arguments[index]);
    }
    if (analysis.types[index] != SemanticType::Unknown &&
        parameter != SemanticType::Unknown &&
        !callArgumentMatches(parameter, analysis.types[index],
                             arguments[index])) {
      reportCallArgumentMismatch(index, parameter, analysis.types[index],
                                 arguments[index], callable);
      valid = false;
    }
  }
  (void)location;
  return valid;
}

auto SemanticVisitor::Impl::callArgumentRejectedByOwnership(
    const SemanticType &parameter, const SemanticType &argument,
    const ExprPtr &expression) const -> bool {
  if (expression == nullptr || parameter != argument ||
      parameter.kind == SemanticType::Reference ||
      parameter.kind == SemanticType::TypePack) {
    return false;
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  if (info == nullptr) {
    return false;
  }
  const SemanticTypeTraits traits = typeTraits(argument);
  return (info->category == ValueCategory::Value && !traits.movable) ||
         (info->category == ValueCategory::Place && !traits.copyable);
}

auto SemanticVisitor::Impl::callArgumentMatches(const SemanticType &parameter,
                                                const SemanticType &argument,
                                                const ExprPtr &expression,
                                                bool allowValueAssignment) const
    -> bool {
  if (parameter == SemanticType::Unknown || argument == SemanticType::Unknown) {
    return true;
  }
  if (parameter.kind != SemanticType::Reference) {
    if (parameter == SemanticType::CString) {
      return argument == SemanticType::CString ||
             argument == SemanticType::StringView ||
             argument == SemanticType::NullPtr;
    }
    if (parameter.kind == SemanticType::RawPointer) {
      return isAssignable(parameter, argument, expression.get());
    }
    if (parameter.kind == SemanticType::NativeFunction) {
      return isAssignable(parameter, argument, expression.get());
    }
    if (parameter == argument && expression &&
        parameter.kind != SemanticType::TypePack) {
      const ExpressionInfo *info = semanticModel.findExpression(*expression);
      if (info != nullptr) {
        const SemanticTypeTraits traits = typeTraits(parameter);
        if ((info->category == ValueCategory::Value && !traits.movable) ||
            (info->category == ValueCategory::Place && !traits.copyable)) {
          return false;
        }
      }
    }
    return allowValueAssignment
               ? isAssignable(parameter, argument, expression.get())
               : parameter == argument;
  }
  if (parameter.arguments.size() != 1 || !expression) {
    return false;
  }
  const SemanticType &referent = parameter.arguments[0];
  if (referent != argument && !isPublicBaseConversion(argument, referent)) {
    return false;
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  if (info == nullptr || info->category != ValueCategory::Place) {
    return false;
  }
  return parameter.referenceAccess != AccessMode::Mutable ||
         info->access == AccessMode::Mutable;
}

auto SemanticVisitor::Impl::ephemeralBorrowResultSupported(
    const CallableCandidate &function) const -> bool {
  if (function.returnBorrowOrigin != BorrowOriginKind::None ||
      typeTraits(function.returnType).containsBorrowedState) {
    return false;
  }
  switch (function.returnType.kind) {
  case SemanticType::Reference:
  case SemanticType::StringView:
  case SemanticType::CString:
  case SemanticType::RawPointer:
  case SemanticType::NativeFunction:
    return false;
  default:
    return true;
  }
}

auto SemanticVisitor::Impl::ephemeralBorrowTargetSupported(
    const CallableCandidate &function) const -> bool {
  if (function.id == 0 || function.declaration == nullptr ||
      function.declaration->body() == nullptr ||
      function.declaration->runtimeBinding() ||
      function.declaration->hasCLinkage() ||
      function.declaration->operatorName() ||
      function.record().intrinsic != IntrinsicKind::None ||
      !ephemeralBorrowResultSupported(function)) {
    return false;
  }
  const FunctionInfo *target = semanticModel.findFunction(function.id);
  if (target == nullptr && instanceBaseModel != nullptr) {
    target = instanceBaseModel->findFunction(function.id);
  }
  return target != nullptr && target->linkage == LanguageLinkage::Gti &&
         target->declaration != nullptr &&
         target->declaration->body() != nullptr;
}

auto SemanticVisitor::Impl::ephemeralTemporaryArgumentMatches(
    const CallableCandidate &function, const SemanticType &parameter,
    const SemanticType &argument, const ExprPtr &expression) const -> bool {
  if (parameter.kind != SemanticType::Reference ||
      parameter.referenceAccess != AccessMode::ReadOnly ||
      parameter.arguments.size() != 1 ||
      parameter.arguments.front() != argument ||
      argument.kind != SemanticType::Class || !expression ||
      !ephemeralBorrowTargetSupported(function)) {
    return false;
  }
  const FreshTemporaryValueAssessment assessment =
      freshTemporaryValue(expression);
  return assessment.eligible && assessment.type == argument;
}

auto SemanticVisitor::Impl::functionCallArgumentMatch(
    const CallableCandidate &function, std::size_t index,
    const SemanticType &argument, const ExprPtr &expression,
    bool deferred) const -> CallArgumentMatchKind {
  if (index >= function.parameterTypes.size()) {
    return CallArgumentMatchKind::Mismatch;
  }
  const SemanticType &parameter = function.parameterTypes[index];
  if (contextualCallArgumentPotentiallyMatches(parameter, argument, expression,
                                               deferred)) {
    return CallArgumentMatchKind::Exact;
  }
  if (!deferred && ephemeralTemporaryArgumentMatches(function, parameter,
                                                     argument, expression)) {
    return CallArgumentMatchKind::EphemeralReadBorrow;
  }
  return CallArgumentMatchKind::Mismatch;
}

auto SemanticVisitor::Impl::reportCallArgumentMismatch(
    std::size_t index, const SemanticType &parameter,
    const SemanticType &argument, const ExprPtr &expression,
    std::string_view callable, const CallableCandidate *function) -> void {
  const std::string argumentLabel = callable == "Function"
                                        ? "Argument "
                                        : std::string(callable) + " argument ";
  if (parameter.kind == SemanticType::Reference &&
      parameter.arguments.size() == 1 && parameter.arguments[0] == argument &&
      expression) {
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    if (info == nullptr || info->category != ValueCategory::Place) {
      std::string reason = argumentLabel + std::to_string(index + 1) +
                           " cannot bind a reference to a temporary.";
      if (parameter.referenceAccess == AccessMode::Mutable) {
        reason = argumentLabel + std::to_string(index + 1) +
                 " cannot bind a mutable reference to a temporary.";
      } else if (argument.kind == SemanticType::Class) {
        const FreshTemporaryValueAssessment assessment =
            freshTemporaryValue(expression);
        if (assessment.eligible && function != nullptr &&
            !ephemeralBorrowTargetSupported(*function)) {
          reason = !ephemeralBorrowResultSupported(*function)
                       ? argumentLabel + std::to_string(index + 1) +
                             " cannot borrow a temporary because the "
                             "selected function may return a reference or "
                             "value containing tracked borrowed state."
                       : argumentLabel + std::to_string(index + 1) +
                             " cannot borrow a temporary through a native, "
                             "callback, unresolved, or declaration-only "
                             "call target.";
        } else {
          switch (assessment.failure) {
          case FreshTemporaryValueFailure::ExplicitMove:
            reason = argumentLabel + std::to_string(index + 1) +
                     " is an explicit move from existing storage, not a "
                     "fresh temporary value.";
            break;
          case FreshTemporaryValueFailure::BorrowedState:
            reason = argumentLabel + std::to_string(index + 1) +
                     " contains tracked borrowed state and cannot be "
                     "borrowed as a temporary argument.";
            break;
          case FreshTemporaryValueFailure::NonConcreteRecord:
            reason = argumentLabel + std::to_string(index + 1) +
                     " is not a concrete class or struct temporary.";
            break;
          case FreshTemporaryValueFailure::UnsupportedProducer:
            reason = argumentLabel + std::to_string(index + 1) +
                     " is not produced directly by an ordinary constructor "
                     "or source-defined GTI function call.";
            break;
          case FreshTemporaryValueFailure::None:
          case FreshTemporaryValueFailure::NotClassValue:
            break;
          }
        }
      }
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2017", DiagnosticPhase::Semantics,
                         expressionToken(expression), std::move(reason));
      if (function != nullptr && function->declaration != nullptr &&
          !ephemeralBorrowTargetSupported(*function)) {
        diagnostic.related.push_back(
            {tokenSpan(function->declaration->name()),
             "Selected function's result and linkage contract is declared "
             "here."});
      }
      diagnostic.hints.emplace_back(
          "Bind the value to a local before passing it when it must outlive "
          "one call or falls outside the bounded fresh-value producer "
          "family.");
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
    if (parameter.referenceAccess == AccessMode::Mutable &&
        info->access != AccessMode::Mutable) {
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) +
                 " requires a mutable value for parameter '" +
                 typeSpelling(parameter) + "'.",
             "GTI-S2017");
      return;
    }
  }
  if (isMoveOnlyOwnerType(parameter) && parameter == argument) {
    const ClassInfo *owner = classInfo(parameter);
    if (owner != nullptr && owner->copyConstructor &&
        owner->copyConstructor->declaration != nullptr &&
        owner->copyConstructor->declaration->specifier() &&
        owner->copyConstructor->declaration->specifier()->kind ==
            SpecialMemberSpecifierKind::Deleted) {
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) +
                 " would call the deleted copy constructor of '" +
                 typeSpelling(parameter) + "'.",
             "GTI-S2018");
      return;
    }
    std::string copyDescription =
        " would copy a move-only value; use std::move(owner) to ";
    if (typeTraits(parameter).ownership == OwnershipKind::Unique) {
      copyDescription = " would copy a unique owner; use std::move(owner) to ";
    } else if (parameter.kind == SemanticType::Storage ||
               parameter.kind == SemanticType::PrefixStorage) {
      copyDescription = " would copy compiler-private storage; use "
                        "std::move(owner) to ";
    }
    report(expressionToken(expression),
           argumentLabel + std::to_string(index + 1) + copyDescription +
               "transfer ownership.",
           "GTI-S2018");
    return;
  }
  if (parameter == argument && parameter.kind != SemanticType::TypePack &&
      expression) {
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    const SemanticTypeTraits traits = typeTraits(parameter);
    if (info != nullptr && info->category == ValueCategory::Place &&
        !traits.copyable) {
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) +
                 " requires copying type '" + typeSpelling(parameter) +
                 "', but copy construction is unavailable.",
             "GTI-S2018");
      return;
    }
    if (info != nullptr && info->category == ValueCategory::Value &&
        !traits.movable) {
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) +
                 " requires moving type '" + typeSpelling(parameter) +
                 "', but move construction is unavailable.",
             "GTI-S2018");
      return;
    }
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2003", DiagnosticPhase::Semantics, expressionToken(expression),
      argumentLabel + std::to_string(index + 1) + " has type '" +
          typeSpelling(argument) + "' but the parameter requires '" +
          typeSpelling(parameter) + "'.");
  if (parameter.kind == SemanticType::RawPointer) {
    diagnostic.hints.emplace_back(
        argument.kind == SemanticType::Array
            ? "Fixed arrays do not decay to raw pointers; inside an unsafe "
              "block, pass an element address such as '&values[0]' when "
              "the callee's bounds and lifetime contract are satisfied."
            : "Raw pointer casts are not available; pass nullptr or an "
              "exact compatible pointer, with only T* to const T* "
              "qualification permitted.");
  } else if (parameter.kind != SemanticType::Reference) {
    diagnostic.hints.emplace_back(
        "Calls require exact argument types; use an explicit "
        "conversion such as '" +
        typeSpelling(parameter) +
        "(value)' when the conversion is intentional.");
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::resolveClassArguments(
    ClassId classId, const std::vector<TypeRef> &arguments, const Token &site,
    std::string diagnosticCode) -> ResolvedClassArguments {
  ResolvedClassArguments result;
  if (classId == 0 || classId > classes.size()) {
    result.valid = false;
    return result;
  }

  const ClassInfo &owner = classInfo(classId);
  result.types.reserve(genericTypeParameterCount(owner.genericParameters));
  result.values.reserve(genericValueParameterCount(owner.genericParameters));
  if (arguments.size() != owner.genericParameters.size()) {
    const bool typeOnly =
        genericValueParameterCount(owner.genericParameters) == 0;
    report(site,
           "Type '" + owner.name.lexeme + "' requires " +
               std::to_string(owner.genericParameters.size()) +
               (typeOnly ? " generic type argument" : " generic argument") +
               (owner.genericParameters.size() == 1 ? "." : "s."),
           diagnosticCode);
    result.valid = false;
  }

  const std::size_t count =
      std::min(arguments.size(), owner.genericParameters.size());
  for (std::size_t index = 0; index < count; ++index) {
    const GenericParameterInfo &parameter = owner.genericParameters[index];
    const TypeRef &argument = arguments[index];
    if (parameter.value) {
      const std::optional<CompileTimeValue> value =
          genericValueArgument(argument);
      if (!value) {
        report(argument.name.last(),
               "Generic parameter '" + parameter.name.lexeme +
                   "' requires a uint64_t compile-time value.",
               diagnosticCode);
        result.values.emplace_back();
        result.valid = false;
      } else {
        result.values.emplace_back(*value);
      }
      continue;
    }

    if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
      report(argument.name.last(),
             "Generic parameter '" + parameter.name.lexeme +
                 "' requires a type argument.",
             diagnosticCode);
      result.types.emplace_back(SemanticType::Unknown);
      result.valid = false;
      continue;
    }
    validateType(argument);
    const SemanticType type = typeOf(argument);
    if (type == SemanticType::Void) {
      report(argument.name.last(), "Generic type arguments cannot be void.",
             diagnosticCode);
      result.valid = false;
    } else if (const std::optional<GenericConstraintKind> failed =
                   firstUnsatisfiedConstraint(type, parameter.constraints)) {
      reportConstraintFailure(
          argument.name.last(),
          ConstraintFailure{.parameter = parameter.name,
                            .argument = type,
                            .constraints = parameter.constraints,
                            .failed = *failed,
                            .constraintName = parameter.constraintName});
      result.valid = false;
    }
    result.types.emplace_back(type);
  }
  return result;
}

auto SemanticVisitor::Impl::analyzeConstructorCall(
    const Expr &construction, ClassId classId,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments,
    const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
    const Token &paren) -> void {
  std::vector<AnalyzedCallArgument> analyzedArguments;
  analyzedArguments.reserve(argumentTypes.size());
  bool deferred = false;
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    const ExprPtr *expression =
        index < arguments.size() ? &arguments[index] : nullptr;
    deferred = deferred || (expression != nullptr &&
                            isContextualArrayArgument(*expression));
    analyzedArguments.push_back({.type = argumentTypes[index],
                                 .expression = expression,
                                 .deferred = deferred});
  }
  analyzeConstructorCallArguments(construction, classId, typeArguments,
                                  valueArguments, analyzedArguments, paren);
}

auto SemanticVisitor::Impl::forwardedPackArgumentMatches(
    const SemanticType &parameter, const SemanticType &argument) const -> bool {
  if (parameter == SemanticType::Unknown || argument == SemanticType::Unknown) {
    return true;
  }
  if (parameter.kind != SemanticType::Reference) {
    if (parameter.kind == SemanticType::RawPointer) {
      return isAssignable(parameter, argument, nullptr);
    }
    return parameter == argument;
  }
  return parameter.arguments.size() == 1 &&
         parameter.arguments.front() == argument &&
         parameter.referenceAccess == AccessMode::ReadOnly;
}

auto SemanticVisitor::Impl::analyzeConstructorCallArguments(
    const Expr &construction, ClassId classId,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments,
    std::vector<AnalyzedCallArgument> arguments, const Token &paren,
    bool requireExactNullParameter) -> void {
  const SemanticType selectedClass = selectExactClassSpecialization(
      SemanticType::classType(classId, typeArguments, valueArguments));
  classId = selectedClass.classId;
  if (classId == 0 || classId > classes.size()) {
    currentType = SemanticType::Unknown;
    return;
  }

  const ClassInfo &owner = classInfo(classId);
  GenericSubstitution substitution;
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : owner.genericParameters) {
    if (parameter.value) {
      if (valueIndex < selectedClass.valueArguments.size()) {
        substitution.values.emplace(parameter.id,
                                    selectedClass.valueArguments[valueIndex++]);
      }
    } else if (typeIndex < selectedClass.arguments.size()) {
      substitution.types.emplace(parameter.id,
                                 selectedClass.arguments[typeIndex++]);
    }
  }
  const SemanticType constructedType = selectedClass;
  if (!isCompilerTrustedRequester(paren) &&
      typeRequiresThreads(constructedType) &&
      !validateThreadSurfaceAvailability(paren, "This managed-thread value")) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (owner.abstract) {
    report(paren,
           "Cannot construct abstract " +
               std::string(classKindSpelling(owner.kind)) + " '" +
               owner.name.lexeme + "'.",
           "GTI-S2044");
  }

  struct ViableConstructor {
    const ConstructorInfo *constructor = nullptr;
    std::vector<SemanticType> parameterTypes;
    std::vector<SemanticType> typeArguments;
    std::vector<CompileTimeValue> valueArguments;
    bool generatedDefault = false;
    ConstructorKind kind = ConstructorKind::Ordinary;
    std::vector<AppliedConceptRequirement> requirements;
  };
  struct RejectedConstructorArgument {
    const ConstructorInfo *constructor = nullptr;
    std::vector<SemanticType> parameterTypes;
    std::size_t argumentIndex = 0;
  };
  struct RejectedConstructorInference {
    const ConstructorInfo *constructor = nullptr;
    const GenericParameterInfo *parameter = nullptr;
    Token site;
    bool conflicting = false;
  };
  std::vector<ViableConstructor> viable;
  std::vector<const ConstructorInfo *> privateViable;
  std::vector<RejectedConstructorArgument> rejectedArguments;
  std::vector<RejectedConstructorInference> rejectedInferences;
  std::optional<ConstraintFailure> rejectedConstraint;
  for (const ConstructorInfo &constructor : owner.constructors) {
    if (arguments.size() < constructor.requiredParameterCount ||
        arguments.size() > constructor.parameterTypes.size()) {
      continue;
    }
    GenericSubstitution constructorSubstitution = substitution;
    bool inferred = true;
    GenericParameterId conflictingParameter = 0;
    bool conflictingValueParameter = false;
    Token conflictingSite = paren;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const SemanticType pattern =
          substituteType(constructor.parameterTypes[index], substitution);
      GenericParameterId currentConflict = 0;
      bool currentValueConflict = false;
      const bool currentInferred = tryInferTypeArguments(
          pattern, arguments[index].type, constructor.genericParameters,
          constructorSubstitution, arguments[index].expression,
          &currentConflict, &currentValueConflict);
      if (!currentInferred && conflictingParameter == 0) {
        conflictingParameter = currentConflict;
        conflictingValueParameter = currentValueConflict;
        if (arguments[index].expression != nullptr) {
          conflictingSite = expressionToken(*arguments[index].expression);
        }
      }
      inferred = currentInferred && inferred;
    }
    std::vector<SemanticType> constructorTypeArguments;
    std::vector<CompileTimeValue> constructorValueArguments;
    const GenericParameterInfo *missingParameter = nullptr;
    for (const GenericParameterInfo &parameter :
         constructor.genericParameters) {
      if (parameter.value) {
        const auto found = constructorSubstitution.values.find(parameter.id);
        if (found == constructorSubstitution.values.end()) {
          inferred = false;
          if (missingParameter == nullptr) {
            missingParameter = &parameter;
          }
        } else {
          constructorValueArguments.emplace_back(found->second);
        }
      } else {
        const auto found = constructorSubstitution.types.find(parameter.id);
        if (found == constructorSubstitution.types.end()) {
          inferred = false;
          if (missingParameter == nullptr) {
            missingParameter = &parameter;
          }
        } else {
          constructorTypeArguments.emplace_back(found->second);
        }
      }
    }
    if (!inferred) {
      const GenericParameterInfo *conflicting =
          conflictingParameter == 0
              ? nullptr
              : findGenericParameter(constructor.genericParameters,
                                     conflictingParameter);
      const GenericParameterInfo *failed =
          conflicting != nullptr ? conflicting : missingParameter;
      if (failed != nullptr &&
          (isCompilerTrustedRequester(paren) || !constructor.compilerPrivate) &&
          (constructor.access != AccessModifier::Private ||
           currentClass == classId)) {
        rejectedInferences.push_back(
            {.constructor = &constructor,
             .parameter = failed,
             .site = conflicting != nullptr ? conflictingSite : paren,
             .conflicting = conflicting != nullptr});
      }
      static_cast<void>(conflictingValueParameter);
      continue;
    }
    if (const std::optional<ConstraintFailure> failure = firstConstraintFailure(
            constructor.genericParameters, constructorTypeArguments)) {
      if (!rejectedConstraint) {
        rejectedConstraint = *failure;
      }
      continue;
    }
    std::vector<AppliedConceptRequirement> requirements =
        constructor.requirements;
    for (AppliedConceptRequirement &requirement : requirements) {
      for (SemanticType &argument : requirement.arguments) {
        argument = substituteType(argument, constructorSubstitution);
      }
    }
    if (const std::optional<ConstraintFailure> failure =
            firstRequirementFailure(requirements)) {
      if (!rejectedConstraint) {
        rejectedConstraint = *failure;
      }
      continue;
    }
    std::vector<SemanticType> parameterTypes;
    parameterTypes.reserve(constructor.parameterTypes.size());
    bool exact = true;
    std::optional<std::size_t> rejectedArgument;
    for (std::size_t index = 0; index < constructor.parameterTypes.size();
         ++index) {
      const SemanticType parameterType = substituteType(
          constructor.parameterTypes[index], constructorSubstitution);
      parameterTypes.emplace_back(parameterType);
      if (index >= arguments.size()) {
        continue;
      }
      const AnalyzedCallArgument &argument = arguments[index];
      const bool matches =
          requireExactNullParameter ? parameterType == SemanticType::NullPtr &&
                                          argument.type == SemanticType::NullPtr
          : argument.forwardedPackElement
              ? forwardedPackArgumentMatches(parameterType, argument.type)
              : argument.expression != nullptr &&
                    contextualCallArgumentPotentiallyMatches(
                        parameterType, argument.type, *argument.expression,
                        argument.deferred);
      if (!matches) {
        if (argument.forwardedPackElement || argument.deferred ||
            argument.expression == nullptr ||
            !callArgumentRejectedByOwnership(parameterType, argument.type,
                                             *argument.expression)) {
          exact = false;
          rejectedArgument.reset();
          break;
        }
        exact = false;
        if (!rejectedArgument) {
          rejectedArgument = index;
        }
      }
    }
    if (exact) {
      if (!isCompilerTrustedRequester(paren) && constructor.compilerPrivate) {
        privateViable.push_back(&constructor);
        continue;
      }
      viable.push_back({&constructor, std::move(parameterTypes),
                        std::move(constructorTypeArguments),
                        std::move(constructorValueArguments),
                        requestsStructuralDefaultConstruction(constructor),
                        ConstructorKind::Ordinary, std::move(requirements)});
    } else if (rejectedArgument &&
               (isCompilerTrustedRequester(paren) ||
                !constructor.compilerPrivate) &&
               (constructor.access != AccessModifier::Private ||
                currentClass == classId)) {
      rejectedArguments.push_back(
          {&constructor, std::move(parameterTypes), *rejectedArgument});
    }
  }

  if (owner.kind == ClassKind::Union && arguments.size() == 1 &&
      !owner.fields.empty() && owner.fields.front().declaration != nullptr) {
    const auto firstMember =
        owner.members.find(owner.fields.front().declaration->name().lexeme);
    if (firstMember != owner.members.end()) {
      const SemanticType firstType = firstMember->second.symbol.type;
      const AnalyzedCallArgument &argument = arguments.front();
      const bool exact =
          argument.forwardedPackElement
              ? forwardedPackArgumentMatches(firstType, argument.type)
              : argument.expression != nullptr &&
                    contextualCallArgumentPotentiallyMatches(
                        firstType, argument.type, *argument.expression,
                        argument.deferred);
      if (exact) {
        viable.push_back(
            {nullptr, {firstType}, {}, {}, false, ConstructorKind::Ordinary});
      }
    }
  }

  if (arguments.empty() && defaultConstructor(owner) == nullptr &&
      classCanGenerateDefaultConstructor(owner)) {
    viable.push_back({nullptr, {}, {}, {}, true, ConstructorKind::Ordinary});
  }

  std::optional<ConstructorKind> attemptedSpecial;
  if (arguments.size() == 1 && arguments.front().type == constructedType) {
    std::optional<bool> moving;
    if (arguments.front().forwardedPackElement) {
      moving = !typeTraits(constructedType).copyable;
    } else if (arguments.front().expression != nullptr) {
      const ExpressionInfo *argumentInfo =
          semanticModel.findExpression(**arguments.front().expression);
      if (argumentInfo != nullptr) {
        moving = argumentInfo->category == ValueCategory::Value;
      }
    }
    if (moving) {
      attemptedSpecial =
          *moving ? ConstructorKind::Move : ConstructorKind::Copy;
      const SemanticTypeTraits traits = typeTraits(constructedType);
      const bool available = *moving ? traits.movable : traits.copyable;
      if (available) {
        const std::optional<ConstructorInfo> &declared =
            *moving ? owner.moveConstructor : owner.copyConstructor;
        std::vector<SemanticType> parameterTypes;
        if (declared && !declared->parameterTypes.empty()) {
          parameterTypes.push_back(
              substituteType(declared->parameterTypes.front(), substitution));
        } else {
          parameterTypes.push_back(SemanticType::referenceTo(constructedType));
        }
        viable.push_back({declared ? &*declared : nullptr,
                          std::move(parameterTypes),
                          {},
                          {},
                          false,
                          *attemptedSpecial});
      }
    }
  }

  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(arguments.size());
  for (const AnalyzedCallArgument &argument : arguments) {
    argumentTypes.emplace_back(argument.type);
  }
  retainBestCallConversionMatches(viable, argumentTypes,
                                  [](const ViableConstructor &candidate)
                                      -> const std::vector<SemanticType> & {
                                    return candidate.parameterTypes;
                                  });

  bool hasUnknownArgument = std::any_of(
      argumentTypes.begin(), argumentTypes.end(),
      [](const SemanticType &type) { return type == SemanticType::Unknown; });
  const bool hasDeferredArgument = std::any_of(
      arguments.begin(), arguments.end(),
      [](const AnalyzedCallArgument &argument) { return argument.deferred; });
  if (viable.empty() && !privateViable.empty() && !hasUnknownArgument) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2058", DiagnosticPhase::Semantics, paren,
                       "Compiler-private constructor of '" + owner.name.lexeme +
                           "' is unavailable to application source.");
    for (const ConstructorInfo *candidate : privateViable) {
      if (candidate != nullptr && candidate->declaration != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(candidate->declaration->name()),
             "Compiler-private constructor declared here."});
      }
    }
    diagnostic.hints.emplace_back(
        "Use the public factory or default construction surface instead.");
    diagnostics.emplace_back(std::move(diagnostic));
    currentType = constructedType;
    return;
  }
  if (viable.size() != 1) {
    if (!hasUnknownArgument || hasDeferredArgument) {
      if (viable.empty() && rejectedInferences.size() == 1) {
        const RejectedConstructorInference &rejected =
            rejectedInferences.front();
        const bool value = rejected.parameter->value;
        Diagnostic diagnostic = makeDiagnostic(
            value ? "GTI-S2026" : "GTI-S2023", DiagnosticPhase::Semantics,
            rejected.site,
            rejected.conflicting
                ? "Conflicting " + std::string(value ? "values" : "types") +
                      " were inferred for constructor-local " +
                      std::string(value ? "value" : "type") + " parameter '" +
                      rejected.parameter->name.lexeme + "'."
                : "Cannot infer constructor-local " +
                      std::string(value ? "value" : "type") + " parameter '" +
                      rejected.parameter->name.lexeme +
                      "' from the supplied constructor arguments.");
        diagnostic.related.push_back(
            {tokenSpan(rejected.parameter->name),
             "Constructor-local generic parameter declared here."});
        diagnostic.hints.emplace_back(
            "Constructor-local generic arguments are inference-only; the "
            "parameter must occur in an exactly matching constructor "
            "parameter type.");
        diagnostics.emplace_back(std::move(diagnostic));
        currentType = constructedType;
        return;
      }
      if (viable.empty() && rejectedConstraint) {
        reportConstraintFailure(paren, *rejectedConstraint);
        currentType = constructedType;
        return;
      }
      if (viable.empty() && attemptedSpecial) {
        const bool moving = *attemptedSpecial == ConstructorKind::Move;
        const std::optional<ConstructorInfo> &declared =
            moving ? owner.moveConstructor : owner.copyConstructor;
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2020", DiagnosticPhase::Semantics, paren,
            std::string(moving ? "Move" : "Copy") + " construction of '" +
                owner.name.lexeme + "' is deleted.");
        if (declared && declared->declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(declared->declaration->name()),
               std::string(moving ? "Move" : "Copy") +
                   " constructor policy declared here."});
        }
        diagnostic.hints.emplace_back(
            moving ? "Remove std::move only if the type remains copyable."
                   : "Transfer the value with std::move only if the type "
                     "remains movable.");
        diagnostics.emplace_back(std::move(diagnostic));
        currentType = constructedType;
        return;
      }
      if (viable.empty() && rejectedArguments.size() == 1) {
        const RejectedConstructorArgument &rejected = rejectedArguments.front();
        const std::size_t index = rejected.argumentIndex;
        const std::size_t diagnosticCount = diagnostics.size();
        reportCallArgumentMismatch(index, rejected.parameterTypes[index],
                                   arguments[index].type,
                                   *arguments[index].expression, "Constructor");
        if (diagnostics.size() > diagnosticCount &&
            rejected.constructor != nullptr &&
            rejected.constructor->declaration != nullptr) {
          diagnostics.back().related.push_back(
              {tokenSpan(rejected.constructor->declaration->name()),
               "Exact-type candidate: " +
                   constructorSignatureSpelling(*rejected.constructor)});
          if (diagnostics.back().code == "GTI-S2018" &&
              diagnostics.back().message.find("std::move") ==
                  std::string::npos &&
              typeTraits(rejected.parameterTypes[index]).movable) {
            diagnostics.back().hints.emplace_back(
                "Pass the argument with std::move(...) when this call "
                "should transfer ownership.");
          }
        }
        currentType = constructedType;
        return;
      }
      std::string argumentsSpelling;
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
          argumentsSpelling += ", ";
        }
        argumentsSpelling += typeSpelling(arguments[index].type);
      }
      Diagnostic diagnostic = makeDiagnostic(
          viable.empty() ? "GTI-S2012" : "GTI-S2013",
          DiagnosticPhase::Semantics, paren,
          viable.empty()
              ? "No constructor of '" + owner.name.lexeme +
                    "' exactly matches argument types (" + argumentsSpelling +
                    ")."
              : "Construction of '" + owner.name.lexeme +
                    "' is ambiguous; multiple constructors exactly match.");
      diagnostic.hints.emplace_back(
          "Constructor calls do not perform implicit conversions; convert "
          "an argument explicitly with syntax such as 'uint64_t(value)'.");
      for (const ConstructorInfo &candidate : owner.constructors) {
        if (candidate.declaration != nullptr &&
            (isCompilerTrustedRequester(paren) || !candidate.compilerPrivate)) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->name()),
               "Candidate: " + constructorSignatureSpelling(candidate)});
        }
      }
      diagnostics.emplace_back(std::move(diagnostic));
    }
    currentType = constructedType;
    return;
  }

  const ViableConstructor &selected = viable.front();
  if (hasDeferredArgument) {
    bool valid = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      AnalyzedCallArgument &argument = arguments[index];
      if (!argument.deferred || argument.expression == nullptr) {
        continue;
      }
      const SemanticType &parameter = selected.parameterTypes[index];
      if (isContextualArrayArgument(*argument.expression)) {
        if (parameter.kind != SemanticType::Array ||
            parameter.arrayLengthParameterId != 0) {
          report(expressionToken(*argument.expression),
                 "A brace argument requires one exact fixed-array "
                 "constructor parameter type.",
                 "GTI-S2015");
          argument.type = SemanticType::Unknown;
          valid = false;
          continue;
        }
        argument.type = analyzeInitializer(*argument.expression, parameter);
      } else {
        argument.type = analyze(*argument.expression);
      }
      if (argument.type != SemanticType::Unknown &&
          !callArgumentMatches(parameter, argument.type,
                               *argument.expression)) {
        reportCallArgumentMismatch(index, parameter, argument.type,
                                   *argument.expression, "Constructor");
        valid = false;
      }
    }
    if (!valid) {
      currentType = constructedType;
      return;
    }
    argumentTypes.clear();
    argumentTypes.reserve(arguments.size());
    for (const AnalyzedCallArgument &argument : arguments) {
      argumentTypes.emplace_back(argument.type);
    }
    hasUnknownArgument = std::any_of(
        argumentTypes.begin(), argumentTypes.end(),
        [](const SemanticType &type) { return type == SemanticType::Unknown; });
  }

  // An earlier argument diagnostic owns recovery. Do not select or expose a
  // constructor merely because Unknown is treated as a wildcard afterward.
  if (hasUnknownArgument) {
    currentType = constructedType;
    return;
  }

  if (selected.constructor != nullptr &&
      selected.constructor->access == AccessModifier::Private &&
      currentClass != classId) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, paren,
        "Constructor of '" + owner.name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(selected.constructor->declaration->name()),
         "Constructor declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  for (std::size_t index = 0;
       index < selected.parameterTypes.size() && index < arguments.size();
       ++index) {
    const SemanticType &parameter = selected.parameterTypes[index];
    const AnalyzedCallArgument &argument = arguments[index];
    if (parameter.kind != SemanticType::Reference ||
        parameter.referenceAccess != AccessMode::Mutable ||
        argument.forwardedPackElement || argument.expression == nullptr) {
      continue;
    }
    const SemanticLoanPlace place = transientLoanPlace(*argument.expression);
    if (place.root != 0 || place.receiver) {
      reportLoanPlaceConflict(
          paren, place, AccessMode::Mutable,
          "Mutable constructor argument overlaps a borrow that may still "
          "be live.",
          accessLoan(*argument.expression));
    }
  }
  std::vector<SemanticType> concreteParameterTypes;
  std::vector<const ExprPtr *> concreteArguments;
  const std::size_t concreteCount =
      std::min(selected.parameterTypes.size(), arguments.size());
  concreteParameterTypes.reserve(concreteCount);
  concreteArguments.reserve(concreteCount);
  for (std::size_t index = 0; index < concreteCount; ++index) {
    if (arguments[index].forwardedPackElement ||
        arguments[index].expression == nullptr) {
      continue;
    }
    concreteParameterTypes.push_back(selected.parameterTypes[index]);
    concreteArguments.push_back(arguments[index].expression);
  }
  validateCallPlaceExclusivityImpl(concreteParameterTypes, concreteArguments,
                                   paren);
  std::vector<const Expr *> defaultArguments =
      selected.constructor == nullptr ||
              selected.constructor->declaration == nullptr
          ? std::vector<const Expr *>{}
          : omittedDefaultArguments(
                selected.constructor->declaration->parameters(),
                arguments.size());
  recordDefaultArgumentDependencies(defaultArguments);
  semanticModel.record(
      construction,
      ResolvedConstructionInfo{
          .constructor =
              selected.constructor == nullptr ? 0 : selected.constructor->id,
          .declaration = selected.constructor == nullptr
                             ? nullptr
                             : selected.constructor->declaration,
          .constructedType = constructedType,
          .typeArguments = selected.typeArguments,
          .valueArguments = selected.valueArguments,
          .requirements = selected.requirements,
          .parameterTypes = selected.parameterTypes,
          .defaultArguments = std::move(defaultArguments),
          .borrowOrigin =
              selected.constructor != nullptr &&
                      selected.constructor->borrowParameter
                  ? BorrowOriginKind::Argument
                  : (selected.kind != ConstructorKind::Ordinary &&
                             typeTraits(constructedType).containsBorrowedState
                         ? BorrowOriginKind::Argument
                         : BorrowOriginKind::None),
          .borrowArgument = selected.constructor != nullptr &&
                                    selected.constructor->borrowParameter
                                ? *selected.constructor->borrowParameter
                                : 0,
          .borrowAccess = selected.constructor == nullptr
                              ? AccessMode::ReadOnly
                              : selected.constructor->borrowAccess,
          .generatedDefault = selected.generatedDefault,
          .kind = selected.kind});
  if (const auto *call = exprAs<Call>(&construction)) {
    const ResolvedConstructionInfo *resolved =
        semanticModel.findConstruction(construction);
    const Token token = callableToken(call->callee());
    if (resolved != nullptr) {
      SymbolId symbol = 0;
      if (resolved->declaration != nullptr) {
        symbol = symbolForDeclaration(resolved->declaration->name());
        if (symbol == 0) {
          symbol = recordToolingSymbol(
              resolved->declaration->name(), SymbolKind::Constructor,
              qualifiedName(owner.namespaceScope,
                            owner.name.lexeme + "::" + owner.name.lexeme),
              constructedType, false, true,
              constructorAccess(owner, resolved->declaration));
        }
      } else if (owner.declaration != nullptr) {
        symbol = symbolForDeclaration(owner.declaration->name());
        if (symbol == 0) {
          symbol = recordToolingSymbol(
              owner.declaration->name(),
              owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                              : SymbolKind::Class,
              qualifiedName(owner.namespaceScope, owner.name.lexeme),
              SemanticType::classType(owner.id));
        }
      }
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(token),
           .kind = SemanticOccurrenceKind::SelectedConstruction,
           .symbol = symbol,
           .roles = OccurrenceRole::Reference | OccurrenceRole::Call |
                    OccurrenceRole::TypeUse,
           .name = token.lexeme,
           .type = constructedType,
           .traits = typeTraits(constructedType),
           .constructor = resolved->declaration,
           .selectedConstruction = *resolved,
           .callGeometry = callArgumentGeometry(*call)});
    }
  }
  currentType = constructedType;
}

auto SemanticVisitor::Impl::analyzeExpectedMemberCall(
    const Call &call, const Get &member, const SemanticType &expected,
    const std::vector<SemanticType> &argumentTypes, const Token &paren)
    -> void {
  const auto requireArity = [&](std::size_t expectedCount) {
    if (argumentTypes.size() != expectedCount) {
      report(paren,
             "Expected member called with the wrong number of arguments.");
      return false;
    }
    return true;
  };
  // The builtin expected members carry their identity to MIR as
  // intrinsics: the calls name no declaration, so without the record
  // the lowered Call would be target-less and unnameable by a backend.
  const auto recordIntrinsic = [&](IntrinsicKind intrinsic) {
    ResolvedCallInfo resolved{.returnType = currentType,
                              .intrinsic = intrinsic,
                              .dispatchOwner = expected};
    semanticModel.record(call, resolved);
    recordSelectedCallOccurrence(call, std::move(resolved));
  };

  if (member.name().lexeme == "has_value") {
    if (!requireArity(0)) {
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = SemanticType::Bool;
    recordIntrinsic(IntrinsicKind::ExpectedHasValue);
    return;
  }
  if (member.name().lexeme == "value") {
    requireArity(0);
    currentType = expected.arguments[0];
    recordIntrinsic(IntrinsicKind::ExpectedValue);
    return;
  }
  if (member.name().lexeme == "error") {
    requireArity(0);
    currentType = expected.arguments[1];
    recordIntrinsic(IntrinsicKind::ExpectedError);
    return;
  }
  if (member.name().lexeme == "value_or") {
    if (expected.arguments[0] == SemanticType::Void) {
      report(member.name(), "expected<void, E> does not provide 'value_or'.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (requireArity(1) && argumentTypes[0] != SemanticType::Unknown &&
        argumentTypes[0] != expected.arguments[0]) {
      report(member.name(),
             "value_or fallback must exactly match the value type.");
    }
    currentType = expected.arguments[0];
    recordIntrinsic(IntrinsicKind::ExpectedValueOr);
  }
}

auto SemanticVisitor::Impl::analyzeArrayMemberCall(
    const Call &call, const Get &member,
    const std::vector<SemanticType> &argumentTypes, const Token &paren)
    -> void {
  const bool generatedBegin =
      member.name().generated &&
      (member.name().lexeme == "begin" || member.name().lexeme == "begin_mut");
  const bool generatedEnd =
      member.name().generated && member.name().lexeme == "end";
  if (member.name().lexeme != "size" && !generatedBegin && !generatedEnd) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (!argumentTypes.empty()) {
    report(paren,
           "Fixed array '" + member.name().lexeme + "' expects no arguments.",
           "GTI-S2016");
  }
  currentType = SemanticType::UInt64;
  // The builtin member carries its identity to MIR as an intrinsic: the
  // call names no declaration, so without the record the lowered Call
  // would be target-less and unnameable by a backend.
  ResolvedCallInfo resolved{.returnType = currentType,
                            .intrinsic = generatedBegin
                                             ? IntrinsicKind::ArrayRangeBegin
                                             : IntrinsicKind::ArraySize};
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
}

auto SemanticVisitor::Impl::analyzeStringViewMemberCall(
    const Call &call, const Get &member,
    const std::vector<SemanticType> &argumentTypes, const Token &paren)
    -> void {
  if (member.name().lexeme != "size" && member.name().lexeme != "empty") {
    currentType = SemanticType::Unknown;
    return;
  }
  if (!argumentTypes.empty()) {
    report(paren,
           "std::string_view '" + member.name().lexeme +
               "' expects no arguments.",
           "GTI-S2035");
  }
  const bool size = member.name().lexeme == "size";
  currentType = size ? SemanticType::UInt64 : SemanticType::Bool;
  // The builtin view members carry their identity to MIR as intrinsics:
  // the call names no declaration, so without this record the lowered
  // Call would be target-less and the backends could not name it.
  ResolvedCallInfo resolved{.returnType = currentType,
                            .intrinsic = size ? IntrinsicKind::StringViewSize
                                              : IntrinsicKind::StringViewEmpty,
                            .dispatchOwner = SemanticType::StringView};
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
}

auto SemanticVisitor::Impl::analyzeStringViewIndexAfterOperands(
    const ExprPtr &object, const ExprPtr &index, const SemanticType &indexType,
    const Token &) -> SemanticType {
  if (indexType != SemanticType::Unknown && !isInteger(indexType)) {
    report(expressionToken(index),
           "std::string_view index must have an integer type, not '" +
               typeSpelling(indexType) + "'.",
           "GTI-S2035");
  }
  const auto *literal = object ? exprAs<LiteralExpr>(object.get()) : nullptr;
  const auto *text = literal == nullptr
                         ? nullptr
                         : std::get_if<std::string>(&literal->value());
  if (text != nullptr) {
    if (const std::optional<IntegerConstant> constant =
            integerConstant(index.get());
        constant &&
        (constant->negative || constant->magnitude >= text->size())) {
      report(expressionToken(index),
             "String-view index is outside the valid range [0, " +
                 std::to_string(text->size()) + ").",
             "GTI-S2035");
    }
  }
  return SemanticType::Char;
}

auto SemanticVisitor::Impl::analyzeArrayIndexAfterOperands(
    const SemanticType &objectType, const ExprPtr &index,
    const SemanticType &indexType, const Token &bracket) -> SemanticType {
  if (objectType != SemanticType::Unknown &&
      (objectType.kind != SemanticType::Array ||
       objectType.arguments.size() != 1)) {
    report(bracket, "Indexing requires a fixed array value.", "GTI-S2016");
    return SemanticType::Unknown;
  }
  if (indexType != SemanticType::Unknown && !isInteger(indexType)) {
    report(expressionToken(index),
           "Fixed array index must have an integer type, not '" +
               typeSpelling(indexType) + "'.",
           "GTI-S2016");
  }
  if (objectType.kind != SemanticType::Array ||
      objectType.arguments.size() != 1) {
    return SemanticType::Unknown;
  }
  if (objectType.arrayLengthParameterId == 0) {
    if (const std::optional<IntegerConstant> constant =
            integerConstant(index.get());
        constant &&
        (constant->negative || constant->magnitude >= objectType.arrayLength)) {
      report(expressionToken(index),
             "Fixed array index is outside the valid range [0, " +
                 std::to_string(objectType.arrayLength) + ").",
             "GTI-S2016");
    }
  }
  return objectType.arguments[0];
}

auto SemanticVisitor::Impl::isDefaultInitializable(const SemanticType &type,
                                                   bool requirePublic) const
    -> bool {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
  case SemanticType::Bool:
  case SemanticType::Char:
  case SemanticType::StringView:
  case SemanticType::RawPointer:
    return true;
  case SemanticType::TypeParameter:
    return !requirePublic ||
           constraintImplies(constraintOf(type),
                             GenericConstraintKind::DefaultInitializable);
  case SemanticType::Array:
    return (type.arrayLengthParameterId == 0 && type.arrayLength == 0) ||
           (type.arguments.size() == 1 &&
            isDefaultInitializable(type.arguments[0], requirePublic));
  case SemanticType::Class: {
    const ClassInfo *owner = classInfo(type);
    if (owner == nullptr || owner->abstract) {
      return false;
    }
    if (const ConstructorInfo *constructor = defaultConstructor(*owner)) {
      const bool accessible = constructor->access == AccessModifier::Public ||
                              (!requirePublic && currentClass == owner->id);
      return accessible &&
             (!requestsStructuralDefaultConstruction(*constructor) ||
              classCanUseStructuralDefaultConstruction(*owner));
    }
    return classCanGenerateDefaultConstructor(*owner);
  }
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::fixedArrayClassLeafType(const SemanticType &type)
    -> SemanticType {
  const SemanticType *leaf = &type;
  while (leaf->kind == SemanticType::Array && leaf->arguments.size() == 1 &&
         leaf->arrayLengthParameterId == 0 && leaf->arrayLength != 0) {
    leaf = &leaf->arguments.front();
  }
  return leaf->kind == SemanticType::Class
             ? *leaf
             : SemanticType(SemanticType::Unknown);
}

auto SemanticVisitor::Impl::resolveArrayElementDefaultConstruction(
    const SemanticType &type) const -> std::optional<ResolvedConstructionInfo> {
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || owner->abstract) {
    return std::nullopt;
  }

  const ConstructorInfo *constructor = defaultConstructor(*owner);
  if (constructor == nullptr) {
    if (!classCanGenerateDefaultConstructor(*owner)) {
      return std::nullopt;
    }
    return ResolvedConstructionInfo{.constructedType = type,
                                    .generatedDefault = true};
  }
  if (!constructor->genericParameters.empty() ||
      (constructor->access == AccessModifier::Private &&
       currentClass != owner->id)) {
    return std::nullopt;
  }

  const GenericSubstitution substitution = classSubstitution(type);
  std::vector<AppliedConceptRequirement> requirements =
      constructor->requirements;
  for (AppliedConceptRequirement &requirement : requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, substitution);
    }
  }
  if (firstRequirementFailure(requirements)) {
    return std::nullopt;
  }

  std::vector<SemanticType> parameterTypes;
  parameterTypes.reserve(constructor->parameterTypes.size());
  for (const SemanticType &parameter : constructor->parameterTypes) {
    parameterTypes.push_back(substituteType(parameter, substitution));
  }
  std::vector<const Expr *> defaultArguments =
      constructor->declaration == nullptr
          ? std::vector<const Expr *>{}
          : omittedDefaultArguments(constructor->declaration->parameters(), 0);
  if (!parameterTypes.empty() &&
      defaultArguments.size() != parameterTypes.size()) {
    return std::nullopt;
  }

  return ResolvedConstructionInfo{
      .constructor = constructor->id,
      .declaration = constructor->declaration,
      .constructedType = type,
      .requirements = std::move(requirements),
      .parameterTypes = std::move(parameterTypes),
      .defaultArguments = std::move(defaultArguments),
      .borrowOrigin = constructor->borrowParameter ? BorrowOriginKind::Argument
                                                   : BorrowOriginKind::None,
      .borrowArgument =
          constructor->borrowParameter ? *constructor->borrowParameter : 0,
      .borrowAccess = constructor->borrowAccess,
      .generatedDefault = requestsStructuralDefaultConstruction(*constructor),
      .kind = ConstructorKind::Ordinary};
}

auto SemanticVisitor::Impl::genericTypeParameterCount(
    const std::vector<GenericParameterInfo> &parameters) -> std::size_t {
  return static_cast<std::size_t>(std::count_if(
      parameters.begin(), parameters.end(),
      [](const GenericParameterInfo &parameter) { return !parameter.value; }));
}

auto SemanticVisitor::Impl::genericValueParameterCount(
    const std::vector<GenericParameterInfo> &parameters) -> std::size_t {
  return parameters.size() - genericTypeParameterCount(parameters);
}

auto SemanticVisitor::Impl::constantFor(const Symbol &symbol) const
    -> const ConstantValue * {
  if (symbol.variableDeclaration != nullptr) {
    const BindingInfo *binding =
        semanticModel.findBinding(*symbol.variableDeclaration);
    if (binding == nullptr && instanceBaseModel != nullptr) {
      binding = instanceBaseModel->findBinding(*symbol.variableDeclaration);
    }
    if (binding != nullptr && binding->constant) {
      return &*binding->constant;
    }
  }
  return symbol.constant ? &*symbol.constant : nullptr;
}

auto SemanticVisitor::Impl::genericValueArgument(const TypeRef &argument) const
    -> std::optional<CompileTimeValue> {
  if (argument.name.segments.size() != 1 || !argument.arguments.empty() ||
      argument.pointer || argument.outerPointer ||
      !argument.arrayExtents.empty() || argument.reference) {
    return std::nullopt;
  }
  const Token &token = argument.name.last();
  if (token.kind == TokenKind::INT_LITERAL) {
    if (const auto *value = std::get_if<std::uint64_t>(&token.literal)) {
      return CompileTimeValue::uint64(*value);
    }
    return std::nullopt;
  }
  if (token.kind == TokenKind::IDENTIFIER) {
    if (const std::optional<CompileTimeValue> parameter =
            resolveValueParameter(token)) {
      return parameter;
    }
    const Symbol *symbol = resolve(token);
    const ConstantValue *constant =
        symbol == nullptr ? nullptr : constantFor(*symbol);
    if (constant != nullptr) {
      if (const std::optional<std::uint64_t> value =
              constantUnsignedMagnitude(*constant)) {
        return CompileTimeValue::uint64(*value);
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::evaluateResolvedArrayExtent(
    const ArrayExtentExpr &expression) const -> ArrayExtentEvaluation {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return {.value = *value};
    }
    if (expression.token.kind == TokenKind::IDENTIFIER) {
      const Symbol *symbol = resolve(expression.token);
      const ConstantValue *constant =
          symbol == nullptr ? nullptr : constantFor(*symbol);
      if (constant != nullptr) {
        if (const std::optional<std::uint64_t> value =
                constantUnsignedMagnitude(*constant)) {
          return {.value = *value};
        }
      }
    }
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (!expression.left || !expression.right) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const ArrayExtentEvaluation left =
      evaluateResolvedArrayExtent(*expression.left);
  if (!left.value) {
    return left;
  }
  const ArrayExtentEvaluation right =
      evaluateResolvedArrayExtent(*expression.right);
  if (!right.value) {
    return right;
  }

  const ConstantEvaluation evaluated = evaluateConstantBinary(
      expression.token.kind,
      ConstantValue{
          makeConstantInteger({.magnitude = *left.value}, {.width = 64})},
      ConstantValue{
          makeConstantInteger({.magnitude = *right.value}, {.width = 64})},
      CheckedIntegerDomain{.width = 64});
  if (evaluated) {
    return {.value = constantUnsignedMagnitude(*evaluated.value)};
  }
  if (evaluated.failure == ConstantEvaluationFailure::IntegerOverflow) {
    return {.error = expression.token.kind == TokenKind::MINUS
                         ? ArrayExtentEvaluationError::Underflow
                         : ArrayExtentEvaluationError::Overflow,
            .token = &expression.token};
  }
  if (evaluated.failure == ConstantEvaluationFailure::DivisionByZero ||
      evaluated.failure == ConstantEvaluationFailure::ModuloByZero) {
    return {.error = ArrayExtentEvaluationError::ZeroDivisor,
            .token = &expression.token};
  }
  return {.error = ArrayExtentEvaluationError::NonLiteral,
          .token = &expression.token};
}

auto SemanticVisitor::Impl::resolvedArrayExtent(
    const ArrayExtentExprPtr &extent) const -> std::optional<CompileTimeValue> {
  if (!extent) {
    return std::nullopt;
  }
  const ArrayExtentEvaluation evaluation = evaluateResolvedArrayExtent(*extent);
  if (evaluation.value) {
    return CompileTimeValue::uint64(*evaluation.value);
  }
  if (extent->isAtom() && extent->token.kind == TokenKind::IDENTIFIER) {
    return resolveValueParameter(extent->token);
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::validateArrayExtent(
    const ArrayExtentExprPtr &extent) -> void {
  if (!extent) {
    return;
  }
  const ArrayExtentEvaluation evaluation = evaluateResolvedArrayExtent(*extent);
  if (evaluation.value) {
    semanticModel.record(*extent, CompileTimeValue::uint64(*evaluation.value));
    recordArrayExtentUses(*extent);
    return;
  }

  const Token &location =
      evaluation.token == nullptr ? extent->token : *evaluation.token;
  if (evaluation.error == ArrayExtentEvaluationError::NonLiteral &&
      location.kind == TokenKind::IDENTIFIER) {
    const std::optional<CompileTimeValue> value =
        resolveValueParameter(location);
    if (value) {
      recordGenericParameterUse(location, *value, OccurrenceRole::Read);
      if (extent->isAtom()) {
        semanticModel.record(*extent, *value);
        return;
      }
      report(location,
             "Array extent arithmetic currently requires integer literals; "
             "a uint64_t value generic parameter may only be used as the "
             "complete extent.",
             "GTI-S2026");
      return;
    }
    if (const Symbol *symbol = resolve(location);
        symbol != nullptr && constantFor(*symbol) != nullptr) {
      report(location,
             "Fixed array extents require a non-negative integer "
             "constexpr value.",
             "GTI-S2026");
      return;
    }
    report(location,
           "Fixed array extent '" + location.lexeme +
               "' is not an in-scope constexpr integer or uint64_t value "
               "generic parameter.",
           "GTI-S2026");
    return;
  }

  switch (evaluation.error) {
  case ArrayExtentEvaluationError::Overflow:
    report(location, "Fixed array extent arithmetic overflows uint64_t.",
           "GTI-S2026");
    return;
  case ArrayExtentEvaluationError::Underflow:
    report(location,
           "Fixed array extent arithmetic cannot produce a negative value.",
           "GTI-S2026");
    return;
  case ArrayExtentEvaluationError::ZeroDivisor:
    report(location,
           "Fixed array extent arithmetic cannot divide or take modulo by "
           "zero.",
           "GTI-S2026");
    return;
  case ArrayExtentEvaluationError::None:
  case ArrayExtentEvaluationError::NonLiteral:
    report(location,
           "Fixed array extent must be an integer constant expression or "
           "uint64_t value generic parameter.",
           "GTI-S2026");
    return;
  }
}

auto SemanticVisitor::Impl::validateType(const TypeRef &type,
                                         bool allowNativeCOuterPointer,
                                         bool allowNativeFunctionType) -> void {
  if (type.isNativeFunction()) {
    const Token &location = type.name.last();
    if (!allowNativeFunctionType) {
      report(location,
             "A native function type must be named by a namespace-scope "
             "'using' declaration before it is used.",
             "GTI-S2076");
    }
    const std::size_t parameterCount =
        type.nativeFunctionParameterCount.value_or(0);
    const bool exactShape = type.arguments.size() == parameterCount + 1;
    if (!exactShape) {
      report(location, "Malformed native function type.", "GTI-S2076");
      return;
    }
    if (type.pointeeConst || type.pointer || type.outerPointer ||
        !type.arrayExtents.empty() || type.reference) {
      report(location,
             "A native function type cannot carry pointer, array, "
             "reference, or const suffixes; the type already represents "
             "a nullable C function pointer.",
             "GTI-S2076");
    }
    for (std::size_t index = 0; index < parameterCount; ++index) {
      validateType(type.arguments[index]);
    }
    validateType(type.arguments.back());
    const SemanticType resolved = baseTypeOf(type, currentNamespace);
    const SemanticType *returnType = resolved.nativeFunctionReturnType();
    if (returnType == nullptr ||
        !nativeFunctionBoundaryTypeAllowed(*returnType, true)) {
      report(type.arguments.back().name.last(),
             "A native function return must be void, a fixed-width C ABI "
             "value, c_string, a passive [[c_abi]] record, or one raw "
             "pointer to an admitted C ABI pointee.",
             "GTI-S2076");
    }
    const auto parameters = resolved.nativeFunctionParameterTypes();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (!nativeFunctionBoundaryTypeAllowed(parameters[index], false)) {
        report(type.arguments[index].name.last(),
               "A native function parameter must be an immutable "
               "fixed-width C ABI value, c_string, a passive [[c_abi]] "
               "record, or one raw pointer to an admitted C ABI pointee.",
               "GTI-S2076");
      }
    }
    return;
  }
  if (type.name.last().kind == TokenKind::AUTO) {
    report(type.name.last(),
           "'auto' is only valid for an initialized local binding.",
           "GTI-S2028");
    return;
  }
  if (reportCompilerPrivatePath(type.name)) {
    for (const TypeRef &argument : type.arguments) {
      if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
        validateType(argument);
      }
    }
    return;
  }
  const CompilerCapabilityTypeKind compilerCapability =
      compilerCapabilityType(type);
  semanticModel.recordCompilerCapabilityType(type, compilerCapability);
  recordTypeUse(type);
  if (type.pointeeConst && !type.pointer) {
    report(*type.pointeeConst,
           "'const' is currently supported only as a raw-pointer pointee "
           "qualifier, as in 'const T*'.",
           "GTI-S2056");
  }
  if (type.pointer) {
    const SemanticType pointee = baseTypeOf(type, currentNamespace);
    if (type.name.last().kind == TokenKind::AUTO ||
        pointee == SemanticType::NullPtr ||
        pointee.kind == SemanticType::TypeName ||
        pointee.kind == SemanticType::Function ||
        pointee.kind == SemanticType::NativeFunction ||
        pointee.kind == SemanticType::TypePack ||
        pointee.kind == SemanticType::RawPointer ||
        pointee.kind == SemanticType::Array) {
      report(*type.pointer,
             pointee.kind == SemanticType::RawPointer
                 ? "Pointer-to-pointer types are not supported in the "
                   "initial raw-pointer model."
             : pointee.kind == SemanticType::Array
                 ? "Pointer-to-array types are not supported in the "
                   "initial raw-pointer model."
                 : "A raw pointer requires a concrete object type or void "
                   "pointee.",
             "GTI-S2056");
    }
  }
  if (type.outerPointer && !allowNativeCOuterPointer) {
    report(*type.outerPointer,
           "A two-level pointer is available only as an opaque C handle "
           "out parameter or on a [[c_array(count)]] extern \"C\" return "
           "type.",
           "GTI-S2074");
  }
  if (!type.pointer) {
    const SemanticType direct = baseTypeOf(type, currentNamespace);
    if (direct.kind == SemanticType::Class && direct.classId != 0 &&
        direct.classId <= classes.size() &&
        classInfo(direct.classId).cOpaqueHandle) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2065", DiagnosticPhase::Semantics, type.name.last(),
          "Opaque C handle type '" + typeSpelling(direct) +
              "' is incomplete and may be used only behind one raw "
              "pointer.");
      const ClassInfo &owner = classInfo(direct.classId);
      if (owner.cOpaqueAttribute) {
        diagnostic.related.push_back(
            {tokenSpan(*owner.cOpaqueAttribute),
             "The pointer-only native handle is declared here."});
      }
      diagnostic.hints.emplace_back(
          "Use '" + typeSpelling(direct) +
          "*' at the native boundary and put ownership in an ordinary GTI "
          "wrapper.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
    validateArrayExtent(extent);
  }
  if (!allowPackTypeReference && isActiveTypePack(type)) {
    report(type.name.last(),
           "A generic type pack can only appear in its matching final "
           "parameter-pack declaration.",
           "GTI-S2023");
  }
  if (compilerCapability == CompilerCapabilityTypeKind::UniqueOwner) {
    if (type.arguments.size() != 1) {
      report(type.name.last(),
             "gti_internal::unique_owner<T> requires exactly one pointee "
             "type.",
             "GTI-S2018");
    } else {
      if (type.arguments[0].genericArgumentSyntax ==
          GenericArgumentSyntax::Value) {
        report(type.arguments[0].name.last(),
               "gti_internal::unique_owner requires a type argument.",
               "GTI-S2026");
        return;
      }
      validateType(type.arguments[0]);
      const SemanticType pointee = typeOf(type.arguments[0]);
      if (pointee == SemanticType::Void ||
          pointee.kind == SemanticType::Reference ||
          pointee.kind == SemanticType::Array) {
        report(type.arguments[0].name.last(),
               "Compiler-private unique ownership requires a concrete "
               "non-reference object type.",
               "GTI-S2018");
      }
    }
    if (!type.arrayExtents.empty()) {
      report(type.name.last(),
             "Fixed arrays of compiler-private unique owners are not "
             "supported.",
             "GTI-S2018");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::Storage) {
    if (type.arguments.size() != 1) {
      report(type.name.last(),
             "gti_internal::storage<T> requires exactly one element type.",
             "GTI-S2019");
    } else {
      if (type.arguments.front().genericArgumentSyntax ==
          GenericArgumentSyntax::Value) {
        report(type.arguments.front().name.last(),
               "gti_internal::storage requires a type argument.", "GTI-S2026");
        return;
      }
      validateType(type.arguments.front());
      const SemanticType elementType = typeOf(type.arguments.front());
      if (!isStorageElementType(elementType)) {
        report(type.arguments.front().name.last(),
               "Compiler-private storage requires a concrete value element "
               "type.",
               "GTI-S2019");
      } else if (elementType.kind != SemanticType::TypeParameter &&
                 typeTraits(elementType).containsBorrowedState) {
        report(type.arguments.front().name.last(),
               "Compiler-private storage cannot contain a value that "
               "retains borrowed state until general owner dependencies "
               "are available.",
               "GTI-S2019");
      }
    }
    if (!type.arrayExtents.empty()) {
      report(type.name.last(),
             "Fixed arrays of compiler-private storage are not supported.",
             "GTI-S2019");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::TextView) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "gti_internal::text_view does not take generic arguments.",
             "GTI-S2034");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::CString) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "gti_internal::c_string does not take generic arguments.",
             "GTI-S2054");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::ThreadHandle) {
    if (type.arguments.size() > 1) {
      report(type.name.last(),
             "A compiler-private thread handle accepts at most one result "
             "type.",
             "GTI-S2084");
    } else if (type.arguments.size() == 1) {
      validateType(type.arguments.front());
      const SemanticType resultType = typeOf(type.arguments.front());
      if (resultType == SemanticType::Void ||
          resultType.kind == SemanticType::Reference ||
          resultType.kind == SemanticType::RawPointer) {
        report(type.arguments.front().name.last(),
               "A managed-thread result must be an owned non-void value "
               "type.",
               "GTI-S2084");
      }
    }
    return;
  }
  if (!type.arrayExtents.empty() && !type.pointer &&
      baseTypeOf(type, currentNamespace) == SemanticType::Void) {
    // A pointer declarator makes the element `void*`, a sized pointer
    // scalar; only a bare `void` element has no object representation.
    report(type.name.last(), "Fixed array elements cannot have type void.",
           "GTI-S2015");
  }
  if (type.name.last().kind == TokenKind::EXPECTED &&
      (type.arguments.size() != 2 ||
       typeOf(type.arguments[1]) == SemanticType::Void)) {
    report(type.name.last(), "expected<T, E> requires a non-void error type.");
  }
  if (type.name.last().kind == TokenKind::EXPECTED) {
    for (const TypeRef &argument : type.arguments) {
      validateType(argument);
    }
  }
  if (type.name.last().kind != TokenKind::IDENTIFIER) {
    return;
  }
  if (resolveTypeParameter(type.name)) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "Generic type parameters cannot take generic arguments.");
    }
    return;
  }

  if (resolveTypeAliasPath(type.name, currentNamespace)) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "Type alias '" + pathSpelling(type.name) +
                 "' does not take generic arguments.",
             "GTI-S2030");
      for (const TypeRef &argument : type.arguments) {
        if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
          validateType(argument);
        }
      }
    }
    return;
  }

  if (resolveEnumPath(type.name, currentNamespace)) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "Scoped enum type '" + pathSpelling(type.name) +
                 "' does not take generic arguments.",
             "GTI-S2036");
    }
    return;
  }

  const std::optional<ClassId> classId =
      resolveClassPath(type.name, currentNamespace);
  if (!classId) {
    if (type.name.segments.size() == 1 && type.name.last().lexeme == "string") {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2033", DiagnosticPhase::Semantics, type.name.last(),
          "'string' is not a built-in GTI type; choose 'std::string' for "
          "owning text or 'std::string_view' for a non-owning view.");
      diagnostic.hints.push_back(
          "Use '#include <std/string>' before naming 'std::string'.");
      diagnostic.hints.push_back(
          "'std::string_view' is available from the standard prelude.");
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
    if (const std::optional<TypeAliasId> globalAlias =
            resolveTypeAliasPathGlobally(type.name, currentNamespace)) {
      const RegisteredTypeAlias &declaration = typeAliases[*globalAlias - 1];
      if (declaration.compilerPrivate &&
          !isCompilerTrustedRequester(type.name.last())) {
        reportCompilerPrivateAccess(type.name.last(), pathSpelling(type.name));
        return;
      }
      if (reportInvisibleDeclaration(type.name.last(), pathSpelling(type.name),
                                     declaration.declaration->name(),
                                     declaration.sourceUnit)) {
        return;
      }
    }
    const std::optional<ClassId> globalClass =
        resolveClassPathGlobally(type.name, currentNamespace);
    if (globalClass) {
      const ClassInfo &declaration = classInfo(*globalClass);
      if (reportInvisibleDeclaration(type.name.last(), pathSpelling(type.name),
                                     declaration.name,
                                     declaration.sourceUnit)) {
        return;
      }
    }
    const std::optional<EnumId> globalEnum =
        resolveEnumPathGlobally(type.name, currentNamespace);
    if (globalEnum && *globalEnum != 0 && *globalEnum <= enums.size()) {
      const EnumInfo &declaration = enums[*globalEnum - 1];
      if (reportInvisibleDeclaration(type.name.last(), pathSpelling(type.name),
                                     declaration.name,
                                     declaration.sourceUnit)) {
        return;
      }
    }
    report(type.name.last(), "Unknown type '" + pathSpelling(type.name) + "'.");
    return;
  }
  const ClassInfo &declaration = classInfo(*classId);
  const std::size_t expectedArguments = declaration.genericParameters.size();
  if (type.arguments.size() != expectedArguments) {
    const bool typeOnly =
        genericValueParameterCount(declaration.genericParameters) == 0;
    report(type.name.last(),
           "Type '" + pathSpelling(type.name) + "' requires " +
               std::to_string(expectedArguments) +
               (typeOnly ? " generic type argument" : " generic argument") +
               (expectedArguments == 1 ? "." : "s."));
  }
  const std::size_t count =
      std::min(type.arguments.size(), declaration.genericParameters.size());
  for (std::size_t index = 0; index < count; ++index) {
    const GenericParameterInfo &parameter =
        declaration.genericParameters[index];
    const TypeRef &argument = type.arguments[index];
    if (parameter.value) {
      const std::optional<CompileTimeValue> value =
          genericValueArgument(argument);
      if (!value) {
        report(argument.name.last(),
               "Generic parameter '" + parameter.name.lexeme +
                   "' requires a uint64_t compile-time value.",
               "GTI-S2026");
      } else if (argument.name.last().kind == TokenKind::IDENTIFIER) {
        recordGenericParameterUse(argument.name.last(), *value,
                                  OccurrenceRole::Read);
      }
      continue;
    }
    if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
      report(argument.name.last(),
             "Generic parameter '" + parameter.name.lexeme +
                 "' requires a type argument.",
             "GTI-S2026");
      continue;
    }
    validateType(argument);
    const SemanticType argumentType = typeOf(argument);
    if (argumentType == SemanticType::Void) {
      report(argument.name.last(), "Generic type arguments cannot be void.");
    } else if (const std::optional<GenericConstraintKind> failed =
                   firstUnsatisfiedConstraint(argumentType,
                                              parameter.constraints)) {
      reportConstraintFailure(
          argument.name.last(),
          ConstraintFailure{.parameter = parameter.name,
                            .argument = argumentType,
                            .constraints = parameter.constraints,
                            .failed = *failed,
                            .constraintName = parameter.constraintName});
    }
  }
  for (std::size_t index = count; index < type.arguments.size(); ++index) {
    const TypeRef &argument = type.arguments[index];
    if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
      validateType(argument);
    }
  }
}

auto SemanticVisitor::Impl::compilerCapabilityType(
    const TypeRef &type, const std::vector<std::string> &fromScope) const
    -> CompilerCapabilityTypeKind {
  if (!isCompilerTrustedRequester(type.name.first())) {
    return CompilerCapabilityTypeKind::None;
  }
  const std::optional<ClassId> selected =
      resolveClassPath(type.name, fromScope);
  if (!selected || *selected == 0 || *selected > classes.size()) {
    return CompilerCapabilityTypeKind::None;
  }
  return classInfo(*selected).compilerCapability;
}

auto SemanticVisitor::Impl::compilerCapabilityType(const TypeRef &type) const
    -> CompilerCapabilityTypeKind {
  return compilerCapabilityType(type, currentNamespace);
}

auto SemanticVisitor::Impl::isGtiInternalUniqueOwner(const TypeRef &type) const
    -> bool {
  return compilerCapabilityType(type) ==
         CompilerCapabilityTypeKind::UniqueOwner;
}

auto SemanticVisitor::Impl::isGtiInternalStorage(const TypeRef &type) const
    -> bool {
  return compilerCapabilityType(type) == CompilerCapabilityTypeKind::Storage;
}

auto SemanticVisitor::Impl::isGtiInternalTextView(const TypeRef &type) const
    -> bool {
  return compilerCapabilityType(type) == CompilerCapabilityTypeKind::TextView;
}

auto SemanticVisitor::Impl::containsReference(const TypeRef &type) -> bool {
  if (type.reference) {
    return true;
  }
  return std::any_of(
      type.arguments.begin(), type.arguments.end(),
      [](const TypeRef &argument) { return containsReference(argument); });
}

auto SemanticVisitor::Impl::containsRawPointer(const TypeRef &type) -> bool {
  if (type.pointer) {
    return true;
  }
  return std::any_of(
      type.arguments.begin(), type.arguments.end(),
      [](const TypeRef &argument) { return containsRawPointer(argument); });
}

auto SemanticVisitor::Impl::isActiveTypePack(const TypeRef &type) const
    -> bool {
  const std::optional<SemanticType> parameter = resolveTypeParameter(type.name);
  if (!parameter || parameter->kind != SemanticType::TypeParameter) {
    return false;
  }
  return std::any_of(typePackScopes.rbegin(), typePackScopes.rend(),
                     [&](const auto &scope) {
                       return scope.contains(parameter->genericParameterId);
                     });
}

auto SemanticVisitor::Impl::validateReferencePlacement(
    const TypeRef &type, bool allowTopLevel, std::string_view context,
    bool allowRvalueReference, bool allowDefaultLibraryStorageReference)
    -> void {
  if (type.reference) {
    if (type.reference->kind == TokenKind::AND && !allowRvalueReference) {
      report(*type.reference,
             "'&&' is currently confined to a class or struct's exact "
             "move constructor policy.",
             "GTI-S2020");
    }
    if (!allowTopLevel) {
      report(*type.reference,
             "References cannot be used as a " + std::string(context) + " yet.",
             "GTI-S2017");
    }
    if (!type.arrayExtents.empty()) {
      report(*type.reference,
             "References to fixed arrays are not supported yet.", "GTI-S2017");
    }
    const SemanticType resolved = typeOf(type);
    if (resolved.kind == SemanticType::Reference &&
        resolved.arguments.size() == 1 &&
        resolved.arguments.front().kind == SemanticType::RawPointer) {
      report(*type.reference,
             "References to raw pointers are not supported in the initial "
             "raw-pointer model.",
             "GTI-S2056");
    }
    if (baseTypeOf(type, currentNamespace) == SemanticType::Void) {
      report(*type.reference, "References cannot refer to void.", "GTI-S2017");
    }
    const SemanticType referencedType = baseTypeOf(type, currentNamespace);
    const bool permittedStorageBorrow =
        allowDefaultLibraryStorageReference &&
        (referencedType.kind == SemanticType::Storage ||
         referencedType.kind == SemanticType::PrefixStorage);
    if (isDirectOwnerType(referencedType) && !permittedStorageBorrow) {
      const bool unique = referencedType.kind == SemanticType::UniqueOwner;
      report(*type.reference,
             unique
                 ? "References to compiler-private unique owners are not "
                   "supported; borrow the owned value instead."
                 : "References to compiler-private storage are not supported; "
                   "use storage operations instead.",
             "GTI-S2018");
    }
  }
  for (const TypeRef &argument : type.arguments) {
    if (containsReference(argument)) {
      report(argument.reference ? *argument.reference : argument.name.last(),
             "References cannot be nested inside another type yet.",
             "GTI-S2017");
    }
  }
}

auto SemanticVisitor::Impl::isPublicBaseConversion(
    const SemanticType &derived, const SemanticType &base,
    std::unordered_set<ClassId> &visited) const -> bool {
  if (derived.kind != SemanticType::Class || base.kind != SemanticType::Class ||
      derived.classId == 0 || base.classId == 0 ||
      !visited.insert(derived.classId).second) {
    return false;
  }
  const ClassInfo *owner = classInfo(derived);
  if (owner == nullptr) {
    return false;
  }
  const GenericSubstitution substitution = classSubstitution(derived);
  for (const ClassBaseTypeInfo &declaredBase : owner->bases) {
    const SemanticType actualBase =
        substituteType(declaredBase.type, substitution);
    if (actualBase == base ||
        isPublicBaseConversion(actualBase, base, visited)) {
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::isPublicBaseConversion(
    const SemanticType &derived, const SemanticType &base) const -> bool {
  std::unordered_set<ClassId> visited;
  return isPublicBaseConversion(derived, base, visited);
}

auto SemanticVisitor::Impl::validateReferenceBinding(
    const SemanticType &reference, const SemanticType &initializerType,
    const ExprPtr &initializer) -> void {
  if (reference.arguments.size() != 1) {
    return;
  }
  const SemanticType &referent = reference.arguments[0];
  if (initializerType != SemanticType::Unknown && initializerType != referent &&
      !isPublicBaseConversion(initializerType, referent)) {
    report(expressionToken(initializer),
           "Reference requires an exact '" + typeSpelling(referent) +
               "' initializer, but received '" + typeSpelling(initializerType) +
               "'.",
           "GTI-S2017");
    return;
  }
  const ExpressionInfo *info =
      initializer ? semanticModel.findExpression(*initializer) : nullptr;
  if (info == nullptr || info->category != ValueCategory::Place) {
    report(expressionToken(initializer),
           "Reference initializer must be an addressable value, not a "
           "temporary.",
           "GTI-S2017");
    return;
  }
  if (!hasStableBorrowStorage(initializer)) {
    report(expressionToken(initializer),
           "Reference initializer is derived from temporary storage that "
           "does not outlive the binding.",
           "GTI-S2017");
    return;
  }
  if (reference.referenceAccess == AccessMode::Mutable) {
    const std::optional<RetainedLoanSource> source =
        retainedLoanSource(initializer);
    const SemanticLoanInfo *sourceLoan =
        source ? semanticModel.findLoan(source->loan) : nullptr;
    if (sourceLoan != nullptr && sourceLoan->access == AccessMode::ReadOnly) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2017", DiagnosticPhase::Semantics, expressionToken(initializer),
          "A read-only loan cannot be upgraded to a mutable reborrow.");
      if (sourceLoan->origin != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(expressionToken(*sourceLoan->origin)),
             "The read-only parent loan originates here."});
      }
      diagnostic.hints.emplace_back(
          "Create the mutable borrow from mutable owner storage or from an "
          "active mutable parent loan.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else if (info->access != AccessMode::Mutable) {
      if (!reportReceiverRestrictedReference(initializer)) {
        report(expressionToken(initializer),
               "A mutable reference requires a mutable initializer.",
               "GTI-S2017");
      }
    }
  }
}

auto SemanticVisitor::Impl::validateStoredBorrowInitialization(
    const SemanticType &type, const ExprPtr &initializer) -> void {
  if (!isDirectStoredReferenceType(type) &&
      type.kind != SemanticType::StringView) {
    report(expressionToken(initializer),
           "Borrowed state cannot be nested inside another stored value in "
           "the current lifetime model.",
           "GTI-S2045");
    return;
  }
  if (!hasStableBorrowStorage(initializer)) {
    report(expressionToken(initializer),
           "Stored-reference value is derived from temporary storage that "
           "does not outlive the binding.",
           "GTI-S2045");
  }
}

auto SemanticVisitor::Impl::rejectEscapingLocalChildLoan(
    const ExprPtr &value, std::string_view destination) -> bool {
  const std::optional<RetainedLoanSource> source = retainedLoanSource(value);
  const Symbol *symbol = !source || source->carrier == nullptr
                             ? nullptr
                             : resolve(source->carrier->name());
  const SemanticLoanInfo *loan =
      !source ? nullptr : semanticModel.findLoan(source->loan);
  if (symbol == nullptr ||
      symbol->bindingKind != SemanticBindingKind::LocalVariable ||
      loan == nullptr || loan->parent == 0) {
    return false;
  }
  report(expressionToken(value),
         "A local child reborrow cannot escape through " +
             std::string(destination) + ".",
         "GTI-S2017");
  return true;
}

auto SemanticVisitor::Impl::validateStoredBorrowReturn(
    const SemanticType &returnType, const SemanticType &valueType,
    const ExprPtr &value) -> void {
  // An earlier expression diagnostic owns an unknown result. In
  // particular, a rejected confined callable result must not cascade into
  // an unrelated owner-origin diagnostic for the enclosing return type.
  if (valueType == SemanticType::Unknown) {
    return;
  }
  if (!isDirectStoredReferenceType(returnType)) {
    report(expressionToken(value),
           "A return type cannot nest borrowed state in the current "
           "lifetime model.",
           "GTI-S2045");
    return;
  }
  if (valueType != returnType) {
    return;
  }
  if (rejectEscapingLocalChildLoan(value, "a stored-reference return")) {
    return;
  }
  validateCurrentReturnBorrowSource(value, "stored-reference value",
                                    "GTI-S2045");
}

auto SemanticVisitor::Impl::validateOwnerDependentStringViewReturn(
    const SemanticType &valueType, const ExprPtr &value) -> void {
  if (valueType == SemanticType::Unknown) {
    return;
  }
  const ExpressionInfo *info =
      value ? semanticModel.findExpression(*value) : nullptr;
  if (info == nullptr || !info->traits.containsBorrowedState) {
    return;
  }
  const FunctionInfo *function = currentFunctionInfo();
  if (function == nullptr ||
      function->returnBorrowOrigin == BorrowOriginKind::None) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2045", DiagnosticPhase::Semantics, expressionToken(value),
        "A dynamic std::string_view cannot escape without one exact owner "
        "dependency.");
    diagnostic.hints.emplace_back(
        "Return a literal-backed view, or relay a view tied to this "
        "method's receiver or one exact read-only parameter.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  if (rejectEscapingLocalChildLoan(value,
                                   "a dynamic std::string_view return")) {
    return;
  }
  validateCurrentReturnBorrowSource(value, "dynamic std::string_view",
                                    "GTI-S2045");
}

auto SemanticVisitor::Impl::validateReferenceReturn(
    const SemanticType &reference, const SemanticType &valueType,
    const ExprPtr &value) -> void {
  if (reference.arguments.size() != 1) {
    return;
  }
  const SemanticType &referent = reference.arguments[0];
  if (valueType != SemanticType::Unknown && valueType != referent &&
      !isPublicBaseConversion(valueType, referent)) {
    report(expressionToken(value),
           "Reference return requires an exact '" + typeSpelling(referent) +
               "' value, but received '" + typeSpelling(valueType) + "'.",
           "GTI-S2017");
    return;
  }
  const ExpressionInfo *info =
      value ? semanticModel.findExpression(*value) : nullptr;
  if (info == nullptr || info->category != ValueCategory::Place) {
    report(expressionToken(value),
           "Reference return requires an addressable value, not a "
           "temporary.",
           "GTI-S2017");
    return;
  }
  if (reference.referenceAccess == AccessMode::Mutable &&
      info->access != AccessMode::Mutable) {
    report(expressionToken(value),
           "Mutable reference return requires a mutable value.", "GTI-S2017");
    return;
  }
  if (rejectEscapingLocalChildLoan(value, "a reference return")) {
    return;
  }
  validateCurrentReturnBorrowSource(value, "reference", "GTI-S2017");
}

auto SemanticVisitor::Impl::validateCurrentReturnBorrowSource(
    const ExprPtr &value, std::string_view description, std::string code)
    -> void {
  const FunctionInfo *function = currentFunctionInfo();
  if (function == nullptr ||
      function->returnBorrowOrigin == BorrowOriginKind::None) {
    return;
  }
  const ExpressionInfo *valueInfo =
      value == nullptr ? nullptr : semanticModel.findExpression(*value);
  if (valueInfo == nullptr || valueInfo->type == SemanticType::Unknown) {
    return;
  }
  if (function->returnBorrowOrigin == BorrowOriginKind::Receiver) {
    if (!isReceiverDerivedBorrow(value)) {
      Diagnostic diagnostic = makeDiagnostic(
          std::move(code), DiagnosticPhase::Semantics, expressionToken(value),
          "Returned " + std::string(description) +
              " must derive from this method's receiver.");
      diagnostic.hints.emplace_back(
          "Return a value borrowed from 'this'; locals, parameters, "
          "temporaries, and global storage cannot provide this result.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    return;
  }
  if (function->returnBorrowOrigin == BorrowOriginKind::Global) {
    const SemanticPlace source = semanticPlace(value);
    const BorrowOriginPlace actual{
        .root = source.root == nullptr
                    ? 0
                    : (source.rootIdentity == 0 ? toolingSymbolFor(*source.root)
                                                : source.rootIdentity),
        .projections = source.projections};
    if (function->returnBorrowPlace && *function->returnBorrowPlace == actual) {
      return;
    }
    Diagnostic diagnostic = makeDiagnostic(
        std::move(code), DiagnosticPhase::Semantics, expressionToken(value),
        "Returned " + std::string(description) +
            " must derive from this function's exact summarized "
            "global/static storage place.");
    diagnostic.hints.emplace_back(
        "Return the same global or static place on every path; locals, "
        "parameters, temporaries, and unrelated storage cannot escape.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }

  if (function->declaration == nullptr ||
      function->returnBorrowParameter >=
          function->declaration->parameters().size()) {
    return;
  }
  const Parameter &source =
      function->declaration->parameters()[function->returnBorrowParameter];
  const bool storedValueSource =
      function->returnBorrowParameter < function->parameterTypes.size() &&
      isDirectStoredReferenceType(
          function->parameterTypes[function->returnBorrowParameter]);
  if (storedValueSource) {
    validateStoredCarrierReturnSource(*function, source, value, description);
    return;
  }
  if (isParameterDerivedBorrow(value, source)) {
    return;
  }
  Diagnostic diagnostic = makeDiagnostic(
      std::move(code), DiagnosticPhase::Semantics, expressionToken(value),
      "Returned " + std::string(description) +
          " must derive from read-only parameter '" + source.name.lexeme +
          "'.");
  diagnostic.related.push_back(
      {tokenSpan(source.name), "Required owner dependency source is here."});
  diagnostic.hints.emplace_back(
      "Return the parameter, a checked projection of it, or a borrowed "
      "value constructed from it; local, temporary, global, and unrelated "
      "parameter storage cannot escape.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::storedCarrierReturnProvenance(
    const ExprPtr &expression, const Parameter &parameter,
    std::unordered_set<const Expr *> &visiting) const
    -> StoredCarrierReturnProvenance {
  if (!expression || !visiting.insert(expression.get()).second) {
    return StoredCarrierReturnProvenance::None;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
    if (symbol != nullptr && symbol->parameterDeclaration == &parameter) {
      return StoredCarrierReturnProvenance::CarrierValue;
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return storedCarrierReturnProvenance(*source, parameter, visiting);
    }
    return StoredCarrierReturnProvenance::None;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return storedCarrierReturnProvenance(grouping->expression(), parameter,
                                         visiting);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return storedCarrierReturnProvenance(binary->right(), parameter, visiting);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    const StoredCarrierReturnProvenance object =
        storedCarrierReturnProvenance(member->object(), parameter, visiting);
    const SemanticType objectType = memberAccessObjectType(*member);
    const ClassInfo *owner = classInfo(objectType);
    const MemberInfo *selected = findMember(objectType, member->name());
    if (object == StoredCarrierReturnProvenance::CarrierValue) {
      if (owner != nullptr && owner->storedReference && selected != nullptr &&
          selected->symbol.variableDeclaration ==
              owner->storedReference->field) {
        return StoredCarrierReturnProvenance::ExternalReferent;
      }
      return StoredCarrierReturnProvenance::CarrierShellProjection;
    }
    if (object == StoredCarrierReturnProvenance::CarrierShellProjection) {
      return object;
    }
    if (object == StoredCarrierReturnProvenance::ExternalReferent ||
        object == StoredCarrierReturnProvenance::ExternalReferentProjection) {
      return StoredCarrierReturnProvenance::ExternalReferentProjection;
    }
    return StoredCarrierReturnProvenance::None;
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    const StoredCarrierReturnProvenance object =
        storedCarrierReturnProvenance(index->object(), parameter, visiting);
    if (object == StoredCarrierReturnProvenance::CarrierValue ||
        object == StoredCarrierReturnProvenance::CarrierShellProjection) {
      return StoredCarrierReturnProvenance::CarrierShellProjection;
    }
    if (object == StoredCarrierReturnProvenance::ExternalReferent ||
        object == StoredCarrierReturnProvenance::ExternalReferentProjection) {
      return StoredCarrierReturnProvenance::ExternalReferentProjection;
    }
    return StoredCarrierReturnProvenance::None;
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    const StoredCarrierReturnProvenance operand =
        storedCarrierReturnProvenance(unary->right(), parameter, visiting);
    if (operand == StoredCarrierReturnProvenance::CarrierValue ||
        operand == StoredCarrierReturnProvenance::CarrierShellProjection) {
      return StoredCarrierReturnProvenance::CarrierShellProjection;
    }
    if (operand == StoredCarrierReturnProvenance::ExternalReferent ||
        operand == StoredCarrierReturnProvenance::ExternalReferentProjection) {
      return StoredCarrierReturnProvenance::ExternalReferentProjection;
    }
    return StoredCarrierReturnProvenance::None;
  }
  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(*expression);
      construction != nullptr &&
      construction->borrowOrigin == BorrowOriginKind::Argument) {
    const ExprPtr *source = nullptr;
    if (const auto *call = exprAs<Call>(expression.get());
        call != nullptr &&
        construction->borrowArgument < call->arguments().size()) {
      source = &call->arguments()[construction->borrowArgument];
    } else if (const auto *initializer =
                   exprAs<DirectInitializer>(expression.get());
               initializer != nullptr &&
               construction->borrowArgument < initializer->arguments().size()) {
      source = &initializer->arguments()[construction->borrowArgument];
    }
    if (source != nullptr &&
        storedCarrierReturnProvenance(*source, parameter, visiting) ==
            StoredCarrierReturnProvenance::ExternalReferent) {
      return StoredCarrierReturnProvenance::CarrierValue;
    }
    return StoredCarrierReturnProvenance::None;
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    if (resolvedIntrinsicKind(*call) == IntrinsicKind::Move) {
      return call->arguments().size() == 1
                 ? storedCarrierReturnProvenance(call->arguments().front(),
                                                 parameter, visiting)
                 : StoredCarrierReturnProvenance::None;
    }
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved == nullptr ||
        resolved->borrowOrigin != BorrowOriginKind::Argument ||
        resolved->borrowArgument >= call->arguments().size()) {
      return StoredCarrierReturnProvenance::None;
    }
    const StoredCarrierReturnProvenance source = storedCarrierReturnProvenance(
        call->arguments()[resolved->borrowArgument], parameter, visiting);
    if (source == StoredCarrierReturnProvenance::None ||
        source == StoredCarrierReturnProvenance::CarrierShellProjection ||
        source == StoredCarrierReturnProvenance::ExternalReferentProjection) {
      return source;
    }
    const bool formalStoredCarrier =
        resolved->borrowArgument < resolved->parameterTypes.size() &&
        isDirectStoredReferenceType(
            resolved->parameterTypes[resolved->borrowArgument]);
    // An ordinary reference-parameter summary proves only that the result
    // depends on that owner root; it does not prove that the runtime
    // referent is the exact argument instead of a same-typed projection.
    // Only a carrier formal has itself passed the stricter return check.
    if (!formalStoredCarrier ||
        source != StoredCarrierReturnProvenance::CarrierValue) {
      return StoredCarrierReturnProvenance::None;
    }
    if (isDirectStoredReferenceType(resolved->returnType)) {
      return StoredCarrierReturnProvenance::CarrierValue;
    }
    if (resolved->returnType.kind == SemanticType::Reference) {
      return StoredCarrierReturnProvenance::ExternalReferent;
    }
    return StoredCarrierReturnProvenance::None;
  }
  return StoredCarrierReturnProvenance::None;
}

auto SemanticVisitor::Impl::validateStoredCarrierReturnSource(
    const FunctionInfo &function, const Parameter &source, const ExprPtr &value,
    std::string_view description) -> void {
  const SemanticType &sourceType =
      function.parameterTypes[function.returnBorrowParameter];
  const std::optional<SemanticType> sourceReferent =
      storedReferenceReferent(sourceType);
  std::optional<SemanticType> resultReferent;
  StoredCarrierReturnProvenance required = StoredCarrierReturnProvenance::None;
  if (function.returnType.kind == SemanticType::Reference &&
      function.returnType.arguments.size() == 1) {
    resultReferent = function.returnType.arguments.front();
    required = StoredCarrierReturnProvenance::ExternalReferent;
  } else if (isDirectStoredReferenceType(function.returnType)) {
    resultReferent = storedReferenceReferent(function.returnType);
    required = StoredCarrierReturnProvenance::CarrierValue;
  }

  std::unordered_set<const Expr *> visiting;
  const StoredCarrierReturnProvenance actual =
      storedCarrierReturnProvenance(value, source, visiting);
  if (sourceReferent && resultReferent && *sourceReferent == *resultReferent &&
      actual == required) {
    return;
  }

  std::string message;
  if (sourceReferent && resultReferent && *sourceReferent != *resultReferent) {
    message = "Returned " + std::string(description) +
              " has owner-dependency referent type '" +
              typeSpelling(*resultReferent) +
              "', but by-value carrier parameter '" + source.name.lexeme +
              "' proves only '" + typeSpelling(*sourceReferent) + "'.";
  } else if (actual == StoredCarrierReturnProvenance::CarrierShellProjection) {
    message = "Returned " + std::string(description) +
              " is a projection of the by-value carrier shell rather than "
              "its external owner dependency.";
  } else if (actual ==
             StoredCarrierReturnProvenance::ExternalReferentProjection) {
    message = "Returned " + std::string(description) +
              " is a projection of the carried referent rather than the "
              "exact external owner dependency.";
  } else if (actual == StoredCarrierReturnProvenance::CarrierValue &&
             required == StoredCarrierReturnProvenance::ExternalReferent) {
    message = "Returned " + std::string(description) +
              " refers to the by-value carrier shell rather than its "
              "external owner dependency.";
  } else {
    message = "Returned " + std::string(description) +
              " does not relay the exact external owner dependency carried "
              "by parameter '" +
              source.name.lexeme + "'.";
  }
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2085", DiagnosticPhase::Semantics,
                     expressionToken(value), std::move(message));
  diagnostic.related.push_back(
      {tokenSpan(source.name), "Stored owner dependency enters here."});
  diagnostic.hints.emplace_back(
      "Return the carrier value itself, or return its one exact stored "
      "referent with the same type; ordinary carrier fields and shell "
      "projections cannot escape.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::isParameterDerivedBorrow(
    const ExprPtr &expression, const Parameter &parameter) const -> bool {
  std::unordered_set<const Expr *> visiting;
  return isParameterDerivedBorrow(expression, parameter, visiting);
}

auto SemanticVisitor::Impl::isParameterDerivedBorrow(
    const ExprPtr &expression, const Parameter &parameter,
    std::unordered_set<const Expr *> &visiting) const -> bool {
  if (!expression || !visiting.insert(expression.get()).second) {
    return false;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
    if (symbol != nullptr && symbol->parameterDeclaration == &parameter) {
      return true;
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return isParameterDerivedBorrow(*source, parameter, visiting);
    }
    return false;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return isParameterDerivedBorrow(grouping->expression(), parameter,
                                    visiting);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return isParameterDerivedBorrow(binary->right(), parameter, visiting);
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return isParameterDerivedBorrow(index->object(), parameter, visiting);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return isParameterDerivedBorrow(member->object(), parameter, visiting);
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    return isParameterDerivedBorrow(unary->right(), parameter, visiting);
  }
  if (const ExprPtr *source = storedBorrowSource(expression)) {
    return isParameterDerivedBorrow(*source, parameter, visiting);
  }
  return false;
}

auto SemanticVisitor::Impl::isReceiverDerivedBorrow(
    const ExprPtr &expression) const -> bool {
  std::unordered_set<const Expr *> visiting;
  return isReceiverDerivedBorrow(expression, visiting);
}

auto SemanticVisitor::Impl::isReceiverDerivedBorrow(
    const ExprPtr &expression, std::unordered_set<const Expr *> &visiting) const
    -> bool {
  if (!expression || !visiting.insert(expression.get()).second) {
    return false;
  }
  if (exprAs<This>(expression.get()) != nullptr) {
    return true;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
    if (currentClass && symbol != nullptr &&
        symbol->bindingKind == SemanticBindingKind::Field &&
        !symbol->staticMember) {
      // An unqualified instance field is the same receiver-rooted place as
      // an explicit this.field projection. Keep that resolved provenance
      // while leaving a same-spelled local or parameter independent.
      return true;
    }
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return isReceiverDerivedBorrow(*source, visiting);
    }
    return false;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return isReceiverDerivedBorrow(grouping->expression(), visiting);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return isReceiverDerivedBorrow(binary->right(), visiting);
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return isReceiverDerivedBorrow(index->object(), visiting);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return isReceiverDerivedBorrow(member->object(), visiting);
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    return isReceiverDerivedBorrow(unary->right(), visiting);
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved != nullptr &&
        resolved->borrowOrigin == BorrowOriginKind::Receiver &&
        exprAs<Get>(call->callee().get()) == nullptr) {
      return currentClass.has_value();
    }
  }
  if (const ExprPtr *source = storedBorrowSource(expression)) {
    return isReceiverDerivedBorrow(*source, visiting);
  }
  return false;
}

auto SemanticVisitor::Impl::hasStableBorrowStorage(
    const ExprPtr &expression) const -> bool {
  std::unordered_set<const Expr *> visiting;
  return hasStableBorrowStorage(expression, visiting);
}

auto SemanticVisitor::Impl::hasStableBorrowStorage(
    const ExprPtr &expression, std::unordered_set<const Expr *> &visiting) const
    -> bool {
  if (!expression || !visiting.insert(expression.get()).second) {
    return false;
  }
  if (exprAs<Variable>(expression.get()) != nullptr) {
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      return hasStableBorrowStorage(*source, visiting);
    }
    return true;
  }
  if (exprAs<QualifiedName>(expression.get()) != nullptr ||
      exprAs<This>(expression.get()) != nullptr) {
    return true;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return hasStableBorrowStorage(grouping->expression(), visiting);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return hasStableBorrowStorage(binary->right(), visiting);
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return hasStableBorrowStorage(index->object(), visiting);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return hasStableBorrowStorage(member->object(), visiting);
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    const ExpressionInfo *owner = semanticModel.findExpression(*unary->right());
    return owner != nullptr && owner->category == ValueCategory::Place &&
           hasStableBorrowStorage(unary->right(), visiting);
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved != nullptr &&
        resolved->borrowOrigin == BorrowOriginKind::Global &&
        resolved->borrowPlace && resolved->borrowPlace->valid()) {
      return true;
    }
    if (resolved != nullptr &&
        resolved->borrowOrigin == BorrowOriginKind::Receiver &&
        exprAs<Get>(call->callee().get()) == nullptr) {
      return currentClass.has_value();
    }
  }
  if (const ExprPtr *source = storedBorrowSource(expression)) {
    return hasStableBorrowStorage(*source, visiting);
  }
  return false;
}

auto SemanticVisitor::Impl::makeGenericParameters(
    const std::vector<GenericParameter> &parameters,
    const Token &declarationName, const std::vector<std::string> &fromScope)
    -> std::vector<GenericParameterInfo> {
  std::vector<GenericParameterInfo> result;
  result.reserve(parameters.size());
  std::unordered_set<std::string> names;
  bool sawValueParameter = false;
  for (const GenericParameter &parameter : parameters) {
    if (parameter.name.lexeme == declarationName.lexeme) {
      report(parameter.name, "Generic parameter cannot have the same name "
                             "as its declaration.");
      continue;
    }
    if (!names.insert(parameter.name.lexeme).second) {
      report(parameter.name, std::string("Duplicate generic ") +
                                 (parameter.valueType ? "value" : "type") +
                                 " parameter '" + parameter.name.lexeme + "'.");
      continue;
    }
    const bool valueParameter = parameter.valueType.has_value();
    if (valueParameter) {
      sawValueParameter = true;
      if (parameter.valueType->kind != TokenKind::UINT64) {
        report(*parameter.valueType,
               "Value generic parameters currently require type uint64_t.",
               "GTI-S2026");
      }
    } else if (sawValueParameter) {
      report(parameter.name,
             "Generic type parameters must appear before value "
             "parameters.",
             "GTI-S2026");
    }
    GenericConstraintSet constraints = 0;
    if (parameter.constraint) {
      if (reportCompilerPrivatePath(*parameter.constraint)) {
        constraints = constraintBit(GenericConstraintKind::Invalid);
        const GenericParameterId id = nextGenericParameterId++;
        genericConstraints.insert_or_assign(id, constraints);
        result.push_back(
            GenericParameterInfo{.id = id,
                                 .name = parameter.name,
                                 .pack = parameter.pack.has_value(),
                                 .value = valueParameter,
                                 .constraints = constraints,
                                 .constraintName = parameter.constraint});
        continue;
      }
      if (valueParameter) {
        report(parameter.constraint->last(),
               "Generic constraints can only apply to type parameters.",
               "GTI-S2029");
        constraints = constraintBit(GenericConstraintKind::Invalid);
      } else if (const std::optional<ConceptId> resolved =
                     resolveConceptPath(*parameter.constraint, fromScope)) {
        const RegisteredConcept &conceptInfo = concepts[*resolved - 1];
        const std::vector<std::string> enclosingNamespace = currentNamespace;
        currentNamespace = fromScope;
        recordQualifiedPathUses(*parameter.constraint);
        currentNamespace = enclosingNamespace;
        semanticModel.recordOccurrence(
            {.sourceUnit = currentSourceUnit,
             .span = tokenSpan(parameter.constraint->last()),
             .kind = SemanticOccurrenceKind::Symbol,
             .symbol = conceptInfo.symbol,
             .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
             .name = parameter.constraint->last().lexeme});
        if (conceptInfo.declaration->compilerBinding() &&
            !isPreludeUnit(currentSourceUnit)) {
          report(parameter.constraint->last(),
                 "Compiler capability concept '" + conceptInfo.qualifiedName +
                     "' is private to the standard prelude; compose a "
                     "public concept instead.",
                 "GTI-S2049");
          constraints = constraintBit(GenericConstraintKind::Invalid);
        } else {
          (void)resolveConcept(*resolved, &parameter.constraint->last());
          if (conceptInfo.declaration->typeParameters().size() != 1) {
            report(parameter.constraint->last(),
                   "Inline generic constraints require a unary concept; "
                   "use a trailing requires clause for relational "
                   "concepts.",
                   "GTI-S2029");
            constraints = constraintBit(GenericConstraintKind::Invalid);
          } else if (!conceptInfo.structuralConstraints.empty()) {
            report(parameter.constraint->last(),
                   "Structural concepts must be written in a trailing "
                   "requires clause.",
                   "GTI-S2029");
            constraints = constraintBit(GenericConstraintKind::Invalid);
          } else {
            constraints = conceptInfo.parameterConstraints.empty()
                              ? constraintBit(GenericConstraintKind::Invalid)
                              : conceptInfo.parameterConstraints.front();
          }
        }
      } else {
        const std::optional<ConceptId> global =
            resolveConceptPathGlobally(*parameter.constraint, fromScope);
        if (!(global && *global != 0 && *global <= concepts.size() &&
              reportInvisibleDeclaration(
                  parameter.constraint->last(),
                  pathSpelling(*parameter.constraint),
                  concepts[*global - 1].declaration->name(),
                  concepts[*global - 1].sourceUnit))) {
          report(parameter.constraint->last(),
                 "Unknown generic concept '" +
                     pathSpelling(*parameter.constraint) +
                     "'. Concepts must be declared or made visible through "
                     "the prelude or a direct #include.",
                 "GTI-S2029");
        }
        constraints = constraintBit(GenericConstraintKind::Invalid);
      }
    }
    const GenericParameterId id = nextGenericParameterId++;
    genericConstraints.insert_or_assign(id, constraints);
    result.push_back(
        GenericParameterInfo{.id = id,
                             .name = parameter.name,
                             .pack = parameter.pack.has_value(),
                             .value = valueParameter,
                             .constraints = constraints,
                             .constraintName = parameter.constraint});
    const SemanticType parameterType =
        valueParameter ? SemanticType::UInt64 : SemanticType::typeParameter(id);
    const SymbolId symbol = recordToolingSymbol(
        parameter.name,
        valueParameter ? SymbolKind::ValueParameter : SymbolKind::TypeParameter,
        declarationName.lexeme + "::" + parameter.name.lexeme, parameterType);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(parameter.name),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = parameter.name.lexeme,
         .type = parameterType});
  }
  return result;
}

auto SemanticVisitor::Impl::genericParametersFor(const FunctionDecl &function)
    const -> const std::vector<GenericParameterInfo> & {
  static const std::vector<GenericParameterInfo> empty;
  const auto found = functionGenericParameters.find(&function);
  return found == functionGenericParameters.end() ? empty : found->second;
}

auto SemanticVisitor::Impl::beginTypeParameterScope(
    const std::vector<GenericParameterInfo> &parameters) -> void {
  auto &scope = typeParameterScopes.emplace_back();
  auto &values = valueParameterScopes.emplace_back();
  auto &packs = typePackScopes.emplace_back();
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.value) {
      const auto concrete = instanceValueSubstitution.find(parameter.id);
      values.emplace(parameter.name.lexeme,
                     concrete == instanceValueSubstitution.end()
                         ? CompileTimeValue::parameter(parameter.id)
                         : concrete->second);
      continue;
    }
    const auto concrete = instanceTypeSubstitution.find(parameter.id);
    scope.emplace(parameter.name.lexeme,
                  concrete == instanceTypeSubstitution.end()
                      ? SemanticType::typeParameter(parameter.id)
                      : concrete->second);
    if (parameter.pack) {
      packs.insert(parameter.id);
    }
  }
}

auto SemanticVisitor::Impl::endTypeParameterScope() -> void {
  typeParameterScopes.pop_back();
  valueParameterScopes.pop_back();
  typePackScopes.pop_back();
}

auto SemanticVisitor::Impl::beginRequirementScope(const FunctionInfo *function)
    -> bool {
  if (function == nullptr) {
    requirementScopes.emplace_back();
    return true;
  }
  std::vector<AppliedConceptRequirement> requirements = function->requirements;
  for (AppliedConceptRequirement &requirement : requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, instanceTypeSubstitution);
    }
  }
  if (instanceAnalysisActive) {
    if (const std::optional<ConstraintFailure> failure =
            firstRequirementFailure(requirements)) {
      requirementScopes.emplace_back();
      if (function->declaration != nullptr) {
        const Token &site =
            function->declaration->requiresClause()
                ? function->declaration->requiresClause()->keyword
                : function->declaration->name();
        reportConstraintFailure(site, *failure);
      }
      return false;
    }
  }
  requirementScopes.emplace_back(std::move(requirements));
  return true;
}

auto SemanticVisitor::Impl::beginRequirementScope(
    const ConstructorInfo *constructor) -> bool {
  if (constructor == nullptr) {
    requirementScopes.emplace_back();
    return true;
  }
  std::vector<AppliedConceptRequirement> requirements =
      constructor->requirements;
  for (AppliedConceptRequirement &requirement : requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, instanceTypeSubstitution);
    }
  }
  if (instanceAnalysisActive) {
    if (const std::optional<ConstraintFailure> failure =
            firstRequirementFailure(requirements)) {
      requirementScopes.emplace_back();
      if (constructor->declaration != nullptr) {
        const Token &site =
            constructor->declaration->requiresClause()
                ? constructor->declaration->requiresClause()->keyword
                : constructor->declaration->name();
        reportConstraintFailure(site, *failure);
      }
      return false;
    }
  }
  requirementScopes.emplace_back(std::move(requirements));
  return true;
}

auto SemanticVisitor::Impl::endRequirementScope() -> void {
  requirementScopes.pop_back();
}

auto SemanticVisitor::Impl::resolveTypeParameter(const NamePath &name) const
    -> std::optional<SemanticType> {
  if (name.segments.size() != 1) {
    return std::nullopt;
  }
  for (auto scope = typeParameterScopes.rbegin();
       scope != typeParameterScopes.rend(); ++scope) {
    if (const auto found = scope->find(name.last().lexeme);
        found != scope->end()) {
      return found->second;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::resolveValueParameter(const Token &name) const
    -> std::optional<CompileTimeValue> {
  for (auto scope = valueParameterScopes.rbegin();
       scope != valueParameterScopes.rend(); ++scope) {
    if (const auto found = scope->find(name.lexeme); found != scope->end()) {
      return found->second;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::substituteType(
    const SemanticType &type, const GenericSubstitution &substitution) const
    -> SemanticType {
  if (type.kind == SemanticType::TypeParameter) {
    const auto found = substitution.types.find(type.genericParameterId);
    return found == substitution.types.end()
               ? type
               : selectExactClassSpecialization(found->second);
  }

  SemanticType result = type;
  for (SemanticType &argument : result.arguments) {
    argument = substituteType(argument, substitution);
  }
  for (SemanticType &argument : result.lambdaEnclosingClassTypes) {
    argument = substituteType(argument, substitution);
  }
  for (SemanticType &argument : result.lambdaEnclosingFunctionTypes) {
    argument = substituteType(argument, substitution);
  }
  for (CompileTimeValue &argument : result.valueArguments) {
    if (argument.kind != CompileTimeValue::Parameter) {
      continue;
    }
    const auto found = substitution.values.find(argument.parameterId);
    if (found != substitution.values.end()) {
      argument = found->second;
    }
  }
  const auto substituteValues = [&](std::vector<CompileTimeValue> &values) {
    for (CompileTimeValue &argument : values) {
      if (argument.kind != CompileTimeValue::Parameter) {
        continue;
      }
      const auto found = substitution.values.find(argument.parameterId);
      if (found != substitution.values.end()) {
        argument = found->second;
      }
    }
  };
  substituteValues(result.lambdaEnclosingClassValues);
  substituteValues(result.lambdaEnclosingFunctionValues);
  if (result.arrayLengthParameterId != 0) {
    const auto found = substitution.values.find(result.arrayLengthParameterId);
    if (found != substitution.values.end() &&
        found->second.kind == CompileTimeValue::UInt64) {
      result.arrayLength = found->second.value;
      result.arrayLengthParameterId = 0;
    } else if (found != substitution.values.end() &&
               found->second.kind == CompileTimeValue::Parameter) {
      result.arrayLengthParameterId = found->second.parameterId;
    }
  }
  return selectExactClassSpecialization(std::move(result));
}

auto SemanticVisitor::Impl::substituteType(
    const SemanticType &type, const TypeSubstitution &substitution) const
    -> SemanticType {
  return substituteType(type, GenericSubstitution{.types = substitution});
}

auto SemanticVisitor::Impl::classSubstitution(
    const SemanticType &objectType) const -> GenericSubstitution {
  GenericSubstitution result;
  if (objectType.kind != SemanticType::Class || objectType.classId == 0 ||
      objectType.classId > classes.size()) {
    return result;
  }
  const ClassInfo &owner = classInfo(objectType.classId);
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : owner.genericParameters) {
    if (parameter.value) {
      if (valueIndex < objectType.valueArguments.size()) {
        result.values.emplace(parameter.id,
                              objectType.valueArguments[valueIndex++]);
      }
    } else if (typeIndex < objectType.arguments.size()) {
      result.types.emplace(parameter.id, objectType.arguments[typeIndex++]);
    }
  }
  return result;
}

auto SemanticVisitor::Impl::openClassType(ClassId id) const -> SemanticType {
  const ClassInfo &owner = classInfo(id);
  std::vector<SemanticType> arguments;
  std::vector<CompileTimeValue> valueArguments;
  arguments.reserve(genericTypeParameterCount(owner.genericParameters));
  valueArguments.reserve(genericValueParameterCount(owner.genericParameters));
  for (const GenericParameterInfo &parameter : owner.genericParameters) {
    if (parameter.value) {
      const auto concrete = instanceValueSubstitution.find(parameter.id);
      valueArguments.emplace_back(
          concrete == instanceValueSubstitution.end()
              ? CompileTimeValue::parameter(parameter.id)
              : concrete->second);
      continue;
    }
    const auto concrete = instanceTypeSubstitution.find(parameter.id);
    arguments.emplace_back(concrete == instanceTypeSubstitution.end()
                               ? SemanticType::typeParameter(parameter.id)
                               : concrete->second);
  }
  return selectExactClassSpecialization(SemanticType::classType(
      id, std::move(arguments), std::move(valueArguments)));
}

auto SemanticVisitor::Impl::containsOpenTypeParameter(const SemanticType &type)
    -> bool {
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsOpenTypeParameter);
}

auto SemanticVisitor::Impl::containsGenericParameter(
    const SemanticType &type, GenericParameterId parameter, bool value)
    -> bool {
  if (!value && type.kind == SemanticType::TypeParameter &&
      type.genericParameterId == parameter) {
    return true;
  }
  if (value && type.kind == SemanticType::Array &&
      type.arrayLengthParameterId == parameter) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsGenericParameter(argument, parameter,
                                                       value);
                     });
}

auto SemanticVisitor::Impl::resolveRequirements(
    const RequiresClause *clause, const std::vector<std::string> &scope)
    -> std::vector<AppliedConceptRequirement> {
  std::vector<AppliedConceptRequirement> result;
  if (clause == nullptr) {
    return result;
  }

  const std::vector<std::string> enclosingNamespace = currentNamespace;
  currentNamespace = scope;
  for (const ConceptApplication &application : clause->requirements) {
    if (reportCompilerPrivatePath(application.name)) {
      continue;
    }
    recordQualifiedPathUses(application.name);
    const std::optional<ConceptId> resolved =
        resolveConceptPath(application.name, scope);
    if (!resolved) {
      const std::optional<ConceptId> global =
          resolveConceptPathGlobally(application.name, scope);
      if (!(global && *global != 0 && *global <= concepts.size() &&
            reportInvisibleDeclaration(
                application.name.last(), pathSpelling(application.name),
                concepts[*global - 1].declaration->name(),
                concepts[*global - 1].sourceUnit))) {
        report(application.name.last(),
               "Unknown concept '" + pathSpelling(application.name) +
                   "' in requires clause.",
               "GTI-S2049");
      }
      continue;
    }

    RegisteredConcept &conceptInfo = concepts[*resolved - 1];
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(application.name.last()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = conceptInfo.symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
         .name = application.name.last().lexeme});
    (void)resolveConcept(*resolved, &application.name.last());
    if (conceptInfo.resolution != ConceptResolution::Resolved) {
      continue;
    }
    if (application.arguments.size() !=
        conceptInfo.declaration->typeParameters().size()) {
      report(
          application.name.last(),
          "Concept '" + conceptInfo.qualifiedName + "' requires " +
              std::to_string(conceptInfo.declaration->typeParameters().size()) +
              " type argument" +
              (conceptInfo.declaration->typeParameters().size() == 1 ? "."
                                                                     : "s."),
          "GTI-S2049");
      continue;
    }

    AppliedConceptRequirement requirement{.conceptId = *resolved,
                                          .syntax = &application};
    bool valid = true;
    for (const TypeRef &argument : application.arguments) {
      const std::optional<SemanticType> parameter =
          resolveTypeParameter(argument.name);
      const bool plainParameter = parameter && argument.arguments.empty() &&
                                  !argument.pointer && !argument.outerPointer &&
                                  !argument.reference &&
                                  argument.arrayExtents.empty();
      if (plainParameter && isActiveTypePack(argument)) {
        report(argument.name.last(),
               "Requires-clause arguments cannot use an unexpanded type "
               "pack.",
               "GTI-S2049");
        requirement.arguments.emplace_back(SemanticType::Unknown);
        valid = false;
        continue;
      }
      validateType(argument);
      const SemanticType type = typeOf(argument, scope);
      if (type == SemanticType::Unknown) {
        requirement.arguments.emplace_back(type);
        valid = false;
        continue;
      }
      if (!plainParameter && containsOpenTypeParameter(type)) {
        report(argument.name.last(),
               "Requires-clause arguments may be a visible generic type "
               "parameter or a closed exact type; dependent compound "
               "types are not supported.",
               "GTI-S2049");
        requirement.arguments.emplace_back(SemanticType::Unknown);
        valid = false;
        continue;
      }
      requirement.arguments.emplace_back(type);
      recordTypeUse(argument);
    }
    if (valid) {
      result.emplace_back(std::move(requirement));
    }
  }
  currentNamespace = enclosingNamespace;
  return result;
}

auto SemanticVisitor::Impl::analyzeConcreteClassStaticAssertions(
    const StmtList &members) -> void {
  for (const StmtPtr &member : members) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(member.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        analyzeConcreteClassStaticAssertions(*branch);
      }
      continue;
    }
    if (const auto *assertion = stmtAs<StaticAssertDecl>(member.get())) {
      assertion->accept(*this);
    }
  }
}

auto SemanticVisitor::Impl::prepareInstanceAnalysis() -> void {
  diagnostics.clear();
  scopes.clear();
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  requirementScopes.clear();
  currentNamespace.clear();
  currentSourceUnit = 0;
  currentClass.reset();
  instanceTypeSubstitution.clear();
  instanceValueSubstitution.clear();
  instanceClassContextActive = false;
  instanceAnalysisActive = false;
  instanceBaseModel = nullptr;
  currentFunctionDeclaration = nullptr;
  analyzingFieldInitializer = false;
  analyzingConstructorInitializer = false;
  analyzingDefaultArgument = false;
  analyzingStaticAssertCondition = false;
  staticAssertGenericDependency = false;
  currentDefaultArgumentRoot = nullptr;
  currentDefaultParameterNames.clear();
  analyzingCallCallee = false;
  currentStaticMemberFunction = false;
  allowPackTypeReference = false;
  loanFlow = {};
  valueControlFlow.clear();
  semanticModel.clearLoans();
  nextSemanticLoanId = 1;
  nextPlaceSelectionId = 1;
  contextualInitializerType.reset();
  contextualInitializerExpression = nullptr;
  contextualOperatorIntegerType.reset();
  contextualCallableResult.reset();
  contextualNativeOutPointerType.reset();
  currentReceiverMutability = ReceiverMutability::ReadOnly;
  suppressedProjectedPlaceReads.clear();
  constructorDepth = 0;
  destructorDepth = 0;
  functionDepth = 0;
  loopDepth = 0;
  switchDepth = 0;
  lambdaDepth = 0;
  lambdaUncapturedLocals.clear();
  structuredBindingElements.clear();
  currentType = SemanticType::Unknown;
  currentReturnType = SemanticType::Unknown;
}

auto SemanticVisitor::Impl::seedExternalLambdaTypes(
    const std::vector<SemanticType> &types, const SemanticModel &sourceModel)
    -> void {
  const auto seed = [&](const SemanticType &type, const auto &self) -> void {
    if (type.kind == SemanticType::Lambda) {
      const LambdaInfo *lambda = sourceModel.findLambda(type.lambdaId);
      if (lambda != nullptr && lambda->declaration != nullptr) {
        LambdaInfo concrete = *lambda;
        if (const SemanticType *returnType = type.lambdaReturnType();
            returnType != nullptr &&
            type.lambdaParameterTypes().size() ==
                concrete.parameterTypes.size() &&
            type.lambdaCaptureTypes().size() == concrete.captures.size()) {
          concrete.returnType = *returnType;
          concrete.parameterTypes.assign(type.lambdaParameterTypes().begin(),
                                         type.lambdaParameterTypes().end());
          for (std::size_t index = 0; index < concrete.captures.size();
               ++index) {
            concrete.captures[index].type = type.lambdaCaptureTypes()[index];
            concrete.captures[index].traits =
                typeTraits(concrete.captures[index].type);
          }
          concrete.traits = lambdaTraitsForCaptures(type.lambdaCaptureTypes());
        }
        semanticModel.record(*concrete.declaration, std::move(concrete));
      }
    }
    for (const SemanticType &argument : type.arguments) {
      self(argument, self);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      self(argument, self);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      self(argument, self);
    }
  };
  for (const SemanticType &type : types) {
    seed(type, seed);
  }
}

auto SemanticVisitor::Impl::prepareClassInstanceContext(
    ClassId classId, const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments) -> bool {
  if (classId == 0 || classId > classes.size()) {
    return false;
  }
  const ClassInfo &owner = classInfo(classId);
  if (typeArguments.size() !=
          genericTypeParameterCount(owner.genericParameters) ||
      valueArguments.size() !=
          genericValueParameterCount(owner.genericParameters)) {
    return false;
  }
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : owner.genericParameters) {
    if (parameter.value) {
      instanceValueSubstitution.insert_or_assign(parameter.id,
                                                 valueArguments[valueIndex++]);
    } else {
      instanceTypeSubstitution.insert_or_assign(parameter.id,
                                                typeArguments[typeIndex++]);
    }
  }
  currentClass = classId;
  currentNamespace = owner.namespaceScope;
  currentSourceUnit = owner.sourceUnit;
  beginTypeParameterScope(owner.genericParameters);
  beginScope();
  const SemanticType concreteClass =
      SemanticType::classType(classId, typeArguments, valueArguments);
  for (const auto &[name, member] : owner.members) {
    scopes.back().emplace(name, substituteSymbol(member.symbol, concreteClass));
  }
  instanceClassContextActive = true;
  return true;
}

auto SemanticVisitor::Impl::prepareInstanceContext(
    const FunctionInfo &function,
    const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &functionTypeArguments,
    const std::vector<CompileTimeValue> &functionValueArguments) -> bool {
  if (function.ownerClass != 0 &&
      !prepareClassInstanceContext(function.ownerClass, classTypeArguments,
                                   classValueArguments)) {
    return false;
  }
  if (function.ownerClass == 0) {
    currentNamespace = function.namespaceScope;
    currentSourceUnit = function.sourceUnit;
  }

  const std::size_t fixedTypeCount =
      function.parameterPack && !function.genericParameters.empty()
          ? genericTypeParameterCount(function.genericParameters) - 1
          : genericTypeParameterCount(function.genericParameters);
  if (functionTypeArguments.size() < fixedTypeCount ||
      (!function.parameterPack &&
       functionTypeArguments.size() != fixedTypeCount) ||
      functionValueArguments.size() !=
          genericValueParameterCount(function.genericParameters)) {
    return false;
  }
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : function.genericParameters) {
    if (parameter.pack) {
      continue;
    }
    if (parameter.value) {
      instanceValueSubstitution.insert_or_assign(
          parameter.id, functionValueArguments[valueIndex++]);
    } else {
      instanceTypeSubstitution.insert_or_assign(
          parameter.id, functionTypeArguments[typeIndex++]);
    }
  }
  if (function.parameterPack && !function.genericParameters.empty()) {
    const GenericParameterInfo &pack = function.genericParameters.back();
    std::vector<SemanticType> elements(
        functionTypeArguments.begin() +
            static_cast<std::ptrdiff_t>(fixedTypeCount),
        functionTypeArguments.end());
    instanceTypeSubstitution.insert_or_assign(
        pack.id, SemanticType::concreteTypePack(pack.id, std::move(elements)));
  }
  return true;
}

auto SemanticVisitor::Impl::finishClassInstanceContext() -> void {
  if (!instanceClassContextActive) {
    return;
  }
  endScope();
  endTypeParameterScope();
  instanceClassContextActive = false;
  currentClass.reset();
}

auto SemanticVisitor::Impl::finishInstanceContext(const FunctionInfo &function)
    -> void {
  if (function.ownerClass != 0) {
    finishClassInstanceContext();
  }
}

auto SemanticVisitor::Impl::substituteSymbol(
    const Symbol &symbol, const SemanticType &objectType) const -> Symbol {
  Symbol result = symbol;
  const GenericSubstitution substitution = classSubstitution(objectType);
  result.type = substituteType(result.type, substitution);
  for (CallableCandidate &overload : result.overloads) {
    overload.returnType = substituteType(overload.returnType, substitution);
    for (SemanticType &parameter : overload.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
    for (AppliedConceptRequirement &requirement : overload.requirements) {
      for (SemanticType &argument : requirement.arguments) {
        argument = substituteType(argument, substitution);
      }
    }
    overload.compilerPrivate =
        overload.compilerPrivate ||
        semanticModel.isCompilerPrivateType(overload.returnType) ||
        std::any_of(overload.parameterTypes.begin(),
                    overload.parameterTypes.end(),
                    [this](const SemanticType &type) {
                      return semanticModel.isCompilerPrivateType(type);
                    });
    overload.dispatchOwner =
        substituteType(overload.dispatchOwner, substitution);
  }
  if (result.type == SemanticType::Function) {
    result.compilerPrivate =
        !result.overloads.empty() &&
        std::all_of(result.overloads.begin(), result.overloads.end(),
                    [](const CallableCandidate &overload) {
                      return overload.compilerPrivate;
                    });
  } else {
    result.compilerPrivate = result.compilerPrivate ||
                             semanticModel.isCompilerPrivateType(result.type);
  }
  return result;
}

auto SemanticVisitor::Impl::functionSymbol(
    const FunctionDecl &function, const std::vector<std::string> &scope)
    -> Symbol {
  const std::vector<GenericParameterInfo> &genericParameters =
      genericParametersFor(function);
  CallableDeclaration *registered = catalog.mutableCallable(function);
  beginTypeParameterScope(genericParameters);
  CallableCandidate candidate{
      .declarationRecord = registered,
      .declaration = &function,
      .returnType =
          typeOf(function.returnType(), function.returnMutability(), scope),
      .requiredParameterCount = requiredParameterCount(function.parameters()),
      .requirements = resolveRequirements(
          function.requiresClause() ? &*function.requiresClause() : nullptr,
          scope),
      .parameterPack = !function.parameters().empty() &&
                       function.parameters().back().pack.has_value(),
      .receiverMutability = function.receiverMutability()};
  if (registered != nullptr) {
    candidate.id = registered->id;
    candidate.staticMember = registered->staticMember;
    candidate.compilerPrivate = registered->compilerPrivate;
  }
  candidate.parameterTypes.reserve(function.parameters().size());
  for (const Parameter &parameter : function.parameters()) {
    candidate.parameterTypes.emplace_back(typeOf(parameter, scope));
  }
  candidate.compilerPrivate =
      candidate.compilerPrivate ||
      semanticModel.isCompilerPrivateType(candidate.returnType) ||
      std::any_of(candidate.parameterTypes.begin(),
                  candidate.parameterTypes.end(),
                  [this](const SemanticType &type) {
                    return semanticModel.isCompilerPrivateType(type);
                  });
  applyFunctionReturnBorrowSummary(candidate);
  if (registered != nullptr) {
    registered->returnType = candidate.returnType;
    registered->parameterTypes = candidate.parameterTypes;
    registered->requirements = candidate.requirements;
    registered->compilerPrivate = candidate.compilerPrivate;
    registered->returnBorrowOrigin = candidate.returnBorrowOrigin;
    registered->returnBorrowParameter = candidate.returnBorrowParameter;
    registered->returnBorrowAccess = candidate.returnBorrowAccess;
    registered->returnBorrowPlace = candidate.returnBorrowPlace;
  }
  const bool compilerPrivate = candidate.compilerPrivate;
  Symbol symbol{
      .type = SemanticType::Function,
      .sourceUnit = currentSourceUnit,
      .assignable = false,
      .overloads = {std::move(candidate)},
      .declaration = function.name(),
      .staticMember = registered != nullptr && registered->staticMember,
      .internalLinkage = registered != nullptr && registered->internalLinkage,
      .compilerPrivate = compilerPrivate};
  endTypeParameterScope();
  return symbol;
}

auto SemanticVisitor::Impl::validateFunctionPacks(
    const FunctionDecl &function,
    const std::vector<GenericParameterInfo> &genericParameters) -> void {
  const GenericParameterInfo *pack = nullptr;
  const bool hasValueParameter = std::any_of(
      genericParameters.begin(), genericParameters.end(),
      [](const GenericParameterInfo &parameter) { return parameter.value; });
  for (std::size_t index = 0; index < genericParameters.size(); ++index) {
    if (!genericParameters[index].pack) {
      continue;
    }
    if (pack != nullptr) {
      report(genericParameters[index].name,
             "A function may declare only one generic type pack.", "GTI-S2023");
    }
    pack = &genericParameters[index];
    if (index + 1 != genericParameters.size()) {
      report(genericParameters[index].name,
             "A generic type pack must be the final generic parameter.",
             "GTI-S2023");
    }
    if (hasValueParameter) {
      report(genericParameters[index].name,
             "Function value generics cannot be combined with a type pack "
             "in the bounded fixed-array inference model.",
             "GTI-S2026");
    }
  }

  const auto containsPack = [&](const SemanticType &type,
                                const auto &self) -> bool {
    if (pack != nullptr &&
        (type.kind == SemanticType::TypeParameter ||
         type.kind == SemanticType::TypePack) &&
        type.genericParameterId == pack->id) {
      return true;
    }
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const SemanticType &argument) { return self(argument, self); });
  };

  if (containsPack(typeOf(function.returnType()), containsPack)) {
    report(function.returnType().name.last(),
           "A generic type pack cannot be used as a function return type.",
           "GTI-S2023");
  }

  const Parameter *declaredPack = nullptr;
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    const Parameter &parameter = function.parameters()[index];
    const SemanticType parameterType = typeOf(parameter);
    if (!parameter.pack) {
      if (containsPack(parameterType, containsPack)) {
        report(parameter.type.name.last(),
               "A generic type pack must be expanded as a function "
               "parameter with '...'.",
               "GTI-S2023");
      }
      continue;
    }

    if (declaredPack != nullptr) {
      report(*parameter.pack, "A function may declare only one parameter pack.",
             "GTI-S2023");
    }
    declaredPack = &parameter;
    if (index + 1 != function.parameters().size()) {
      report(*parameter.pack,
             "A parameter pack must be the final function parameter.",
             "GTI-S2023");
    }
    if (parameter.name.lexeme.empty()) {
      report(*parameter.pack, "A parameter pack requires a name.", "GTI-S2023");
    }
    if (parameter.mutability == Mutability::Mutable) {
      report(*parameter.pack,
             "Parameter packs are immutable; move individual values only "
             "after a future pack iteration feature exists.",
             "GTI-S2023");
    }
    if (parameter.type.reference || !parameter.type.arrayExtents.empty()) {
      report(*parameter.pack,
             "The first variadic layer supports only by-value parameter "
             "packs.",
             "GTI-S2023");
    }
    if (pack == nullptr ||
        (parameterType.kind != SemanticType::TypeParameter &&
         parameterType.kind != SemanticType::TypePack) ||
        parameterType.genericParameterId != pack->id) {
      report(*parameter.pack,
             "A parameter pack type must name the function's generic type "
             "pack.",
             "GTI-S2023");
    }
  }

  if (pack != nullptr && declaredPack == nullptr) {
    report(pack->name,
           "A generic type pack must be bound by a final parameter pack.",
           "GTI-S2023");
  } else if (pack == nullptr && declaredPack != nullptr) {
    report(*declaredPack->pack,
           "A parameter pack requires a matching final generic type pack.",
           "GTI-S2023");
  }
}

auto SemanticVisitor::Impl::validateOperatorDeclaration(
    const FunctionDecl &function, bool methodDeclaration) -> void {
  if (!function.operatorName()) {
    return;
  }

  const OperatorName &name = *function.operatorName();
  const auto fail = [&](std::string message) {
    report(name.keyword, std::move(message), "GTI-S2022");
  };
  if (!methodDeclaration) {
    fail("Operator overloads can only be declared as class or struct "
         "members.");
    return;
  }
  if (function.isStatic()) {
    fail("Operator overloads require an object receiver and cannot be "
         "static.");
  }
  if (!function.genericParameters().empty()) {
    fail("Operator overloads cannot declare method type parameters.");
  }
  if (function.runtimeBinding()) {
    fail("Operator overloads cannot be runtime bindings.");
  }

  const bool comparisonOperator = name.kind == OverloadedOperator::Equal ||
                                  name.kind == OverloadedOperator::NotEqual ||
                                  name.kind == OverloadedOperator::Less ||
                                  name.kind == OverloadedOperator::LessEqual ||
                                  name.kind == OverloadedOperator::Greater ||
                                  name.kind == OverloadedOperator::GreaterEqual;
  const bool valueOperator = isArithmeticOrBitwiseValueOperator(name.kind);
  const bool mutationOperator = isCompoundOperator(name.kind) ||
                                name.kind == OverloadedOperator::PreDecrement;
  std::optional<std::size_t> expectedArity = 0;
  switch (name.kind) {
  case OverloadedOperator::Assignment:
  case OverloadedOperator::Multiply:
  case OverloadedOperator::Divide:
  case OverloadedOperator::Remainder:
  case OverloadedOperator::AddAssign:
  case OverloadedOperator::SubtractAssign:
  case OverloadedOperator::MultiplyAssign:
  case OverloadedOperator::DivideAssign:
  case OverloadedOperator::RemainderAssign:
  case OverloadedOperator::BitwiseAnd:
  case OverloadedOperator::BitwiseOr:
  case OverloadedOperator::BitwiseXor:
  case OverloadedOperator::ShiftLeft:
  case OverloadedOperator::ShiftRight:
  case OverloadedOperator::BitwiseAndAssign:
  case OverloadedOperator::BitwiseOrAssign:
  case OverloadedOperator::BitwiseXorAssign:
  case OverloadedOperator::ShiftLeftAssign:
  case OverloadedOperator::ShiftRightAssign:
  case OverloadedOperator::Subscript:
  case OverloadedOperator::Equal:
  case OverloadedOperator::NotEqual:
  case OverloadedOperator::Less:
  case OverloadedOperator::LessEqual:
  case OverloadedOperator::Greater:
  case OverloadedOperator::GreaterEqual:
    expectedArity = 1;
    break;
  case OverloadedOperator::Call:
    expectedArity = std::nullopt;
    break;
  case OverloadedOperator::Plus:
  case OverloadedOperator::Minus:
    if (function.parameters().size() > 1) {
      fail(std::string(operatorSourceSpelling(name.kind)) +
           " expects zero parameters for its unary form or one parameter "
           "for its binary form.");
    }
    expectedArity = std::nullopt;
    break;
  case OverloadedOperator::Dereference:
  case OverloadedOperator::Arrow:
  case OverloadedOperator::PreIncrement:
  case OverloadedOperator::PreDecrement:
  case OverloadedOperator::BitwiseNot:
  case OverloadedOperator::ContextualBool:
    break;
  }
  if (expectedArity && function.parameters().size() != *expectedArity) {
    fail(std::string(operatorSourceSpelling(name.kind)) + " expects " +
         std::to_string(*expectedArity) + " parameter" +
         (*expectedArity == 1 ? "." : "s."));
  }

  const SemanticType returnType =
      typeOf(function.returnType(), function.returnMutability());
  if (function.receiverMutability() == ReceiverMutability::Consuming) {
    if (name.kind != OverloadedOperator::Call) {
      fail("A consuming receiver is supported only by operator().");
    }
    if (function.isVirtual() || function.isOverride() || function.isPure() ||
        (currentClass &&
         classInfo(*currentClass).kind == ClassKind::Interface)) {
      fail("operator() && cannot be virtual, pure, overriding, or an "
           "interface requirement in the bounded consuming-call model.");
    }
    if (returnType.kind == SemanticType::Reference ||
        typeTraits(returnType).containsBorrowedState) {
      fail("operator() && cannot return borrowed state from its consumed "
           "receiver.");
    }
  }
  if ((comparisonOperator || name.kind == OverloadedOperator::ContextualBool) &&
      returnType != SemanticType::Bool) {
    fail(std::string(operatorSourceSpelling(name.kind)) + " must return bool.");
  }
  if ((name.kind == OverloadedOperator::Dereference ||
       name.kind == OverloadedOperator::Arrow) &&
      returnType.kind != SemanticType::Reference) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must return a checked reference.");
  }
  if (name.kind == OverloadedOperator::Subscript &&
      returnType == SemanticType::Void) {
    fail("operator[] must return a value or checked reference.");
  }
  if (name.kind == OverloadedOperator::PreIncrement &&
      returnType != SemanticType::Void) {
    fail("Prefix operator++ must return void in GTI.");
  }
  if (name.kind == OverloadedOperator::PreIncrement &&
      function.receiverMutability() != ReceiverMutability::Mutable) {
    fail("Prefix operator++ must use a mutable receiver.");
  }
  if (valueOperator && (returnType == SemanticType::Void ||
                        returnType.kind == SemanticType::Reference ||
                        typeTraits(returnType).containsBorrowedState)) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must return an owned value, not void, a reference, or "
         "receiver-tied borrowed state.");
  }
  if (valueOperator &&
      function.receiverMutability() != ReceiverMutability::ReadOnly) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must use a read-only receiver because it produces a value.");
  }
  if (mutationOperator && returnType != SemanticType::Void) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must return void in GTI.");
  }
  if (mutationOperator &&
      function.receiverMutability() != ReceiverMutability::Mutable) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must use a mutable receiver with trailing 'mut'.");
  }
  if (name.kind == OverloadedOperator::Assignment &&
      function.receiverMutability() != ReceiverMutability::Mutable) {
    fail("operator= must use a mutable receiver.");
  }
  if ((comparisonOperator || name.kind == OverloadedOperator::ContextualBool) &&
      function.receiverMutability() != ReceiverMutability::ReadOnly) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must use a read-only receiver.");
  }
}

auto SemanticVisitor::Impl::validateRuntimeBinding(const FunctionDecl &function)
    -> void {
  if (!function.runtimeBinding()) {
    return;
  }

  const RuntimeBinding &binding = *function.runtimeBinding();
  if (!isPreludeUnit(currentSourceUnit)) {
    report(binding.attribute,
           "@runtime is reserved for compiler-owned declarations in the "
           "trusted prelude.",
           "GTI-S2058");
    return;
  }
  const std::string functionName =
      qualifiedName(currentNamespace, function.name().lexeme);
  const auto hasParameters =
      [&](std::initializer_list<SemanticType::Kind> expected) {
        if (function.parameters().size() != expected.size()) {
          return false;
        }
        std::size_t index = 0;
        for (const SemanticType::Kind kind : expected) {
          const Parameter &parameter = function.parameters()[index++];
          if (typeOf(parameter.type).kind != kind ||
              parameter.mutability != Mutability::Immutable) {
            return false;
          }
        }
        return true;
      };
  const bool common = function.genericParameters().empty() && !function.body();
  const bool valid =
      common && ((binding.name == "stdout.write" &&
                  functionName == "gti_internal::runtime::write_stdout" &&
                  typeOf(function.returnType()) == SemanticType::Void &&
                  hasParameters({SemanticType::StringView})) ||
                 (binding.name == "stdin.read_byte" &&
                  functionName == "gti_internal::runtime::read_stdin_byte" &&
                  typeOf(function.returnType()) == SemanticType::Int32 &&
                  hasParameters({})) ||
                 (binding.name == "file.open_read" &&
                  functionName == "gti_internal::runtime::open_file_read" &&
                  typeOf(function.returnType()) == SemanticType::Int64 &&
                  hasParameters({SemanticType::StringView})) ||
                 (binding.name == "file.read_byte" &&
                  functionName == "gti_internal::runtime::read_file_byte" &&
                  typeOf(function.returnType()) == SemanticType::Int32 &&
                  hasParameters({SemanticType::Int64})) ||
                 (binding.name == "file.close" &&
                  functionName == "gti_internal::runtime::close_file" &&
                  typeOf(function.returnType()) == SemanticType::Int32 &&
                  hasParameters({SemanticType::Int64})));
  if (!valid) {
    report(binding.attribute,
           "Invalid declaration for runtime binding '" + binding.name + "'.");
  }
}

auto SemanticVisitor::Impl::isCAbiScalar(const SemanticType &type) -> bool {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
    return true;
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::isCAbiRecordType(const SemanticType &type) const
    -> bool {
  if (type.kind != SemanticType::Class) {
    return false;
  }
  const ClassTypeInfo *record = semanticModel.findClassType(type.classId);
  return record != nullptr && record->cAbiRecord && record->cAbiLayout;
}

auto SemanticVisitor::Impl::isCOpaqueHandleType(const SemanticType &type) const
    -> bool {
  if (type.kind != SemanticType::Class) {
    return false;
  }
  const ClassTypeInfo *handle = semanticModel.findClassType(type.classId);
  return handle != nullptr && handle->cOpaqueHandle;
}

auto SemanticVisitor::Impl::isCAbiRawPointer(const SemanticType &type) const
    -> bool {
  return type.kind == SemanticType::RawPointer && type.arguments.size() == 1 &&
         (type.arguments.front() == SemanticType::Void ||
          isCAbiScalar(type.arguments.front()) ||
          type.arguments.front() == SemanticType::CString ||
          isCAbiRecordType(type.arguments.front()) ||
          isCOpaqueHandleType(type.arguments.front()));
}

auto SemanticVisitor::Impl::isCAbiOpaqueHandleOutPointer(
    const SemanticType &type) const -> bool {
  if (type.kind != SemanticType::RawPointer ||
      type.pointerAccess != AccessMode::Mutable || type.arguments.size() != 1) {
    return false;
  }
  const SemanticType &inner = type.arguments.front();
  return inner.kind == SemanticType::RawPointer &&
         inner.pointerAccess == AccessMode::Mutable &&
         inner.arguments.size() == 1 &&
         isCOpaqueHandleType(inner.arguments.front());
}

auto SemanticVisitor::Impl::nativeFunctionBoundaryTypeAllowed(
    const SemanticType &type, bool returnPosition) const -> bool {
  if (returnPosition && type == SemanticType::Void) {
    return true;
  }
  if (isCAbiScalar(type) || type == SemanticType::CString ||
      type == SemanticType::StringView) {
    return true;
  }
  if (type.kind == SemanticType::NativeFunction) {
    if (!type.hasNativeFunctionShape()) {
      return false;
    }
    const SemanticType *returnType = type.nativeFunctionReturnType();
    return returnType != nullptr &&
           nativeFunctionBoundaryTypeAllowed(*returnType, true) &&
           std::all_of(type.nativeFunctionParameterTypes().begin(),
                       type.nativeFunctionParameterTypes().end(),
                       [this](const SemanticType &parameter) {
                         return nativeFunctionBoundaryTypeAllowed(parameter,
                                                                  false);
                       });
  }
  if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1) {
    const SemanticType &pointee = type.arguments.front();
    if (pointee == SemanticType::Void || isCAbiScalar(pointee) ||
        pointee == SemanticType::CString) {
      return true;
    }
    if (pointee.kind != SemanticType::Class || pointee.classId == 0 ||
        pointee.classId > classes.size()) {
      return false;
    }
    const ClassInfo &owner = classInfo(pointee.classId);
    return owner.cAbiRecord || owner.cOpaqueHandle;
  }
  return type.kind == SemanticType::Class && type.classId != 0 &&
         type.classId <= classes.size() && classInfo(type.classId).cAbiRecord;
}

auto SemanticVisitor::Impl::isCAbiNativeFunction(const SemanticType &type) const
    -> bool {
  if (!type.hasNativeFunctionShape()) {
    return false;
  }
  const SemanticType *returnType = type.nativeFunctionReturnType();
  return returnType != nullptr &&
         nativeFunctionBoundaryTypeAllowed(*returnType, true) &&
         std::all_of(type.nativeFunctionParameterTypes().begin(),
                     type.nativeFunctionParameterTypes().end(),
                     [this](const SemanticType &parameter) {
                       return nativeFunctionBoundaryTypeAllowed(parameter,
                                                                false);
                     });
}

auto SemanticVisitor::Impl::isCAbiArrayReturnPointer(
    const SemanticType &type) const -> bool {
  if (isCAbiRawPointer(type)) {
    return true;
  }
  if (type.kind != SemanticType::RawPointer || type.arguments.size() != 1) {
    return false;
  }
  const SemanticType &inner = type.arguments.front();
  return inner.kind == SemanticType::RawPointer &&
         inner.arguments.size() == 1 &&
         inner.arguments.front() != SemanticType::CString &&
         isCAbiRawPointer(inner);
}

auto SemanticVisitor::Impl::validateNativeCArrayAttribute(
    const FunctionDecl &function, const SemanticType &returnType) -> void {
  if (!function.nativeCArray()) {
    return;
  }
  const NativeCArrayAttribute &attribute = *function.nativeCArray();
  const auto fail = [&](const Token &location, std::string message) {
    report(location, std::move(message), "GTI-S2073");
  };
  if (!function.hasCLinkage()) {
    fail(attribute.attribute,
         "[[c_array(count)]] is valid only on an extern \"C\" function "
         "declaration.");
  }
  if (!isCAbiArrayReturnPointer(returnType)) {
    fail(attribute.attribute,
         "[[c_array(count)]] requires a returned C ABI pointer or the "
         "bounded two-level pointer form.");
  }

  const FunctionInfo *info = semanticModel.findFunction(function);
  if (info == nullptr || !info->cArrayCountParameter ||
      *info->cArrayCountParameter >= function.parameters().size()) {
    fail(attribute.countParameter,
         "[[c_array(" + attribute.countParameter.lexeme +
             ")]] does not name a parameter of this function.");
    return;
  }
  const std::size_t index = *info->cArrayCountParameter;
  const Parameter &count = function.parameters()[index];
  const SemanticType countType = typeOf(count);
  const bool validCount =
      count.mutability == Mutability::Immutable && !count.pack &&
      !count.type.reference && count.type.arrayExtents.empty() &&
      !count.type.outerPointer && countType.kind == SemanticType::RawPointer &&
      countType.pointerAccess == AccessMode::Mutable &&
      countType.arguments.size() == 1 && isInteger(countType.arguments.front());
  if (!validCount) {
    fail(attribute.countParameter,
         "[[c_array(count)]] must name an immutable one-level pointer to a "
         "mutable fixed-width integer count.");
    return;
  }

  if (const BindingInfo *binding = semanticModel.findBinding(count);
      binding != nullptr && binding->symbol != 0) {
    semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                    .span = tokenSpan(attribute.countParameter),
                                    .kind = SemanticOccurrenceKind::Symbol,
                                    .symbol = binding->symbol,
                                    .roles = OccurrenceRole::Reference,
                                    .name = attribute.countParameter.lexeme,
                                    .type = countType});
  }
}

auto SemanticVisitor::Impl::validateExternCFunction(
    const FunctionDecl &function, bool methodDeclaration,
    const SemanticType &returnType) -> void {
  if (!function.hasCLinkage()) {
    return;
  }

  const auto fail = [&](const Token &location, std::string message) {
    report(location, std::move(message), "GTI-S2054");
  };
  (void)validateNativeCIdentifier(function.name(), "GTI-S2054",
                                  "extern C symbol", true, true, true);
  if (methodDeclaration || currentClass) {
    fail(function.name(),
         "extern \"C\" declarations must be namespace-scope free "
         "functions.");
  }
  if (function.name().lexeme == "main") {
    fail(function.name(),
         "The C symbol 'main' is reserved for the GTI entry point.");
  }
  if (function.body() || function.isPure()) {
    fail(function.name(),
         "An extern \"C\" function must be a bodyless declaration.");
  }
  if (!function.genericParameters().empty()) {
    fail(function.name(),
         "extern \"C\" functions cannot declare generic parameters.");
  }
  if (function.runtimeBinding()) {
    fail(function.name(),
         "extern \"C\" declarations cannot also be runtime bindings.");
  }
  if (function.isStatic() || function.isVirtual() || function.isOverride() ||
      function.operatorName() ||
      function.receiverMutability() == ReceiverMutability::Mutable) {
    fail(function.name(),
         "extern \"C\" functions cannot use static, virtual, override, "
         "operator, or receiver qualifiers.");
  }
  if (function.returnMutability() == Mutability::Mutable ||
      function.returnType().reference ||
      (returnType != SemanticType::Void &&
       returnType != SemanticType::CString && !isCAbiScalar(returnType) &&
       !isCAbiRawPointer(returnType) && !isCAbiArrayReturnPointer(returnType) &&
       !isCAbiRecordType(returnType) && !isCAbiNativeFunction(returnType))) {
    fail(function.returnType().name.last(),
         "extern \"C\" return types are limited to void, fixed-width "
         "integer or float scalars, c_string, valid [[c_abi]] records, and "
         "one-level raw pointers to those types, opaque C handles, void, "
         "or an exact named native function type.");
  }

  for (const Parameter &parameter : function.parameters()) {
    if (!parameter.name.lexeme.empty()) {
      (void)validateNativeCIdentifier(parameter.name, "GTI-S2054",
                                      "extern C parameter name", true, false);
    }
    const SemanticType parameterType = typeOf(parameter);
    if (parameter.mutability == Mutability::Mutable || parameter.pack ||
        parameter.type.reference || !parameter.type.arrayExtents.empty() ||
        (!isCAbiScalar(parameterType) &&
         parameterType != SemanticType::StringView &&
         parameterType != SemanticType::CString &&
         !isCAbiRawPointer(parameterType) &&
         !isCAbiOpaqueHandleOutPointer(parameterType) &&
         !isCAbiRecordType(parameterType) &&
         !isCAbiNativeFunction(parameterType))) {
      const Token &location = parameter.name.lexeme.empty()
                                  ? parameter.type.name.last()
                                  : parameter.name;
      fail(location,
           "extern \"C\" parameters are limited to immutable by-value "
           "fixed-width scalars, valid [[c_abi]] records, std::string_view "
           "counted buffers, c_string NUL-terminated pointers, and "
           "one-level raw pointers to those record or scalar types, opaque "
           "C handles, void, exact opaque-handle out pointers, or exact "
           "named native function types.");
    }
  }

  const auto [previous, inserted] =
      externCSymbols.emplace(function.name().lexeme, &function);
  if (!inserted && previous->second != &function) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2054", DiagnosticPhase::Semantics, function.name(),
        "C symbol '" + function.name().lexeme +
            "' is declared more than once; extern \"C\" functions "
            "cannot be overloaded or redeclared.");
    diagnostic.related.push_back({tokenSpan(previous->second->name()),
                                  "Previous C-linkage declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }

  if (const auto storage =
          rootNativeStorageSymbols.find(function.name().lexeme);
      storage != rootNativeStorageSymbols.end()) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2054", DiagnosticPhase::Semantics, function.name(),
                       "C symbol '" + function.name().lexeme +
                           "' conflicts with a root namespace GTI variable.");
    diagnostic.related.push_back(
        {tokenSpan(storage->second->name()),
         "Conflicting GTI variable is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

} // namespace lang
