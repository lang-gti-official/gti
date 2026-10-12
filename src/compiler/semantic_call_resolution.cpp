#include "semantic_analyzer_impl.h"

namespace lang {

namespace {

enum class SymbolicRangeCarrier : ClassId {
  None = 0,
  ReadIterator = 1,
  MutableIterator = 2,
  Sentinel = 3,
};

[[nodiscard]] SemanticType
symbolicRangeCarrierType(const SemanticType &range,
                         SymbolicRangeCarrier carrier) {
  SemanticType result = SemanticType::typeParameter(range.genericParameterId);
  result.classId = static_cast<ClassId>(carrier);
  result.arguments = {range};
  return result;
}

[[nodiscard]] std::optional<std::pair<SemanticType, SymbolicRangeCarrier>>
symbolicRangeCarrier(const SemanticType &type) {
  if (type.kind != SemanticType::TypeParameter || type.arguments.size() != 1 ||
      type.arguments.front().kind != SemanticType::TypeParameter ||
      type.classId < static_cast<ClassId>(SymbolicRangeCarrier::ReadIterator) ||
      type.classId > static_cast<ClassId>(SymbolicRangeCarrier::Sentinel)) {
    return std::nullopt;
  }
  return std::pair{type.arguments.front(),
                   static_cast<SymbolicRangeCarrier>(type.classId)};
}

} // namespace

auto SemanticVisitor::Impl::validateStorageValueConstruction(
    const Call &expr, const SemanticType &elementType,
    const std::vector<AnalyzedCallArgument> &arguments) -> void {
  if (arguments.empty()) {
    if (!satisfiesConstraint(
            elementType,
            constraintBit(GenericConstraintKind::DefaultInitializable))) {
      report(expr.paren(),
             "Storage element type '" + typeSpelling(elementType) +
                 "' is not default-initializable.",
             "GTI-S2019");
    }
    return;
  }

  bool exact = arguments.size() == 1;
  if (exact) {
    const AnalyzedCallArgument &argument = arguments.front();
    exact = argument.forwardedPackElement
                ? forwardedPackArgumentMatches(elementType, argument.type)
                : argument.expression != nullptr &&
                      callArgumentMatches(elementType, argument.type,
                                          *argument.expression);
  }
  if (exact) {
    return;
  }

  std::string argumentSpelling;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) {
      argumentSpelling += ", ";
    }
    argumentSpelling += typeSpelling(arguments[index].type);
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2019", DiagnosticPhase::Semantics, expr.paren(),
      "Storage construction of '" + typeSpelling(elementType) +
          "' requires zero arguments or one exact value, but received (" +
          argumentSpelling + ").");
  diagnostic.hints.emplace_back(
      "Storage construction does not perform implicit conversions; "
      "construct or convert the value explicitly before passing it.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::uniqueOwnerIntrinsicName(IntrinsicKind intrinsic)
    -> std::string {
  switch (intrinsic) {
  case IntrinsicKind::AllocateUniqueOwner:
    return "allocate_unique_owner";
  case IntrinsicKind::UniqueOwnerBorrow:
    return "unique_owner_borrow";
  case IntrinsicKind::UniqueOwnerBorrowMut:
    return "unique_owner_borrow_mut";
  case IntrinsicKind::UniqueOwnerIsNull:
    return "unique_owner_is_null";
  case IntrinsicKind::UniqueOwnerSwap:
    return "unique_owner_swap";
  case IntrinsicKind::UniqueOwnerUpcast:
    return "unique_owner_upcast";
  default:
    return "unique-owner operation";
  }
}

auto SemanticVisitor::Impl::typeAliasForCallee(const ExprPtr &callee) const
    -> std::optional<ResolvedTypeAlias> {
  const auto substituteCurrentInstance =
      [&](std::optional<ResolvedTypeAlias> alias)
      -> std::optional<ResolvedTypeAlias> {
    if (alias) {
      alias->type = substituteType(
          alias->type,
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
    }
    return alias;
  };
  if (callee && callee->exprKind() == ExprKind::Variable) {
    const auto *variable = static_cast<const Variable *>(callee.get());
    return substituteCurrentInstance(
        resolveTypeAliasPath(NamePath(variable->name()), currentNamespace));
  }
  if (callee && callee->exprKind() == ExprKind::QualifiedName) {
    const auto *qualified = static_cast<const QualifiedName *>(callee.get());
    return substituteCurrentInstance(
        resolveTypeAliasPath(qualified->name(), currentNamespace));
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::directCall(const ExprPtr &expression)
    -> const Call * {
  const Expr *candidate = expression.get();
  while (candidate != nullptr && candidate->exprKind() == ExprKind::Grouping) {
    candidate = static_cast<const Grouping *>(candidate)->expression().get();
  }
  return candidate != nullptr && candidate->exprKind() == ExprKind::Call
             ? static_cast<const Call *>(candidate)
             : nullptr;
}

auto SemanticVisitor::Impl::explicitMoveSource(const ExprPtr &expression) const
    -> const ExprPtr * {
  const Call *call = directCall(expression);
  if (call == nullptr || resolvedIntrinsicKind(*call) != IntrinsicKind::Move ||
      call->arguments().size() != 1) {
    return nullptr;
  }
  return &call->arguments().front();
}

auto SemanticVisitor::Impl::isExplicitMoveReceiver(
    const ExprPtr &expression) const -> bool {
  return explicitMoveSource(expression) != nullptr;
}

auto SemanticVisitor::Impl::directCallableParameterVariable(
    const ExprPtr &expression) const -> const Variable * {
  const ExprPtr *candidate = &expression;
  if (const ExprPtr *moved = explicitMoveSource(expression)) {
    candidate = moved;
  }
  return movedVariable(*candidate);
}

auto SemanticVisitor::Impl::currentFunctionInfo() const
    -> const FunctionInfo * {
  if (currentFunctionDeclaration == nullptr) {
    return nullptr;
  }
  if (const FunctionInfo *function =
          semanticModel.findFunction(*currentFunctionDeclaration)) {
    return function;
  }
  return instanceBaseModel == nullptr
             ? nullptr
             : instanceBaseModel->findFunction(*currentFunctionDeclaration);
}

auto SemanticVisitor::Impl::callableParameterContract(
    const ExprPtr &callee) const -> const CallableParameterContract * {
  const Variable *variable = directCallableParameterVariable(callee);
  const Symbol *symbol =
      variable == nullptr ? nullptr : resolve(variable->name());
  const FunctionInfo *function = currentFunctionInfo();
  if (symbol == nullptr || symbol->parameterDeclaration == nullptr ||
      function == nullptr || function->declaration == nullptr) {
    return nullptr;
  }
  const auto parameter =
      std::find_if(function->declaration->parameters().begin(),
                   function->declaration->parameters().end(),
                   [&](const Parameter &candidate) {
                     return &candidate == symbol->parameterDeclaration;
                   });
  if (parameter == function->declaration->parameters().end()) {
    return nullptr;
  }
  const std::size_t index = static_cast<std::size_t>(
      std::distance(function->declaration->parameters().begin(), parameter));
  const auto contract = std::find_if(
      function->callableParameters.begin(), function->callableParameters.end(),
      [index](const CallableParameterContract &candidate) {
        return candidate.parameterIndex == index;
      });
  return contract == function->callableParameters.end() ? nullptr : &*contract;
}

auto SemanticVisitor::Impl::callableParameterContract(
    FunctionId functionId, std::size_t parameterIndex) const
    -> const CallableParameterContract * {
  const FunctionInfo *function = semanticModel.findFunction(functionId);
  if (function == nullptr && instanceBaseModel != nullptr) {
    function = instanceBaseModel->findFunction(functionId);
  }
  if (function == nullptr) {
    return nullptr;
  }
  if (function->parameterPack && !function->parameterTypes.empty())
    parameterIndex =
        std::min(parameterIndex, function->parameterTypes.size() - 1);
  const auto contract = std::find_if(
      function->callableParameters.begin(), function->callableParameters.end(),
      [parameterIndex](const CallableParameterContract &candidate) {
        return candidate.parameterIndex == parameterIndex;
      });
  return contract == function->callableParameters.end() ? nullptr : &*contract;
}

auto SemanticVisitor::Impl::callableSignatureRequirement(const Call &call) const
    -> std::optional<CallableSignatureRequirement> {
  const CallableParameterContract *contract =
      callableParameterContract(call.callee());
  if (contract == nullptr) {
    if (const DeferredCallableCallInfo *stored =
            semanticModel.findDeferredCallableCall(call);
        stored != nullptr && stored->receiverField != 0) {
      CallableSignatureRequirement result{
          .source = &call,
          .returnType =
              substituteType(stored->returnType, instanceTypeSubstitution),
          .parameterTypes = stored->parameterTypes,
          .capability = stored->capability,
          .inferResultFromInstantiation = stored->inferResultFromInstantiation};
      for (SemanticType &type : result.parameterTypes) {
        type = substituteType(type, instanceTypeSubstitution);
      }
      return result;
    }
    return std::nullopt;
  }
  const auto signature =
      std::find_if(contract->signatures.begin(), contract->signatures.end(),
                   [&](const CallableSignatureRequirement &candidate) {
                     return candidate.source == &call;
                   });
  if (signature == contract->signatures.end()) {
    return std::nullopt;
  }
  CallableSignatureRequirement result = *signature;
  result.returnType =
      substituteType(result.returnType, instanceTypeSubstitution);
  for (SemanticType &parameter : result.parameterTypes) {
    parameter = substituteType(parameter, instanceTypeSubstitution);
  }
  return result;
}

auto SemanticVisitor::Impl::analyzeDeferredCallableCall(
    const Call &call, const SemanticType &calleeType,
    const std::vector<SemanticType> &argumentTypes) -> void {
  const bool consuming = isExplicitMoveReceiver(call.callee());
  const Variable *variable = directCallableParameterVariable(call.callee());
  const Symbol *symbol =
      variable == nullptr ? nullptr : resolve(variable->name());
  const Parameter *parameter =
      symbol == nullptr ? nullptr : symbol->parameterDeclaration;
  const FunctionInfo *function = currentFunctionInfo();
  const ExprPtr *receiver = &call.callee();
  if (const ExprPtr *moved = explicitMoveSource(call.callee())) {
    receiver = moved;
  }
  const SymbolRecord *receiverSymbol = semanticModel.database().findSymbol(
      semanticModel.findResolvedSymbol(**receiver));
  const ExpressionInfo *receiverInfo = semanticModel.findExpression(**receiver);
  const bool storedReceiver =
      receiverSymbol != nullptr && receiverSymbol->kind == SymbolKind::Field &&
      receiverInfo != nullptr &&
      receiverInfo->category == ValueCategory::Place &&
      receiverInfo->type.kind == SemanticType::TypeParameter;
  bool valid = true;
  SemanticType returnType = SemanticType::Void;
  bool inferResultFromInstantiation = false;
  if (contextualCallableResult && contextualCallableResult->call == &call) {
    returnType = contextualCallableResult->type;
    inferResultFromInstantiation =
        contextualCallableResult->inferResultFromInstantiation;
    if (returnType == SemanticType::Unknown && !inferResultFromInstantiation) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2046", DiagnosticPhase::Semantics,
                         expressionToken(call.callee()),
                         std::string("The result type of generic callable ") +
                             (storedReceiver ? "field '" : "parameter '") +
                             expressionToken(call.callee()).lexeme +
                             "' cannot be inferred with 'auto'.");
      diagnostic.hints.emplace_back(
          "Use an explicit result binding, assignment target, condition, or "
          "enclosing return type so the exact callable result is known.");
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    } else if (returnType.kind == SemanticType::Reference ||
               typeTraits(returnType).containsBorrowedState ||
               containsLambdaType(returnType)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics,
          expressionToken(call.callee()),
          std::string(storedReceiver ? "Stored generic callable results"
                                     : "Confined generic callable results") +
              " must be exact non-reference "
              "values without tracked borrowed state or lambda identity, but "
              "this context requires '" +
              typeSpelling(returnType) + "'.");
      diagnostic.hints.emplace_back(
          "Reference, tracked borrowed-state, and lambda results require an "
          "escape-aware ownership contract that is not available yet.");
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    }
  }
  if (!storedReceiver &&
      (variable == nullptr || parameter == nullptr || function == nullptr ||
       function->declaration == nullptr || parameter->pack ||
       calleeType.kind != SemanticType::TypeParameter)) {
    report(call.paren(),
           "Only a direct generic function parameter can be used "
           "as a confined callable.",
           "GTI-S2046");
    valid = false;
  }
  if (!call.typeArguments().empty()) {
    report(call.paren(),
           "Calls through a generic callable parameter do not take explicit "
           "type arguments.",
           "GTI-S2046");
    valid = false;
  }
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    if (argumentTypes[index].kind == SemanticType::Lambda) {
      report(expressionToken(call.arguments()[index]),
             "A confined callable parameter cannot receive another "
             "lambda value yet.",
             "GTI-S2046");
      valid = false;
    }
    // An unresolved callable may take this place by mutable reference. Until
    // an exact declaration supplies its access contract, removing mut is not
    // a declaration-wide semantics-preserving edit.
    if (const auto *info =
            semanticModel.findExpression(*call.arguments()[index]);
        info != nullptr && info->category == ValueCategory::Place &&
        info->access == AccessMode::Mutable) {
      recordMutableBindingUse(call.arguments()[index]);
    }
  }
  if (!valid) {
    currentType = SemanticType::Unknown;
    return;
  }

  if (storedReceiver) {
    semanticModel.recordDeferredCallableCall(
        call,
        DeferredCallableCallInfo{
            .genericParameter = calleeType.genericParameterId,
            .returnType = returnType,
            .parameterTypes = argumentTypes,
            .access = receiverInfo->access,
            .capability =
                consuming ? CallableInvocationCapability::Once
                          : callableInvocationCapability(receiverInfo->access),
            .boundary = std::nullopt,
            .receiverField = receiverSymbol->id,
            .inferResultFromInstantiation = inferResultFromInstantiation});
    currentType = returnType;
    return;
  }

  const auto found = std::find_if(
      function->declaration->parameters().begin(),
      function->declaration->parameters().end(),
      [&](const Parameter &candidate) { return &candidate == parameter; });
  if (found == function->declaration->parameters().end()) {
    currentType = SemanticType::Unknown;
    return;
  }
  const std::size_t parameterIndex = static_cast<std::size_t>(
      std::distance(function->declaration->parameters().begin(), found));
  const AccessMode access = parameter->mutability == Mutability::Mutable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly;
  const CallableInvocationCapability capability =
      consuming ? CallableInvocationCapability::Once
                : callableInvocationCapability(access);
  semanticModel.recordDeferredCallableCall(
      call, DeferredCallableCallInfo{
                .genericParameter = calleeType.genericParameterId,
                .returnType = returnType,
                .parameterTypes = argumentTypes,
                .access = access,
                .capability = capability,
                .inferResultFromInstantiation = inferResultFromInstantiation});
  semanticModel.recordCallableRequirement(
      *function->declaration,
      CallableParameterContract{
          .parameterIndex = parameterIndex,
          .genericParameter = calleeType.genericParameterId,
          .callableType = calleeType,
          .access = access,
          .signatures = {
              {.source = &call,
               .returnType = returnType,
               .parameterTypes = argumentTypes,
               .capability = capability,
               .inferResultFromInstantiation = inferResultFromInstantiation}}});
  currentType = returnType;
}

auto SemanticVisitor::Impl::validateCallableReturn(
    const Call &call, const SemanticType &returnType) -> bool {
  const std::optional<CallableSignatureRequirement> requirement =
      callableSignatureRequirement(call);
  if (requirement && requirement->inferResultFromInstantiation &&
      returnType != SemanticType::Unknown) {
    if (returnType != SemanticType::Void &&
        returnType.kind != SemanticType::Reference &&
        !typeTraits(returnType).containsBorrowedState &&
        !containsLambdaType(returnType))
      return true;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2046", DiagnosticPhase::Semantics, expressionToken(call.callee()),
        "An inferred generic callable result must be a non-void value "
        "without references, tracked borrowed state, or lambda identity; "
        "the selected callable returns '" +
            typeSpelling(returnType) + "'.");
    diagnostic.hints.emplace_back(
        "Use an explicit result binding and a supported ownership contract "
        "for reference, borrowed, or callable results.");
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  if (!requirement || requirement->returnType == returnType ||
      returnType == SemanticType::Unknown) {
    return true;
  }
  const Variable *variable = directCallableParameterVariable(call.callee());
  const Token location =
      variable == nullptr ? expressionToken(call.callee()) : variable->name();
  const DeferredCallableCallInfo *deferred =
      semanticModel.findDeferredCallableCall(call);
  const SymbolRecord *field =
      deferred && deferred->receiverField != 0
          ? semanticModel.database().findSymbol(deferred->receiverField)
          : nullptr;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2046", DiagnosticPhase::Semantics, location,
      std::string(field ? "Callable field '" : "Callable parameter '") +
          location.lexeme + "' must return '" +
          typeSpelling(requirement->returnType) +
          "' for this invocation, "
          "but the selected callable returns '" +
          typeSpelling(returnType) + "'.");
  if (field) {
    diagnostic.related.push_back(
        {field->nameSpan, "Stored callable field declared here."});
  }
  if (variable != nullptr) {
    if (const Symbol *symbol = resolve(variable->name());
        symbol != nullptr && symbol->parameterDeclaration != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(symbol->parameterDeclaration->name),
           "Confined callable parameter declared here."});
    }
  }
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::validateCallableCapability(
    const Call &call, CallableInvocationCapability provided) -> bool {
  const std::optional<CallableSignatureRequirement> requirement =
      callableSignatureRequirement(call);
  if (!requirement ||
      callableCapabilitySatisfies(provided, requirement->capability)) {
    return true;
  }

  const Variable *variable = directCallableParameterVariable(call.callee());
  const Token location =
      variable == nullptr ? expressionToken(call.callee()) : variable->name();
  const std::string parameterName =
      variable == nullptr ? location.lexeme : variable->name().lexeme;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2046", DiagnosticPhase::Semantics, location,
      provided == CallableInvocationCapability::Once
          ? "Callable parameter '" + parameterName +
                "' is invoked repeatably by this function, but the "
                "selected operator() is once-callable."
          : "Callable parameter '" + parameterName +
                "' permits only read-callable invocation, but the "
                "selected callable requires mutable invocation.");
  if (variable != nullptr) {
    if (const Symbol *symbol = resolve(variable->name());
        symbol != nullptr && symbol->parameterDeclaration != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(symbol->parameterDeclaration->name),
           provided == CallableInvocationCapability::Once
               ? "Repeatable callable parameter declared here."
               : "Read-only callable parameter declared here."});
    }
  }
  diagnostic.hints.emplace_back(
      provided == CallableInvocationCapability::Once
          ? "A once-callable client must consume the parameter explicitly "
            "with std::move(value)() and cannot invoke it on another "
            "reachable path."
          : "Declare the generic callable parameter with 'mut' when the "
            "function may invoke a stateful mutable call operator.");
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::analyzeLambdaCall(
    const Call &call, const SemanticType &calleeType,
    const std::vector<SemanticType> &argumentTypes) -> void {
  const LambdaInfo *lambda = semanticModel.findLambda(calleeType.lambdaId);
  if (lambda == nullptr) {
    report(call.paren(), "Unknown lambda value.", "GTI-S2027");
    currentType = SemanticType::Unknown;
    return;
  }
  bool valid = true;
  const CallableParameterContract *contract =
      callableParameterContract(call.callee());
  const std::optional<CallableBoundary> boundary =
      contract == nullptr ? std::nullopt : std::optional{contract->boundary};
  if (!call.typeArguments().empty()) {
    report(call.paren(), "Lambdas do not take explicit generic arguments.",
           "GTI-S2027");
    valid = false;
  }
  const auto arguments = concreteCallArguments(argumentTypes, call.arguments());
  // A declaration-wide pack has no arity yet. Concrete reanalysis must select
  // the complete invocation; do not publish an executable provisional target.
  if (!arguments) {
    currentType = valid ? lambda->returnType : SemanticType::Unknown;
    return;
  }
  if (arguments->size() != lambda->parameterTypes.size()) {
    report(call.paren(),
           "Lambda expects " + std::to_string(lambda->parameterTypes.size()) +
               " argument" + (lambda->parameterTypes.size() == 1 ? "" : "s") +
               " but received " + std::to_string(arguments->size()) + ".",
           "GTI-S2005");
    valid = false;
  }
  const std::size_t count =
      std::min(arguments->size(), lambda->parameterTypes.size());
  std::vector<SemanticType> ordinaryParameterTypes;
  std::vector<const ExprPtr *> ordinaryArguments;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &argument = (*arguments)[index];
    const auto &expression = *argument.expression;
    const auto &parameter = lambda->parameterTypes[index];
    if (!argument.forwardedPackElement) {
      ordinaryParameterTypes.push_back(parameter);
      ordinaryArguments.push_back(argument.expression);
      validateMutableCallArgumentLoans(
          std::span<const SemanticType>(&parameter, 1),
          std::span<const ExprPtr>(&expression, 1), call.paren());
    }
    if (argument.type.kind == SemanticType::Lambda) {
      report(expressionToken(expression),
             "Lambda values cannot be passed to another lambda yet.",
             "GTI-S2027");
      valid = false;
      continue;
    }
    const bool matches =
        argument.forwardedPackElement
            ? forwardedPackArgumentMatches(parameter, argument.type)
            : callArgumentMatches(parameter, argument.type, expression);
    if (argument.type != SemanticType::Unknown &&
        parameter != SemanticType::Unknown && !matches) {
      reportCallArgumentMismatch(index, parameter, argument.type, expression,
                                 "Lambda");
      valid = false;
    }
  }
  validateCallPlaceExclusivityImpl(ordinaryParameterTypes, ordinaryArguments,
                                   call.paren());
  valid = validateCallableReturn(call, lambda->returnType) && valid;
  valid =
      validateCallableCapability(call, CallableInvocationCapability::Read) &&
      valid;
  if (valid) {
    semanticModel.recordLambdaCall(
        call,
        ResolvedLambdaCallInfo{.lambda = lambda->id,
                               .returnType = lambda->returnType,
                               .parameterTypes = lambda->parameterTypes,
                               .capability = CallableInvocationCapability::Read,
                               .boundary = boundary});
  }
  currentType =
      valid ? callExpressionType(lambda->returnType) : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::analyzeNativeIndirectCall(
    const Call &call, const SemanticType &calleeType,
    const std::vector<SemanticType> &argumentTypes) -> void {
  const SemanticType *result = calleeType.nativeFunctionReturnType();
  if (result == nullptr || !isCAbiNativeFunction(calleeType)) {
    report(call.paren(), "Native function pointer has no valid C signature.",
           "GTI-S2076");
    currentType = SemanticType::Unknown;
    return;
  }
  bool valid = true;
  if (!call.typeArguments().empty()) {
    report(call.paren(),
           "Native function pointers do not take explicit generic arguments.",
           "GTI-S2076");
    valid = false;
  }
  requireUnsafe(call, UnsafeOperationKind::ForeignPointerCall, call.paren(),
                "Calling a native function pointer");
  const auto arguments = concreteCallArguments(argumentTypes, call.arguments());
  if (!arguments) {
    currentType = valid ? *result : SemanticType::Unknown;
    return;
  }
  const auto parameters = calleeType.nativeFunctionParameterTypes();
  if (arguments->size() != parameters.size()) {
    report(call.paren(),
           "Native function pointer expects " +
               std::to_string(parameters.size()) + " argument" +
               (parameters.size() == 1 ? "" : "s") + " but received " +
               std::to_string(arguments->size()) + ".",
           "GTI-S2005");
    valid = false;
  }
  std::vector<SemanticType> ordinaryParameters;
  std::vector<const ExprPtr *> ordinaryArguments;
  for (std::size_t index = 0;
       index < std::min(arguments->size(), parameters.size()); ++index) {
    const auto &argument = (*arguments)[index];
    const auto &expression = *argument.expression;
    if (!argument.forwardedPackElement) {
      ordinaryParameters.push_back(parameters[index]);
      ordinaryArguments.push_back(argument.expression);
    }
    const bool matches =
        argument.forwardedPackElement
            ? forwardedPackArgumentMatches(parameters[index], argument.type)
            : callArgumentMatches(parameters[index], argument.type, expression);
    if (argument.type != SemanticType::Unknown && !matches) {
      reportCallArgumentMismatch(index, parameters[index], argument.type,
                                 expression, "Native function pointer");
      valid = false;
    }
  }
  validateCallPlaceExclusivityImpl(ordinaryParameters, ordinaryArguments,
                                   call.paren());
  valid = validateCallableReturn(call, *result) && valid;
  valid =
      validateCallableCapability(call, CallableInvocationCapability::Read) &&
      valid;
  if (valid) {
    ResolvedCallInfo resolved{
        .nativeCalleeType = calleeType,
        .callableBoundary =
            callableParameterContract(call.callee()) == nullptr
                ? std::nullopt
                : std::optional{callableParameterContract(call.callee())
                                    ->boundary},
        .returnType = *result,
        .parameterTypes = {parameters.begin(), parameters.end()}};
    semanticModel.record(call, resolved);
    semanticModel.recordDefinedFailure(call, {});
    recordSelectedCallOccurrence(call, std::move(resolved));
  }
  currentType = valid ? callExpressionType(*result) : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::analyzeFunctionItemCall(
    const Call &call, const SemanticType &calleeType,
    const std::vector<SemanticType> &argumentTypes) -> void {
  const FunctionInfo *target =
      semanticModel.findFunction(calleeType.functionId);
  if (target == nullptr && instanceBaseModel != nullptr) {
    target = instanceBaseModel->findFunction(calleeType.functionId);
  }
  const SemanticType *itemReturn = calleeType.functionItemReturnType();
  const std::span<const SemanticType> itemParameters =
      calleeType.functionItemParameterTypes();
  if (target == nullptr || target->declaration == nullptr ||
      target->ownerClass != 0 || target->entryPoint ||
      target->linkage != LanguageLinkage::Gti ||
      target->returnType !=
          (itemReturn == nullptr ? SemanticType::Unknown : *itemReturn) ||
      target->parameterTypes.size() != itemParameters.size() ||
      !std::equal(target->parameterTypes.begin(), target->parameterTypes.end(),
                  itemParameters.begin())) {
    report(call.paren(),
           "Function-item invocation lost its exact namespace function "
           "identity.",
           "GTI-S2090");
    currentType = SemanticType::Unknown;
    return;
  }

  bool valid = true;
  if (!call.typeArguments().empty()) {
    for (const TypeRef &argument : call.typeArguments()) {
      validateType(argument);
    }
    report(call.paren(),
           "Function items do not take explicit generic arguments.",
           "GTI-S2090");
    valid = false;
  }
  const auto arguments = concreteCallArguments(argumentTypes, call.arguments());
  if (!arguments) {
    currentType =
        valid ? callExpressionType(target->returnType) : SemanticType::Unknown;
    return;
  }
  if (arguments->size() != target->parameterTypes.size()) {
    report(call.paren(),
           "Function item expects " +
               std::to_string(target->parameterTypes.size()) + " argument" +
               (target->parameterTypes.size() == 1 ? "" : "s") +
               " but received " + std::to_string(arguments->size()) + ".",
           "GTI-S2005");
    valid = false;
  }
  const std::size_t count =
      std::min(arguments->size(), target->parameterTypes.size());
  std::vector<SemanticType> ordinaryParameterTypes;
  std::vector<const ExprPtr *> ordinaryArguments;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &argument = (*arguments)[index];
    const auto &expression = *argument.expression;
    const auto &parameter = target->parameterTypes[index];
    if (!argument.forwardedPackElement) {
      ordinaryParameterTypes.push_back(parameter);
      ordinaryArguments.push_back(argument.expression);
      validateMutableCallArgumentLoans(
          std::span<const SemanticType>(&parameter, 1),
          std::span<const ExprPtr>(&expression, 1), call.paren());
    }
    const bool matches =
        argument.forwardedPackElement
            ? forwardedPackArgumentMatches(parameter, argument.type)
            : callArgumentMatches(parameter, argument.type, expression);
    if (!matches) {
      reportCallArgumentMismatch(index, parameter, argument.type, expression,
                                 "Function item");
      valid = false;
    }
  }
  valid = validateCallableReturn(call, target->returnType) && valid;
  valid =
      validateCallableCapability(call, CallableInvocationCapability::Read) &&
      valid;
  validateCallPlaceExclusivityImpl(ordinaryParameterTypes, ordinaryArguments,
                                   call.paren());
  if (!valid) {
    currentType = SemanticType::Unknown;
    return;
  }

  ResolvedCallInfo resolved{.function = target->id,
                            .declaration = target->declaration,
                            .returnType = target->returnType,
                            .parameterTypes = target->parameterTypes,
                            .borrowOrigin = target->returnBorrowOrigin,
                            .borrowArgument = target->returnBorrowParameter,
                            .borrowAccess = target->returnBorrowAccess,
                            .borrowPlace = target->returnBorrowPlace,
                            .dispatch = CallDispatch::Static,
                            .receiverMode = CallReceiverMode::None};
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
  currentType = callExpressionType(target->returnType);
}

auto SemanticVisitor::Impl::findGenericParameter(
    const std::vector<GenericParameterInfo> &parameters, GenericParameterId id)
    -> const GenericParameterInfo * {
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.id == id) {
      return &parameter;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::packGenericParameter(
    const CallableCandidate &function) -> const GenericParameterInfo * {
  const auto found = std::find_if(
      function.record().genericParameters.begin(),
      function.record().genericParameters.end(),
      [](const GenericParameterInfo &parameter) { return parameter.pack; });
  return found == function.record().genericParameters.end() ? nullptr : &*found;
}

auto SemanticVisitor::Impl::fixedParameterCount(
    const CallableCandidate &function) -> std::size_t {
  return function.parameterPack && !function.parameterTypes.empty()
             ? function.parameterTypes.size() - 1
             : function.parameterTypes.size();
}

auto SemanticVisitor::Impl::hasPackExpansion(
    const std::vector<SemanticType> &arguments) -> bool {
  return !arguments.empty() && arguments.back().kind == SemanticType::TypePack;
}

auto SemanticVisitor::Impl::appendConcreteCallArgument(
    const SemanticType &type, const ExprPtr &expression,
    std::vector<AnalyzedCallArgument> &result,
    std::vector<std::size_t> packPath) const -> bool {
  if (type.kind != SemanticType::TypePack) {
    result.push_back({.type = type,
                      .expression = &expression,
                      .forwardedPackElement = !packPath.empty(),
                      .packPath = std::move(packPath)});
    return true;
  }
  if (!type.concretePack) {
    return false;
  }
  for (std::size_t index = 0; index < type.arguments.size(); ++index) {
    auto path = packPath;
    path.push_back(index);
    if (!appendConcreteCallArgument(type.arguments[index], expression, result,
                                    std::move(path)))
      return false;
  }
  return true;
}

auto SemanticVisitor::Impl::concreteCallArguments(
    const std::vector<SemanticType> &types, const ExprList &expressions) const
    -> std::optional<std::vector<AnalyzedCallArgument>> {
  if (types.size() != expressions.size()) {
    return std::nullopt;
  }
  std::vector<AnalyzedCallArgument> result;
  for (std::size_t index = 0; index < types.size(); ++index) {
    if (!appendConcreteCallArgument(types[index], expressions[index], result)) {
      return std::nullopt;
    }
  }
  return result;
}

auto SemanticVisitor::Impl::acceptsArgumentShape(
    const CallableCandidate &function,
    const std::vector<SemanticType> &arguments) -> bool {
  if (!function.parameterPack) {
    const bool groupedResolvedPack =
        function.record().parameterPack && !function.parameterTypes.empty() &&
        function.parameterTypes.back().kind == SemanticType::TypePack;
    return (!hasPackExpansion(arguments) || groupedResolvedPack) &&
           arguments.size() >= function.requiredParameterCount &&
           arguments.size() <= function.parameterTypes.size();
  }
  const std::size_t fixed = fixedParameterCount(function);
  if (hasPackExpansion(arguments)) {
    return arguments.size() >= fixed + 1;
  }
  return arguments.size() >= fixed;
}

auto SemanticVisitor::Impl::hasExactComparisonOperator(
    const SemanticType &type, OverloadedOperator operation) const -> bool {
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr) {
    return false;
  }
  const auto member =
      owner->members.find(std::string(operatorFunctionName(operation)));
  if (member == owner->members.end() ||
      member->second.symbol.type != SemanticType::Function) {
    return false;
  }

  const SemanticType parameter = SemanticType::referenceTo(type);
  const Symbol overloadSet = substituteSymbol(member->second.symbol, type);
  return std::any_of(overloadSet.overloads.begin(), overloadSet.overloads.end(),
                     [&](const CallableCandidate &candidate) {
                       return !candidate.staticMember &&
                              candidate.access == AccessModifier::Public &&
                              candidate.requirements.empty() &&
                              candidate.receiverMutability ==
                                  ReceiverMutability::ReadOnly &&
                              candidate.returnType == SemanticType::Bool &&
                              candidate.parameterTypes.size() == 1 &&
                              candidate.parameterTypes.front() == parameter;
                     });
}

auto SemanticVisitor::Impl::isEqualityComparableConstraintType(
    const SemanticType &type) const -> bool {
  if (type.kind == SemanticType::Class) {
    return hasExactComparisonOperator(type, OverloadedOperator::Equal) &&
           hasExactComparisonOperator(type, OverloadedOperator::NotEqual);
  }
  return isInteger(type) || semanticFloatFormat(type).has_value() ||
         type == SemanticType::Bool || type == SemanticType::Char ||
         type == SemanticType::StringView || type == SemanticType::NullPtr ||
         type.kind == SemanticType::RawPointer ||
         type.kind == SemanticType::Enum;
}

auto SemanticVisitor::Impl::isRelationallyOrderedConstraintType(
    const SemanticType &type) const -> bool {
  if (isInteger(type) || semanticFloatFormat(type).has_value()) {
    return true;
  }
  return type.kind == SemanticType::Class &&
         hasExactComparisonOperator(type, OverloadedOperator::Less) &&
         hasExactComparisonOperator(type, OverloadedOperator::LessEqual) &&
         hasExactComparisonOperator(type, OverloadedOperator::Greater) &&
         hasExactComparisonOperator(type, OverloadedOperator::GreaterEqual);
}

auto SemanticVisitor::Impl::satisfiesAtomicConstraint(
    const SemanticType &argument, GenericConstraintKind constraint) const
    -> bool {
  if (argument == SemanticType::Unknown ||
      constraint == GenericConstraintKind::None ||
      constraint == GenericConstraintKind::Invalid) {
    return true;
  }
  switch (constraint) {
  case GenericConstraintKind::EqualityComparable:
    return isEqualityComparableConstraintType(argument);
  case GenericConstraintKind::RelationallyOrdered:
    return isRelationallyOrderedConstraintType(argument);
  case GenericConstraintKind::Numeric:
    return isInteger(argument) || semanticFloatFormat(argument).has_value();
  case GenericConstraintKind::SignedNumeric:
    return isSignedInteger(argument) ||
           semanticFloatFormat(argument).has_value();
  case GenericConstraintKind::Integral:
    return isInteger(argument);
  case GenericConstraintKind::IntegralEnum:
    return argument.kind == SemanticType::Enum && argument.enumId != 0 &&
           argument.enumId <= enums.size() &&
           !enums[argument.enumId - 1].payload;
  case GenericConstraintKind::Reflectable:
    return isReflectableType(argument);
  case GenericConstraintKind::SignedIntegral:
    return isSignedInteger(argument);
  case GenericConstraintKind::UnsignedIntegral:
    return isUnsignedInteger(argument);
  case GenericConstraintKind::FloatingPoint:
    return semanticFloatFormat(argument).has_value();
  case GenericConstraintKind::AtomicScalar:
    return argument == SemanticType::Bool || isInteger(argument);
  case GenericConstraintKind::CopyConstructible:
    return typeTraits(argument).copyable;
  case GenericConstraintKind::MoveConstructible:
    return typeTraits(argument).movable;
  case GenericConstraintKind::Copyable: {
    const SemanticTypeTraits traits = typeTraits(argument);
    return traits.copyable && traits.copyAssignable;
  }
  case GenericConstraintKind::Movable: {
    const SemanticTypeTraits traits = typeTraits(argument);
    return traits.movable && traits.moveAssignable;
  }
  case GenericConstraintKind::Transferable:
    return typeTraits(argument).transferCapable;
  case GenericConstraintKind::Relocatable:
    if (argument.kind == SemanticType::Array && argument.arguments.size() == 1)
      return satisfiesConstraint(
          argument.arguments.front(),
          constraintBit(GenericConstraintKind::Relocatable));
    return typeTraits(argument).relocation != RelocationKind::Unavailable;
  case GenericConstraintKind::BitwiseRelocatable:
    if (argument.kind == SemanticType::Array && argument.arguments.size() == 1)
      return satisfiesConstraint(
          argument.arguments.front(),
          constraintBit(GenericConstraintKind::BitwiseRelocatable));
    return typeTraits(argument).relocation == RelocationKind::Bitwise;
  case GenericConstraintKind::Shareable:
    return typeTraits(argument).shareCapable;
  case GenericConstraintKind::DefaultInitializable:
    return isDefaultInitializable(argument, true);
  case GenericConstraintKind::None:
  case GenericConstraintKind::Invalid:
  case GenericConstraintKind::Count:
    return true;
  }
  return false;
}

auto SemanticVisitor::Impl::firstUnsatisfiedConstraint(
    const SemanticType &argument, GenericConstraintSet constraints) const
    -> std::optional<GenericConstraintKind> {
  if (argument == SemanticType::Unknown || constraints == 0 ||
      invalidConstraintSet(constraints) ||
      (resolvingGenericTypeDefault && containsOpenTypeParameter(argument))) {
    return std::nullopt;
  }
  if (argument.kind == SemanticType::TypeParameter ||
      argument.kind == SemanticType::TypePack) {
    const GenericConstraintSet actual = constraintOf(argument);
    if (invalidConstraintSet(actual)) {
      return std::nullopt;
    }
    for (GenericConstraintKind required : genericConstraintKinds) {
      if (hasConstraint(constraints, required) &&
          !hasConstraint(actual, required)) {
        return required;
      }
    }
    return std::nullopt;
  }
  for (GenericConstraintKind required : genericConstraintKinds) {
    if (hasConstraint(constraints, required) &&
        !satisfiesAtomicConstraint(argument, required)) {
      return required;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::satisfiesConstraint(
    const SemanticType &argument, GenericConstraintSet constraints) const
    -> bool {
  return !firstUnsatisfiedConstraint(argument, constraints).has_value();
}

auto SemanticVisitor::Impl::firstConstraintFailure(
    const std::vector<GenericParameterInfo> &parameters,
    const std::vector<SemanticType> &arguments) const
    -> std::optional<ConstraintFailure> {
  if (deferringTypeAliasConstraints) {
    return std::nullopt;
  }
  std::size_t argumentIndex = 0;
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.value) {
      continue;
    }
    const std::size_t end =
        parameter.pack ? arguments.size() : argumentIndex + 1;
    while (argumentIndex < end && argumentIndex < arguments.size()) {
      if (parameter.conceptId != 0) {
        AppliedConceptRequirement requirement{
            .conceptId = parameter.conceptId,
            .arguments = {arguments[argumentIndex]}};
        if (std::optional<ConstraintFailure> failure =
                firstConceptApplicationFailure(requirement)) {
          failure->parameter = parameter.name;
          failure->constraintName = parameter.constraintName;
          failure->application = nullptr;
          failure->parameterId =
              failure->applicationArgumentIndex ? parameter.id : 0;
          if (parameter.constraintName && !failure->context.empty()) {
            failure->context.front().location =
                parameter.constraintName->last();
          }
          if (parameter.constraintName && failure->context.empty()) {
            failure->context.push_back({pathSpelling(*parameter.constraintName),
                                        {arguments[argumentIndex]},
                                        parameter.constraintName->last()});
          }
          return failure;
        }
        ++argumentIndex;
        continue;
      }
      const std::optional<GenericConstraintKind> failed =
          firstUnsatisfiedConstraint(arguments[argumentIndex],
                                     parameter.constraints);
      if (failed) {
        return ConstraintFailure{.parameter = parameter.name,
                                 .argument = arguments[argumentIndex],
                                 .constraints = parameter.constraints,
                                 .failed = *failed,
                                 .constraintName = parameter.constraintName,
                                 .parameterId = parameter.id};
      }
      ++argumentIndex;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::guaranteedStructuralKinds(
    StructuralConstraintKind kind) -> std::vector<StructuralConstraintKind> {
  switch (kind) {
  case StructuralConstraintKind::ReadCallable:
    return {kind, StructuralConstraintKind::MutCallable,
            StructuralConstraintKind::OnceCallable};
  case StructuralConstraintKind::MutCallable:
    return {kind, StructuralConstraintKind::OnceCallable};
  case StructuralConstraintKind::ReadBinaryCallable:
    return {kind, StructuralConstraintKind::MutBinaryCallable,
            StructuralConstraintKind::OnceBinaryCallable};
  case StructuralConstraintKind::MutBinaryCallable:
    return {kind, StructuralConstraintKind::OnceBinaryCallable};
  default:
    return {kind};
  }
}

auto SemanticVisitor::Impl::conceptSubstitution(
    const RegisteredConcept &conceptInfo,
    std::span<const SemanticType> arguments) const -> TypeSubstitution {
  TypeSubstitution result;
  for (std::size_t index = 0;
       index < arguments.size() && index < conceptInfo.parameterIds.size();
       ++index)
    result.emplace(conceptInfo.parameterIds[index], arguments[index]);
  return result;
}

auto SemanticVisitor::Impl::expandStructuralRequirements(
    std::span<const AppliedConceptRequirement> requirements) const
    -> std::vector<ExpandedStructuralRequirement> {
  const auto expandLeaf = [&](const AppliedConceptRequirement &requirement) {
    std::vector<ExpandedStructuralRequirement> result;
    if (requirement.syntax && requirement.syntax->memberRequirement) {
      result.push_back(
          {.kind = StructuralConstraintKind::ReadMember,
           .arguments = requirement.arguments,
           .application = requirement.syntax,
           .memberName = requirement.syntax->memberRequirement->member().lexeme,
           .mutableReceiver = requirement.syntax->memberRequirement->receiver()
                                  .referenceMutability.has_value(),
           .requiresUnsafe =
               requirement.syntax->memberRequirement->unsafeKeyword()
                   .has_value()});
      if (!result.back().requiresUnsafe) {
        auto weaker = result.back();
        weaker.requiresUnsafe = true;
        result.emplace_back(std::move(weaker));
      }
      return result;
    }
    if (requirement.conceptId == 0 || requirement.conceptId > concepts.size()) {
      return result;
    }
    const RegisteredConcept &conceptInfo = concepts[requirement.conceptId - 1];
    const auto substitution =
        conceptSubstitution(conceptInfo, requirement.arguments);
    for (const RegisteredConcept::StructuralPattern &pattern :
         conceptInfo.structuralConstraints) {
      ExpandedStructuralRequirement expanded{
          .kind = pattern.kind,
          .application = requirement.syntax,
          .memberName = pattern.memberName,
          .mutableReceiver = pattern.mutableReceiver,
          .requiresUnsafe = pattern.requiresUnsafe};
      bool valid = true;
      for (const RegisteredConcept::StructuralPattern::Argument &argument :
           pattern.arguments) {
        if (!argument.parameter) {
          expanded.arguments.emplace_back(
              substituteType(argument.concrete, substitution));
          continue;
        }
        if (*argument.parameter >= requirement.arguments.size()) {
          valid = false;
          break;
        }
        expanded.arguments.emplace_back(
            requirement.arguments[*argument.parameter]);
      }
      if (valid) {
        for (StructuralConstraintKind guaranteed :
             guaranteedStructuralKinds(expanded.kind)) {
          ExpandedStructuralRequirement implied = expanded;
          implied.kind = guaranteed;
          result.emplace_back(std::move(implied));
        }
      }
    }
    return result;
  };
  const ConstraintExpression *expression = nullptr;
  for (const AppliedConceptRequirement &requirement : requirements) {
    if (requirement.expression != nullptr) {
      expression = requirement.expression;
      break;
    }
  }
  if (expression == nullptr ||
      expression->root == ConstraintExpressionNode::invalidIndex) {
    std::vector<ExpandedStructuralRequirement> result;
    for (const AppliedConceptRequirement &requirement : requirements) {
      std::vector<ExpandedStructuralRequirement> expanded =
          expandLeaf(requirement);
      result.insert(result.end(), std::make_move_iterator(expanded.begin()),
                    std::make_move_iterator(expanded.end()));
    }
    return result;
  }
  const auto sameRequirement = [](const ExpandedStructuralRequirement &left,
                                  const ExpandedStructuralRequirement &right) {
    return left.kind == right.kind && left.arguments == right.arguments &&
           left.memberName == right.memberName &&
           left.mutableReceiver == right.mutableReceiver &&
           left.requiresUnsafe == right.requiresUnsafe;
  };
  std::function<std::vector<ExpandedStructuralRequirement>(std::size_t)>
      expandNode = [&](std::size_t nodeIndex) {
        std::vector<ExpandedStructuralRequirement> result;
        if (nodeIndex >= expression->nodes.size()) {
          return result;
        }
        const ConstraintExpressionNode &node = expression->nodes[nodeIndex];
        if (node.kind == ConstraintExpressionKind::Application) {
          const auto found = std::find_if(
              requirements.begin(), requirements.end(),
              [&](const AppliedConceptRequirement &requirement) {
                return requirement.expression == expression &&
                       requirement.expressionApplication == node.application;
              });
          return found == requirements.end() ? result : expandLeaf(*found);
        }
        std::vector<ExpandedStructuralRequirement> left = expandNode(node.left);
        std::vector<ExpandedStructuralRequirement> right =
            expandNode(node.right);
        if (node.kind == ConstraintExpressionKind::Conjunction) {
          result = std::move(left);
          for (ExpandedStructuralRequirement &candidate : right) {
            if (std::none_of(result.begin(), result.end(),
                             [&](const auto &existing) {
                               return sameRequirement(existing, candidate);
                             })) {
              result.emplace_back(std::move(candidate));
            }
          }
          return result;
        }
        for (ExpandedStructuralRequirement &candidate : left) {
          if (std::any_of(right.begin(), right.end(), [&](const auto &other) {
                return sameRequirement(candidate, other);
              })) {
            result.emplace_back(std::move(candidate));
          }
        }
        return result;
      };
  return expandNode(expression->root);
}

auto SemanticVisitor::Impl::activeStructuralRequirements() const
    -> std::vector<ExpandedStructuralRequirement> {
  std::vector<ExpandedStructuralRequirement> result;
  for (const std::vector<AppliedConceptRequirement> &scope :
       requirementScopes) {
    std::vector<ExpandedStructuralRequirement> expanded =
        expandStructuralRequirements(scope);
    result.insert(result.end(), std::make_move_iterator(expanded.begin()),
                  std::make_move_iterator(expanded.end()));
  }
  return result;
}

auto SemanticVisitor::Impl::activeAtomicConstraints(
    const SemanticType &type) const -> GenericConstraintSet {
  GenericConstraintSet result = 0;
  for (const std::vector<AppliedConceptRequirement> &scope :
       requirementScopes) {
    const auto atomicForLeaf =
        [&](const AppliedConceptRequirement &requirement) {
          GenericConstraintSet leaf = 0;
          if (requirement.conceptId == 0 ||
              requirement.conceptId > concepts.size()) {
            return leaf;
          }
          const RegisteredConcept &conceptInfo =
              concepts[requirement.conceptId - 1];
          const std::size_t count =
              std::min(requirement.arguments.size(),
                       conceptInfo.parameterConstraints.size());
          for (std::size_t index = 0; index < count; ++index) {
            if (requirement.arguments[index] == type) {
              leaf |= conceptInfo.parameterConstraints[index];
            }
          }
          return leaf;
        };
    const ConstraintExpression *expression = nullptr;
    for (const AppliedConceptRequirement &requirement : scope) {
      if (requirement.expression != nullptr) {
        expression = requirement.expression;
        break;
      }
    }
    if (expression == nullptr ||
        expression->root == ConstraintExpressionNode::invalidIndex) {
      for (const AppliedConceptRequirement &requirement : scope) {
        result |= atomicForLeaf(requirement);
      }
      continue;
    }
    std::function<GenericConstraintSet(std::size_t)> atomicForNode =
        [&](std::size_t nodeIndex) -> GenericConstraintSet {
      if (nodeIndex >= expression->nodes.size()) {
        return 0;
      }
      const ConstraintExpressionNode &node = expression->nodes[nodeIndex];
      if (node.kind == ConstraintExpressionKind::Application) {
        const auto found = std::find_if(
            scope.begin(), scope.end(),
            [&](const AppliedConceptRequirement &requirement) {
              return requirement.expression == expression &&
                     requirement.expressionApplication == node.application;
            });
        return found == scope.end() ? 0 : atomicForLeaf(*found);
      }
      const GenericConstraintSet left = atomicForNode(node.left);
      const GenericConstraintSet right = atomicForNode(node.right);
      return node.kind == ConstraintExpressionKind::Conjunction ? left | right
                                                                : left & right;
    };
    result |= atomicForNode(expression->root);
  }
  return result;
}

auto SemanticVisitor::Impl::structuralRequirementAvailable(
    StructuralConstraintKind kind,
    std::span<const SemanticType> arguments) const -> bool {
  // These declaration-bound capabilities admit only complete, borrow-free
  // scalar values. Their generic uses retain those structural guarantees;
  // movement/copying alone says nothing about layout or stored borrows.
  if ((kind == StructuralConstraintKind::ObjectLayout ||
       kind == StructuralConstraintKind::IndependentObject) &&
      arguments.size() == 1) {
    const auto constraints = constraintOf(arguments.front());
    if (!invalidConstraintSet(constraints)) {
      for (const auto scalar : {GenericConstraintKind::Numeric,
                                GenericConstraintKind::SignedNumeric,
                                GenericConstraintKind::Integral,
                                GenericConstraintKind::SignedIntegral,
                                GenericConstraintKind::UnsignedIntegral,
                                GenericConstraintKind::FloatingPoint,
                                GenericConstraintKind::AtomicScalar,
                                GenericConstraintKind::IntegralEnum}) {
        if (hasConstraint(constraints, scalar))
          return true;
      }
    }
  }
  const std::vector<ExpandedStructuralRequirement> active =
      activeStructuralRequirements();
  const auto callableCapability = [](StructuralConstraintKind candidate)
      -> std::optional<CallableInvocationCapability> {
    switch (candidate) {
    case StructuralConstraintKind::ReadCallable:
    case StructuralConstraintKind::ReadBinaryCallable:
      return CallableInvocationCapability::Read;
    case StructuralConstraintKind::MutCallable:
    case StructuralConstraintKind::MutBinaryCallable:
      return CallableInvocationCapability::Mutable;
    case StructuralConstraintKind::OnceCallable:
    case StructuralConstraintKind::OnceBinaryCallable:
      return CallableInvocationCapability::Once;
    default:
      return std::nullopt;
    }
  };
  const auto callableArity = [](StructuralConstraintKind candidate) {
    switch (candidate) {
    case StructuralConstraintKind::ReadCallable:
    case StructuralConstraintKind::MutCallable:
    case StructuralConstraintKind::OnceCallable:
      return 1;
    case StructuralConstraintKind::ReadBinaryCallable:
    case StructuralConstraintKind::MutBinaryCallable:
    case StructuralConstraintKind::OnceBinaryCallable:
      return 2;
    default:
      return 0;
    }
  };
  return std::any_of(
      active.begin(), active.end(), [&](const auto &requirement) {
        if (requirement.arguments.size() != arguments.size() ||
            !std::equal(requirement.arguments.begin(),
                        requirement.arguments.end(), arguments.begin(),
                        arguments.end())) {
          return false;
        }
        if (requirement.kind == kind) {
          return true;
        }
        const auto provided = callableCapability(requirement.kind);
        const auto required = callableCapability(kind);
        return provided && required &&
               callableArity(requirement.kind) == callableArity(kind) &&
               callableCapabilitySatisfies(*provided, *required);
      });
}

auto SemanticVisitor::Impl::firstRequirementFailure(
    std::span<const AppliedConceptRequirement> requirements) const
    -> std::optional<ConstraintFailure> {
  const auto firstFlatFailure = [&]() -> std::optional<ConstraintFailure> {
    for (const AppliedConceptRequirement &requirement : requirements) {
      if (std::optional<ConstraintFailure> failure =
              firstConceptApplicationFailure(requirement)) {
        return failure;
      }
    }
    return std::nullopt;
  };
  const ConstraintExpression *expression = nullptr;
  for (const AppliedConceptRequirement &requirement : requirements) {
    if (requirement.expression != nullptr) {
      expression = requirement.expression;
      break;
    }
  }
  if (expression == nullptr ||
      expression->root == ConstraintExpressionNode::invalidIndex) {
    return firstFlatFailure();
  }
  const auto expressionOf = [](std::span<const AppliedConceptRequirement> set) {
    const auto found = std::find_if(set.begin(), set.end(),
                                    [](const AppliedConceptRequirement &item) {
                                      return item.expression != nullptr;
                                    });
    return found == set.end()
               ? static_cast<const ConstraintExpression *>(nullptr)
               : found->expression;
  };
  const auto requirementAt =
      [](std::span<const AppliedConceptRequirement> set,
         const ConstraintExpression *shape,
         std::size_t application) -> const AppliedConceptRequirement * {
    const auto found = std::find_if(
        set.begin(), set.end(), [&](const AppliedConceptRequirement &item) {
          return item.expression == shape &&
                 item.expressionApplication == application;
        });
    return found == set.end() ? nullptr : &*found;
  };
  for (const std::vector<AppliedConceptRequirement> &active :
       requirementScopes) {
    const ConstraintExpression *activeExpression = expressionOf(active);
    if (activeExpression == nullptr ||
        activeExpression->root == ConstraintExpressionNode::invalidIndex) {
      continue;
    }
    std::function<bool(std::size_t, std::size_t)> sameExpression =
        [&](std::size_t leftIndex, std::size_t rightIndex) {
          if (leftIndex >= expression->nodes.size() ||
              rightIndex >= activeExpression->nodes.size()) {
            return false;
          }
          const ConstraintExpressionNode &left = expression->nodes[leftIndex];
          const ConstraintExpressionNode &right =
              activeExpression->nodes[rightIndex];
          if (left.kind != right.kind) {
            return false;
          }
          if (left.kind == ConstraintExpressionKind::Application) {
            const AppliedConceptRequirement *leftRequirement =
                requirementAt(requirements, expression, left.application);
            const AppliedConceptRequirement *rightRequirement =
                requirementAt(active, activeExpression, right.application);
            return leftRequirement != nullptr && rightRequirement != nullptr &&
                   leftRequirement->conceptId == rightRequirement->conceptId &&
                   (!leftRequirement->syntax ||
                    !leftRequirement->syntax->memberRequirement ||
                    (rightRequirement->syntax &&
                     rightRequirement->syntax->memberRequirement &&
                     leftRequirement->syntax->memberRequirement->member()
                             .lexeme ==
                         rightRequirement->syntax->memberRequirement->member()
                             .lexeme &&
                     leftRequirement->syntax->memberRequirement->receiver()
                             .referenceMutability.has_value() ==
                         rightRequirement->syntax->memberRequirement->receiver()
                             .referenceMutability.has_value() &&
                     (leftRequirement->syntax->memberRequirement
                          ->unsafeKeyword() ||
                      !rightRequirement->syntax->memberRequirement
                           ->unsafeKeyword()))) &&
                   leftRequirement->arguments == rightRequirement->arguments;
          }
          return sameExpression(left.left, right.left) &&
                 sameExpression(left.right, right.right);
        };
    if (sameExpression(expression->root, activeExpression->root)) {
      return std::nullopt;
    }
  }
  std::function<std::optional<ConstraintFailure>(std::size_t)> failureFor =
      [&](std::size_t nodeIndex) -> std::optional<ConstraintFailure> {
    if (nodeIndex >= expression->nodes.size()) {
      return std::nullopt;
    }
    const ConstraintExpressionNode &node = expression->nodes[nodeIndex];
    if (node.kind == ConstraintExpressionKind::Application) {
      const auto found = std::find_if(
          requirements.begin(), requirements.end(),
          [&](const AppliedConceptRequirement &requirement) {
            return requirement.expression == expression &&
                   requirement.expressionApplication == node.application;
          });
      return found == requirements.end()
                 ? std::nullopt
                 : firstConceptApplicationFailure(*found);
    }
    std::optional<ConstraintFailure> left = failureFor(node.left);
    if (node.kind == ConstraintExpressionKind::Conjunction) {
      return left ? left : failureFor(node.right);
    }
    if (!left) {
      return std::nullopt;
    }
    std::optional<ConstraintFailure> right = failureFor(node.right);
    if (!right) {
      return std::nullopt;
    }
    return ConstraintFailure{.parameter = left->parameter,
                             .argument = left->argument,
                             .disjunction = &node.operation,
                             .alternatives = {*left, *right}};
  };
  return failureFor(expression->root);
}

auto SemanticVisitor::Impl::firstConceptApplicationFailure(
    const AppliedConceptRequirement &requirement) const
    -> std::optional<ConstraintFailure> {
  if (requirement.syntax && requirement.syntax->memberRequirement) {
    const auto &member = requirement.syntax->memberRequirement->member();
    SemanticMemberRequirementFailure memberFailure;
    if (requirement.arguments.size() >= 2 &&
        readMemberRequirementSatisfied(
            requirement.arguments[0], member.lexeme, requirement.arguments[1],
            &memberFailure, std::span(requirement.arguments).subspan(2),
            requirement.syntax->memberRequirement->receiver()
                .referenceMutability.has_value(),
            requirement.syntax->memberRequirement->unsafeKeyword().has_value(),
            requirement.trustedMemberAccess)) {
      return std::nullopt;
    }
    return ConstraintFailure{.parameter = member,
                             .argument = requirement.arguments.empty()
                                             ? SemanticType::Unknown
                                             : requirement.arguments[0],
                             .structural = StructuralConstraintKind::ReadMember,
                             .application = requirement.syntax,
                             .memberName = member.lexeme,
                             .leafApplication = requirement.syntax,
                             .applicationArgumentIndex = 0,
                             .memberFailure = memberFailure};
  }
  if (requirement.conceptId == 0 || requirement.conceptId > concepts.size()) {
    return std::nullopt;
  }
  if (requirement.arguments.size() == 1 &&
      requirement.arguments.front().kind == SemanticType::TypeParameter) {
    const auto declared = genericConstraintConcepts.find(
        requirement.arguments.front().genericParameterId);
    if (declared != genericConstraintConcepts.end() &&
        declared->second == requirement.conceptId) {
      return std::nullopt;
    }
  }
  const auto sameAppliedRequirement =
      [&](const AppliedConceptRequirement &left,
          const AppliedConceptRequirement &right) {
        return left.conceptId == right.conceptId &&
               left.arguments == right.arguments;
      };
  for (const std::vector<AppliedConceptRequirement> &scope :
       requirementScopes) {
    const ConstraintExpression *shape = nullptr;
    for (const AppliedConceptRequirement &active : scope) {
      if (active.expression != nullptr) {
        shape = active.expression;
        break;
      }
    }
    if (shape == nullptr ||
        shape->root == ConstraintExpressionNode::invalidIndex) {
      if (std::any_of(scope.begin(), scope.end(), [&](const auto &active) {
            return sameAppliedRequirement(requirement, active);
          })) {
        return std::nullopt;
      }
      continue;
    }
    std::function<std::vector<const AppliedConceptRequirement *>(std::size_t)>
        guaranteedAt = [&](std::size_t nodeIndex) {
          std::vector<const AppliedConceptRequirement *> result;
          if (nodeIndex >= shape->nodes.size()) {
            return result;
          }
          const ConstraintExpressionNode &node = shape->nodes[nodeIndex];
          if (node.kind == ConstraintExpressionKind::Application) {
            const auto found = std::find_if(
                scope.begin(), scope.end(),
                [&](const AppliedConceptRequirement &active) {
                  return active.expression == shape &&
                         active.expressionApplication == node.application;
                });
            if (found != scope.end()) {
              result.push_back(&*found);
            }
            return result;
          }
          std::vector<const AppliedConceptRequirement *> left =
              guaranteedAt(node.left);
          std::vector<const AppliedConceptRequirement *> right =
              guaranteedAt(node.right);
          if (node.kind == ConstraintExpressionKind::Conjunction) {
            result = std::move(left);
            for (const AppliedConceptRequirement *candidate : right) {
              if (std::none_of(
                      result.begin(), result.end(), [&](const auto *item) {
                        return sameAppliedRequirement(*item, *candidate);
                      })) {
                result.push_back(candidate);
              }
            }
            return result;
          }
          for (const AppliedConceptRequirement *candidate : left) {
            if (std::any_of(right.begin(), right.end(), [&](const auto *item) {
                  return sameAppliedRequirement(*item, *candidate);
                })) {
              result.push_back(candidate);
            }
          }
          return result;
        };
    const std::vector<const AppliedConceptRequirement *> guaranteed =
        guaranteedAt(shape->root);
    if (std::any_of(guaranteed.begin(), guaranteed.end(),
                    [&](const auto *active) {
                      return sameAppliedRequirement(requirement, *active);
                    })) {
      return std::nullopt;
    }
  }
  const RegisteredConcept &conceptInfo = concepts[requirement.conceptId - 1];
  const auto substitution =
      conceptSubstitution(conceptInfo, requirement.arguments);
  if (conceptInfo.definitionExpression != nullptr &&
      !conceptInfo.definitionRequirements.empty()) {
    std::vector<AppliedConceptRequirement> expanded;
    expanded.reserve(conceptInfo.definitionRequirements.size());
    for (const RegisteredConcept::RequirementPattern &pattern :
         conceptInfo.definitionRequirements) {
      AppliedConceptRequirement applied{
          .conceptId = pattern.conceptId,
          .syntax = pattern.syntax,
          .expression = conceptInfo.definitionExpression,
          .expressionApplication = pattern.expressionApplication};
      bool valid = true;
      for (const RegisteredConcept::StructuralPattern::Argument &argument :
           pattern.arguments) {
        if (!argument.parameter) {
          applied.arguments.emplace_back(
              substituteType(argument.concrete, substitution));
        } else if (*argument.parameter < requirement.arguments.size()) {
          applied.arguments.emplace_back(
              requirement.arguments[*argument.parameter]);
        } else {
          valid = false;
          break;
        }
      }
      if (valid) {
        expanded.emplace_back(std::move(applied));
      }
    }
    if (std::optional<ConstraintFailure> failure =
            firstRequirementFailure(expanded)) {
      // Follow the declared parameter map, not coincidentally equal concrete
      // types. A relationship may reorder parameters or mention a closed type.
      std::optional<std::size_t> outerArgument;
      if (failure->applicationArgumentIndex) {
        const auto pattern = std::find_if(
            conceptInfo.definitionRequirements.begin(),
            conceptInfo.definitionRequirements.end(), [&](const auto &item) {
              return item.syntax == failure->application;
            });
        if (pattern != conceptInfo.definitionRequirements.end() &&
            *failure->applicationArgumentIndex < pattern->arguments.size()) {
          outerArgument =
              pattern->arguments[*failure->applicationArgumentIndex].parameter;
          if (!outerArgument) {
            const auto &compound =
                pattern->arguments[*failure->applicationArgumentIndex].concrete;
            for (std::size_t index = 0; index < conceptInfo.parameterIds.size();
                 ++index) {
              if (!containsGenericParameter(
                      compound, conceptInfo.parameterIds[index], false))
                continue;
              if (outerArgument) {
                outerArgument.reset();
                break;
              }
              outerArgument = index;
            }
          }
        }
      }
      failure->applicationArgumentIndex = outerArgument;
      if (!conceptInfo.compilerPrivate) {
        failure->context.insert(
            failure->context.begin(),
            {conceptInfo.qualifiedName, requirement.arguments,
             requirement.syntax != nullptr ? requirement.syntax->name.last()
                                           : conceptInfo.declaration->name()});
      }
      if (requirement.syntax != nullptr) {
        failure->constraintName = requirement.syntax->name;
        failure->application = requirement.syntax;
        failure->parameter =
            requirement.syntax->arguments.empty()
                ? requirement.syntax->name.last()
                : requirement.syntax->arguments.front().name.last();
      }
      return failure;
    }
    return std::nullopt;
  }

  const std::size_t count = std::min(requirement.arguments.size(),
                                     conceptInfo.parameterConstraints.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (const std::optional<GenericConstraintKind> failed =
            firstUnsatisfiedConstraint(
                requirement.arguments[index],
                conceptInfo.parameterConstraints[index])) {
      return ConstraintFailure{
          .parameter = requirement.syntax != nullptr &&
                               index < requirement.syntax->arguments.size()
                           ? requirement.syntax->arguments[index].name.last()
                           : Token{},
          .argument = requirement.arguments[index],
          .constraints = conceptInfo.parameterConstraints[index],
          .failed = *failed,
          .constraintName =
              requirement.syntax == nullptr
                  ? std::optional<NamePath>{}
                  : std::optional<NamePath>{requirement.syntax->name},
          .application = requirement.syntax,
          .context =
              conceptInfo.compilerPrivate
                  ? std::vector<SemanticConstraintContext>{}
                  : std::vector<
                        SemanticConstraintContext>{{conceptInfo.qualifiedName,
                                                    requirement.arguments,
                                                    requirement.syntax !=
                                                            nullptr
                                                        ? requirement.syntax
                                                              ->name.last()
                                                        : conceptInfo
                                                              .declaration
                                                              ->name()}},
          .leafApplication = requirement.syntax,
          .applicationArgumentIndex = index};
    }
  }
  AppliedConceptRequirement direct = requirement;
  direct.expression = nullptr;
  for (const ExpandedStructuralRequirement &structural :
       expandStructuralRequirements(
           std::span<const AppliedConceptRequirement>(&direct, 1))) {
    if (!structuralConstraintSatisfied(structural.kind, structural.arguments)) {
      return ConstraintFailure{
          .parameter = requirement.syntax == nullptr ? Token{}
                       : requirement.syntax->arguments.empty()
                           ? requirement.syntax->name.last()
                           : requirement.syntax->arguments.front().name.last(),
          .argument = structural.arguments.empty()
                          ? SemanticType::Unknown
                          : structural.arguments.front(),
          .constraintName =
              requirement.syntax == nullptr
                  ? std::optional<NamePath>{}
                  : std::optional<NamePath>{requirement.syntax->name},
          .structural = structural.kind,
          .application = requirement.syntax,
          .context =
              conceptInfo.compilerPrivate
                  ? std::vector<SemanticConstraintContext>{}
                  : std::vector<
                        SemanticConstraintContext>{{conceptInfo.qualifiedName,
                                                    requirement.arguments,
                                                    requirement.syntax !=
                                                            nullptr
                                                        ? requirement.syntax
                                                              ->name.last()
                                                        : conceptInfo
                                                              .declaration
                                                              ->name()}},
          .leafApplication = requirement.syntax};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::hasConstraintFailure(
    const ConstraintFailure &failure) -> bool {
  return failure.failed != GenericConstraintKind::None ||
         failure.structural != StructuralConstraintKind::None ||
         !failure.alternatives.empty();
}

auto SemanticVisitor::Impl::reportConstraintFailure(
    const Token &site, const ConstraintFailure &failure) -> void {
  reporter.reportConstraintFailure(
      site, failure,
      [&](const SemanticType &type) { return typeSpelling(type); });
}

auto SemanticVisitor::Impl::attachConstraintCallContext(
    ConstraintFailure &failure, const CallableCandidate &candidate,
    const std::vector<SemanticType> &argumentTypes,
    const ExprList &expressions) const -> void {
  if (candidate.declaration != nullptr) {
    failure.candidate = candidate.declaration->name();
  }
  // Both single-candidate checking and overload trials use this helper. The
  // former has already substituted its mutable candidate by the time a
  // trailing requirement fails, so inference origins come from the immutable
  // declaration catalog rather than that candidate's now-concrete types.
  const CallableDeclaration &declaration = candidate.record();
  GenericParameterId parameter = failure.parameterId;
  if (parameter == 0 && failure.applicationArgumentIndex) {
    const auto source = std::find_if(
        declaration.requirements.begin(), declaration.requirements.end(),
        [&](const auto &item) { return item.syntax == failure.application; });
    if (source != declaration.requirements.end() &&
        *failure.applicationArgumentIndex < source->arguments.size()) {
      const SemanticType &type =
          source->arguments[*failure.applicationArgumentIndex];
      if (type.kind == SemanticType::TypeParameter) {
        parameter = type.genericParameterId;
      }
    }
  }
  if (parameter == 0 ||
      std::none_of(declaration.genericParameters.begin(),
                   declaration.genericParameters.end(),
                   [&](const auto &item) { return item.id == parameter; })) {
    return;
  }
  const auto usesParameter = [&](const auto &self,
                                 const SemanticType &type) -> bool {
    if ((type.kind == SemanticType::TypeParameter ||
         type.kind == SemanticType::TypePack) &&
        type.genericParameterId == parameter) {
      return true;
    }
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const auto &argument) { return self(self, argument); });
  };
  const std::size_t fixed =
      declaration.parameterTypes.size() - (declaration.parameterPack ? 1 : 0);
  std::optional<std::size_t> origin;
  for (std::size_t index = 0;
       index < argumentTypes.size() && index < expressions.size(); ++index) {
    const std::size_t formal = declaration.parameterPack && index >= fixed
                                   ? declaration.parameterTypes.size() - 1
                                   : index;
    if (formal >= declaration.parameterTypes.size() ||
        !usesParameter(usesParameter, declaration.parameterTypes[formal])) {
      continue;
    }
    // A pack's failing concrete element can identify one written argument,
    // but a forwarded expansion cannot identify an individual source token.
    if (declaration.parameterPack && index >= fixed &&
        argumentTypes[index] != failure.argument) {
      continue;
    }
    if (origin || argumentTypes[index].kind == SemanticType::TypePack) {
      return;
    }
    origin = index;
  }
  if (origin) {
    failure.callArgumentIndex = *origin;
    failure.callArgument = expressionToken(expressions[*origin]);
    failure.callArgumentType = argumentTypes[*origin];
  }
}

auto SemanticVisitor::Impl::applyVariadicFunctionTypeArguments(
    CallableCandidate &function,
    const std::vector<SemanticType> &explicitTypeArguments,
    const std::vector<CompileTimeValue> &explicitValueArguments,
    const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
    const Token &paren) -> bool {
  if (!explicitValueArguments.empty()) {
    report(paren, "Variadic functions do not admit value generic arguments.",
           "GTI-S2026");
    return false;
  }
  CallableCandidate resolved;
  std::vector<SemanticType> resolvedTypeArguments;
  std::vector<CompileTimeValue> resolvedValueArguments;
  ConstraintFailure constraintFailure;
  if (tryInstantiateFunction(function, explicitTypeArguments,
                             explicitValueArguments, argumentTypes, arguments,
                             resolved, resolvedTypeArguments,
                             resolvedValueArguments, &constraintFailure)) {
    function = std::move(resolved);
    return true;
  }
  if (hasConstraintFailure(constraintFailure)) {
    reportConstraintFailure(paren, constraintFailure);
    return false;
  }

  bool valid = true;
  const GenericParameterInfo *pack = packGenericParameter(function);
  const std::size_t fixedGenerics =
      pack == nullptr ? function.record().genericParameters.size()
                      : function.record().genericParameters.size() - 1;
  if (pack == nullptr) {
    if (function.ownerClass != 0) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2005", DiagnosticPhase::Semantics, paren,
          "Call arguments must match the parameter pack prescribed by the "
          "receiver's class type; the class pack is not inferred at the call.");
      if (function.declaration != nullptr &&
          !function.declaration->parameters().empty()) {
        diagnostic.related.push_back(
            {tokenSpan(function.declaration->parameters().back().name),
             "Class-owned parameter pack is declared here."});
      }
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    report(paren,
           "A parameter pack requires a matching final generic type pack.",
           "GTI-S2023");
    return false;
  }
  if (!explicitTypeArguments.empty() &&
      explicitTypeArguments.size() < fixedGenerics) {
    report(paren,
           "Variadic function calls must explicitly provide every fixed "
           "generic type before the inferred pack.",
           "GTI-S2023");
    valid = false;
  }
  if (!acceptsArgumentShape(function, argumentTypes)) {
    report(paren,
           std::string(function.parameterPack
                           ? "Variadic function expects at least "
                           : "Function with type-pack metadata expects ") +
               std::to_string(fixedParameterCount(function)) +
               " fixed argument" +
               (fixedParameterCount(function) == 1 ? "" : "s") +
               (function.parameterPack ? " before its parameter pack." : "."),
           "GTI-S2005");
    valid = false;
  }

  GenericSubstitution substitution;
  if (!explicitTypeArguments.empty()) {
    const std::size_t count =
        std::min(fixedGenerics, explicitTypeArguments.size());
    for (std::size_t index = 0; index < count; ++index) {
      substitution.types.emplace(function.record().genericParameters[index].id,
                                 explicitTypeArguments[index]);
    }
  } else {
    const std::size_t count =
        std::min(fixedParameterCount(function), argumentTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
      valid = inferTypeArguments(
                  function.parameterTypes[index], argumentTypes[index],
                  function.record().genericParameters, substitution,
                  expressionToken(arguments[index]), &arguments[index]) &&
              valid;
    }
  }
  for (std::size_t index = 0; index < fixedGenerics; ++index) {
    const GenericParameterInfo &parameter =
        function.record().genericParameters[index];
    if (!substitution.types.contains(parameter.id)) {
      report(paren,
             "Cannot infer generic type parameter '" + parameter.name.lexeme +
                 "'; provide it explicitly before the type pack.",
             "GTI-S2023");
      valid = false;
    }
  }

  if (function.parameterPack && !explicitTypeArguments.empty() &&
      explicitTypeArguments.size() > fixedGenerics &&
      !hasPackExpansion(argumentTypes)) {
    const std::size_t explicitPackSize =
        explicitTypeArguments.size() - fixedGenerics;
    const std::size_t valuePackSize =
        argumentTypes.size() >= fixedParameterCount(function)
            ? argumentTypes.size() - fixedParameterCount(function)
            : 0;
    if (explicitPackSize != valuePackSize) {
      report(paren,
             "Explicit variadic type arguments must match the number of "
             "expanded value arguments.",
             "GTI-S2023");
      valid = false;
    }
  }
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    const SemanticType &argument = argumentTypes[index];
    if (argument == SemanticType::Void) {
      report(paren, "Variadic type packs cannot contain void.", "GTI-S2023");
      valid = false;
    }
  }
  if (valid) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2023", DiagnosticPhase::Semantics, paren,
                       "Cannot deduce one exact type pack from the supplied "
                       "type and value arguments.");
    diagnostic.related.push_back(
        {tokenSpan(pack->name), "Generic type pack declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  return false;
}

auto SemanticVisitor::Impl::inferTypeArguments(
    const SemanticType &pattern, const SemanticType &argument,
    const std::vector<GenericParameterInfo> &parameters,
    GenericSubstitution &substitution, const Token &argumentToken,
    const ExprPtr *expression) -> bool {
  GenericParameterId conflict = 0;
  bool valueConflict = false;
  if (tryInferTypeArguments(pattern, argument, parameters, substitution,
                            expression, &conflict, &valueConflict)) {
    return true;
  }
  const GenericParameterInfo *parameter =
      findGenericParameter(parameters, conflict);
  if (parameter != nullptr) {
    report(
        argumentToken,
        std::string(
            valueConflict
                ? "Conflicting lengths inferred for generic value parameter '"
                : "Conflicting types inferred for generic type parameter '") +
            parameter->name.lexeme + "'.",
        valueConflict     ? "GTI-S2026"
        : parameter->pack ? "GTI-S2023"
                          : "GTI-S2000");
  }
  return false;
}

auto SemanticVisitor::Impl::applyFunctionTypeArguments(
    CallableCandidate &function,
    const std::vector<SemanticType> &explicitTypeArguments,
    const std::vector<CompileTimeValue> &explicitValueArguments,
    const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
    const Token &paren) -> bool {
  if (function.parameterPack || packGenericParameter(function) != nullptr) {
    return applyVariadicFunctionTypeArguments(function, explicitTypeArguments,
                                              explicitValueArguments,
                                              argumentTypes, arguments, paren);
  }
  bool valid = true;
  if (function.record().genericParameters.empty()) {
    if (!explicitTypeArguments.empty() || !explicitValueArguments.empty()) {
      report(paren, "Non-generic functions do not take generic arguments.");
      valid = false;
    }
    return valid;
  }

  GenericSubstitution substitution;
  if (!explicitTypeArguments.empty() || !explicitValueArguments.empty()) {
    const std::size_t typeParameterCount =
        genericTypeParameterCount(function.record().genericParameters);
    if (explicitTypeArguments.size() > typeParameterCount) {
      report(paren,
             "Generic function called with the wrong number of type arguments.",
             "GTI-S2026");
      valid = false;
    }
    const std::size_t valueParameterCount =
        genericValueParameterCount(function.record().genericParameters);
    if (explicitValueArguments.size() > valueParameterCount) {
      report(paren,
             "Generic function called with the wrong number of explicit "
             "value arguments.",
             "GTI-S2026");
      valid = false;
    }
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter :
         function.record().genericParameters) {
      if (parameter.value) {
        if (valueIndex < explicitValueArguments.size()) {
          substitution.values.emplace(parameter.id,
                                      explicitValueArguments[valueIndex++]);
        }
        continue;
      }
      if (typeIndex >= explicitTypeArguments.size()) {
        continue;
      }
      substitution.types.emplace(parameter.id,
                                 explicitTypeArguments[typeIndex++]);
    }
    for (const GenericParameterInfo &parameter :
         function.record().genericParameters) {
      if (parameter.pack) {
        continue;
      }
      if (!parameter.value && !substitution.types.contains(parameter.id) &&
          !parameter.defaultType) {
        report(paren,
               "Generic function called with the wrong number of type "
               "arguments.",
               "GTI-S2026");
        valid = false;
      }
      if (parameter.value && !explicitValueArguments.empty() &&
          !substitution.values.contains(parameter.id) &&
          !parameter.defaultValue) {
        report(paren,
               "Generic function called with the wrong number of explicit "
               "value arguments.",
               "GTI-S2026");
        valid = false;
      }
    }
  }
  const std::size_t count =
      std::min(argumentTypes.size(), function.parameterTypes.size());
  const GenericSubstitution explicitSubstitution = substitution;
  for (std::size_t index = 0; index < count; ++index) {
    valid = inferTypeArguments(substituteType(function.parameterTypes[index],
                                              explicitSubstitution),
                               argumentTypes[index],
                               function.record().genericParameters,
                               substitution, expressionToken(arguments[index]),
                               &arguments[index]) &&
            valid;
  }

  valid = applyGenericDefaults(function.record().genericParameters,
                               substitution, paren, true) &&
          valid;

  for (const GenericParameterInfo &parameter :
       function.record().genericParameters) {
    const bool resolved = parameter.value
                              ? substitution.values.contains(parameter.id)
                              : substitution.types.contains(parameter.id);
    if (!resolved) {
      report(paren,
             "Cannot infer generic " +
                 std::string(parameter.value ? "value" : "type") +
                 " parameter '" + parameter.name.lexeme +
                 (parameter.value
                      ? "' from the supplied arguments and no default "
                        "is declared; provide a uint64_t compile-time value "
                        "explicitly."
                      : "' and no default is declared; provide explicit type "
                        "arguments."),
             parameter.value ? "GTI-S2026" : "GTI-S2023");
      valid = false;
    }
  }
  std::vector<SemanticType> resolvedTypeArguments;
  resolvedTypeArguments.reserve(
      genericTypeParameterCount(function.record().genericParameters));
  for (const GenericParameterInfo &parameter :
       function.record().genericParameters) {
    if (parameter.value) {
      continue;
    }
    if (const auto found = substitution.types.find(parameter.id);
        found != substitution.types.end()) {
      resolvedTypeArguments.emplace_back(found->second);
    }
  }
  if (resolvedTypeArguments.size() ==
      genericTypeParameterCount(function.record().genericParameters)) {
    if (std::optional<ConstraintFailure> failure = firstConstraintFailure(
            function.record().genericParameters, resolvedTypeArguments)) {
      attachConstraintCallContext(*failure, function, argumentTypes, arguments);
      reportConstraintFailure(paren, *failure);
      valid = false;
    }
  }
  function.returnType = substituteType(function.returnType, substitution);
  for (SemanticType &parameter : function.parameterTypes) {
    parameter = substituteType(parameter, substitution);
  }
  for (AppliedConceptRequirement &requirement : function.requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, substitution);
    }
  }
  if (resolvedTypeArguments.size() ==
      genericTypeParameterCount(function.record().genericParameters)) {
    if (std::optional<ConstraintFailure> failure =
            firstRequirementFailure(function.requirements)) {
      attachConstraintCallContext(*failure, function, argumentTypes, arguments);
      reportConstraintFailure(paren, *failure);
      valid = false;
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::tryInferTypeArguments(
    const SemanticType &pattern, const SemanticType &argument,
    const std::vector<GenericParameterInfo> &parameters,
    GenericSubstitution &substitution, const ExprPtr *expression,
    GenericParameterId *conflictingParameter, bool *conflictingValueParameter)
    -> bool {
  if (pattern.kind == SemanticType::Reference &&
      pattern.arguments.size() == 1) {
    const SemanticType &referent = argument.kind == SemanticType::Reference &&
                                           argument.arguments.size() == 1
                                       ? argument.arguments.front()
                                       : argument;
    return tryInferTypeArguments(pattern.arguments[0], referent, parameters,
                                 substitution, expression, conflictingParameter,
                                 conflictingValueParameter);
  }
  const GenericParameterInfo *typeParameter =
      pattern.kind == SemanticType::TypeParameter
          ? findGenericParameter(parameters, pattern.genericParameterId)
          : nullptr;
  if (typeParameter != nullptr && !typeParameter->value) {
    if (argument == SemanticType::Unknown) {
      return true;
    }
    const auto found = substitution.types.find(pattern.genericParameterId);
    if (found == substitution.types.end()) {
      substitution.types.emplace(pattern.genericParameterId, argument);
      return true;
    }
    if (found->second != argument) {
      if (conflictingParameter != nullptr) {
        *conflictingParameter = pattern.genericParameterId;
      }
      if (conflictingValueParameter != nullptr) {
        *conflictingValueParameter = false;
      }
      return false;
    }
    return true;
  }

  if (pattern.kind == SemanticType::Array) {
    std::optional<CompileTimeValue> inferredLength;
    if (argument.kind == SemanticType::Array &&
        argument.arrayLengthParameterId == 0) {
      inferredLength = CompileTimeValue::uint64(argument.arrayLength);
    } else if (argument.kind == SemanticType::Array &&
               argument.arrayLengthParameterId != 0) {
      inferredLength =
          CompileTimeValue::parameter(argument.arrayLengthParameterId);
    } else if (expression != nullptr && *expression != nullptr) {
      if (const auto *initializer =
              exprAs<ArrayInitializer>(expression->get())) {
        inferredLength =
            CompileTimeValue::uint64(initializer->elements().size());
      }
    }
    if (pattern.arrayLengthParameterId != 0 && inferredLength) {
      const GenericParameterInfo *parameter =
          findGenericParameter(parameters, pattern.arrayLengthParameterId);
      if (parameter != nullptr && parameter->value) {
        const CompileTimeValue value = *inferredLength;
        const auto found =
            substitution.values.find(pattern.arrayLengthParameterId);
        if (found == substitution.values.end()) {
          substitution.values.emplace(pattern.arrayLengthParameterId, value);
        } else if (found->second != value) {
          if (conflictingParameter != nullptr) {
            *conflictingParameter = pattern.arrayLengthParameterId;
          }
          if (conflictingValueParameter != nullptr) {
            *conflictingValueParameter = true;
          }
          return false;
        }
      }
    }
    if (argument.kind == SemanticType::Array && pattern.arguments.size() == 1 &&
        argument.arguments.size() == 1) {
      return tryInferTypeArguments(pattern.arguments.front(),
                                   argument.arguments.front(), parameters,
                                   substitution, nullptr, conflictingParameter,
                                   conflictingValueParameter);
    }
    return true;
  }

  if (pattern.kind != argument.kind ||
      pattern.nativeCScalar != argument.nativeCScalar ||
      pattern.classId != argument.classId ||
      pattern.enumId != argument.enumId ||
      pattern.signatureReceiver != argument.signatureReceiver ||
      pattern.arrayLength != argument.arrayLength ||
      pattern.arrayLengthParameterId != argument.arrayLengthParameterId ||
      pattern.valueArguments.size() != argument.valueArguments.size()) {
    return true;
  }
  for (std::size_t index = 0; index < pattern.valueArguments.size(); ++index) {
    const auto &formal = pattern.valueArguments[index];
    const auto &actual = argument.valueArguments[index];
    const auto *parameter =
        formal.kind == CompileTimeValue::Parameter
            ? findGenericParameter(parameters, formal.parameterId)
            : nullptr;
    if (!parameter || !parameter->value) {
      if (formal != actual)
        return true;
      continue;
    }
    if (actual.kind == CompileTimeValue::Unknown)
      continue;
    const auto [found, inserted] =
        substitution.values.emplace(parameter->id, actual);
    if (!inserted && found->second != actual) {
      if (conflictingParameter)
        *conflictingParameter = parameter->id;
      if (conflictingValueParameter)
        *conflictingValueParameter = true;
      return false;
    }
  }
  const GenericParameterInfo *tail = nullptr;
  if ((pattern.kind == SemanticType::Class ||
       pattern.kind == SemanticType::Enum ||
       pattern.kind == SemanticType::FunctionSignature) &&
      !pattern.arguments.empty() &&
      pattern.arguments.back().kind == SemanticType::TypePack &&
      !pattern.arguments.back().concretePack) {
    tail = findGenericParameter(parameters,
                                pattern.arguments.back().genericParameterId);
    if (tail != nullptr && !tail->pack) {
      tail = nullptr;
    }
  }
  const std::size_t fixed =
      pattern.arguments.size() - (tail != nullptr ? 1 : 0);
  if (argument.arguments.size() < fixed ||
      (tail == nullptr && argument.arguments.size() != fixed)) {
    return true;
  }
  for (std::size_t index = 0; index < fixed; ++index) {
    if (!tryInferTypeArguments(pattern.arguments[index],
                               argument.arguments[index], parameters,
                               substitution, nullptr, conflictingParameter,
                               conflictingValueParameter)) {
      return false;
    }
  }
  if (tail != nullptr) {
    std::vector<SemanticType> elements(argument.arguments.begin() +
                                           static_cast<std::ptrdiff_t>(fixed),
                                       argument.arguments.end());
    SemanticType inferred =
        elements.size() == 1 &&
                elements.front().kind == SemanticType::TypePack &&
                !elements.front().concretePack
            ? elements.front()
            : SemanticType::concreteTypePack(tail->id, std::move(elements));
    const auto [found, inserted] =
        substitution.types.emplace(tail->id, inferred);
    if (!inserted && found->second != inferred) {
      if (conflictingParameter != nullptr) {
        *conflictingParameter = tail->id;
      }
      if (conflictingValueParameter != nullptr) {
        *conflictingValueParameter = false;
      }
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::tryInstantiateFunction(
    const CallableCandidate &candidate,
    const std::vector<SemanticType> &explicitTypeArguments,
    const std::vector<CompileTimeValue> &explicitValueArguments,
    const std::vector<SemanticType> &argumentTypes, const ExprList &expressions,
    CallableCandidate &resolved,
    std::vector<SemanticType> &resolvedTypeArguments,
    std::vector<CompileTimeValue> &resolvedValueArguments,
    ConstraintFailure *constraintFailure) -> bool {
  // A class-owned pack is prescribed by the receiver, not deduced from this
  // call. Group the prescribed parameters by source argument expression so
  // the ordinary exact argument/ownership checks can also check forwarding.
  if (candidate.parameterPack && candidate.ownerClass != 0 &&
      packGenericParameter(candidate) == nullptr &&
      !candidate.parameterTypes.empty()) {
    const SemanticType &declaredPack = candidate.parameterTypes.back();
    if (declaredPack.kind != SemanticType::TypePack) {
      return false;
    }
    const std::size_t fixed = fixedParameterCount(candidate);
    if (argumentTypes.size() < fixed) {
      return false;
    }
    std::vector<SemanticType> elements;
    SemanticType::appendExpandedArgument(elements, declaredPack);
    CallableCandidate prescribed = candidate;
    prescribed.parameterTypes.resize(fixed);
    std::size_t element = 0;
    for (std::size_t index = fixed; index < argumentTypes.size(); ++index) {
      const SemanticType &argument = argumentTypes[index];
      if (argument.kind == SemanticType::TypePack) {
        if (!argument.concretePack) {
          if (index + 1 != argumentTypes.size() ||
              element + 1 != elements.size() || elements[element] != argument) {
            return false;
          }
          prescribed.parameterTypes.push_back(elements[element++]);
        } else {
          std::vector<SemanticType> actualElements;
          SemanticType::appendExpandedArgument(actualElements, argument);
          if (actualElements.size() > elements.size() - element) {
            return false;
          }
          std::vector<SemanticType> group(
              elements.begin() + static_cast<std::ptrdiff_t>(element),
              elements.begin() +
                  static_cast<std::ptrdiff_t>(element + actualElements.size()));
          prescribed.parameterTypes.push_back(SemanticType::concreteTypePack(
              argument.genericParameterId, std::move(group)));
          element += actualElements.size();
        }
      } else {
        if (element == elements.size() ||
            elements[element].kind == SemanticType::TypePack) {
          return false;
        }
        prescribed.parameterTypes.push_back(elements[element++]);
      }
    }
    if (element != elements.size()) {
      return false;
    }
    prescribed.parameterPack = false;
    prescribed.requiredParameterCount = prescribed.parameterTypes.size();
    return tryInstantiateFunction(prescribed, explicitTypeArguments,
                                  explicitValueArguments, argumentTypes,
                                  expressions, resolved, resolvedTypeArguments,
                                  resolvedValueArguments, constraintFailure);
  }
  resolved = candidate;
  resolvedTypeArguments.clear();
  resolvedValueArguments.clear();
  if (candidate.record().genericParameters.empty()) {
    if (!explicitTypeArguments.empty() || !explicitValueArguments.empty() ||
        !acceptsArgumentShape(candidate, argumentTypes)) {
      return false;
    }
    if (const std::optional<ConstraintFailure> failure =
            firstRequirementFailure(candidate.requirements)) {
      if (constraintFailure != nullptr) {
        *constraintFailure = *failure;
        attachConstraintCallContext(*constraintFailure, candidate,
                                    argumentTypes, expressions);
      }
      return false;
    }
    return true;
  }

  GenericSubstitution substitution;
  const GenericParameterInfo *pack = packGenericParameter(candidate);
  const std::size_t fixedGenerics =
      pack == nullptr
          ? genericTypeParameterCount(candidate.record().genericParameters)
          : candidate.record().genericParameters.size() - 1;
  if ((candidate.parameterPack && pack == nullptr) ||
      !acceptsArgumentShape(candidate, argumentTypes)) {
    return false;
  }
  if (!explicitTypeArguments.empty() || !explicitValueArguments.empty()) {
    if (pack == nullptr && explicitTypeArguments.size() > fixedGenerics) {
      return false;
    }
    if (pack != nullptr && explicitTypeArguments.size() < fixedGenerics) {
      return false;
    }
    const std::size_t valueGenerics =
        genericValueParameterCount(candidate.record().genericParameters);
    if ((explicitValueArguments.size() > valueGenerics) ||
        (pack != nullptr && !explicitValueArguments.empty())) {
      return false;
    }
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter :
         candidate.record().genericParameters) {
      if (parameter.value) {
        if (valueIndex < explicitValueArguments.size()) {
          substitution.values.emplace(parameter.id,
                                      explicitValueArguments[valueIndex++]);
        }
        continue;
      }
      if (parameter.pack) {
        if (typeIndex < explicitTypeArguments.size()) {
          std::vector<SemanticType> elements(
              explicitTypeArguments.begin() +
                  static_cast<std::ptrdiff_t>(typeIndex),
              explicitTypeArguments.end());
          substitution.types.emplace(parameter.id,
                                     SemanticType::concreteTypePack(
                                         parameter.id, std::move(elements)));
        }
        break;
      }
      if (typeIndex >= explicitTypeArguments.size()) {
        continue;
      }
      substitution.types.emplace(parameter.id,
                                 explicitTypeArguments[typeIndex++]);
    }
    for (const GenericParameterInfo &parameter :
         candidate.record().genericParameters) {
      if (parameter.pack) {
        continue;
      }
      if (!parameter.value && !substitution.types.contains(parameter.id) &&
          !parameter.defaultType) {
        return false;
      }
      if (parameter.value && !explicitValueArguments.empty() &&
          !substitution.values.contains(parameter.id) &&
          !parameter.defaultValue) {
        return false;
      }
    }
  }
  const std::size_t count =
      std::min(fixedParameterCount(candidate), argumentTypes.size());
  for (std::size_t index = 0; index < count; ++index) {
    const ExprPtr *expression =
        index < expressions.size() ? &expressions[index] : nullptr;
    if (!tryInferTypeArguments(
            candidate.parameterTypes[index], argumentTypes[index],
            candidate.record().genericParameters, substitution, expression)) {
      return false;
    }
  }

  if (!applyGenericDefaults(candidate.record().genericParameters, substitution,
                            candidate.record().genericParameters.front().name,
                            false)) {
    return false;
  }

  for (const GenericParameterInfo &parameter :
       candidate.record().genericParameters) {
    if (parameter.pack) {
      continue;
    }
    if (parameter.value) {
      const auto found = substitution.values.find(parameter.id);
      if (found == substitution.values.end()) {
        return false;
      }
      resolvedValueArguments.emplace_back(found->second);
    } else {
      const auto found = substitution.types.find(parameter.id);
      if (found == substitution.types.end()) {
        return false;
      }
      resolvedTypeArguments.emplace_back(found->second);
    }
  }

  std::vector<SemanticType> packTypes;
  std::vector<SemanticType> resolvedPackParameters;
  if (pack != nullptr && !candidate.parameterPack) {
    const auto inferred = substitution.types.find(pack->id);
    if (inferred == substitution.types.end()) {
      return false;
    }
    SemanticType::appendExpandedArgument(packTypes, inferred->second);
  } else if (pack != nullptr) {
    const std::size_t fixedValues = fixedParameterCount(candidate);
    if (hasPackExpansion(argumentTypes)) {
      if (explicitTypeArguments.size() > fixedGenerics) {
        return false;
      }
      resolvedPackParameters.assign(
          argumentTypes.begin() + static_cast<std::ptrdiff_t>(fixedValues),
          argumentTypes.end());
      for (const SemanticType &argument : resolvedPackParameters) {
        if (argument.kind == SemanticType::TypePack && argument.concretePack) {
          packTypes.insert(packTypes.end(), argument.arguments.begin(),
                           argument.arguments.end());
        } else {
          packTypes.emplace_back(argument);
        }
      }
    } else if (explicitTypeArguments.size() > fixedGenerics) {
      packTypes.assign(explicitTypeArguments.begin() +
                           static_cast<std::ptrdiff_t>(fixedGenerics),
                       explicitTypeArguments.end());
      if (packTypes.size() != argumentTypes.size() - fixedValues) {
        return false;
      }
      resolvedPackParameters = packTypes;
    } else {
      packTypes.assign(argumentTypes.begin() +
                           static_cast<std::ptrdiff_t>(fixedValues),
                       argumentTypes.end());
      resolvedPackParameters = packTypes;
    }
    const auto inferred = substitution.types.find(pack->id);
    if (inferred != substitution.types.end()) {
      std::vector<SemanticType> prescribed;
      SemanticType::appendExpandedArgument(prescribed, inferred->second);
      if (prescribed != packTypes) {
        return false;
      }
    } else {
      substitution.types.emplace(
          pack->id, packTypes.size() == 1 &&
                            packTypes.front().kind == SemanticType::TypePack &&
                            !packTypes.front().concretePack
                        ? packTypes.front()
                        : SemanticType::concreteTypePack(pack->id, packTypes));
    }
  }
  resolvedTypeArguments.insert(resolvedTypeArguments.end(), packTypes.begin(),
                               packTypes.end());
  if (candidate.parameterPack && std::any_of(packTypes.begin(), packTypes.end(),
                                             [](const SemanticType &argument) {
                                               return argument ==
                                                      SemanticType::Void;
                                             })) {
    return false;
  }
  if (const std::optional<ConstraintFailure> failure = firstConstraintFailure(
          candidate.record().genericParameters, resolvedTypeArguments)) {
    if (constraintFailure != nullptr) {
      *constraintFailure = *failure;
      attachConstraintCallContext(*constraintFailure, candidate, argumentTypes,
                                  expressions);
    }
    return false;
  }
  resolved.returnType = substituteType(resolved.returnType, substitution);
  std::vector<SemanticType> resolvedParameters;
  resolvedParameters.reserve(fixedParameterCount(candidate) + packTypes.size());
  for (std::size_t index = 0; index < fixedParameterCount(candidate); ++index) {
    resolvedParameters.emplace_back(
        substituteType(candidate.parameterTypes[index], substitution));
  }
  resolvedParameters.insert(resolvedParameters.end(),
                            resolvedPackParameters.begin(),
                            resolvedPackParameters.end());
  resolved.parameterTypes = std::move(resolvedParameters);
  for (AppliedConceptRequirement &requirement : resolved.requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, substitution);
    }
  }
  if (const std::optional<ConstraintFailure> failure =
          firstRequirementFailure(resolved.requirements)) {
    if (constraintFailure != nullptr) {
      *constraintFailure = *failure;
      attachConstraintCallContext(*constraintFailure, candidate, argumentTypes,
                                  expressions);
    }
    return false;
  }
  // A concrete pack expansion replaces the symbolic final parameter with
  // zero or more ordinary parameters. Defaults and packs cannot coexist, so
  // every concrete parameter remains required after this substitution.
  if (candidate.record().parameterPack) {
    resolved.requiredParameterCount = resolved.parameterTypes.size();
  }
  resolved.parameterPack = false;

  const FunctionInfo *declarationInfo = semanticModel.findFunction(resolved.id);
  if (declarationInfo == nullptr && instanceBaseModel != nullptr) {
    declarationInfo = instanceBaseModel->findFunction(resolved.id);
  }
  if (declarationInfo != nullptr) {
    FunctionInfo concrete = *declarationInfo;
    concrete.returnType = resolved.returnType;
    concrete.parameterTypes = resolved.parameterTypes;
    if (const std::optional<FunctionReturnBorrowSummary> dynamicText =
            dynamicTextReturnBorrowSummary(concrete)) {
      resolved.returnBorrowOrigin = dynamicText->origin;
      resolved.returnBorrowParameter = dynamicText->parameter;
      resolved.returnBorrowAccess = dynamicText->access;
      resolved.returnBorrowPlace = dynamicText->place;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::containsLambdaType(const SemanticType &type)
    -> bool {
  if (type.kind == SemanticType::Lambda) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsLambdaType);
}

// A raw pointer transports an address, not the closure object it may name.
// It holds no capture and invokes nothing; reaching the object again is an
// explicit unsafe obligation (`assume_ref`/`assume_mut`) whose checked borrow
// is tied to an owner. So it creates no callable argument boundary, while a
// reference to a closure-bearing object still does.
auto SemanticVisitor::Impl::containsClosureObjectType(const SemanticType &type)
    -> bool {
  if (type.kind == SemanticType::Lambda) {
    return true;
  }
  return type.kind != SemanticType::RawPointer &&
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsClosureObjectType);
}

auto SemanticVisitor::Impl::containsExactType(const SemanticType &type,
                                              const SemanticType &candidate)
    -> bool {
  if (candidate != SemanticType::Unknown && type == candidate) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsExactType(argument, candidate);
                     });
}

auto SemanticVisitor::Impl::containsDisallowedCallableCaptureState(
    const SemanticType &type, std::unordered_set<ClassId> &visiting,
    bool reviewedRawState) const -> bool {
  if (type.kind == SemanticType::FunctionItem) {
    // Parameters and results describe the selected target; they are not
    // stored state in the zero-state function-item value.
    return false;
  }
  if (type.kind == SemanticType::RawPointer)
    return !reviewedRawState;
  if (type.kind == SemanticType::Reference ||
      type.kind == SemanticType::StringView) {
    return true;
  }
  if (type.kind == SemanticType::Lambda) {
    return type.hasLambdaShape() &&
           std::any_of(type.lambdaCaptureTypes().begin(),
                       type.lambdaCaptureTypes().end(),
                       [&](const SemanticType &capture) {
                         return containsDisallowedCallableCaptureState(
                             capture, visiting, reviewedRawState);
                       });
  }
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size()) {
    const ClassInfo &owner = classInfo(type.classId);
    // A reviewed transfer assertion covers the wrapper's raw implementation
    // state, not just native thread entry. Reopening that implementation here
    // would reject source-defined owners which safely move their allocations.
    // It cannot erase a tracked borrow or make a false conditional assertion
    // true; unasserted raw fields still reach the conservative check below.
    if (owner.transferPolicy == ConcurrencyCapabilityPolicy::UnsafeAsserted ||
        owner.transferPolicy ==
            ConcurrencyCapabilityPolicy::UnsafeConditional) {
      const auto traits = typeTraits(type);
      if (traits.transferCapable && !traits.containsBorrowedState)
        reviewedRawState = true;
    }
    if (!visiting.insert(type.classId).second) {
      return false;
    }
    const GenericSubstitution substitution = classSubstitution(type);
    bool found = false;
    if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
      found = containsDisallowedCallableCaptureState(
          substituteType(base->type, substitution), visiting, reviewedRawState);
    }
    for (const FieldInfo &field : owner.fields) {
      if (found || field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end()) {
        found = containsDisallowedCallableCaptureState(
            substituteType(member->second.symbol.type, substitution), visiting,
            reviewedRawState);
      }
    }
    visiting.erase(type.classId);
    return found;
  }
  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size() && enums[type.enumId - 1].payload) {
    const auto concrete = enumTypeFor(type);
    if (!concrete)
      return false;
    return std::any_of(concrete->enumerators.begin(),
                       concrete->enumerators.end(), [&](const auto &entry) {
                         return std::any_of(
                             entry.payloadTypes.begin(),
                             entry.payloadTypes.end(),
                             [&](const SemanticType &payload) {
                               return containsDisallowedCallableCaptureState(
                                   payload, visiting, reviewedRawState);
                             });
                       });
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsDisallowedCallableCaptureState(
                           argument, visiting, reviewedRawState);
                     });
}

auto SemanticVisitor::Impl::containsDisallowedCallableCaptureState(
    const SemanticType &type) const -> bool {
  std::unordered_set<ClassId> visiting;
  return containsDisallowedCallableCaptureState(type, visiting);
}

auto SemanticVisitor::Impl::movedFunctionParameter(
    const ExprPtr &expression, const FunctionInfo &function) const
    -> std::optional<std::size_t> {
  if (function.declaration == nullptr) {
    return std::nullopt;
  }
  const ExprPtr *moved = explicitMoveSource(expression);
  const auto *variable =
      moved == nullptr ? nullptr : exprAs<Variable>(moved->get());
  const SymbolId symbol =
      variable == nullptr ? 0 : semanticModel.findResolvedSymbol(*variable);
  if (symbol == 0) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < function.declaration->parameters().size();
       ++index) {
    const BindingInfo *binding =
        semanticModel.findBinding(function.declaration->parameters()[index]);
    if (binding != nullptr && binding->symbol == symbol) {
      return index;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::exactMovedReturnParameter(
    const FunctionInfo &function) const -> std::optional<std::size_t> {
  if (function.declaration == nullptr ||
      function.declaration->body() == nullptr) {
    return std::nullopt;
  }
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.declaration->body().get(), returns);
  if (returns.empty()) {
    return std::nullopt;
  }
  std::optional<std::size_t> result;
  for (const ExprPtr *returned : returns) {
    if (returned == nullptr || !*returned) {
      return std::nullopt;
    }
    const std::optional<std::size_t> parameter =
        movedFunctionParameter(*returned, function);
    if (!parameter || (result && *result != *parameter)) {
      return std::nullopt;
    }
    result = parameter;
  }
  return result;
}

auto SemanticVisitor::Impl::constructionArguments(const ExprPtr &expression)
    -> const ExprList * {
  if (const auto *call = exprAs<Call>(expression.get())) {
    return &call->arguments();
  }
  if (const auto *initializer = exprAs<DirectInitializer>(expression.get())) {
    return &initializer->arguments();
  }
  return nullptr;
}

auto SemanticVisitor::Impl::exactOwnedFieldTransport(
    const FunctionInfo &function, std::size_t parameterIndex,
    const ExprPtr &expression) const -> std::optional<CallableOwnedTransport> {
  if (function.returnType.kind != SemanticType::Class ||
      parameterIndex >= function.parameterTypes.size()) {
    return std::nullopt;
  }
  const ExprList *arguments = constructionArguments(expression);
  const ResolvedConstructionInfo *construction =
      expression ? semanticModel.findConstruction(*expression) : nullptr;
  if (arguments == nullptr || arguments->size() != 1 ||
      construction == nullptr || construction->declaration == nullptr ||
      construction->constructedType != function.returnType ||
      construction->parameterTypes.size() != 1 ||
      movedFunctionParameter(arguments->front(), function) != parameterIndex ||
      construction->parameterTypes.front() !=
          function.parameterTypes[parameterIndex]) {
    return std::nullopt;
  }

  const ClassInfo &owner = classInfo(function.returnType.classId);
  if (owner.fields.size() != 1) {
    return std::nullopt;
  }
  const GenericSubstitution substitution =
      classSubstitution(function.returnType);
  for (const ConstructorInitializer &initializer :
       construction->declaration->initializers()) {
    const ResolvedConstructorInitializerInfo *resolved =
        semanticModel.findConstructorInitializer(initializer);
    if (resolved == nullptr ||
        resolved->kind != ConstructorInitializerTargetKind::Field ||
        resolved->ownedParameter != std::optional<std::size_t>{0} ||
        resolved->field == 0 ||
        substituteType(resolved->targetType, substitution) !=
            function.parameterTypes[parameterIndex]) {
      continue;
    }
    return CallableOwnedTransport{.kind =
                                      CallableOwnedTransportKind::ExactField,
                                  .destinationType = function.returnType,
                                  .field = resolved->field};
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::discoverOwnedCallableContracts() -> void {
  struct PendingOwnedContract {
    const FunctionDecl *declaration = nullptr;
    CallableParameterContract contract;
  };
  std::vector<PendingOwnedContract> pending;
  for (const auto &[_, function] : semanticModel.functions) {
    if (function.declaration == nullptr || function.ownerClass != 0 ||
        function.declaration->body() == nullptr ||
        !finalizesFunction(function)) {
      continue;
    }
    const ReturnStmt *soleReturn =
        function.declaration->body()->statements().size() == 1
            ? stmtAs<ReturnStmt>(
                  function.declaration->body()->statements().front().get())
            : nullptr;
    const std::optional<std::size_t> exactReturn =
        exactMovedReturnParameter(function);
    for (std::size_t index = 0;
         index < function.parameterTypes.size() &&
         index < function.declaration->parameters().size();
         ++index) {
      const Parameter &parameter = function.declaration->parameters()[index];
      const SemanticType &parameterType = function.parameterTypes[index];
      if (parameterType.kind != SemanticType::TypeParameter ||
          parameter.type.reference || parameter.pack) {
        continue;
      }

      std::optional<CallableOwnedTransport> transport;
      if (function.returnType == parameterType &&
          exactReturn == std::optional<std::size_t>{index}) {
        transport = CallableOwnedTransport{
            .kind = CallableOwnedTransportKind::ExactReturn,
            .destinationType = function.returnType};
      } else if (parameter.mutability == Mutability::Immutable &&
                 soleReturn != nullptr && soleReturn->value()) {
        transport =
            exactOwnedFieldTransport(function, index, soleReturn->value());
      }
      if (!transport) {
        continue;
      }
      pending.push_back(
          {.declaration = function.declaration,
           .contract = CallableParameterContract{
               .parameterIndex = index,
               .genericParameter = parameterType.genericParameterId,
               .callableType = parameterType,
               .access = parameter.mutability == Mutability::Mutable
                             ? AccessMode::Mutable
                             : AccessMode::ReadOnly,
               .boundary = CallableBoundary::Owned,
               .ownedTransport = std::move(transport)}});
    }
  }
  for (PendingOwnedContract &contract : pending) {
    semanticModel.recordCallableRequirement(*contract.declaration,
                                            std::move(contract.contract));
  }
}

auto SemanticVisitor::Impl::discoverOwnedCallableValueParameters() -> void {
  for (auto &[_, function] : semanticModel.functions) {
    if (function.declaration == nullptr || !function.declaration->body() ||
        !finalizesFunction(function))
      continue;
    for (std::size_t index = 0; index < function.parameterTypes.size();
         ++index) {
      if (index >= function.declaration->parameters().size())
        continue;
      const auto &parameter = function.declaration->parameters()[index];
      const auto &type = function.parameterTypes[index];
      if ((type.kind != SemanticType::TypeParameter &&
           !(parameter.pack && type.kind == SemanticType::TypePack) &&
           !(type.kind == SemanticType::Class &&
             containsOpenTypeParameter(type))) ||
          parameter.type.reference ||
          callableParameterContract(function.id, index) != nullptr)
        continue;
      // Confined invocation/forwarding contracts have already reached their
      // fixed point. An otherwise ordinary by-value parameter is not confined:
      // its concrete instance must satisfy the normal ownership rules.
      semanticModel.recordCallableRequirement(
          *function.declaration,
          {.parameterIndex = index,
           .genericParameter = type.genericParameterId,
           .callableType = type,
           .access = parameter.mutability == Mutability::Mutable
                         ? AccessMode::Mutable
                         : AccessMode::ReadOnly,
           .boundary = CallableBoundary::Owned,
           .ownedTransport =
               CallableOwnedTransport{.kind = CallableOwnedTransportKind::Value,
                                      .destinationType = type}});
    }
  }
}

auto SemanticVisitor::Impl::ownedTransportMatchesConcrete(
    const FunctionInfo &function, const CallableParameterContract &contract,
    const ResolvedCallInfo &call) -> bool {
  std::vector<SemanticType> parameterTypes;
  for (const SemanticType &type : call.parameterTypes)
    SemanticType::appendExpandedArgument(parameterTypes, type);
  if (!contract.ownedTransport ||
      contract.parameterIndex >= parameterTypes.size()) {
    return false;
  }
  GenericSubstitution substitution;
  if (function.ownerClass != 0 &&
      call.dispatchOwner.kind == SemanticType::Class) {
    substitution = classSubstitution(call.dispatchOwner);
  }
  std::size_t typeIndex = 0;
  for (const GenericParameterInfo &parameter : function.genericParameters) {
    if (!parameter.value && typeIndex < call.typeArguments.size()) {
      substitution.types.emplace(parameter.id, call.typeArguments[typeIndex++]);
    }
  }
  const SemanticType destination =
      substituteType(contract.ownedTransport->destinationType, substitution);
  const SemanticType &callableType = parameterTypes[contract.parameterIndex];
  if (contract.ownedTransport->kind == CallableOwnedTransportKind::Value)
    return contract.ownedTransport->field == 0 && destination == callableType;
  if (contract.ownedTransport->kind == CallableOwnedTransportKind::ThreadTask) {
    const SemanticTypeTraits traits = typeTraits(callableType);
    return contract.ownedTransport->field == 0 &&
           isTransferableThreadResult(destination) &&
           isExactThreadTaskType(callableType, destination) && traits.movable &&
           traits.transferCapable && !traits.containsBorrowedState &&
           !containsDisallowedCallableCaptureState(callableType);
  }
  if (destination != call.returnType) {
    return false;
  }
  if (contract.ownedTransport->kind ==
      CallableOwnedTransportKind::ExactReturn) {
    return destination == callableType && contract.ownedTransport->field == 0;
  }
  if (destination.kind != SemanticType::Class ||
      contract.ownedTransport->field == 0) {
    return false;
  }
  const ClassInfo &owner = classInfo(destination.classId);
  const GenericSubstitution classArguments = classSubstitution(destination);
  return std::any_of(owner.fields.begin(), owner.fields.end(),
                     [&](const FieldInfo &field) {
                       if (field.declaration == nullptr) {
                         return false;
                       }
                       const auto member =
                           owner.members.find(field.declaration->name().lexeme);
                       return member != owner.members.end() &&
                              toolingSymbolFor(member->second.symbol) ==
                                  contract.ownedTransport->field &&
                              substituteType(member->second.symbol.type,
                                             classArguments) == callableType;
                     });
}

auto SemanticVisitor::Impl::validateOwnedCallableArguments() -> void {
  std::vector<Diagnostic> ownedDiagnostics;
  for (const auto &[call, resolved] : semanticModel.calls) {
    if (call == nullptr || !finalizesCall(call)) {
      continue;
    }
    const FunctionInfo *target = semanticModel.findFunction(resolved.function);
    if (target == nullptr || target->declaration == nullptr) {
      continue;
    }
    std::vector<SemanticType> argumentTypes;
    for (const ExprPtr &argument : call->arguments())
      argumentTypes.push_back(semanticModel.typeOf(*argument));
    const auto arguments =
        concreteCallArguments(argumentTypes, call->arguments());
    if (!arguments)
      continue;
    for (const CallableParameterContract &contract :
         target->instantiatedCallableParameters(resolved.parameterTypes)) {
      const bool threadTask =
          contract.ownedTransport && contract.ownedTransport->kind ==
                                         CallableOwnedTransportKind::ThreadTask;
      const SemanticType *argumentType = &contract.callableType;
      const bool callableTransport =
          contract.ownedTransport && argumentType != nullptr &&
          (threadTask || !contract.signatures.empty() ||
           containsLambdaType(*argumentType));
      if (!callableTransport || contract.parameterIndex >= arguments->size()) {
        continue;
      }
      const AnalyzedCallArgument &input = (*arguments)[contract.parameterIndex];
      const ExprPtr &argument = *input.expression;
      const SemanticType &concreteArgumentType = contract.callableType;
      const std::size_t declarationIndex =
          target->parameterPack
              ? std::min(contract.parameterIndex,
                         target->declaration->parameters().size() - 1)
              : contract.parameterIndex;
      const SemanticTypeTraits traits = typeTraits(concreteArgumentType);
      const ExpressionInfo *argumentInfo =
          semanticModel.findExpression(*argument);
      const bool consumed = input.forwardedPackElement ||
                            explicitMoveSource(argument) != nullptr ||
                            (argumentInfo != nullptr &&
                             argumentInfo->category == ValueCategory::Value);
      if (threadTask && consumed &&
          !ownedTransportMatchesConcrete(*target, contract, resolved)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2084", DiagnosticPhase::Semantics, expressionToken(argument),
            std::string(contract.ownedTransport->destinationType ==
                                SemanticType::Void
                            ? "A managed thread requires one consumed, "
                              "transfer-safe exact void() task."
                            : "A managed thread requires one consumed, "
                              "transfer-safe exact task whose return type "
                              "matches its transferable result."));
        diagnostic.related.push_back(
            {tokenSpan(target->declaration->parameters()[declarationIndex]
                           .sourceAnchor()),
             "Managed task parameter declared here."});
        diagnostic.hints.emplace_back(
            "Pass a fresh task value or std::move(task); captures must own "
            "their state and cannot contain references, tracked borrows, "
            "or unreviewed raw pointers.");
        ownedDiagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      std::string failure;
      if (!consumed) {
        failure = "requires an explicit std::move ownership transfer";
      } else if (!traits.movable) {
        failure = "is not movable";
      } else if (traits.containsBorrowedState ||
                 (!threadTask && containsDisallowedCallableCaptureState(
                                     concreteArgumentType))) {
        failure = "contains borrowed or raw-pointer capture state";
      } else if (!ownedTransportMatchesConcrete(*target, contract, resolved)) {
        failure = "does not preserve the callable's exact concrete type in "
                  "its declared owner";
      }
      if (failure.empty()) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics, expressionToken(argument),
          "Owned callable argument " +
              std::to_string(contract.parameterIndex + 1) + " " + failure +
              ".");
      diagnostic.related.push_back(
          {tokenSpan(target->declaration->parameters()[declarationIndex]
                         .sourceAnchor()),
           "Exact owned callable parameter declared here."});
      diagnostic.hints.emplace_back(
          "Move an exact movable callable whose stored state contains no "
          "references or tracked borrowed state. Raw implementation state "
          "must be encapsulated by an owner with a valid unsafe transfer "
          "contract; direct raw-pointer captures are not supported.");
      ownedDiagnostics.emplace_back(std::move(diagnostic));
    }
  }
  std::sort(ownedDiagnostics.begin(), ownedDiagnostics.end(),
            [](const Diagnostic &left, const Diagnostic &right) {
              if (left.primary.source != right.primary.source) {
                return left.primary.source < right.primary.source;
              }
              if (left.primary.start != right.primary.start) {
                return left.primary.start < right.primary.start;
              }
              if (left.primary.end != right.primary.end) {
                return left.primary.end < right.primary.end;
              }
              return left.message < right.message;
            });
  for (Diagnostic &diagnostic : ownedDiagnostics) {
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::validateProvisionalCallableArguments() -> void {
  std::vector<Diagnostic> argumentDiagnostics;
  for (const auto &[call, resolved] : semanticModel.calls) {
    if (call == nullptr || resolved.callableArguments.empty() ||
        !finalizesCall(call)) {
      continue;
    }
    const FunctionInfo *target = semanticModel.findFunction(resolved.function);
    for (const CallableArgumentBoundary &argument :
         resolved.callableArguments) {
      const bool provenBoundary =
          target != nullptr &&
          callableParameterContract(target->id, argument.parameterIndex) !=
              nullptr;
      if (provenBoundary ||
          argument.parameterIndex >= call->arguments().size()) {
        continue;
      }

      const bool escapingReturn = containsLambdaType(resolved.returnType);
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics,
          expressionToken(call->arguments()[argument.parameterIndex]),
          escapingReturn
              ? "Lambda argument " +
                    std::to_string(argument.parameterIndex + 1) +
                    " would escape through the function return value."
              : "Lambda argument " +
                    std::to_string(argument.parameterIndex + 1) +
                    " cannot bind because the selected target parameter is "
                    "not proven confined or owned.");
      if (target != nullptr && target->declaration != nullptr &&
          argument.parameterIndex < target->declaration->parameters().size()) {
        diagnostic.related.push_back(
            {tokenSpan(
                 target->declaration->parameters()[argument.parameterIndex]
                     .name),
             "Selected generic parameter declared here."});
      }
      diagnostic.hints.emplace_back(
          escapingReturn
              ? "Return a callable only through an exact owned generic "
                "transport using std::move."
              : "Pass a lambda to a direct generic value or checked-reference "
                "parameter with a visible, proven confined contract; exact "
                "owned transport requires a value parameter.");
      argumentDiagnostics.emplace_back(std::move(diagnostic));
    }
  }

  std::sort(argumentDiagnostics.begin(), argumentDiagnostics.end(),
            [](const Diagnostic &left, const Diagnostic &right) {
              if (left.primary.source != right.primary.source) {
                return left.primary.source < right.primary.source;
              }
              if (left.primary.start != right.primary.start) {
                return left.primary.start < right.primary.start;
              }
              if (left.primary.end != right.primary.end) {
                return left.primary.end < right.primary.end;
              }
              return left.message < right.message;
            });
  for (Diagnostic &diagnostic : argumentDiagnostics) {
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::validateConfinedCallableContracts() -> void {
  std::vector<Diagnostic> contractDiagnostics;
  for (const auto &[_, function] : semanticModel.functions) {
    if (function.declaration == nullptr || !finalizesFunction(function)) {
      continue;
    }
    for (const CallableParameterContract &contract :
         function.callableParameters) {
      const bool exactReturnTransport =
          contract.ownedTransport &&
          contract.ownedTransport->kind ==
              CallableOwnedTransportKind::ExactReturn &&
          !contract.signatures.empty();
      if (contract.boundary != CallableBoundary::Confined ||
          !containsExactType(function.returnType, contract.callableType) ||
          exactReturnTransport) {
        continue;
      }
      const Parameter *parameter =
          contract.parameterIndex < function.declaration->parameters().size()
              ? &function.declaration->parameters()[contract.parameterIndex]
              : nullptr;
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics,
          function.declaration->returnType().name.last(),
          "Confined callable parameter" +
              std::string(parameter == nullptr ? "" : " '") +
              (parameter == nullptr ? std::string{}
                                    : parameter->name.lexeme + "'") +
              " cannot escape through the function return value.");
      if (parameter != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(parameter->sourceAnchor()),
             "Confined callable parameter declared here."});
      }
      diagnostic.hints.emplace_back(
          "Confined callable parameters may only be invoked or forwarded "
          "through another proven confined parameter during the selected "
          "call.");
      contractDiagnostics.emplace_back(std::move(diagnostic));
    }
  }

  for (const SemanticModel::PendingCallableForwarding &forwarding :
       semanticModel.pendingCallableForwardings) {
    const FunctionInfo *source =
        forwarding.source == nullptr
            ? nullptr
            : semanticModel.findFunction(*forwarding.source);
    if (source == nullptr || forwarding.call == nullptr) {
      continue;
    }
    const CallableParameterContract *sourceContract =
        callableParameterContract(source->id, forwarding.sourceParameterIndex);
    if (sourceContract == nullptr ||
        sourceContract->boundary != CallableBoundary::Confined) {
      continue;
    }
    const CallableParameterContract *targetContract = callableParameterContract(
        forwarding.target, forwarding.targetParameterIndex);
    if (targetContract != nullptr &&
        targetContract->boundary == CallableBoundary::Confined) {
      continue;
    }
    const ExprList &arguments = forwarding.call->arguments();
    if (forwarding.targetParameterIndex >= arguments.size()) {
      continue;
    }
    const Parameter *sourceParameter =
        source->declaration != nullptr &&
                forwarding.sourceParameterIndex <
                    source->declaration->parameters().size()
            ? &source->declaration
                   ->parameters()[forwarding.sourceParameterIndex]
            : nullptr;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2046", DiagnosticPhase::Semantics,
        expressionToken(arguments[forwarding.targetParameterIndex]),
        "Confined callable parameter" +
            std::string(sourceParameter == nullptr ? "" : " '") +
            (sourceParameter == nullptr ? std::string{}
                                        : sourceParameter->name.lexeme + "'") +
            " cannot be forwarded because the selected target parameter "
            "is not proven confined.");
    if (sourceParameter != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(sourceParameter->sourceAnchor()),
           "Confined callable parameter declared here."});
    }
    diagnostic.hints.emplace_back(
        "Forward a confined callable only to a direct by-value generic "
        "parameter whose visible body invokes or forwards it under a "
        "confined contract.");
    contractDiagnostics.emplace_back(std::move(diagnostic));
  }

  std::sort(contractDiagnostics.begin(), contractDiagnostics.end(),
            [](const Diagnostic &left, const Diagnostic &right) {
              if (left.primary.source != right.primary.source) {
                return left.primary.source < right.primary.source;
              }
              if (left.primary.start != right.primary.start) {
                return left.primary.start < right.primary.start;
              }
              if (left.primary.end != right.primary.end) {
                return left.primary.end < right.primary.end;
              }
              return left.message < right.message;
            });
  for (Diagnostic &diagnostic : contractDiagnostics) {
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::callableContractRequiresOnce(
    FunctionId function, std::size_t parameterIndex,
    std::vector<std::pair<FunctionId, std::size_t>> &visiting) const -> bool {
  const CallableParameterContract *contract =
      callableParameterContract(function, parameterIndex);
  if (contract == nullptr || contract->boundary != CallableBoundary::Confined) {
    return false;
  }
  if (std::any_of(contract->signatures.begin(), contract->signatures.end(),
                  [](const CallableSignatureRequirement &signature) {
                    return signature.capability ==
                           CallableInvocationCapability::Once;
                  })) {
    return true;
  }

  const std::pair key{function, parameterIndex};
  if (std::find(visiting.begin(), visiting.end(), key) != visiting.end()) {
    return false;
  }
  visiting.push_back(key);
  const bool requiresOnce = std::any_of(
      contract->forwardings.begin(), contract->forwardings.end(),
      [&](const CallableForwardingRequirement &forwarding) {
        return callableContractRequiresOnce(
            forwarding.function, forwarding.parameterIndex, visiting);
      });
  visiting.pop_back();
  return requiresOnce;
}

auto SemanticVisitor::Impl::callableContractRequiresOnce(
    FunctionId function, std::size_t parameterIndex) const -> bool {
  std::vector<std::pair<FunctionId, std::size_t>> visiting;
  return callableContractRequiresOnce(function, parameterIndex, visiting);
}

auto SemanticVisitor::Impl::validateOnceCallableForwardings() -> void {
  std::vector<Diagnostic> forwardingDiagnostics;
  const auto &forwardings = semanticModel.pendingCallableForwardings;
  for (std::size_t index =
           lateFinalization == nullptr ? 0 : lateFinalization->forwardings;
       index < forwardings.size(); ++index) {
    const SemanticModel::PendingCallableForwarding &forwarding =
        forwardings[index];
    if (forwarding.call == nullptr ||
        !callableContractRequiresOnce(forwarding.target,
                                      forwarding.targetParameterIndex) ||
        forwarding.targetParameterIndex >=
            forwarding.call->arguments().size()) {
      continue;
    }

    const ExprPtr &argument =
        forwarding.call->arguments()[forwarding.targetParameterIndex];
    if (explicitMoveSource(argument) != nullptr) {
      continue;
    }

    const FunctionInfo *source =
        forwarding.source == nullptr
            ? nullptr
            : semanticModel.findFunction(*forwarding.source);
    const Parameter *sourceParameter =
        source != nullptr && source->declaration != nullptr &&
                forwarding.sourceParameterIndex <
                    source->declaration->parameters().size()
            ? &source->declaration
                   ->parameters()[forwarding.sourceParameterIndex]
            : nullptr;
    const FunctionInfo *target = semanticModel.findFunction(forwarding.target);
    const Parameter *targetParameter =
        target != nullptr && target->declaration != nullptr &&
                forwarding.targetParameterIndex <
                    target->declaration->parameters().size()
            ? &target->declaration
                   ->parameters()[forwarding.targetParameterIndex]
            : nullptr;
    const std::string parameterName = sourceParameter == nullptr
                                          ? std::string("callable")
                                          : sourceParameter->name.lexeme;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2046", DiagnosticPhase::Semantics, expressionToken(argument),
        "Once-confined forwarding of callable parameter '" + parameterName +
            "' requires explicit ownership transfer.");
    if (sourceParameter != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(sourceParameter->sourceAnchor()),
           "Confined callable parameter declared here."});
    }
    if (targetParameter != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(targetParameter->sourceAnchor()),
           "The selected confined target consumes this callable directly "
           "or through another forwarding edge."});
    }
    diagnostic.hints.emplace_back(
        "Forward it as std::move(" + parameterName +
        ") so ordinary path-sensitive move checking proves that no "
        "reachable path forwards or invokes it again.");
    forwardingDiagnostics.emplace_back(std::move(diagnostic));
  }

  std::sort(forwardingDiagnostics.begin(), forwardingDiagnostics.end(),
            [](const Diagnostic &left, const Diagnostic &right) {
              if (left.primary.source != right.primary.source) {
                return left.primary.source < right.primary.source;
              }
              if (left.primary.start != right.primary.start) {
                return left.primary.start < right.primary.start;
              }
              if (left.primary.end != right.primary.end) {
                return left.primary.end < right.primary.end;
              }
              return left.message < right.message;
            });
  for (Diagnostic &diagnostic : forwardingDiagnostics) {
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::validateConfinedCallableUses() -> void {
  std::vector<Diagnostic> useDiagnostics;
  for (const auto &[_, function] : semanticModel.functions) {
    if (function.declaration == nullptr || !function.declaration->body() ||
        !finalizesFunction(function)) {
      continue;
    }
    for (const CallableParameterContract &contract :
         function.callableParameters) {
      const bool exactReturnTransport =
          contract.ownedTransport &&
          contract.ownedTransport->kind ==
              CallableOwnedTransportKind::ExactReturn &&
          !contract.signatures.empty();
      if (contract.boundary != CallableBoundary::Confined ||
          contract.parameterIndex >=
              function.declaration->parameters().size()) {
        continue;
      }

      // validateConfinedCallableContracts owns the one focused diagnostic
      // when a confined callable reaches the declared result without exact
      // transport. Avoid a second error at the return expression.
      if (!exactReturnTransport &&
          containsExactType(function.returnType, contract.callableType)) {
        continue;
      }

      const Parameter &parameter =
          function.declaration->parameters()[contract.parameterIndex];
      const BindingInfo *binding = semanticModel.findBinding(parameter);
      if (binding == nullptr || binding->symbol == 0) {
        continue;
      }

      std::unordered_set<const Expr *> permittedUses;
      for (const CallableSignatureRequirement &signature :
           contract.signatures) {
        if (signature.source == nullptr || !signature.source->callee()) {
          continue;
        }
        const Variable *variable =
            directCallableParameterVariable(signature.source->callee());
        const Expr *callee = variable == nullptr
                                 ? signature.source->callee().get()
                                 : static_cast<const Expr *>(variable);
        if (semanticModel.findResolvedSymbol(*callee) == binding->symbol) {
          permittedUses.insert(callee);
        }
      }
      for (const CallableForwardingRequirement &forwarding :
           contract.forwardings) {
        if (forwarding.source == nullptr ||
            forwarding.parameterIndex >=
                forwarding.source->arguments().size()) {
          continue;
        }
        const Expr *argument =
            forwarding.source->arguments()[forwarding.parameterIndex].get();
        const Variable *variable = directCallableParameterVariable(
            forwarding.source->arguments()[forwarding.parameterIndex]);
        const Expr *permitted = variable == nullptr
                                    ? argument
                                    : static_cast<const Expr *>(variable);
        if (permitted != nullptr &&
            semanticModel.findResolvedSymbol(*permitted) == binding->symbol) {
          permittedUses.insert(permitted);
        }
      }
      if (exactReturnTransport) {
        std::vector<const ExprPtr *> returns;
        collectBorrowReturnExpressions(function.declaration->body().get(),
                                       returns);
        for (const ExprPtr *returned : returns) {
          if (returned == nullptr || !*returned ||
              movedFunctionParameter(*returned, function) !=
                  contract.parameterIndex) {
            continue;
          }
          if (const ExprPtr *source = explicitMoveSource(*returned)) {
            permittedUses.insert(source->get());
          }
        }
      }

      // A pending forwarding edge that did not acquire a confined target is
      // diagnosed by validateConfinedCallableContracts. Treat its direct
      // argument use as already owned so one invalid edge produces one
      // diagnostic rather than an ordinary-transport cascade.
      for (const SemanticModel::PendingCallableForwarding &forwarding :
           semanticModel.pendingCallableForwardings) {
        if (forwarding.source != function.declaration ||
            forwarding.sourceParameterIndex != contract.parameterIndex ||
            forwarding.call == nullptr ||
            forwarding.targetParameterIndex >=
                forwarding.call->arguments().size()) {
          continue;
        }
        const Expr *argument =
            forwarding.call->arguments()[forwarding.targetParameterIndex].get();
        const Variable *variable = directCallableParameterVariable(
            forwarding.call->arguments()[forwarding.targetParameterIndex]);
        const Expr *permitted = variable == nullptr
                                    ? argument
                                    : static_cast<const Expr *>(variable);
        if (permitted != nullptr &&
            semanticModel.findResolvedSymbol(*permitted) == binding->symbol) {
          permittedUses.insert(permitted);
        }
      }

      // Capture initialization is diagnosed once at the capture target
      // below. Its generated copy source or explicit move source is an
      // implementation constituent, not a second ordinary-value escape.
      for (const auto &[_, lambda] : semanticModel.lambdas) {
        for (const LambdaCaptureInfo &capture : lambda.captures) {
          if (capture.sourceSymbol != binding->symbol ||
              capture.initializer == nullptr) {
            continue;
          }
          permittedUses.insert(capture.initializer);
          if (capture.mode == LambdaCaptureMode::Move) {
            const Lambda *declaration = lambda.declaration;
            if (declaration == nullptr) {
              continue;
            }
            const auto syntax = std::find_if(
                declaration->captures().begin(), declaration->captures().end(),
                [&](const LambdaCapture &candidate) {
                  return candidate.initializer.get() == capture.initializer;
                });
            if (syntax != declaration->captures().end()) {
              if (const ExprPtr *source =
                      explicitMoveSource(syntax->initializer)) {
                permittedUses.insert(source->get());
              }
            }
          }
        }
      }

      for (const auto &[expression, symbol] : semanticModel.resolvedSymbols) {
        if (expression == nullptr || symbol != binding->symbol ||
            permittedUses.contains(expression)) {
          continue;
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2046", DiagnosticPhase::Semantics,
            expressionToken(*expression),
            (exactReturnTransport ? "Exact-return callable parameter '"
                                  : "Confined callable parameter '") +
                parameter.name.lexeme +
                "' cannot be used as an ordinary value outside direct "
                "invocation" +
                (exactReturnTransport ? " and its final exact move."
                                      : " or proven confined forwarding."));
        diagnostic.related.push_back(
            {tokenSpan(parameter.sourceAnchor()),
             exactReturnTransport
                 ? "Exact-return callable parameter declared here."
                 : "Confined callable parameter declared here."});
        diagnostic.hints.emplace_back(
            exactReturnTransport
                ? "Invoke the callable directly and return it only through "
                  "the declaration's exact std::move transport."
                : "Invoke the callable directly or forward it to a direct "
                  "generic value or checked-reference parameter with a proven "
                  "confined "
                  "contract; assignment, storage, and other value "
                  "transport require an owned callable boundary.");
        useDiagnostics.emplace_back(std::move(diagnostic));
      }

      // Capture names are declaration-linked semantic uses rather than Expr
      // nodes. Compare the captured declaration's canonical symbol so a
      // same-spelling local cannot be mistaken for this parameter.
      for (const auto &[_, lambda] : semanticModel.lambdas) {
        for (const LambdaCaptureInfo &capture : lambda.captures) {
          if (symbolForDeclaration(capture.declaration) != binding->symbol) {
            continue;
          }
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2046", DiagnosticPhase::Semantics, capture.capture,
              (exactReturnTransport ? "Exact-return callable parameter '"
                                    : "Confined callable parameter '") +
                  parameter.name.lexeme +
                  "' cannot be captured or otherwise transported.");
          diagnostic.related.push_back(
              {tokenSpan(parameter.sourceAnchor()),
               exactReturnTransport
                   ? "Exact-return callable parameter declared here."
                   : "Confined callable parameter declared here."});
          diagnostic.hints.emplace_back(
              exactReturnTransport
                  ? "Invoke the callable directly and return it only "
                    "through the declaration's exact std::move transport."
                  : "Invoke the callable directly or forward it to a direct "
                    "generic value or checked-reference parameter with a "
                    "proven confined "
                    "contract; assignment, storage, capture, and other "
                    "value transport require an owned callable boundary.");
          useDiagnostics.emplace_back(std::move(diagnostic));
        }
      }
    }
  }

  std::sort(useDiagnostics.begin(), useDiagnostics.end(),
            [](const Diagnostic &left, const Diagnostic &right) {
              if (left.primary.source != right.primary.source) {
                return left.primary.source < right.primary.source;
              }
              if (left.primary.start != right.primary.start) {
                return left.primary.start < right.primary.start;
              }
              if (left.primary.end != right.primary.end) {
                return left.primary.end < right.primary.end;
              }
              return left.message < right.message;
            });
  for (Diagnostic &diagnostic : useDiagnostics) {
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::validateConfinedCallableArguments(
    const CallableCandidate &candidate, const CallableCandidate &,
    const std::vector<SemanticType> &argumentTypes, const ExprList &arguments)
    -> bool {
  bool valid = true;
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    const CallableParameterContract *sourceContract =
        index < arguments.size() ? callableParameterContract(arguments[index])
                                 : nullptr;
    const CallableParameterContract *targetContract =
        callableParameterContract(candidate.id, index);
    const bool lexicalClosure = containsClosureObjectType(argumentTypes[index]);
    const bool ordinaryValueContract = sourceContract != nullptr &&
                                       sourceContract->ownedTransport &&
                                       sourceContract->ownedTransport->kind ==
                                           CallableOwnedTransportKind::Value;
    const bool provenBoundary =
        (sourceContract != nullptr &&
         (sourceContract->boundary == CallableBoundary::Confined ||
          (sourceContract->boundary == CallableBoundary::Owned &&
           (!ordinaryValueContract || lexicalClosure)))) ||
        (lexicalClosure && targetContract != nullptr);
    if (!lexicalClosure && !provenBoundary) {
      continue;
    }
    const std::size_t formalIndex =
        candidate.parameterPack && !candidate.parameterTypes.empty()
            ? std::min(index, candidate.parameterTypes.size() - 1)
            : index;
    const Parameter *parameter =
        candidate.declaration != nullptr &&
                formalIndex < candidate.declaration->parameters().size()
            ? &candidate.declaration->parameters()[formalIndex]
            : nullptr;
    const SemanticType *parameterType =
        formalIndex < candidate.parameterTypes.size()
            ? &candidate.parameterTypes[formalIndex]
            : nullptr;
    const bool borrowedCallableBoundary =
        parameterType != nullptr &&
        parameterType->kind == SemanticType::Reference &&
        parameterType->arguments.size() == 1 &&
        parameterType->arguments.front().kind == SemanticType::TypeParameter &&
        parameter != nullptr && !parameter->pack;
    const bool byValueCallableBoundary =
        formalIndex < candidate.parameterTypes.size() && parameter != nullptr &&
        (candidate.parameterTypes[formalIndex].kind ==
             SemanticType::TypeParameter ||
         (parameter->pack && candidate.parameterTypes[formalIndex].kind ==
                                 SemanticType::TypePack) ||
         (candidate.parameterTypes[formalIndex].kind == SemanticType::Class &&
          (containsOpenTypeParameter(candidate.parameterTypes[formalIndex]) ||
           (targetContract != nullptr && targetContract->ownedTransport &&
            targetContract->ownedTransport->kind ==
                CallableOwnedTransportKind::Value)))) &&
        !parameter->type.reference;
    if (instanceAnalysisActive) {
      if ((!byValueCallableBoundary && !borrowedCallableBoundary) ||
          candidate.declaration == nullptr || !candidate.declaration->body() ||
          targetContract == nullptr) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2046", DiagnosticPhase::Semantics,
            expressionToken(arguments[index]),
            "Callable argument " + std::to_string(index + 1) +
                " cannot be forwarded because the selected parameter is "
                "not proven confined or owned.");
        diagnostic.hints.emplace_back(
            "Use a generic value/reference parameter or generic owning carrier "
            "whose visible body proves confined use or exact owned transport.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
    } else if (lexicalClosure &&
               ((!byValueCallableBoundary && !borrowedCallableBoundary) ||
                candidate.declaration == nullptr ||
                !candidate.declaration->body())) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2046", DiagnosticPhase::Semantics,
                         expressionToken(arguments[index]),
                         "Callable argument " + std::to_string(index + 1) +
                             " must bind to a generic value/reference "
                             "parameter or generic owning "
                             "carrier with a visible function body.");
      diagnostic.hints.emplace_back(
          "Use a parameter such as 'Operation operation', 'Operation& "
          "operation', "
          "'mut Operation& operation', or 'Owner<Operation> owner'.");
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::lambdaArgumentBoundaries(
    const std::vector<SemanticType> &argumentTypes)
    -> std::vector<CallableArgumentBoundary> {
  std::vector<CallableArgumentBoundary> result;
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    if (argumentTypes[index].kind == SemanticType::Lambda) {
      result.push_back(
          {.parameterIndex = index, .boundary = CallableBoundary::Confined});
    }
  }
  return result;
}

auto SemanticVisitor::Impl::functionSignatureSpelling(
    const CallableCandidate &function) -> std::string {
  if (function.declaration == nullptr) {
    return "function";
  }
  const FunctionDecl &declaration = *function.declaration;
  std::string result = declaration.operatorName()
                           ? std::string(operatorSourceSpelling(
                                 declaration.operatorName()->kind))
                           : declaration.name().lexeme;
  if (function.record().requiresUnsafe) {
    result = "unsafe " + result;
  }
  if (!declaration.genericParameters().empty()) {
    result += '<';
    for (std::size_t index = 0; index < declaration.genericParameters().size();
         ++index) {
      if (index != 0) {
        result += ", ";
      }
      if (declaration.genericParameters()[index].constraint) {
        result +=
            pathSpelling(*declaration.genericParameters()[index].constraint) +
            " ";
      }
      result += declaration.genericParameters()[index].name.lexeme;
      if (declaration.genericParameters()[index].pack) {
        result += "...";
      }
    }
    result += '>';
  }
  result += '(';
  for (std::size_t index = 0; index < declaration.parameters().size();
       ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += typeRefSpelling(declaration.parameters()[index].type);
    if (declaration.parameters()[index].pack) {
      result += "...";
    }
    if (declaration.parameters()[index].hasDefault()) {
      result += " = <default>";
    }
  }
  result += ')';
  if (declaration.receiverMutability() == ReceiverMutability::Mutable) {
    result += " mut";
  } else if (declaration.receiverMutability() ==
             ReceiverMutability::Consuming) {
    result += " &&";
  }
  return result;
}

auto SemanticVisitor::Impl::typeRequiresThreads(
    const SemanticType &type, std::unordered_set<ClassId> &visiting) const
    -> bool {
  if (std::any_of(type.arguments.begin(), type.arguments.end(),
                  [&](const SemanticType &argument) {
                    return typeRequiresThreads(argument, visiting);
                  })) {
    return true;
  }
  if (type.kind != SemanticType::Class) {
    return false;
  }
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr) {
    return false;
  }
  if (owner->compilerCapability == CompilerCapabilityTypeKind::ThreadHandle) {
    return true;
  }
  if (owner->compilerCapability == CompilerCapabilityTypeKind::AtomicStorage) {
    return true;
  }
  if (owner->compilerCapability == CompilerCapabilityTypeKind::MutexStorage ||
      owner->compilerCapability == CompilerCapabilityTypeKind::MutexLockToken) {
    return true;
  }
  if (!visiting.insert(owner->id).second) {
    return false;
  }
  const GenericSubstitution substitution = classSubstitution(type);
  const bool required =
      std::any_of(owner->bases.begin(), owner->bases.end(),
                  [&](const ClassBaseTypeInfo &base) {
                    return typeRequiresThreads(
                        substituteType(base.type, substitution), visiting);
                  }) ||
      std::any_of(
          owner->fields.begin(), owner->fields.end(),
          [&](const FieldInfo &field) {
            if (field.declaration == nullptr) {
              return false;
            }
            const auto member =
                owner->members.find(field.declaration->name().lexeme);
            return member != owner->members.end() &&
                   typeRequiresThreads(
                       substituteType(member->second.symbol.type, substitution),
                       visiting);
          });
  visiting.erase(owner->id);
  return required;
}

auto SemanticVisitor::Impl::typeRequiresThreads(const SemanticType &type) const
    -> bool {
  std::unordered_set<ClassId> visiting;
  return typeRequiresThreads(type, visiting);
}

auto SemanticVisitor::Impl::rawBorrowArgument(const ExprPtr &argument,
                                              const Token &location)
    -> RawBorrowCallArgument {
  RawBorrowCallArgument snapshot;
  snapshot.place = loanPlace(argument);
  snapshot.dependencies = storedBorrowDependencies(argument);
  if (snapshot.dependencies.size() == 1 &&
      snapshot.dependencies.front().designatedFields.empty()) {
    const auto type = semanticModel.typeOf(*argument);
    const auto paths = storedReferencePaths(type);
    if (paths.size() == 1)
      snapshot.dependencies.front().designatedFields =
          storedFieldSymbols(type, paths.front().fields);
  }
  for (const auto &field : rawPointerFields(argument.get()))
    mergeRawPointerOrigins(snapshot.origins, field.origins);
  completeRawBorrowArgument(snapshot, location);
  return snapshot;
}

auto SemanticVisitor::Impl::completeRawBorrowArgument(
    RawBorrowCallArgument &snapshot, const Token &location) -> void {
  const auto formalParameter =
      [&](SymbolId root) -> std::optional<std::size_t> {
    if (!root || !currentFunctionDeclaration)
      return std::nullopt;
    const auto &parameters = currentFunctionDeclaration->parameters();
    for (std::size_t index = 0; index < parameters.size(); ++index)
      if (symbolForDeclaration(parameters[index].name) == root)
        return index;
    return std::nullopt;
  };
  snapshot.ownerParameter = formalParameter(snapshot.place.root);
  if (std::any_of(snapshot.origins.begin(), snapshot.origins.end(),
                  [](const auto &origin) { return origin.loopInput != 0; }))
    snapshot.loopUse = deferredRawPointerLoopUse(snapshot.origins, location);
  for (const auto &origin : snapshot.origins) {
    if (origin.parameterPointee) {
      snapshot.pointerReceiver |= origin.place.receiver;
      if (auto index = formalParameter(origin.place.root);
          index && std::find(snapshot.pointerParameters.begin(),
                             snapshot.pointerParameters.end(),
                             *index) == snapshot.pointerParameters.end())
        snapshot.pointerParameters.push_back(*index);
    } else if (auto diagnostic =
                   rawPointerLifetimeDiagnostic(origin, location)) {
      snapshot.lifetimeDiagnostics.push_back(std::move(*diagnostic));
    }
  }
}

auto SemanticVisitor::Impl::recordRawBorrowCall(
    const Call &call, const ResolvedCallInfo &resolved) -> void {
  if (!rawAuditEnabled())
    return;
  if (resolved.declaration == nullptr ||
      (resolved.intrinsic != IntrinsicKind::None &&
       resolved.intrinsic != IntrinsicKind::RawBorrow &&
       resolved.intrinsic != IntrinsicKind::RawBorrowMut &&
       resolved.intrinsic != IntrinsicKind::TextViewFromAddress &&
       resolved.intrinsic != IntrinsicKind::CStringFromAddress))
    return;
  RawBorrowCallUse use{.caller = currentFunctionDeclaration,
                       .callee = resolved.declaration,
                       .intrinsic = resolved.intrinsic,
                       .location = call.paren()};
  for (std::size_t index = 0;
       index < resolvedCallArgumentCount(call, &resolved); ++index) {
    use.arguments.push_back(rawBorrowArgument(
        *resolvedCallArgument(call, &resolved, index), call.paren()));
  }
  if (const auto *member = exprAs<Get>(call.callee().get());
      member && resolved.receiverMode != CallReceiverMode::None) {
    use.receiver = rawBorrowArgument(member->object(), call.paren());
  } else if (resolved.receiverMode != CallReceiverMode::None) {
    // Brace-initialize rather than emplace(): Clang 18 does not consider this
    // aggregate constructible from no arguments, so optional::emplace fails.
    use.receiver = RawBorrowCallArgument{};
    use.receiver->place = {.receiver = true};
    for (const auto &field : rawPointerFieldsAt(use.receiver->place))
      mergeRawPointerOrigins(use.receiver->origins, field.origins);
    completeRawBorrowArgument(*use.receiver, call.paren());
  }
  if (use.receiver ||
      std::any_of(use.arguments.begin(), use.arguments.end(),
                  [](const auto &arg) { return !arg.origins.empty(); }))
    requestSession.rawBorrowCallUses.push_back(std::move(use));
}

auto SemanticVisitor::Impl::analyzeRawReferenceReturn(const ExprPtr &expression)
    -> bool {
  const FunctionInfo *function = currentFunctionInfo();
  const Expr *source = expression.get();
  while (const auto *group = exprAs<Grouping>(source))
    source = group->expression().get();
  const auto *dereference = exprAs<Unary>(source);
  if (!function || !function->explicitReturnBorrowOrigin || unsafeDepth == 0 ||
      !dereference || dereference->oper().kind != TokenKind::STAR ||
      semanticModel.typeOf(*dereference->right()).kind !=
          SemanticType::RawPointer)
    return false;

  RawReferenceAssertionInfo assertion;
  RawBorrowCallArgument witness;
  const AccessMode access = currentReturnType.referenceAccess;
  if (function->returnBorrowOrigin == BorrowOriginKind::Argument &&
      function->declaration &&
      function->returnBorrowParameter < function->parameterTypes.size()) {
    const auto index = function->returnBorrowParameter;
    const auto *binding =
        semanticModel.findBinding(function->declaration->parameters()[index]);
    if (!binding ||
        function->parameterTypes[index].kind != SemanticType::Reference)
      return false;
    assertion.owner = {.root = binding->symbol};
    assertion.ownerReference = SemanticType::referenceTo(
        function->parameterTypes[index].arguments.front(), access);
    witness.ownerParameter = index;
  } else if (function->returnBorrowOrigin == BorrowOriginKind::Receiver) {
    const Symbol *receiver = receiverStateSymbol();
    if (!receiver || (access == AccessMode::Mutable &&
                      currentReceiverMutability != ReceiverMutability::Mutable))
      return false;
    assertion.owner = {.receiver = true};
    assertion.ownerReference =
        SemanticType::referenceTo(receiver->type, access);
  } else {
    return false;
  }
  witness.place = assertion.owner;
  // The programmer asserts raw validity/owner association. Optional auditing
  // checks available evidence, while the checked reference fact below is
  // always recorded and remains subject to ordinary lifetime verification.
  if (rawAuditEnabled()) {
    validateRawPointerAccess(dereference->right(), dereference->oper(), access);
    RawBorrowCallUse use{.caller = currentFunctionDeclaration,
                         .intrinsic = access == AccessMode::Mutable
                                          ? IntrinsicKind::RawBorrowMut
                                          : IntrinsicKind::RawBorrow,
                         .location = dereference->oper(),
                         .arguments = {rawBorrowArgument(dereference->right(),
                                                         dereference->oper()),
                                       std::move(witness)}};
    requestSession.rawBorrowCallUses.push_back(std::move(use));
  }
  semanticModel.recordRawReferenceAssertion(*source, std::move(assertion));
  return true;
}

auto SemanticVisitor::Impl::finalizeRawBorrowContracts() -> void {
  if (!rawAuditEnabled())
    return;
  const auto inputFor = [](const RawBorrowCallUse &use, bool receiver,
                           std::size_t index) -> const RawBorrowCallArgument * {
    if (receiver)
      return use.receiver ? &*use.receiver : nullptr;
    return index < use.arguments.size() ? &use.arguments[index] : nullptr;
  };
  const auto witnessFor = [](const RawBorrowCallArgument &owner,
                             const RawBorrowContract &contract) {
    auto witness = owner.place;
    auto projections = contract.ownerProjections;
    // A reference field retains external storage; its shell is not that
    // storage. Resolve the field using the same checked dependency identity
    // as ordinary projected borrowing, then append any subobject path.
    for (const auto &dependency : owner.dependencies) {
      const auto &fields = dependency.designatedFields;
      if (fields.empty() || fields.size() > projections.size())
        continue;
      bool matches = true;
      for (std::size_t index = 0; index < fields.size(); ++index)
        matches &= projections[index].kind == PlaceProjectionKind::Field &&
                   projections[index].field == fields[index];
      if (!matches)
        continue;
      witness = dependency.place;
      projections.erase(projections.begin(),
                        projections.begin() + fields.size());
      break;
    }
    witness.projections.insert(witness.projections.end(), projections.begin(),
                               projections.end());
    return witness;
  };
  const auto contractsFor = [&](const RawBorrowCallUse &use) {
    if (use.intrinsic == IntrinsicKind::TextViewFromAddress ||
        use.intrinsic == IntrinsicKind::CStringFromAddress)
      // Text assertions retain the nominated checked borrow independently of
      // pointer audit policy. Their raw bytes can legitimately be static (or
      // null for an empty/nullable carrier), so a shorter witness need not be
      // the address's physical owner. Still transport known expired address
      // evidence through ordinary helpers, exactly as other raw assertions do.
      return std::vector<RawBorrowContract>{
          {.pointerParameter = 0, .access = AccessMode::ReadOnly}};
    if (use.intrinsic == IntrinsicKind::RawBorrow ||
        use.intrinsic == IntrinsicKind::RawBorrowMut)
      return std::vector<RawBorrowContract>{
          {.pointerParameter = 0,
           .ownerParameter = 1,
           .access = use.intrinsic == IntrinsicKind::RawBorrowMut
                         ? AccessMode::Mutable
                         : AccessMode::ReadOnly}};
    const auto *callee = semanticModel.findFunction(*use.callee);
    if (!callee && instanceBaseModel)
      callee = instanceBaseModel->findFunction(*use.callee);
    return callee ? callee->rawBorrowContracts
                  : std::vector<RawBorrowContract>{};
  };
  // Bodies may occur after their callers, and wrappers may be recursive.
  // Propagate finite formal-parameter obligations, not source spellings.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &use : requestSession.rawBorrowCallUses) {
      if (!use.caller)
        continue;
      const auto *caller = semanticModel.findFunction(*use.caller);
      if (!caller)
        continue;
      auto updated = *caller;
      for (const auto &contract : contractsFor(use)) {
        const auto *input =
            inputFor(use, contract.pointerReceiver, contract.pointerParameter);
        if (!input)
          continue;
        const auto &argument = *input;
        auto inputs = argument.pointerParameters;
        bool receiverInput = argument.pointerReceiver;
        if (argument.loopUse && argument.loopUse->pendingLoops == 0) {
          for (const auto &origin : argument.loopUse->origins) {
            if (!origin.parameterPointee)
              continue;
            receiverInput |= origin.place.receiver;
            const auto &parameters = use.caller->parameters();
            for (std::size_t index = 0; index < parameters.size(); ++index)
              if (symbolForDeclaration(parameters[index].name) ==
                      origin.place.root &&
                  std::find(inputs.begin(), inputs.end(), index) ==
                      inputs.end())
                inputs.push_back(index);
          }
        }
        // Receiver input uses one distinct sentinel in this local iteration;
        // published contracts retain an explicit receiver identity.
        if (receiverInput)
          inputs.push_back(use.caller->parameters().size());
        for (const auto pointer : inputs) {
          const bool fromReceiver = pointer == use.caller->parameters().size();
          RawBorrowContract forwarded{.pointerParameter =
                                          fromReceiver ? 0 : pointer,
                                      .pointerReceiver = fromReceiver,
                                      .access = contract.access};
          if (contract.ownerReceiver || contract.ownerParameter) {
            const auto *owner = inputFor(use, contract.ownerReceiver,
                                         contract.ownerParameter.value_or(0));
            if (owner) {
              const auto witness = witnessFor(*owner, contract);
              forwarded.ownerReceiver = witness.receiver;
              for (std::size_t index = 0;
                   index < use.caller->parameters().size(); ++index)
                if (witness.root != 0 &&
                    symbolForDeclaration(
                        use.caller->parameters()[index].name) == witness.root)
                  forwarded.ownerParameter = index;
              if (forwarded.ownerParameter || forwarded.ownerReceiver)
                forwarded.ownerProjections = witness.projections;
            }
          }
          // Recursive field traversal can produce an unbounded path summary.
          // Fail closed at the analysis budget; never erase the witness.
          if (forwarded.ownerProjections.size() > 32) {
            diagnostics.push_back(
                makeDiagnostic(DiagnosticId::RawAuditIncomplete, use.location,
                               {"Raw-borrow wrapper analysis exceeds its "
                                "owner-projection limit."}));
            return;
          }
          if (std::find(updated.rawBorrowContracts.begin(),
                        updated.rawBorrowContracts.end(),
                        forwarded) == updated.rawBorrowContracts.end()) {
            updated.rawBorrowContracts.push_back(std::move(forwarded));
            changed = true;
          }
        }
      }
      semanticModel.record(*use.caller, std::move(updated));
    }
  }
  std::unordered_set<const FunctionDecl *> checkedDispatchContracts;
  for (const auto &use : requestSession.rawBorrowCallUses) {
    if (!use.caller || !checkedDispatchContracts.insert(use.caller).second)
      continue;
    const auto *function = semanticModel.findFunction(*use.caller);
    if (function && function->virtualMethod &&
        !function->rawBorrowContracts.empty()) {
      diagnostics.push_back(makeDiagnostic(
          DiagnosticId::RawAuditIncomplete, use.caller->name(),
          {"Raw-reference assertion audit is incomplete across virtual or "
           "interface dispatch; verify the unsafe owner relationship for every "
           "target."}));
    }
  }
  for (const auto &use : requestSession.rawBorrowCallUses) {
    std::vector<std::size_t> checkedPointers;
    for (const auto &contract : contractsFor(use)) {
      const auto *input =
          inputFor(use, contract.pointerReceiver, contract.pointerParameter);
      if (!input)
        continue;
      const auto &pointer = *input;
      auto origins = pointer.origins;
      const bool hasLoopInputs =
          std::any_of(origins.begin(), origins.end(),
                      [](const auto &origin) { return origin.loopInput != 0; });
      if (hasLoopInputs &&
          (!pointer.loopUse || pointer.loopUse->pendingLoops != 0 ||
           std::any_of(
               pointer.loopUse->origins.begin(), pointer.loopUse->origins.end(),
               [](const auto &origin) { return origin.loopInput != 0; }))) {
        diagnostics.push_back(
            makeDiagnostic(DiagnosticId::RawAuditIncomplete, use.location,
                           {"Raw-pointer audit could not resolve a "
                            "loop-carried backedge origin "
                            "before checked borrowing."}));
        continue;
      }
      if (hasLoopInputs) {
        // All enclosing loop transfers completed. Replace symbolic input
        // slots with their concrete backedge origin union, including origins
        // whose lifetime expired or whose live owner differs from the witness.
        std::erase_if(origins,
                      [](const auto &origin) { return origin.loopInput != 0; });
        mergeRawPointerOrigins(origins, pointer.loopUse->origins);
      }
      const auto pointerIndex = contract.pointerReceiver
                                    ? use.arguments.size()
                                    : contract.pointerParameter;
      if (std::find(checkedPointers.begin(), checkedPointers.end(),
                    pointerIndex) == checkedPointers.end()) {
        for (const auto &diagnostic : pointer.lifetimeDiagnostics)
          publishRawLifetimeDiagnostic(diagnostic);
        if (pointer.loopUse)
          for (const auto &diagnostic : pointer.loopUse->diagnostics)
            publishRawLifetimeDiagnostic(diagnostic);
        checkedPointers.push_back(pointerIndex);
      }
      if (!contract.ownerParameter && !contract.ownerReceiver)
        continue;
      const auto *owner = inputFor(use, contract.ownerReceiver,
                                   contract.ownerParameter.value_or(0));
      if (!owner)
        continue;
      const auto witness = witnessFor(*owner, contract);
      for (const auto &origin : origins) {
        if (origin.parameterPointee || !origin.place.valid() ||
            !witness.valid())
          continue;
        const auto relation = placeRelation(witness, origin.place);
        if (relation.compatibleDomain &&
            (relation.relation == PlaceRelation::Equal ||
             relation.relation == PlaceRelation::LeftStrictPrefix))
          continue;
        auto diagnostic = makeDiagnostic(
            relation.relation == PlaceRelation::Disjoint &&
                    origins.size() == 1 && !origin.possible
                ? DiagnosticId::RawAuditViolation
                : DiagnosticId::RawAuditPotential,
            use.location,
            {origin.possible
                 ? "The raw address may originate outside its checked owner "
                   "witness."
                 : "The raw address's known origin is not contained in its "
                   "checked owner witness."});
        diagnostic.related.push_back(
            {origin.addressSite, "Raw address obtained from this storage."});
        diagnostic.hints.emplace_back(
            "Use the actual enclosing owner as the lifetime witness; unsafe "
            "does not replace a known origin with an unrelated object.");
        diagnostics.push_back(std::move(diagnostic));
        break;
      }
    }
  }
  requestSession.rawBorrowCallUses.clear();
}

auto SemanticVisitor::Impl::finalizeCThreadRequirements() -> void {
  // Declaration order and wrapper spelling must not determine availability.
  // Propagate the exact native requirement through resolved source calls. A
  // late batch's callers are its own bodies, so it propagates its own uses.
  const auto &uses = requestSession.cThreadCallUses;
  const std::size_t firstUse =
      lateFinalization == nullptr ? 0 : lateFinalization->cThreadCallUses;
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = firstUse; index < uses.size(); ++index) {
      const CThreadCallUse &use = uses[index];
      const FunctionInfo *callee = semanticModel.findFunction(*use.callee);
      const FunctionInfo *caller =
          use.caller == nullptr ? nullptr
                                : semanticModel.findFunction(*use.caller);
      if (callee == nullptr || !callee->requiresCThreadRuntime ||
          caller == nullptr || caller->requiresCThreadRuntime) {
        continue;
      }
      FunctionInfo updated = *caller;
      updated.requiresCThreadRuntime = true;
      semanticModel.record(*use.caller, std::move(updated));
      changed = true;
    }
  }
  for (std::size_t index = firstUse; index < uses.size(); ++index) {
    const CThreadCallUse &use = uses[index];
    const FunctionInfo *callee = semanticModel.findFunction(*use.callee);
    if (!use.trusted && callee != nullptr && callee->requiresCThreadRuntime) {
      (void)validateConcurrentSurfaceAvailability(use.location,
                                                  "This C-thread API");
    }
  }
}

auto SemanticVisitor::Impl::validateConcurrentSurfaceAvailability(
    const Token &location, std::string_view surface) -> bool {
  if (target.executionProfile == ExecutionProfile::Concurrent &&
      target.runtimeCapabilities.threads) {
    return true;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2084", DiagnosticPhase::Semantics, location,
      target.executionProfile != ExecutionProfile::Concurrent
          ? std::string(surface) + " requires the concurrent execution profile."
          : std::string(surface) +
                " is unavailable because the selected target does not "
                "provide thread-capable concurrency support.");
  diagnostic.hints.emplace_back(
      target.executionProfile != ExecutionProfile::Concurrent
          ? "Select the concurrent execution profile for programs that "
            "use threads or synchronization APIs."
          : "Select a target whose runtime capability set includes "
            "threads.");
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::constructorSignatureSpelling(
    const ConstructorInfo &constructor) -> std::string {
  if (constructor.declaration == nullptr) {
    return "generated default constructor";
  }
  std::string result = constructor.declaration->name().lexeme + '(';
  for (std::size_t index = 0;
       index < constructor.declaration->parameters().size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    const Parameter &parameter = constructor.declaration->parameters()[index];
    if (parameter.mutability == Mutability::Mutable &&
        parameter.type.reference) {
      result += "mut ";
    }
    result += typeRefSpelling(parameter.type);
    if (parameter.hasDefault()) {
      result += " = <default>";
    }
  }
  result += ')';
  return result;
}

auto SemanticVisitor::Impl::validateSelectedFunction(
    const CallableCandidate &function, const ExprPtr &callee,
    const Token &paren, std::span<const ExprPtr> arguments,
    CallReceiverMode receiverMode) -> bool {
  if (function.receiverMutability == ReceiverMutability::Consuming &&
      (function.returnType.kind == SemanticType::Reference ||
       function.returnBorrowOrigin != BorrowOriginKind::None ||
       typeTraits(function.returnType).containsBorrowedState)) {
    report(paren,
           "A consuming method cannot return a reference or tracked borrowed "
           "state.",
           "GTI-S2022");
    return false;
  }
  if (function.receiverMutability == ReceiverMutability::Consuming &&
      receiverMode != CallReceiverMode::Consuming) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2022", DiagnosticPhase::Semantics, paren,
                       "A consuming method requires an owned temporary or an "
                       "explicitly moved receiver.");
    diagnostic.hints.emplace_back("Use std::move(value).method(...) to "
                                  "transfer ownership of a named value.");
    if (function.declaration != nullptr)
      diagnostic.related.push_back({tokenSpan(function.declaration->name()),
                                    "Consuming method declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  if (!isCompilerTrustedRequester(paren)) {
    const FunctionInfo *resolved = semanticModel.findFunction(function.id);
    if (resolved == nullptr && instanceBaseModel != nullptr) {
      resolved = instanceBaseModel->findFunction(function.id);
    }
    // Root analysis checks the closed call graph once all bodies exist.
    // Concrete reanalysis consumes that derived requirement from its base.
    if (instanceAnalysisActive && resolved != nullptr &&
        resolved->requiresCThreadRuntime &&
        !validateConcurrentSurfaceAvailability(paren, "This C-thread API")) {
      return false;
    }
    const SemanticType ownerType =
        function.dispatchOwner.kind == SemanticType::Class
            ? function.dispatchOwner
        : function.ownerClass == 0
            ? SemanticType::Unknown
            : SemanticType::classType(function.ownerClass);
    const bool requiresThreads =
        typeRequiresThreads(ownerType) ||
        typeRequiresThreads(function.returnType) ||
        std::any_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(),
                    [&](const SemanticType &parameter) {
                      return typeRequiresThreads(parameter);
                    });
    if (requiresThreads &&
        !validateConcurrentSurfaceAvailability(paren, "This concurrency API")) {
      return false;
    }
  }
  if (!isCompilerTrustedRequester(paren) && function.compilerPrivate) {
    reportCompilerPrivateAccess(paren,
                                function.declaration == nullptr
                                    ? "function"
                                    : function.declaration->name().lexeme);
    return false;
  }
  if (function.ownerClass != 0 && function.access == AccessModifier::Private &&
      currentClass != function.ownerClass) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, paren,
        "Method '" + function.declaration->name().lexeme + "' of '" +
            classInfo(function.ownerClass).name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(function.declaration->name()), "Method declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }

  validateMutableCallArgumentLoans(function.parameterTypes, arguments, paren);
  const auto *member = exprAs<Get>(callee.get());
  if (member == nullptr && function.ownerClass != 0 && !function.staticMember) {
    SemanticPlace receiver{.root = receiverStateSymbol(), .receiver = true};
    if (const auto *state = unavailableProjectedState(receiver)) {
      reportUnavailablePlace(paren, *state, &receiver);
      return false;
    }
  }
  const AccessMode selectedReceiverAccess =
      receiverAllowsMutation(function.receiverMutability)
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  if (member && member->access().kind == TokenKind::ARROW &&
      semanticModel.typeOf(*member->object()).kind == SemanticType::RawPointer)
    validateRawPointerAccess(member->object(), paren, selectedReceiverAccess);
  AccessMode callReceiverAccess = selectedReceiverAccess;
  std::optional<SemanticLoanPlace> receiverPlace;
  SemanticLoanId receiverLoan = 0;
  bool transientReceiver = false;
  const bool temporaryReceiver =
      receiverMode == CallReceiverMode::FreshTemporaryRead ||
      receiverMode == CallReceiverMode::FreshTemporaryMutable;
  if (member != nullptr && !function.staticMember) {
    std::optional<TransientBorrowProduction> production =
        transientBorrowProduction(member->object());
    if (production) {
      transientReceiver = true;
      receiverPlace = std::move(production->place);
      if (temporaryReceiver) {
        // Mutability of the materialized carrier shell does not upgrade a
        // stored shared dependency into exclusive access to its owner.
        callReceiverAccess = production->access;
      } else if (production->access == AccessMode::Mutable) {
        callReceiverAccess = AccessMode::Mutable;
      }
    } else if (!temporaryReceiver) {
      receiverPlace = loanPlace(member->object());
    }
    receiverLoan = accessLoan(member->object());
  } else if (member == nullptr && function.ownerClass != 0 &&
             !function.staticMember) {
    receiverPlace = loanPlace(
        SemanticPlace{.root = receiverStateSymbol(), .receiver = true});
  }
  const std::size_t exclusivityDiagnostics = diagnostics.size();
  validateCallPlaceExclusivity(function.parameterTypes, arguments, paren,
                               receiverPlace, callReceiverAccess,
                               transientReceiver);
  const ExpressionInfo *receiverInfo =
      member == nullptr ? nullptr
                        : semanticModel.findExpression(*member->object());
  if (transientReceiver && receiverInfo != nullptr &&
      receiverInfo->category == ValueCategory::Value &&
      receiverInfo->traits.containsBorrowedState) {
    // The primary transient production preserves its producer's access above.
    // Independently stored owners have their own access and participate in
    // the same receiver-before-arguments checks. Do not extend the lifetime
    // of an existing named carrier or publish repeated argument diagnostics.
    std::vector<SemanticLoanPlace> checkedPlaces;
    if (receiverPlace) {
      checkedPlaces.push_back(*receiverPlace);
    }
    for (const auto &dependency : storedBorrowDependencies(member->object())) {
      if (diagnostics.size() != exclusivityDiagnostics) {
        break;
      }
      if (std::find(checkedPlaces.begin(), checkedPlaces.end(),
                    dependency.place) != checkedPlaces.end()) {
        continue;
      }
      checkedPlaces.push_back(dependency.place);
      validateCallPlaceExclusivity(function.parameterTypes, arguments, paren,
                                   dependency.place, dependency.access, true);
    }
  }
  if (receiverPlace && callReceiverAccess == AccessMode::Mutable) {
    recordMutableBindingUse(receiverPlace->root);
  }

  if (function.staticMember) {
    // Object-qualified static access is diagnosed while resolving Get.
    return true;
  }

  if (member != nullptr && receiverMode == CallReceiverMode::Value) {
    const ExpressionInfo *receiver =
        semanticModel.findExpression(*member->object());
    if (receiver != nullptr && receiver->category == ValueCategory::Value &&
        receiver->type.kind == SemanticType::Class &&
        receiver->traits.containsBorrowedState) {
      reportMutableReceiverFailure(member->object(), paren, function, "Method");
      return false;
    }
  }

  if (member != nullptr &&
      function.receiverMutability == ReceiverMutability::Mutable &&
      receiverMode == CallReceiverMode::Value &&
      freshTemporaryValue(member->object()).eligible &&
      !freshTemporaryMethodSupported(function)) {
    reportMutableReceiverFailure(member->object(), paren, function, "Method");
    return false;
  }
  if (function.receiverMutability == ReceiverMutability::Mutable &&
      !receiverModeAllowsMutation(receiverMode)) {
    if (member != nullptr) {
      reportMutableReceiverFailure(member->object(), paren, function,
                                   "Mutable method");
    } else {
      report(paren, "Mutable method requires a mutable receiver.");
    }
    return false;
  }
  if (receiverPlace) {
    SemanticType receiverType = SemanticType::Unknown;
    if (member != nullptr) {
      if (const SemanticType *resolved =
              semanticModel.findType(*member->object())) {
        receiverType = *resolved;
      }
    } else if (currentClass) {
      receiverType = openClassType(*currentClass);
    }
    const bool moveOnlyReceiver =
        selectedReceiverAccess == AccessMode::Mutable &&
        isMoveOnlyOwnerType(receiverType);
    reportLoanPlaceConflict(
        paren, *receiverPlace, callReceiverAccess,
        moveOnlyReceiver
            ? "Mutable method cannot use move-only storage while a "
              "reference borrowed from it may still be live."
        : receiverAllowsMutation(function.receiverMutability)
            ? "Mutable method receiver overlaps a borrow that may still be "
              "live."
            : "Read-only method receiver overlaps a mutable borrow or "
              "child reborrow that may still be live.",
        receiverLoan);
  }
  return true;
}

auto SemanticVisitor::Impl::structuralOperatorCandidates(
    const SemanticType &receiver, OverloadedOperator kind) const
    -> std::vector<CallableCandidate> {
  const ClassInfo *owner = classInfo(receiver);
  if (owner == nullptr) {
    return {};
  }
  const auto found =
      owner->members.find(std::string(operatorFunctionName(kind)));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function) {
    return {};
  }
  const Symbol overloadSet = substituteSymbol(found->second.symbol, receiver);
  std::vector<CallableCandidate> result;
  for (const CallableCandidate &candidate : overloadSet.overloads) {
    if (!candidate.staticMember && candidate.access == AccessModifier::Public &&
        !candidate.compilerPrivate && candidate.requirements.empty()) {
      result.emplace_back(candidate);
    }
  }
  return result;
}

auto SemanticVisitor::Impl::selectStructuralOperator(
    const SemanticType &receiver, OverloadedOperator kind,
    std::span<const SemanticType> parameterTypes, bool mutableReceiver) const
    -> std::optional<CallableCandidate> {
  std::vector<CallableCandidate> candidates =
      structuralOperatorCandidates(receiver, kind);
  std::erase_if(candidates, [&](const CallableCandidate &candidate) {
    return !std::equal(candidate.parameterTypes.begin(),
                       candidate.parameterTypes.end(), parameterTypes.begin(),
                       parameterTypes.end());
  });
  if (!mutableReceiver) {
    std::erase_if(candidates, [](const CallableCandidate &candidate) {
      return candidate.receiverMutability == ReceiverMutability::Mutable;
    });
  } else if (std::any_of(candidates.begin(), candidates.end(),
                         [](const CallableCandidate &candidate) {
                           return candidate.receiverMutability ==
                                  ReceiverMutability::Mutable;
                         })) {
    std::erase_if(candidates, [](const CallableCandidate &candidate) {
      return candidate.receiverMutability == ReceiverMutability::ReadOnly;
    });
  }
  if (candidates.size() != 1) {
    return std::nullopt;
  }
  return candidates.front();
}

auto SemanticVisitor::Impl::symbolicReadMemberReturnType(
    const SemanticType &receiver, std::string_view member) const
    -> std::optional<SemanticType> {
  std::optional<SemanticType> result;
  for (const auto &requirement : activeStructuralRequirements()) {
    if (requirement.kind != StructuralConstraintKind::ReadMember ||
        requirement.memberName != member || requirement.arguments.size() < 2 ||
        requirement.arguments[0] != receiver) {
      continue;
    }
    // This query establishes member presence for callee analysis. The call
    // checks the complete signature and selects its exact result separately.
    if (!result)
      result = requirement.arguments[1];
  }
  return result;
}

auto SemanticVisitor::Impl::readMemberRequirementSatisfied(
    const SemanticType &receiver, std::string_view member,
    const SemanticType &result, SemanticMemberRequirementFailure *failure,
    std::span<const SemanticType> parameters, bool mutableReceiver,
    bool requiresUnsafe, bool trustedMemberAccess) const -> bool {
  const auto reject = [&](MemberRequirementFailureKind kind,
                          const CallableCandidate *candidate = nullptr) {
    if (failure != nullptr) {
      *failure = {.kind = kind, .requiredResult = result};
      if (candidate != nullptr) {
        failure->actualResult = candidate->returnType;
        if (candidate->declaration != nullptr) {
          failure->declaration = candidate->declaration->name();
        }
      }
    }
    return false;
  };
  // A nominal generic class already supplies a declaration-owned member set.
  // Unresolved payload parameters do not hide an otherwise concrete signature.
  // A bare parameter (or an open required signature) still needs an explicit
  // enclosing requirement; do not infer members on arbitrary unknown types.
  if ((containsOpenTypeParameter(receiver) &&
       receiver.kind != SemanticType::Class) ||
      containsOpenTypeParameter(result) ||
      std::any_of(parameters.begin(), parameters.end(),
                  containsOpenTypeParameter)) {
    for (const auto &requirement : activeStructuralRequirements()) {
      if (requirement.kind == StructuralConstraintKind::ReadMember &&
          requirement.memberName == member &&
          requirement.mutableReceiver == mutableReceiver &&
          (requiresUnsafe || !requirement.requiresUnsafe) &&
          requirement.arguments.size() == parameters.size() + 2 &&
          requirement.arguments[0] == receiver &&
          requirement.arguments[1] == result &&
          std::equal(parameters.begin(), parameters.end(),
                     requirement.arguments.begin() + 2))
        return true;
    }
    return reject(MemberRequirementFailureKind::Unproven);
  }
  // Bound recursively growing receiver types as well as exact cycles.
  constexpr std::size_t memberRequirementDepthLimit = 64;
  if (activeMemberRequirements.size() >= memberRequirementDepthLimit ||
      std::any_of(activeMemberRequirements.begin(),
                  activeMemberRequirements.end(), [&](const auto &active) {
                    return active.receiver == receiver &&
                           active.member == member && active.result == result &&
                           active.mutableReceiver == mutableReceiver &&
                           active.requiresUnsafe == requiresUnsafe &&
                           std::ranges::equal(active.parameters, parameters);
                  }))
    return reject(MemberRequirementFailureKind::Recursive);
  activeMemberRequirements.push_back(
      {receiver, std::string(member), result,
       std::vector<SemanticType>(parameters.begin(), parameters.end()),
       mutableReceiver, requiresUnsafe});
  struct PopRequirement {
    std::vector<ActiveMemberRequirement> &stack;
    ~PopRequirement() { stack.pop_back(); }
  } pop{activeMemberRequirements};
  const auto *owner = classInfo(receiver);
  if (!owner)
    return reject(MemberRequirementFailureKind::Missing);
  const auto found = owner->members.find(std::string(member));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function)
    return reject(MemberRequirementFailureKind::Missing);
  const auto overloads = substituteSymbol(found->second.symbol, receiver);
  const CallableCandidate *selected = nullptr;
  const CallableCandidate *rejectedShape = nullptr;
  const CallableCandidate *rejectedConstraints = nullptr;
  for (const auto &candidate : overloads.overloads) {
    if (candidate.staticMember || candidate.parameterPack ||
        !std::ranges::equal(candidate.parameterTypes, parameters) ||
        candidate.receiverMutability != (mutableReceiver
                                             ? ReceiverMutability::Mutable
                                             : ReceiverMutability::ReadOnly) ||
        !candidate.declaration ||
        !candidate.declaration->genericParameters().empty()) {
      if (rejectedShape == nullptr)
        rejectedShape = &candidate;
      continue;
    }
    if (firstRequirementFailure(candidate.requirements)) {
      if (rejectedConstraints == nullptr)
        rejectedConstraints = &candidate;
      continue;
    }
    if (selected)
      return reject(MemberRequirementFailureKind::Ambiguous, selected);
    selected = &candidate;
  }
  if (selected == nullptr) {
    return rejectedConstraints != nullptr
               ? reject(MemberRequirementFailureKind::Constraints,
                        rejectedConstraints)
               : reject(MemberRequirementFailureKind::Shape, rejectedShape);
  }
  if (selected->access != AccessModifier::Public)
    return reject(MemberRequirementFailureKind::Inaccessible, selected);
  // Admission follows a call written at the requirement: trusted source may
  // select a compiler-private member, application source and concepts never.
  if (selected->compilerPrivate && !trustedMemberAccess)
    return reject(MemberRequirementFailureKind::CompilerPrivate, selected);
  if (selected->returnType != result)
    return reject(MemberRequirementFailureKind::Result, selected);
  if (selected->record().requiresUnsafe && !requiresUnsafe)
    return reject(MemberRequirementFailureKind::Unsafe, selected);
  if (selected->returnBorrowOrigin != BorrowOriginKind::None ||
      result.kind == SemanticType::Reference ||
      typeTraits(result).containsBorrowedState)
    return reject(MemberRequirementFailureKind::BorrowedResult, selected);
  return true;
}

auto SemanticVisitor::Impl::symbolicRangeMemberReturnType(
    const SemanticType &rangeType, std::string_view member) const
    -> std::optional<SemanticType> {
  if (rangeType.kind != SemanticType::TypeParameter || rangeType.classId != 0 ||
      !rangeType.arguments.empty()) {
    return std::nullopt;
  }

  bool readable = false;
  bool writable = false;
  bool sized = false;
  for (const ExpandedStructuralRequirement &requirement :
       activeStructuralRequirements()) {
    if (requirement.arguments.empty() ||
        requirement.arguments.front() != rangeType) {
      continue;
    }
    switch (requirement.kind) {
    case StructuralConstraintKind::InputRange:
    case StructuralConstraintKind::RangeOf:
    case StructuralConstraintKind::RangePredicate:
    case StructuralConstraintKind::RangeOperation:
      readable = true;
      break;
    case StructuralConstraintKind::MutableRangeOf:
      readable = true;
      writable = true;
      break;
    case StructuralConstraintKind::SizedRange:
      sized = true;
      break;
    default:
      break;
    }
  }

  if (member == "begin" && readable) {
    return symbolicRangeCarrierType(rangeType,
                                    SymbolicRangeCarrier::ReadIterator);
  }
  if (member == "begin_mut" && writable) {
    return symbolicRangeCarrierType(rangeType,
                                    SymbolicRangeCarrier::MutableIterator);
  }
  if (member == "end" && readable) {
    return symbolicRangeCarrierType(rangeType, SymbolicRangeCarrier::Sentinel);
  }
  if (member == "size" && sized) {
    return SemanticType::UInt64;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::structuralConstraintSatisfied(
    StructuralConstraintKind kind,
    std::span<const SemanticType> arguments) const -> bool {
  if (std::any_of(arguments.begin(), arguments.end(),
                  [](const SemanticType &type) {
                    return type == SemanticType::Unknown;
                  })) {
    return true;
  }
  if (std::any_of(arguments.begin(), arguments.end(),
                  [](const SemanticType &type) {
                    return type.kind == SemanticType::TypeParameter ||
                           type.kind == SemanticType::TypePack;
                  })) {
    return structuralRequirementAvailable(kind, arguments);
  }

  const auto selectedDereference = [&](const SemanticType &iterator) {
    return selectStructuralOperator(iterator, OverloadedOperator::Dereference,
                                    {}, false);
  };
  const auto readableElement =
      [&](const CallableCandidate &candidate) -> std::optional<SemanticType> {
    const auto &result = candidate.returnType;
    if (result.kind == SemanticType::Reference && result.arguments.size() == 1)
      return result.arguments.front();
    if (candidate.receiverMutability == ReceiverMutability::ReadOnly &&
        candidate.returnBorrowOrigin == BorrowOriginKind::None &&
        readableIteratorValue(result))
      return result;
    return std::nullopt;
  };
  const auto selectedSentinelOperator = [&](const SemanticType &iterator,
                                            OverloadedOperator operation,
                                            const SemanticType &sentinel) {
    const SemanticType reference =
        SemanticType::referenceTo(sentinel, AccessMode::ReadOnly);
    auto borrowed = selectStructuralOperator(
        iterator, operation, std::span<const SemanticType>(&reference, 1),
        false);
    auto copied = selectStructuralOperator(
        iterator, operation, std::span<const SemanticType>(&sentinel, 1),
        false);
    // Both forms accept a named sentinel. The requirement must not claim
    // a usable operation when that ordinary call would be ambiguous, or
    // when its by-value parameter cannot copy the named argument.
    if (borrowed &&
        borrowed->receiverMutability != ReceiverMutability::ReadOnly)
      borrowed.reset();
    if (copied && copied->receiverMutability != ReceiverMutability::ReadOnly)
      copied.reset();
    if (borrowed && copied)
      return std::optional<CallableCandidate>{};
    return copied && typeTraits(sentinel).copyable ? copied : borrowed;
  };
  const auto hasDereference = [&](const SemanticType &iterator,
                                  const SemanticType *referent) {
    const std::optional<CallableCandidate> candidate =
        selectedDereference(iterator);
    if (!candidate) {
      return false;
    }
    const auto element = readableElement(*candidate);
    return element && (referent == nullptr || *element == *referent);
  };
  const auto mutableReference = [](const CallableCandidate &candidate,
                                   const SemanticType *referent) {
    if (candidate.receiverMutability != ReceiverMutability::Mutable ||
        candidate.returnType.kind != SemanticType::Reference ||
        candidate.returnType.referenceAccess != AccessMode::Mutable ||
        candidate.returnType.arguments.size() != 1) {
      return false;
    }
    return referent == nullptr ||
           candidate.returnType.arguments.front() == *referent;
  };
  const auto hasMutableDereference = [&](const SemanticType &iterator,
                                         const SemanticType *referent) {
    const std::optional<CallableCandidate> candidate = selectStructuralOperator(
        iterator, OverloadedOperator::Dereference, {}, true);
    return candidate && mutableReference(*candidate, referent);
  };
  const auto hasVoidMutation = [&](const SemanticType &iterator,
                                   OverloadedOperator mutation,
                                   std::span<const SemanticType> parameters) {
    const std::optional<CallableCandidate> candidate =
        selectStructuralOperator(iterator, mutation, parameters, true);
    return candidate && candidate->returnType == SemanticType::Void &&
           candidate->receiverMutability == ReceiverMutability::Mutable;
  };
  const auto selectedRangeMember =
      [&](const SemanticType &range, std::string_view name,
          ReceiverMutability receiver) -> std::optional<CallableCandidate> {
    if (range == SemanticType::StringView &&
        receiver == ReceiverMutability::ReadOnly) {
      auto factory = textViewMemberFactory(name);
      if (factory && factory->parameterTypes.size() == 1) {
        // Structural member queries consume the same trusted declaration
        // identity as written member calls; its first input is the receiver.
        factory->parameterTypes.clear();
        return factory;
      }
      if (name == "size")
        return CallableCandidate{.returnType = SemanticType::UInt64};
      return std::nullopt;
    }
    const ClassInfo *owner = classInfo(range);
    if (owner == nullptr) {
      return std::nullopt;
    }
    const auto found = owner->members.find(std::string(name));
    if (found == owner->members.end() ||
        found->second.symbol.type != SemanticType::Function) {
      return std::nullopt;
    }
    const Symbol overloadSet = substituteSymbol(found->second.symbol, range);
    std::vector<CallableCandidate> candidates;
    for (const CallableCandidate &candidate : overloadSet.overloads) {
      if (!candidate.staticMember &&
          candidate.access == AccessModifier::Public &&
          !candidate.compilerPrivate && candidate.requirements.empty() &&
          candidate.parameterTypes.empty() &&
          candidate.receiverMutability == receiver) {
        candidates.emplace_back(candidate);
      }
    }
    return candidates.size() == 1
               ? std::optional<CallableCandidate>{candidates.front()}
               : std::nullopt;
  };
  struct ReadRangeShape {
    SemanticType iterator;
    SemanticType sentinel;
  };
  const auto readRangeShape =
      [&](const SemanticType &range) -> std::optional<ReadRangeShape> {
    const std::optional<CallableCandidate> begin =
        selectedRangeMember(range, "begin", ReceiverMutability::ReadOnly);
    const std::optional<CallableCandidate> end =
        selectedRangeMember(range, "end", ReceiverMutability::ReadOnly);
    if (!begin || !end || begin->returnType.kind == SemanticType::Reference ||
        end->returnType.kind == SemanticType::Reference ||
        begin->returnType == SemanticType::Void ||
        end->returnType == SemanticType::Void) {
      return std::nullopt;
    }
    return ReadRangeShape{.iterator = begin->returnType,
                          .sentinel = end->returnType};
  };
  const auto mutableRangeShape =
      [&](const SemanticType &range) -> std::optional<ReadRangeShape> {
    const std::optional<CallableCandidate> begin =
        selectedRangeMember(range, "begin_mut", ReceiverMutability::Mutable);
    const std::optional<CallableCandidate> end =
        selectedRangeMember(range, "end", ReceiverMutability::ReadOnly);
    if (!begin || !end || begin->returnType.kind == SemanticType::Reference ||
        end->returnType.kind == SemanticType::Reference ||
        begin->returnType == SemanticType::Void ||
        end->returnType == SemanticType::Void) {
      return std::nullopt;
    }
    return ReadRangeShape{.iterator = begin->returnType,
                          .sentinel = end->returnType};
  };

  // A domain value operator is an exact read-only member returning an owned
  // value of the same domain type. The result may not be a reference: a
  // borrowed result would tie the produced value to a receiver whose
  // lifetime the requirement cannot name.
  const auto ownedDomainResult = [](const CallableCandidate &candidate,
                                    const SemanticType &domain) {
    return candidate.returnType == domain &&
           candidate.receiverMutability == ReceiverMutability::ReadOnly &&
           candidate.returnBorrowOrigin == BorrowOriginKind::None;
  };
  const auto hasBinaryDomainOperator = [&](const SemanticType &domain,
                                           OverloadedOperator oper,
                                           const SemanticType &operand) {
    const SemanticType parameter =
        SemanticType::referenceTo(operand, AccessMode::ReadOnly);
    const std::optional<CallableCandidate> byReference =
        selectStructuralOperator(
            domain, oper, std::span<const SemanticType>(&parameter, 1), false);
    if (byReference && ownedDomainResult(*byReference, domain)) {
      return true;
    }
    // A scalar operand is ordinarily taken by value.
    const std::optional<CallableCandidate> byValue = selectStructuralOperator(
        domain, oper, std::span<const SemanticType>(&operand, 1), false);
    return byValue && ownedDomainResult(*byValue, domain);
  };

  const auto readableCallableParameter = [](const SemanticType &parameter,
                                            const SemanticType &argument) {
    return parameter == argument ||
           (parameter.kind == SemanticType::Reference &&
            parameter.referenceAccess == AccessMode::ReadOnly &&
            parameter.arguments.size() == 1 &&
            parameter.arguments.front() == argument);
  };
  const auto callableCandidates = [&](const SemanticType &callable) {
    if (callable.kind == SemanticType::Class) {
      return structuralOperatorCandidates(callable, OverloadedOperator::Call);
    }
    std::vector<CallableCandidate> result;
    if (callable.hasFunctionItemShape()) {
      const FunctionInfo *function =
          semanticModel.findFunction(callable.functionId);
      if (function == nullptr && instanceBaseModel != nullptr) {
        function = instanceBaseModel->findFunction(callable.functionId);
      }
      const SemanticType *returnType = callable.functionItemReturnType();
      if (function != nullptr && returnType != nullptr &&
          function->ownerClass == 0 && !function->entryPoint &&
          function->linkage == LanguageLinkage::Gti &&
          function->returnType == *returnType &&
          function->parameterTypes.size() ==
              callable.functionItemParameterTypes().size() &&
          std::equal(function->parameterTypes.begin(),
                     function->parameterTypes.end(),
                     callable.functionItemParameterTypes().begin())) {
        result.push_back({.id = function->id,
                          .returnType = function->returnType,
                          .parameterTypes = function->parameterTypes,
                          .receiverMutability = ReceiverMutability::ReadOnly,
                          .access = AccessModifier::Public});
      }
      return result;
    }
    if (callable.kind != SemanticType::Lambda) {
      return result;
    }
    const LambdaInfo *lambda = semanticModel.findLambda(callable.lambdaId);
    if (lambda == nullptr && instanceBaseModel != nullptr) {
      lambda = instanceBaseModel->findLambda(callable.lambdaId);
    }
    if (lambda != nullptr) {
      result.push_back({.returnType = lambda->returnType,
                        .parameterTypes = lambda->parameterTypes,
                        .receiverMutability = ReceiverMutability::ReadOnly,
                        .access = AccessModifier::Public});
    }
    return result;
  };
  const auto callableSatisfies = [&](const SemanticType &callable,
                                     std::span<const SemanticType> parameters,
                                     const SemanticType &result,
                                     CallableInvocationCapability required,
                                     bool referenceArguments = true) {
    if (result.kind == SemanticType::Reference ||
        typeTraits(result).containsBorrowedState ||
        containsLambdaType(result)) {
      return false;
    }
    std::vector<CallableCandidate> candidates = callableCandidates(callable);
    std::erase_if(candidates, [&](const CallableCandidate &candidate) {
      if (candidate.returnType != result ||
          candidate.returnBorrowOrigin != BorrowOriginKind::None ||
          candidate.parameterTypes.size() != parameters.size() ||
          !callableCapabilitySatisfies(
              callableInvocationCapability(candidate.receiverMutability),
              required)) {
        return true;
      }
      for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (referenceArguments
                ? !readableCallableParameter(candidate.parameterTypes[index],
                                             parameters[index])
                : candidate.parameterTypes[index] != parameters[index]) {
          return true;
        }
      }
      return false;
    });
    if (required == CallableInvocationCapability::Once &&
        std::any_of(candidates.begin(), candidates.end(),
                    [](const CallableCandidate &candidate) {
                      return candidate.receiverMutability ==
                             ReceiverMutability::Consuming;
                    })) {
      std::erase_if(candidates, [](const CallableCandidate &candidate) {
        return candidate.receiverMutability != ReceiverMutability::Consuming;
      });
    } else if (required != CallableInvocationCapability::Read &&
               std::any_of(candidates.begin(), candidates.end(),
                           [](const CallableCandidate &candidate) {
                             return candidate.receiverMutability ==
                                    ReceiverMutability::Mutable;
                           })) {
      std::erase_if(candidates, [](const CallableCandidate &candidate) {
        return candidate.receiverMutability == ReceiverMutability::ReadOnly;
      });
    }
    return candidates.size() == 1;
  };
  const auto unaryCallable = [&](CallableInvocationCapability capability) {
    return arguments.size() == 3 &&
           callableSatisfies(arguments[0],
                             std::span<const SemanticType>(&arguments[1], 1),
                             arguments[2], capability);
  };
  const auto binaryCallable = [&](CallableInvocationCapability capability) {
    return arguments.size() == 4 &&
           callableSatisfies(arguments[0], arguments.subspan(1, 2),
                             arguments[3], capability);
  };
  const auto readableRangeElement =
      [&](const SemanticType &range) -> std::optional<SemanticType> {
    if (range.kind == SemanticType::Array && range.arguments.size() == 1) {
      return range.arguments.front();
    }
    const std::optional<ReadRangeShape> shape = readRangeShape(range);
    if (!shape) {
      return std::nullopt;
    }
    const std::array<SemanticType, 1> iteratorArguments{shape->iterator};
    const std::array<SemanticType, 2> sentinelArguments{shape->sentinel,
                                                        shape->iterator};
    if (!structuralConstraintSatisfied(StructuralConstraintKind::InputIterator,
                                       iteratorArguments) ||
        !structuralConstraintSatisfied(StructuralConstraintKind::SentinelFor,
                                       sentinelArguments)) {
      return std::nullopt;
    }
    const std::optional<CallableCandidate> dereference =
        selectedDereference(shape->iterator);
    if (!dereference) {
      return std::nullopt;
    }
    const auto &result = dereference->returnType;
    if (result.kind == SemanticType::Reference &&
        result.arguments.size() == 1 &&
        result.referenceAccess == AccessMode::ReadOnly)
      return result.arguments.front();
    if (result.kind != SemanticType::Reference)
      return readableElement(*dereference);
    return std::nullopt;
  };

  switch (kind) {
  case StructuralConstraintKind::InputIterator: {
    if (arguments.size() != 1) {
      return false;
    }
    const SemanticType &iterator = arguments.front();
    if (!hasDereference(iterator, nullptr)) {
      return false;
    }
    const std::optional<CallableCandidate> increment = selectStructuralOperator(
        iterator, OverloadedOperator::PreIncrement, {}, true);
    return increment && increment->returnType == SemanticType::Void &&
           increment->receiverMutability == ReceiverMutability::Mutable;
  }
  case StructuralConstraintKind::SentinelFor: {
    if (arguments.size() != 2) {
      return false;
    }
    const SemanticType &sentinel = arguments[0];
    const SemanticType &iterator = arguments[1];
    const std::optional<CallableCandidate> comparison =
        selectedSentinelOperator(iterator, OverloadedOperator::NotEqual,
                                 sentinel);
    return comparison && comparison->returnType == SemanticType::Bool &&
           comparison->receiverMutability == ReceiverMutability::ReadOnly;
  }
  case StructuralConstraintKind::DereferencesTo:
    return arguments.size() == 2 && hasDereference(arguments[0], &arguments[1]);
  case StructuralConstraintKind::WritableIterator:
    return arguments.size() == 1 &&
           hasMutableDereference(arguments.front(), nullptr) &&
           hasVoidMutation(arguments.front(), OverloadedOperator::PreIncrement,
                           {});
  case StructuralConstraintKind::WritesTo:
    return arguments.size() == 2 &&
           hasMutableDereference(arguments[0], &arguments[1]);
  case StructuralConstraintKind::BidirectionalIterator:
    return arguments.size() == 1 &&
           hasVoidMutation(arguments.front(), OverloadedOperator::PreDecrement,
                           {});
  case StructuralConstraintKind::RandomAccessIterator: {
    if (arguments.size() != 1) {
      return false;
    }
    const SemanticType distance = SemanticType::Int64;
    return hasVoidMutation(arguments.front(), OverloadedOperator::AddAssign,
                           std::span<const SemanticType>(&distance, 1)) &&
           hasVoidMutation(arguments.front(),
                           OverloadedOperator::SubtractAssign,
                           std::span<const SemanticType>(&distance, 1));
  }
  case StructuralConstraintKind::SubscriptsToMut: {
    if (arguments.size() != 2) {
      return false;
    }
    const SemanticType index = SemanticType::Int64;
    const std::optional<CallableCandidate> subscript = selectStructuralOperator(
        arguments[0], OverloadedOperator::Subscript,
        std::span<const SemanticType>(&index, 1), true);
    return subscript && mutableReference(*subscript, &arguments[1]);
  }
  case StructuralConstraintKind::SizedSentinelFor: {
    if (arguments.size() != 2) {
      return false;
    }
    const SemanticType &sentinel = arguments[0];
    const SemanticType &iterator = arguments[1];
    const std::optional<CallableCandidate> difference =
        selectedSentinelOperator(iterator, OverloadedOperator::Minus, sentinel);
    return difference && difference->returnType == SemanticType::Int64 &&
           difference->receiverMutability == ReceiverMutability::ReadOnly;
  }
  case StructuralConstraintKind::InputRange:
    return arguments.size() == 1 &&
           readableRangeElement(arguments.front()).has_value();
  case StructuralConstraintKind::RangeOf: {
    if (arguments.size() != 2) {
      return false;
    }
    const std::optional<SemanticType> element =
        readableRangeElement(arguments[0]);
    return element && *element == arguments[1];
  }
  case StructuralConstraintKind::MutableRangeOf: {
    if (arguments.size() != 2) {
      return false;
    }
    if (arguments[0].kind == SemanticType::Array &&
        arguments[0].arguments.size() == 1) {
      return arguments[0].arguments.front() == arguments[1];
    }
    const std::optional<ReadRangeShape> shape = mutableRangeShape(arguments[0]);
    if (!shape) {
      return false;
    }
    const std::array<SemanticType, 1> iteratorArguments{shape->iterator};
    const std::array<SemanticType, 2> sentinelArguments{shape->sentinel,
                                                        shape->iterator};
    const std::array<SemanticType, 2> writeArguments{shape->iterator,
                                                     arguments[1]};
    return structuralConstraintSatisfied(
               StructuralConstraintKind::WritableIterator, iteratorArguments) &&
           structuralConstraintSatisfied(StructuralConstraintKind::SentinelFor,
                                         sentinelArguments) &&
           structuralConstraintSatisfied(StructuralConstraintKind::WritesTo,
                                         writeArguments);
  }
  case StructuralConstraintKind::SizedRange: {
    if (arguments.size() != 1) {
      return false;
    }
    if (arguments.front().kind == SemanticType::Array &&
        arguments.front().arguments.size() == 1) {
      return true;
    }
    const std::optional<CallableCandidate> size = selectedRangeMember(
        arguments.front(), "size", ReceiverMutability::ReadOnly);
    return size && size->returnType == SemanticType::UInt64;
  }
  // The domain family is deliberately member-only, matching the language's
  // member-only user-operator rule. A built-in numeric keeps its own
  // checked arithmetic and its `std::numeric` contract: admitting one here
  // would resolve its operators as symbolic member calls and drop the
  // canonical local failure outcomes that checked integer arithmetic must
  // retain.
  case StructuralConstraintKind::Adds:
    return arguments.size() == 1 &&
           hasBinaryDomainOperator(arguments[0], OverloadedOperator::Plus,
                                   arguments[0]);
  case StructuralConstraintKind::Subtracts:
    return arguments.size() == 1 &&
           hasBinaryDomainOperator(arguments[0], OverloadedOperator::Minus,
                                   arguments[0]);
  case StructuralConstraintKind::Negates: {
    if (arguments.size() != 1) {
      return false;
    }
    // Unary minus is the nullary-parameter member form; the binary
    // difference operator shares the spelling and is excluded by arity.
    const std::optional<CallableCandidate> negate = selectStructuralOperator(
        arguments[0], OverloadedOperator::Minus, {}, false);
    return negate && ownedDomainResult(*negate, arguments[0]);
  }
  case StructuralConstraintKind::ScalesBy:
    return arguments.size() == 2 &&
           hasBinaryDomainOperator(arguments[0], OverloadedOperator::Multiply,
                                   arguments[1]);
  case StructuralConstraintKind::EnumUnderlying:
    return arguments.size() == 2 && arguments[0].kind == SemanticType::Enum &&
           satisfiesAtomicConstraint(arguments[0],
                                     GenericConstraintKind::IntegralEnum) &&
           enums[arguments[0].enumId - 1].underlyingType == arguments[1];
  case StructuralConstraintKind::ObjectLayout:
    return arguments.size() == 1 && runtimeObjectLayoutType(arguments.front());
  case StructuralConstraintKind::IndependentObject:
    return arguments.size() == 1 &&
           runtimeObjectLayoutType(arguments.front()) &&
           !typeTraits(arguments.front()).containsBorrowedState;
  case StructuralConstraintKind::InlinePolymorphicPayload:
    return arguments.size() == 2 &&
           inlinePolymorphicPayload(arguments[0], arguments[1]);
  case StructuralConstraintKind::ReadCallable:
    return unaryCallable(CallableInvocationCapability::Read);
  case StructuralConstraintKind::MutCallable:
    return unaryCallable(CallableInvocationCapability::Mutable);
  case StructuralConstraintKind::OnceCallable:
    return unaryCallable(CallableInvocationCapability::Once);
  case StructuralConstraintKind::ReadBinaryCallable:
    return binaryCallable(CallableInvocationCapability::Read);
  case StructuralConstraintKind::MutBinaryCallable:
    return binaryCallable(CallableInvocationCapability::Mutable);
  case StructuralConstraintKind::OnceBinaryCallable:
    return binaryCallable(CallableInvocationCapability::Once);
  case StructuralConstraintKind::OnceAction:
    return arguments.size() == 1 &&
           callableSatisfies(arguments[0], {}, SemanticType::Void,
                             CallableInvocationCapability::Once);
  case StructuralConstraintKind::IndirectPredicate:
  case StructuralConstraintKind::IndirectOperation: {
    if (arguments.size() !=
        (kind == StructuralConstraintKind::IndirectPredicate ? 2 : 3)) {
      return false;
    }
    const std::optional<CallableCandidate> dereference =
        selectedDereference(arguments[1]);
    const auto element =
        dereference ? readableElement(*dereference) : std::nullopt;
    if (!element) {
      return false;
    }
    const SemanticType &result =
        kind == StructuralConstraintKind::IndirectPredicate ? SemanticType::Bool
                                                            : arguments[2];
    return callableSatisfies(
        arguments[0], std::span<const SemanticType>(&*element, 1), result,
        CallableInvocationCapability::Mutable,
        dereference->returnType.kind == SemanticType::Reference);
  }
  case StructuralConstraintKind::RangePredicate:
  case StructuralConstraintKind::RangeOperation: {
    if (arguments.size() !=
        (kind == StructuralConstraintKind::RangePredicate ? 2 : 3)) {
      return false;
    }
    const std::optional<SemanticType> element =
        readableRangeElement(arguments[1]);
    if (!element) {
      return false;
    }
    const SemanticType &result =
        kind == StructuralConstraintKind::RangePredicate ? SemanticType::Bool
                                                         : arguments[2];
    return callableSatisfies(arguments[0],
                             std::span<const SemanticType>(&*element, 1),
                             result, CallableInvocationCapability::Mutable);
  }
  case StructuralConstraintKind::None:
    return true;
  case StructuralConstraintKind::ReadMember:
    // Exact member requirements also carry a name, and are checked by
    // readMemberRequirementSatisfied rather than this name-free interface.
    return false;
  }
  return false;
}

auto SemanticVisitor::Impl::symbolicOperatorRequirements(
    OverloadedOperator kind, const SemanticType &receiverType,
    std::span<const SemanticType> argumentTypes) const
    -> std::vector<CallableCandidate> {
  std::vector<CallableCandidate> result;
  const auto rangeCarrier = symbolicRangeCarrier(receiverType);
  for (const ExpandedStructuralRequirement &requirement :
       activeStructuralRequirements()) {
    CallableCandidate candidate;
    bool matches = false;
    switch (requirement.kind) {
    case StructuralConstraintKind::InputIterator:
      if (requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        if (kind == OverloadedOperator::PreIncrement) {
          candidate.returnType = SemanticType::Void;
          candidate.receiverMutability = ReceiverMutability::Mutable;
          matches = true;
        } else if (kind == OverloadedOperator::Dereference) {
          candidate.returnType = SemanticType::referenceTo(
              SemanticType::Unknown, AccessMode::ReadOnly);
          candidate.receiverMutability = ReceiverMutability::ReadOnly;
          candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
          candidate.returnBorrowAccess = AccessMode::ReadOnly;
          matches = true;
        }
      }
      break;
    case StructuralConstraintKind::SentinelFor:
      if (kind == OverloadedOperator::NotEqual &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[1] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == requirement.arguments[0]) {
        candidate.returnType = SemanticType::Bool;
        candidate.parameterTypes = {SemanticType::referenceTo(
            requirement.arguments[0], AccessMode::ReadOnly)};
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::DereferencesTo:
      if (kind == OverloadedOperator::Dereference &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        candidate.returnType = SemanticType::referenceTo(
            requirement.arguments[1], AccessMode::ReadOnly);
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
        candidate.returnBorrowAccess = AccessMode::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::WritableIterator:
      if (requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        if (kind == OverloadedOperator::PreIncrement) {
          candidate.returnType = SemanticType::Void;
          candidate.receiverMutability = ReceiverMutability::Mutable;
          matches = true;
        } else if (kind == OverloadedOperator::Dereference) {
          candidate.returnType = SemanticType::referenceTo(
              SemanticType::Unknown, AccessMode::Mutable);
          candidate.receiverMutability = ReceiverMutability::Mutable;
          candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
          candidate.returnBorrowAccess = AccessMode::Mutable;
          matches = true;
        }
      }
      break;
    case StructuralConstraintKind::WritesTo:
      if (kind == OverloadedOperator::Dereference &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        candidate.returnType = SemanticType::referenceTo(
            requirement.arguments[1], AccessMode::Mutable);
        candidate.receiverMutability = ReceiverMutability::Mutable;
        candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
        candidate.returnBorrowAccess = AccessMode::Mutable;
        matches = true;
      }
      break;
    case StructuralConstraintKind::BidirectionalIterator:
      if (kind == OverloadedOperator::PreDecrement &&
          requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        candidate.returnType = SemanticType::Void;
        candidate.receiverMutability = ReceiverMutability::Mutable;
        matches = true;
      }
      break;
    case StructuralConstraintKind::RandomAccessIterator:
      if ((kind == OverloadedOperator::AddAssign ||
           kind == OverloadedOperator::SubtractAssign) &&
          requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == SemanticType::Int64) {
        candidate.returnType = SemanticType::Void;
        candidate.parameterTypes = {SemanticType::Int64};
        candidate.receiverMutability = ReceiverMutability::Mutable;
        matches = true;
      }
      break;
    case StructuralConstraintKind::SubscriptsToMut:
      if (kind == OverloadedOperator::Subscript &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[0] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == SemanticType::Int64) {
        candidate.returnType = SemanticType::referenceTo(
            requirement.arguments[1], AccessMode::Mutable);
        candidate.parameterTypes = {SemanticType::Int64};
        candidate.receiverMutability = ReceiverMutability::Mutable;
        candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
        candidate.returnBorrowAccess = AccessMode::Mutable;
        matches = true;
      }
      break;
    case StructuralConstraintKind::SizedSentinelFor:
      if (kind == OverloadedOperator::Minus &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[1] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == requirement.arguments[0]) {
        candidate.returnType = SemanticType::Int64;
        candidate.parameterTypes = {SemanticType::referenceTo(
            requirement.arguments[0], AccessMode::ReadOnly)};
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::InputRange:
    case StructuralConstraintKind::RangeOf:
    case StructuralConstraintKind::MutableRangeOf: {
      if (!rangeCarrier || requirement.arguments.empty() ||
          requirement.arguments.front() != rangeCarrier->first) {
        break;
      }
      const bool mutableRange =
          requirement.kind == StructuralConstraintKind::MutableRangeOf;
      const SymbolicRangeCarrier expectedCarrier =
          mutableRange ? SymbolicRangeCarrier::MutableIterator
                       : SymbolicRangeCarrier::ReadIterator;
      if (rangeCarrier->second != expectedCarrier) {
        break;
      }
      const SemanticType sentinel = symbolicRangeCarrierType(
          rangeCarrier->first, SymbolicRangeCarrier::Sentinel);
      if (kind == OverloadedOperator::PreIncrement && argumentTypes.empty()) {
        candidate.returnType = SemanticType::Void;
        candidate.receiverMutability = ReceiverMutability::Mutable;
        matches = true;
      } else if (kind == OverloadedOperator::Dereference &&
                 argumentTypes.empty()) {
        const SemanticType element = requirement.arguments.size() == 2
                                         ? requirement.arguments[1]
                                         : SemanticType::Unknown;
        candidate.returnType = SemanticType::referenceTo(
            element, mutableRange ? AccessMode::Mutable : AccessMode::ReadOnly);
        candidate.receiverMutability = mutableRange
                                           ? ReceiverMutability::Mutable
                                           : ReceiverMutability::ReadOnly;
        candidate.returnBorrowOrigin = BorrowOriginKind::Receiver;
        candidate.returnBorrowAccess =
            mutableRange ? AccessMode::Mutable : AccessMode::ReadOnly;
        matches = true;
      } else if (kind == OverloadedOperator::NotEqual &&
                 argumentTypes.size() == 1 &&
                 argumentTypes.front() == sentinel) {
        candidate.returnType = SemanticType::Bool;
        candidate.parameterTypes = {
            SemanticType::referenceTo(sentinel, AccessMode::ReadOnly)};
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    }
    case StructuralConstraintKind::SizedRange:
      // size() is a generated member-call fact rather than an operator.
      break;
    case StructuralConstraintKind::EnumUnderlying:
    case StructuralConstraintKind::ObjectLayout:
    case StructuralConstraintKind::IndependentObject:
    case StructuralConstraintKind::InlinePolymorphicPayload:
      // Enum representation classification grants no numeric operators.
      break;

    // A domain operator produces an owned value of the receiver's own type.
    // The operand is named by reference so the concrete member may take it
    // either way; exact argument matching selects the real declaration in
    // the instance reanalysis.
    case StructuralConstraintKind::Adds:
    case StructuralConstraintKind::Subtracts:
      if (requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == requirement.arguments[0] &&
          kind == (requirement.kind == StructuralConstraintKind::Adds
                       ? OverloadedOperator::Plus
                       : OverloadedOperator::Minus)) {
        candidate.returnType = requirement.arguments[0];
        candidate.parameterTypes = {SemanticType::referenceTo(
            requirement.arguments[0], AccessMode::ReadOnly)};
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::Negates:
      if (kind == OverloadedOperator::Minus &&
          requirement.arguments.size() == 1 &&
          requirement.arguments[0] == receiverType && argumentTypes.empty()) {
        candidate.returnType = requirement.arguments[0];
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::ScalesBy:
      if (kind == OverloadedOperator::Multiply &&
          requirement.arguments.size() == 2 &&
          requirement.arguments[0] == receiverType &&
          argumentTypes.size() == 1 &&
          argumentTypes[0] == requirement.arguments[1]) {
        candidate.returnType = requirement.arguments[0];
        candidate.parameterTypes = {requirement.arguments[1]};
        candidate.receiverMutability = ReceiverMutability::ReadOnly;
        matches = true;
      }
      break;
    case StructuralConstraintKind::ReadCallable:
    case StructuralConstraintKind::MutCallable:
    case StructuralConstraintKind::OnceCallable:
    case StructuralConstraintKind::ReadBinaryCallable:
    case StructuralConstraintKind::MutBinaryCallable:
    case StructuralConstraintKind::OnceBinaryCallable:
    case StructuralConstraintKind::OnceAction:
    case StructuralConstraintKind::IndirectPredicate:
    case StructuralConstraintKind::IndirectOperation:
    case StructuralConstraintKind::RangePredicate:
    case StructuralConstraintKind::RangeOperation:
      // Callable requirements constrain ordinary invocation rather than a
      // member operator expression synthesized by this table.
      break;
    case StructuralConstraintKind::ReadMember:
    case StructuralConstraintKind::None:
      break;
    }
    if (matches) {
      candidate.access = AccessModifier::Public;
      result.emplace_back(std::move(candidate));
    }
  }

  // An exact-referent dereference or subscript relation (dereferences_to,
  // writes_to, subscripts_to_mut) refines the unknown-referent form from
  // the unary capability with the same receiver mutability; the refined
  // candidate replaces only its own mutability's unknown form so a
  // read-only refinement never erases a mutable capability or vice versa.
  const auto knownReference = [](const CallableCandidate &candidate) {
    return candidate.returnType.kind == SemanticType::Reference &&
           !candidate.returnType.arguments.empty() &&
           candidate.returnType.arguments.front() != SemanticType::Unknown;
  };
  const auto unknownReference = [](const CallableCandidate &candidate) {
    return candidate.returnType.kind == SemanticType::Reference &&
           !candidate.returnType.arguments.empty() &&
           candidate.returnType.arguments.front() == SemanticType::Unknown;
  };
  if (kind == OverloadedOperator::Dereference ||
      kind == OverloadedOperator::Subscript) {
    for (const ReceiverMutability mutability :
         {ReceiverMutability::ReadOnly, ReceiverMutability::Mutable}) {
      const bool hasKnown =
          std::any_of(result.begin(), result.end(), [&](const auto &candidate) {
            return candidate.receiverMutability == mutability &&
                   knownReference(candidate);
          });
      if (hasKnown) {
        std::erase_if(result, [&](const CallableCandidate &candidate) {
          return candidate.receiverMutability == mutability &&
                 unknownReference(candidate);
        });
      }
    }
  }
  std::vector<CallableCandidate> unique;
  for (CallableCandidate &candidate : result) {
    const bool duplicate = std::any_of(
        unique.begin(), unique.end(), [&](const CallableCandidate &existing) {
          return existing.returnType == candidate.returnType &&
                 existing.parameterTypes == candidate.parameterTypes &&
                 existing.receiverMutability == candidate.receiverMutability;
        });
    if (!duplicate) {
      unique.emplace_back(std::move(candidate));
    }
  }
  return unique;
}

auto SemanticVisitor::Impl::hasSymbolicOperatorRequirement(
    OverloadedOperator kind, const SemanticType &receiverType,
    std::span<const SemanticType> argumentTypes) const -> bool {
  return !symbolicOperatorRequirements(kind, receiverType, argumentTypes)
              .empty();
}

auto SemanticVisitor::Impl::resolveSymbolicOperator(
    const Expr &site, OverloadedOperator kind, const ExprPtr &receiver,
    const SemanticType &receiverType, const Token &token,
    std::span<const SemanticType> argumentTypes,
    std::span<const ExprPtr> arguments, bool contextual,
    std::optional<CallableBoundary> boundary,
    const std::optional<OperatorReceiverState> &receiverState, bool projection)
    -> std::optional<CallableCandidate> {
  std::vector<CallableCandidate> viable =
      symbolicOperatorRequirements(kind, receiverType, argumentTypes);
  const bool mutableReceiver = receiverState ? receiverState->mutableReceiver
                                             : isMutableObject(receiver);
  std::erase_if(viable, [&](const CallableCandidate &candidate) {
    if (candidate.parameterTypes.size() != argumentTypes.size() ||
        arguments.size() != argumentTypes.size()) {
      return true;
    }
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      if (!exactOperatorArgumentMatches(candidate.parameterTypes[index],
                                        argumentTypes[index],
                                        arguments[index])) {
        return true;
      }
    }
    return candidate.receiverMutability == ReceiverMutability::Mutable &&
           !mutableReceiver;
  });
  // Mirror the concrete structural selection: a mutable receiver prefers
  // the mutable-receiver form when the requirements prove both.
  if (mutableReceiver && std::any_of(viable.begin(), viable.end(),
                                     [](const CallableCandidate &candidate) {
                                       return candidate.receiverMutability ==
                                              ReceiverMutability::Mutable;
                                     })) {
    std::erase_if(viable, [](const CallableCandidate &candidate) {
      return candidate.receiverMutability == ReceiverMutability::ReadOnly;
    });
  }
  if (viable.size() != 1) {
    report(token,
           viable.empty()
               ? "The active requires clause does not prove this exact "
                 "use of " +
                     std::string(operatorSourceSpelling(kind)) + "."
               : "The active requires clause proves more than one exact "
                 "form of " +
                     std::string(operatorSourceSpelling(kind)) + ".",
           "GTI-S2029");
    return std::nullopt;
  }

  const CallableCandidate &selected = viable.front();
  const AccessMode receiverAccess =
      receiverAllowsMutation(selected.receiverMutability)
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  const SemanticLoanPlace receiverPlace =
      receiverState ? loanPlace(receiverState->place) : loanPlace(receiver);
  reportLoanPlaceConflict(
      token, receiverPlace, receiverAccess,
      selected.receiverMutability == ReceiverMutability::Mutable
          ? "Mutable constrained operator receiver overlaps a borrow that "
            "may still be live."
          : "Read-only constrained operator receiver overlaps a mutable "
            "borrow that may still be live.",
      receiverState ? accessLoan(receiverState->place) : accessLoan(receiver));
  validateMutableCallArgumentLoans(selected.parameterTypes, arguments, token);
  validateCallPlaceExclusivity(selected.parameterTypes, arguments, token,
                               receiverPlace, receiverAccess, false);
  if (receiverAccess == AccessMode::Mutable) {
    recordMutableBindingUse(receiverPlace.root);
  }

  ResolvedOperatorInfo resolved{
      .kind = kind,
      .receiverMutability = selected.receiverMutability,
      .receiverMode = selected.receiverMutability == ReceiverMutability::Mutable
                          ? CallReceiverMode::MutablePlace
                          : CallReceiverMode::ReadOnlyPlace,
      .returnType = selected.returnType,
      .parameterTypes = selected.parameterTypes,
      .borrowOrigin = selected.returnBorrowOrigin,
      .borrowArgument = selected.returnBorrowParameter,
      .borrowAccess = selected.returnBorrowAccess,
      .borrowArguments = selected.returnBorrowParameters,
      .borrowFields = selected.returnBorrowFields,
      .capability = callableInvocationCapability(selected.receiverMutability),
      .boundary = boundary,
      .receiverInfo =
          receiverState && receiverState->info
              ? *receiverState->info
              : expressionInfo(receiverType, ValueCategory::Place,
                               mutableReceiver ? AccessMode::Mutable
                                               : AccessMode::ReadOnly)};
  if (contextual) {
    semanticModel.recordContextualConversion(site, std::move(resolved));
  } else if (projection) {
    semanticModel.recordProjectionOperator(site, std::move(resolved));
  } else {
    semanticModel.recordOperator(site, std::move(resolved));
  }
  return selected;
}

auto SemanticVisitor::Impl::hasExactOperatorCandidate(
    OverloadedOperator kind, const SemanticType &receiverType,
    const SemanticType &argumentType) const -> bool {
  const ClassInfo *owner = classInfo(receiverType);
  if (owner == nullptr) {
    return false;
  }
  const auto found =
      owner->members.find(std::string(operatorFunctionName(kind)));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function) {
    return false;
  }
  const Symbol overloadSet =
      substituteSymbol(found->second.symbol, receiverType);
  return std::any_of(overloadSet.overloads.begin(), overloadSet.overloads.end(),
                     [&](const CallableCandidate &candidate) {
                       return candidate.parameterTypes.size() == 1 &&
                              candidate.parameterTypes.front() == argumentType;
                     });
}

auto SemanticVisitor::Impl::exactOperatorArgumentMatches(
    const SemanticType &parameterType, const SemanticType &argumentType,
    const ExprPtr &argument) const -> bool {
  if (parameterType == SemanticType::Unknown ||
      argumentType == SemanticType::Unknown) {
    return true;
  }
  if (parameterType.kind == SemanticType::Reference) {
    if (parameterType.arguments.size() != 1 ||
        parameterType.arguments.front() != argumentType) {
      return false;
    }
  } else if (parameterType != argumentType) {
    return false;
  }
  return callArgumentMatches(parameterType, argumentType, argument);
}

auto SemanticVisitor::Impl::resolveOperator(
    const Expr &site, OverloadedOperator kind, const ExprPtr &receiver,
    const SemanticType &receiverType, const Token &token,
    std::span<const SemanticType> argumentTypes,
    std::span<const ExprPtr> arguments, bool contextual,
    std::optional<CallableBoundary> boundary,
    std::optional<OperatorReceiverState> receiverState, bool projection,
    bool preferReadOnlyReceiver, ContextualCallArguments *contextualArguments)
    -> std::optional<CallableCandidate> {
  if (receiverType.kind == SemanticType::TypeParameter) {
    return resolveSymbolicOperator(site, kind, receiver, receiverType, token,
                                   argumentTypes, arguments, contextual,
                                   boundary, receiverState, projection);
  }
  if (argumentTypes.size() != arguments.size())
    return std::nullopt;
  std::vector<AnalyzedCallArgument> concreteArguments;
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    if (kind != OverloadedOperator::Call) {
      concreteArguments.push_back(
          {.type = argumentTypes[index], .expression = &arguments[index]});
      continue;
    }
    if (!appendConcreteCallArgument(argumentTypes[index], arguments[index],
                                    concreteArguments)) {
      // A symbolic pack has no concrete arity. Instance reanalysis owns
      // overload selection; no provisional operator target is executable.
      return std::nullopt;
    }
  }
  const ClassInfo *owner = classInfo(receiverType);
  if (owner == nullptr) {
    return std::nullopt;
  }

  const std::string memberName(operatorFunctionName(kind));
  const auto found = owner->members.find(memberName);
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2022", DiagnosticPhase::Semantics, token,
        "Type '" + typeSpelling(receiverType) + "' does not define " +
            std::string(operatorSourceSpelling(kind)) + ".");
    if (isCompoundOperator(kind)) {
      diagnostic.hints.emplace_back(
          "GTI does not synthesize compound operators from related value "
          "operators; declare this operator as a void member with one "
          "exact parameter and a trailing 'mut' receiver qualifier.");
    } else if (isArithmeticOrBitwiseValueOperator(kind)) {
      diagnostic.hints.emplace_back(
          "Declare this operator on the left operand type as a read-only "
          "member returning an owned value; binary parameters match "
          "exactly.");
    } else if (kind == OverloadedOperator::PreDecrement) {
      diagnostic.hints.emplace_back(
          "Declare 'void operator--() mut'; postfix decrement overloading "
          "is not available.");
    }
    diagnostics.emplace_back(std::move(diagnostic));
    return std::nullopt;
  }

  const Symbol overloadSet =
      substituteSymbol(found->second.symbol, receiverType);
  const bool consumingReceiver = consumingReceiverValue(receiver) &&
                                 (kind == OverloadedOperator::Call ||
                                  isArithmeticOrBitwiseValueOperator(kind));
  bool rejectedMutableReceiver = false;
  std::optional<CallableCandidate> rejectedMutableFunction;
  bool rejectedConsumingReceiver = false;
  bool rejectedCompilerPrivate = false;
  bool rejectedRequirements = false;
  struct ViableOperator {
    CallableCandidate function;
    CallReceiverMode receiverMode = CallReceiverMode::None;
  };
  std::vector<ViableOperator> viable;
  for (const CallableCandidate &declared : overloadSet.overloads) {
    CallableCandidate candidate = declared;
    if (kind == OverloadedOperator::Call && candidate.parameterPack &&
        candidate.record().genericParameters.empty()) {
      const auto *call = exprAs<Call>(&site);
      if (call == nullptr) {
        continue;
      }
      std::vector<SemanticType> typeArguments;
      std::vector<CompileTimeValue> valueArguments;
      if (!tryInstantiateFunction(
              declared, {}, {},
              std::vector<SemanticType>(argumentTypes.begin(),
                                        argumentTypes.end()),
              call->arguments(), candidate, typeArguments, valueArguments)) {
        continue;
      }
    }
    if (kind == OverloadedOperator::Call) {
      std::vector<SemanticType> concreteParameters;
      for (const auto &parameter : candidate.parameterTypes)
        SemanticType::appendExpandedArgument(concreteParameters, parameter);
      candidate.parameterTypes = std::move(concreteParameters);
    }
    if (candidate.parameterTypes.size() != concreteArguments.size()) {
      continue;
    }
    bool exact = true;
    for (std::size_t index = 0; index < concreteArguments.size(); ++index) {
      const auto &argument = concreteArguments[index];
      if (argument.type != SemanticType::Unknown &&
          candidate.parameterTypes[index] != SemanticType::Unknown &&
          !(argument.forwardedPackElement
                ? forwardedPackArgumentMatches(candidate.parameterTypes[index],
                                               argument.type)
                : exactOperatorArgumentMatches(candidate.parameterTypes[index],
                                               argument.type,
                                               *argument.expression))) {
        exact = false;
        break;
      }
    }
    if (!exact) {
      continue;
    }
    if (!candidate.requirements.empty()) {
      rejectedRequirements = true;
      continue;
    }
    if (candidate.receiverMutability == ReceiverMutability::Consuming &&
        !consumingReceiver) {
      rejectedConsumingReceiver = true;
      continue;
    }
    const bool allowFreshTemporary =
        kind == OverloadedOperator::Call ||
        candidate.receiverMutability == ReceiverMutability::ReadOnly;
    const CallReceiverMode receiverMode =
        receiverState
            ? (receiverState->mutableReceiver ? CallReceiverMode::MutablePlace
                                              : CallReceiverMode::ReadOnlyPlace)
            : receiverModeForObject(receiver, candidate, allowFreshTemporary,
                                    consumingReceiver &&
                                        (candidate.receiverMutability ==
                                             ReceiverMutability::Consuming ||
                                         isExplicitMoveReceiver(receiver)));
    if (candidate.receiverMutability == ReceiverMutability::Mutable &&
        !receiverModeAllowsMutation(receiverMode) && !consumingReceiver) {
      rejectedMutableReceiver = true;
      if (!rejectedMutableFunction) {
        rejectedMutableFunction = candidate;
      }
      continue;
    }
    if (!isCompilerTrustedRequester(token) && candidate.compilerPrivate) {
      rejectedCompilerPrivate = true;
      continue;
    }
    viable.push_back({.function = candidate, .receiverMode = receiverMode});
  }

  if (preferReadOnlyReceiver &&
      std::any_of(viable.begin(), viable.end(),
                  [](const ViableOperator &candidate) {
                    return candidate.function.receiverMutability ==
                           ReceiverMutability::ReadOnly;
                  })) {
    std::erase_if(viable, [](const ViableOperator &candidate) {
      return candidate.function.receiverMutability !=
             ReceiverMutability::ReadOnly;
    });
  } else if (consumingReceiver &&
             std::any_of(viable.begin(), viable.end(),
                         [](const ViableOperator &candidate) {
                           return candidate.function.receiverMutability ==
                                  ReceiverMutability::Consuming;
                         })) {
    std::erase_if(viable, [](const ViableOperator &candidate) {
      return candidate.function.receiverMutability !=
             ReceiverMutability::Consuming;
    });
  } else if (std::any_of(viable.begin(), viable.end(),
                         [](const ViableOperator &candidate) {
                           return candidate.function.receiverMutability ==
                                      ReceiverMutability::Mutable &&
                                  (receiverModeAllowsMutation(
                                       candidate.receiverMode) ||
                                   candidate.receiverMode ==
                                       CallReceiverMode::Consuming);
                         })) {
    std::erase_if(viable, [](const ViableOperator &candidate) {
      return candidate.function.receiverMutability ==
             ReceiverMutability::ReadOnly;
    });
  }

  if (viable.size() != 1) {
    if (viable.empty() && rejectedCompilerPrivate) {
      reportCompilerPrivateAccess(token,
                                  std::string(operatorSourceSpelling(kind)));
    } else if (viable.empty() && rejectedConsumingReceiver) {
      const auto *call =
          kind == OverloadedOperator::Call ? exprAs<Call>(&site) : nullptr;
      if (call != nullptr && boundary) {
        (void)validateCallableCapability(*call,
                                         CallableInvocationCapability::Once);
      } else {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2022", DiagnosticPhase::Semantics, token,
                           "A consuming operator requires an owned temporary "
                           "or an explicitly moved receiver.");
        diagnostic.hints.emplace_back("Use std::move(value) as the receiver to "
                                      "transfer a named value's ownership.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    } else if (viable.empty() && rejectedMutableReceiver) {
      const auto *call =
          kind == OverloadedOperator::Call ? exprAs<Call>(&site) : nullptr;
      if (call != nullptr && boundary) {
        (void)validateCallableCapability(*call,
                                         CallableInvocationCapability::Mutable);
      } else if (rejectedMutableFunction && !receiverState) {
        reportMutableReceiverFailure(receiver, token, *rejectedMutableFunction,
                                     std::string(operatorSourceSpelling(kind)));
      } else {
        report(token,
               std::string(operatorSourceSpelling(kind)) +
                   " requires a mutable receiver.",
               "GTI-S2022");
      }
    } else if (viable.empty() && rejectedRequirements) {
      report(token,
             "Operator candidates with trailing requires clauses are "
             "unavailable in the bounded requirements model.",
             "GTI-S2049");
    } else if (viable.empty()) {
      std::string received;
      if (!argumentTypes.empty()) {
        received = " for argument types (";
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (index != 0) {
            received += ", ";
          }
          received += typeSpelling(argumentTypes[index]);
        }
        received += ')';
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2022", DiagnosticPhase::Semantics, token,
          "No exact overload of " + std::string(operatorSourceSpelling(kind)) +
              received + ".");
      bool referenceMismatch = false;
      for (const CallableCandidate &candidate : overloadSet.overloads) {
        if (candidate.declaration != nullptr &&
            (isCompilerTrustedRequester(token) || !candidate.compilerPrivate)) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->operatorName()->keyword),
               "Candidate: " + functionSignatureSpelling(candidate)});
          if (candidate.parameterTypes.size() != concreteArguments.size())
            continue;
          for (std::size_t index = 0; index < concreteArguments.size();
               ++index) {
            const auto &parameter = candidate.parameterTypes[index];
            const auto &argument = concreteArguments[index];
            if (parameter.kind != SemanticType::Reference ||
                parameter.arguments.size() != 1 ||
                parameter.arguments.front() != argument.type ||
                argument.forwardedPackElement ||
                argument.expression == nullptr || !*argument.expression)
              continue;
            const ExpressionInfo *info =
                semanticModel.findExpression(**argument.expression);
            if (info == nullptr)
              continue;
            std::string reason;
            if (info->category == ValueCategory::Value) {
              reason = "is a temporary value; this candidate requires an "
                       "existing place for its reference parameter.";
            } else if (parameter.referenceAccess == AccessMode::Mutable &&
                       info->access != AccessMode::Mutable) {
              reason = "is read-only; this candidate requires exclusive "
                       "mutable access for its reference parameter.";
            }
            if (!reason.empty()) {
              referenceMismatch = true;
              diagnostic.related.push_back(
                  {tokenSpan(expressionToken(*argument.expression)),
                   "Argument " + std::to_string(index + 1) + " " + reason});
            }
          }
        }
      }
      diagnostic.hints.emplace_back(
          referenceMismatch
              ? "Reference parameters require an existing place with the "
                "declared access. Bind a temporary to a local before using "
                "it, or use an operator accepting an owned value."
              : "GTI operator calls do not perform implicit conversions, rank "
                "candidates, reverse operands, or use ADL; match the exact "
                "parameter types and ownership requirements.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else {
      report(token,
             "Use of " + std::string(operatorSourceSpelling(kind)) +
                 " is ambiguous.",
             "GTI-S2022");
    }
    return std::nullopt;
  }

  const CallableCandidate &selected = viable.front().function;
  requireFunctionDefinition(selected, token);
  const CallReceiverMode selectedReceiverMode = viable.front().receiverMode;
  if (selected.receiverMutability == ReceiverMutability::Consuming &&
      (selected.returnType.kind == SemanticType::Reference ||
       selected.returnBorrowOrigin != BorrowOriginKind::None ||
       typeTraits(selected.returnType).containsBorrowedState)) {
    report(token,
           "A consuming operator cannot return a reference or tracked borrowed "
           "state.",
           "GTI-S2022");
    return std::nullopt;
  }
  if (contextualArguments && contextualArguments->hasDeferred()) {
    const auto *call = exprAs<Call>(&site);
    if (call == nullptr || !finishContextualCallArguments(
                               selected.parameterTypes, call->arguments(),
                               *contextualArguments, token, "Operator"))
      return std::nullopt;
    concreteArguments.clear();
    for (std::size_t index = 0; index < call->arguments().size(); ++index)
      if (!appendConcreteCallArgument(contextualArguments->types[index],
                                      call->arguments()[index],
                                      concreteArguments))
        return std::nullopt;
  }
  if (selected.access == AccessModifier::Private &&
      currentClass != selected.ownerClass) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, token,
                       std::string(operatorSourceSpelling(kind)) + " of '" +
                           owner->name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(selected.declaration->operatorName()->keyword),
         "Operator declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const AccessMode selectedReceiverAccess =
      receiverAllowsMutation(selected.receiverMutability)
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  const Call *movedReceiver = directCall(receiver);
  const ResolvedCallInfo *receiverCall =
      movedReceiver == nullptr ? nullptr
                               : semanticModel.findCall(*movedReceiver);
  if (receiverCall != nullptr && receiverCall->retainedReceiverProjection &&
      (selected.returnType.kind == SemanticType::Reference ||
       typeTraits(selected.returnType).containsBorrowedState)) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2022", DiagnosticPhase::Semantics, token,
                       "Consuming invocation of an owned field cannot return a "
                       "reference or borrowed state.");
    diagnostic.related.push_back(
        {tokenSpan(selected.declaration->operatorName()->keyword),
         "Selected callable declared here."});
    diagnostic.hints.emplace_back(
        "Return an owning value, or invoke the field without consuming it.");
    diagnostics.emplace_back(std::move(diagnostic));
    return std::nullopt;
  }
  const bool temporaryReceiver =
      selectedReceiverMode == CallReceiverMode::FreshTemporaryRead ||
      selectedReceiverMode == CallReceiverMode::FreshTemporaryMutable;
  std::optional<TransientBorrowProduction> receiverProduction;
  std::optional<SemanticLoanPlace> receiverPlace;
  if (receiverState) {
    receiverPlace = loanPlace(receiverState->place);
  } else if (!temporaryReceiver) {
    receiverProduction = transientBorrowProduction(receiver);
    receiverPlace = receiverProduction
                        ? std::optional<SemanticLoanPlace>{std::move(
                              receiverProduction->place)}
                        : loanPlace(receiver);
  }
  const AccessMode callReceiverAccess =
      receiverProduction && receiverProduction->access == AccessMode::Mutable
          ? AccessMode::Mutable
          : selectedReceiverAccess;
  if (receiverPlace) {
    reportLoanPlaceConflict(
        token, *receiverPlace, selectedReceiverAccess,
        receiverAllowsMutation(selected.receiverMutability)
            ? (selected.receiverMutability == ReceiverMutability::Consuming
                   ? "Consuming operator receiver overlaps a borrow that "
                     "may still be live."
                   : "Mutable operator receiver overlaps a borrow that may "
                     "still be live.")
            : "Read-only operator receiver overlaps a mutable borrow or "
              "child reborrow that may still be live.",
        receiverState ? accessLoan(receiverState->place)
                      : accessLoan(receiver));
  }
  std::vector<SemanticType> ordinaryParameters;
  std::vector<const ExprPtr *> ordinaryArguments;
  for (std::size_t index = 0; index < concreteArguments.size(); ++index) {
    const auto &argument = concreteArguments[index];
    if (argument.forwardedPackElement)
      continue;
    const auto &parameter = selected.parameterTypes[index];
    ordinaryParameters.push_back(parameter);
    ordinaryArguments.push_back(argument.expression);
    validateMutableCallArgumentLoans(
        std::span<const SemanticType>(&parameter, 1),
        std::span<const ExprPtr>(argument.expression, 1), token);
  }
  validateCallPlaceExclusivityImpl(ordinaryParameters, ordinaryArguments, token,
                                   receiverPlace, callReceiverAccess,
                                   receiverProduction.has_value());
  if (receiverPlace && callReceiverAccess == AccessMode::Mutable) {
    recordMutableBindingUse(receiverPlace->root);
  }

  ResolvedOperatorInfo resolved{
      .function = selected.id,
      .declaration = selected.declaration,
      .dispatch =
          selected.virtualMethod ? CallDispatch::Virtual : CallDispatch::Static,
      .dispatchOwner = selected.dispatchOwner,
      .kind = kind,
      .receiverMutability = selected.receiverMutability,
      .receiverMode = selectedReceiverMode,
      .returnType = selected.returnType,
      .parameterTypes = selected.parameterTypes,
      .borrowOrigin = selected.returnBorrowOrigin,
      .borrowArgument = selected.returnBorrowParameter,
      .borrowAccess = selected.returnBorrowAccess,
      .borrowArguments = selected.returnBorrowParameters,
      .borrowFields = selected.returnBorrowFields,
      .capability = callableInvocationCapability(selected.receiverMutability),
      .boundary = boundary,
      .receiverInfo = [&]() {
        if (receiverState && receiverState->info) {
          return *receiverState->info;
        }
        if (receiver) {
          if (const ExpressionInfo *recorded =
                  semanticModel.findExpression(*receiver)) {
            return *recorded;
          }
          return classifyExpression(*receiver, receiverType);
        }
        return expressionInfo(receiverType, ValueCategory::Place,
                              receiverModeAllowsMutation(selectedReceiverMode)
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly);
      }()};
  RawInvocation rawInvocation{
      .resolved = {.function = selected.id,
                   .declaration = selected.declaration,
                   .returnType = selected.returnType,
                   .parameterTypes = selected.parameterTypes,
                   .dispatch = resolved.dispatch,
                   .dispatchOwner = selected.dispatchOwner,
                   .receiverMode = selectedReceiverMode},
      .arguments = arguments,
      .receiver = receiverState ? nullptr : receiver.get(),
      .receiverPlace =
          receiverState ? std::optional{receiverState->place} : std::nullopt,
      .receiverType = selected.dispatchOwner.kind == SemanticType::Class
                          ? selected.dispatchOwner
                          : receiverType,
      .site = token};
  applyRawOperatorInvocation(site, rawInvocation, contextual,
                             projection || kind == OverloadedOperator::Arrow);
  if (contextual) {
    semanticModel.recordContextualConversion(site, std::move(resolved));
  } else if (projection) {
    semanticModel.recordProjectionOperator(site, std::move(resolved));
  } else {
    semanticModel.recordOperator(site, std::move(resolved));
  }
  return selected;
}

auto SemanticVisitor::Impl::arrowTargetType(const Expr &site,
                                            const ExprPtr &receiver,
                                            const SemanticType &receiverType,
                                            const Token &token, bool projection)
    -> SemanticType {
  if (receiverType.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(receiverType, token,
                                      "raw-pointer member access")) {
      return SemanticType::Unknown;
    }
    if (receiverType.arguments.size() != 1 ||
        receiverType.arguments.front() == SemanticType::Void) {
      report(token, "void* does not provide member access.", "GTI-S2056");
      return SemanticType::Unknown;
    }
    if (receiverType.arguments.front().kind != SemanticType::Class) {
      report(token,
             "Raw '->' member access requires a class or struct pointee.",
             "GTI-S2056");
      return SemanticType::Unknown;
    }
    requireUnsafe(site, UnsafeOperationKind::RawMember, token,
                  "Raw pointer member access");
    validateRawPointerAccess(receiver, token,
                             site.exprKind() == ExprKind::Set
                                 ? AccessMode::Mutable
                                 : AccessMode::ReadOnly);
    return receiverType.arguments.front();
  }
  if (receiverType.kind == SemanticType::Class) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        site, OverloadedOperator::Arrow, receiver, receiverType, token, {}, {},
        false, std::nullopt, std::nullopt, projection);
    if (!selected || selected->returnType.kind != SemanticType::Reference ||
        selected->returnType.arguments.size() != 1) {
      return SemanticType::Unknown;
    }
    return selected->returnType.arguments[0];
  }
  if (receiverType != SemanticType::Unknown) {
    report(token, "Operator '->' requires a class defining operator->.",
           "GTI-S2022");
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::recordCallableForwardings(
    const Call &call, const CallableCandidate &target) -> void {
  if (instanceAnalysisActive || currentFunctionDeclaration == nullptr ||
      target.id == 0) {
    return;
  }
  const FunctionInfo *targetInfo = semanticModel.findFunction(target.id);
  const FunctionInfo *sourceInfo = currentFunctionInfo();
  if (targetInfo == nullptr || targetInfo->declaration == nullptr ||
      sourceInfo == nullptr || sourceInfo->declaration == nullptr) {
    return;
  }

  const std::size_t count =
      std::min(call.arguments().size(), targetInfo->parameterTypes.size());
  const auto callableType =
      [](const SemanticType &type) -> const SemanticType & {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1
               ? type.arguments.front()
               : type;
  };
  for (std::size_t index = 0; index < count; ++index) {
    if (index >= targetInfo->declaration->parameters().size() ||
        callableType(targetInfo->parameterTypes[index]).kind !=
            SemanticType::TypeParameter) {
      continue;
    }
    const Parameter &targetParameter =
        targetInfo->declaration->parameters()[index];
    if (targetParameter.pack) {
      continue;
    }

    const Variable *variable =
        directCallableParameterVariable(call.arguments()[index]);
    const Symbol *symbol =
        variable == nullptr ? nullptr : resolve(variable->name());
    if (symbol == nullptr || symbol->parameterDeclaration == nullptr) {
      continue;
    }
    const auto sourceParameter =
        std::find_if(sourceInfo->declaration->parameters().begin(),
                     sourceInfo->declaration->parameters().end(),
                     [&](const Parameter &candidate) {
                       return &candidate == symbol->parameterDeclaration;
                     });
    if (sourceParameter == sourceInfo->declaration->parameters().end()) {
      continue;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(std::distance(
        sourceInfo->declaration->parameters().begin(), sourceParameter));
    if (sourceIndex >= sourceInfo->parameterTypes.size() ||
        callableType(sourceInfo->parameterTypes[sourceIndex]).kind !=
            SemanticType::TypeParameter ||
        sourceParameter->pack) {
      continue;
    }

    semanticModel.recordCallableForwarding(
        *sourceInfo->declaration, sourceIndex,
        callableType(sourceInfo->parameterTypes[sourceIndex])
            .genericParameterId,
        callableType(sourceInfo->parameterTypes[sourceIndex]),
        sourceParameter->mutability == Mutability::Mutable
            ? AccessMode::Mutable
            : AccessMode::ReadOnly,
        call, target.id, index);
  }
}

auto SemanticVisitor::Impl::callArgumentGeometry(const Call &call)
    -> std::optional<SemanticCallGeometry> {
  if (!call.leftParen() || call.leftParen()->generated ||
      call.paren().generated) {
    return std::nullopt;
  }
  SemanticCallGeometry geometry{.leftDelimiter = call.leftParen()->position,
                                .rightDelimiter = call.paren().position,
                                .argumentSeparators = {}};
  geometry.argumentSeparators.reserve(call.argumentCommas().size());
  for (const Token &comma : call.argumentCommas()) {
    geometry.argumentSeparators.push_back(comma.position);
  }
  return geometry;
}

auto SemanticVisitor::Impl::recordSelectedCallOccurrence(
    const Call &call, ResolvedCallInfo resolved) -> void {
  recordRawBorrowCall(call, resolved);
  const Token token =
      resolved.nativeCalleeType && exprAs<Call>(call.callee().get()) != nullptr
          ? call.paren()
          : callableToken(call.callee());
  if (token.generated) {
    return;
  }
  const auto *calleeOccurrence = resolved.nativeCalleeType
                                     ? semanticModel.database().findOccurrence(
                                           currentSourceUnit, token.position)
                                     : nullptr;
  const SymbolId symbol =
      resolved.declaration != nullptr
          ? recordFunctionSymbol(*resolved.declaration)
      : calleeOccurrence && calleeOccurrence->type == *resolved.nativeCalleeType
          ? calleeOccurrence->symbol
          : 0;
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(token),
       .kind = SemanticOccurrenceKind::SelectedCall,
       .symbol = symbol,
       .roles = OccurrenceRole::Reference | OccurrenceRole::Call,
       .name = token.lexeme,
       .type = resolved.returnType,
       .traits = typeTraits(resolved.returnType),
       .function = resolved.declaration,
       .selectedCall = std::move(resolved),
       .callGeometry = callArgumentGeometry(call)});
}

auto SemanticVisitor::Impl::bindIntrinsicCallDeclaration(
    const Call &call, const CallableCandidate &function) -> void {
  const ResolvedCallInfo *existing = semanticModel.findCall(call);
  if (existing == nullptr) {
    return;
  }
  ResolvedCallInfo resolved = *existing;
  resolved.function = function.id;
  resolved.declaration = function.declaration;
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
}

auto SemanticVisitor::Impl::recordDefaultArgumentEffects(
    const Call &call, const CallableCandidate &function) -> void {
  if (function.declaration == nullptr ||
      call.arguments().size() >= function.parameterTypes.size()) {
    return;
  }
  const SourceSpan callSite = tokenSpan(call.paren());
  for (std::size_t index = call.arguments().size();
       index < function.parameterTypes.size(); ++index) {
    if (index >= function.record().defaultArgumentEffectOwners.size() ||
        function.record().defaultArgumentEffectOwners[index] == 0) {
      continue;
    }
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::FunctionDefaultArgument,
         .id = function.record().defaultArgumentEffectOwners[index]},
        callSite);
  }
}

auto SemanticVisitor::Impl::containsCAbiRawPointer(
    const SemanticType &type, std::unordered_set<ClassId> &visiting) const
    -> bool {
  if (type.kind == SemanticType::RawPointer ||
      type.kind == SemanticType::NativeFunction) {
    return true;
  }
  if (type.kind == SemanticType::Class) {
    const ClassTypeInfo *record = semanticModel.findClassType(type.classId);
    if (record == nullptr || !record->cAbiRecord ||
        !visiting.insert(type.classId).second) {
      return false;
    }
    const bool contains =
        std::any_of(record->fields.begin(), record->fields.end(),
                    [&](const ClassFieldTypeInfo &field) {
                      return containsCAbiRawPointer(field.type, visiting);
                    });
    visiting.erase(type.classId);
    return contains;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsCAbiRawPointer(argument, visiting);
                     });
}

auto SemanticVisitor::Impl::containsCAbiRawPointer(
    const SemanticType &type) const -> bool {
  std::unordered_set<ClassId> visiting;
  return containsCAbiRawPointer(type, visiting);
}

auto SemanticVisitor::Impl::requireFunctionDefinition(
    const CallableCandidate &function, const Token &location) -> void {
  const FunctionDecl *declaration = function.declaration;
  if (!requireFunctionDefinitions || function.id == 0 ||
      declaration == nullptr || declaration->body() != nullptr ||
      declaration->runtimeBinding() ||
      function.record().linkage != LanguageLinkage::Gti ||
      function.record().intrinsic != IntrinsicKind::None ||
      function.record().pureVirtual) {
    return;
  }
  // A generic primary is a checked declaration, not yet an executable body.
  // Concrete reanalysis selects exact specializations and constexpr branches
  // before enforcing closure. Inspect the enclosing formal bindings, not the
  // callee's signature or just the class's analysis mode: a generic method can
  // still have open formals inside an already-concrete class context.
  const bool openTypes = std::any_of(
      typeParameterScopes.begin(), typeParameterScopes.end(),
      [](const auto &scope) {
        return std::any_of(scope.begin(), scope.end(), [](const auto &binding) {
          const auto &type = binding.second;
          return type.kind == SemanticType::TypeParameter ||
                 (type.kind == SemanticType::TypePack && !type.concretePack);
        });
      });
  const bool openValues = std::any_of(
      valueParameterScopes.begin(), valueParameterScopes.end(),
      [](const auto &scope) {
        return std::any_of(scope.begin(), scope.end(), [](const auto &binding) {
          return binding.second.kind == CompileTimeValue::Parameter;
        });
      });
  if (openTypes || openValues) {
    return;
  }
  Diagnostic diagnostic =
      makeDiagnostic(DiagnosticId::MissingFunctionDefinition, location,
                     {function.record().qualifiedName});
  diagnostic.related.push_back(
      {tokenSpan(declaration->operatorName()
                     ? declaration->operatorName()->keyword
                     : declaration->name()),
       "The bodyless declaration is here."});
  diagnostic.hints.emplace_back(
      "Provide a GTI definition or an exact specialization for this call. "
      "For a foreign function, use "
      "extern \"C\" with a supported native signature.");
  diagnostics.push_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::recordResolvedCall(
    const Call &call, const CallableCandidate &function,
    std::vector<SemanticType> typeArguments,
    std::vector<CompileTimeValue> valueArguments,
    std::vector<std::size_t> ephemeralBorrowArguments,
    std::vector<CallableArgumentBoundary> callableArguments,
    CallReceiverMode receiverMode, const ExprPtr *argumentReceiver) -> void {
  requireFunctionDefinition(function, expressionToken(call.callee()));
  const bool pointerBearingCFunction =
      function.declaration != nullptr && function.declaration->hasCLinkage() &&
      (containsCAbiRawPointer(function.returnType) ||
       std::any_of(function.parameterTypes.begin(),
                   function.parameterTypes.end(),
                   [this](const SemanticType &parameter) {
                     return containsCAbiRawPointer(parameter);
                   }));
  const bool missingUnsafe = pointerBearingCFunction && unsafeDepth == 0;
  // Concrete generic reanalysis must replace the symbolic requirement's
  // permitted unsafe effect when the selected implementation is actually safe.
  // Instance models otherwise inherit the primary expression's unsafe marker.
  semanticModel.recordUnsafeOperation(call, UnsafeOperationKind::None);
  if (function.record().requiresUnsafe) {
    const bool missing = unsafeDepth == 0;
    requireUnsafe(call, UnsafeOperationKind::DeclaredUnsafeCall, call.paren(),
                  "Calling an unsafe function");
    if (missing && !diagnostics.empty() && function.declaration != nullptr &&
        function.declaration->unsafeKeyword()) {
      diagnostics.back().related.push_back(
          {tokenSpan(*function.declaration->unsafeKeyword()),
           "This declaration requires the caller to uphold its unsafe "
           "contract."});
    }
  }
  if (pointerBearingCFunction) {
    requireUnsafe(call, UnsafeOperationKind::ForeignPointerCall, call.paren(),
                  "Calling pointer-bearing extern \"C\" function");
    if (missingUnsafe && !diagnostics.empty()) {
      diagnostics.back().related.push_back(
          {tokenSpan(function.declaration->name()),
           "Pointer-bearing C declaration is here."});
    }
  }
  recordCallableForwardings(call, function);
  recordDefaultArgumentEffects(call, function);
  if (requestSession.hasCThreadRequirements && !instanceAnalysisActive &&
      function.declaration != nullptr) {
    requestSession.cThreadCallUses.push_back(
        {.caller = currentFunctionDeclaration,
         .callee = function.declaration,
         .location = call.paren(),
         .trusted = isCompilerTrustedRequester(call.paren())});
  }
  const FunctionInfo *declarationInfo = semanticModel.findFunction(function.id);
  if (declarationInfo == nullptr && instanceBaseModel != nullptr) {
    declarationInfo = instanceBaseModel->findFunction(function.id);
  }
  // A generic declaration need not itself have a stored-borrow layout. Resolve
  // the contract with the same concrete types used to check this selected call;
  // instance analysis independently checks the body against that contract.
  std::optional<FunctionInfo> concreteDeclaration;
  if (declarationInfo != nullptr &&
      (declarationInfo->returnType != function.returnType ||
       declarationInfo->parameterTypes != function.parameterTypes ||
       !function.dispatchOwner.arguments.empty() ||
       !function.dispatchOwner.valueArguments.empty())) {
    concreteDeclaration = *declarationInfo;
    concreteDeclaration->returnType = function.returnType;
    concreteDeclaration->parameterTypes = function.parameterTypes;
    if (const auto fields = storedFieldReturnBorrowSummary(
            *concreteDeclaration, function.dispatchOwner)) {
      concreteDeclaration->returnBorrowOrigin = fields->origin;
      concreteDeclaration->returnBorrowParameter = fields->parameter;
      concreteDeclaration->returnBorrowAccess = fields->access;
      concreteDeclaration->returnBorrowParameters = fields->parameters;
      concreteDeclaration->returnBorrowFields = fields->fields;
      concreteDeclaration->returnBorrowPlace = fields->place;
      declarationInfo = &*concreteDeclaration;
    }
  }
  // Whether the callee returns a moved parameter is a body-derived fact; ask
  // for the body first so the answer is the same whatever the source order.
  if (declarationInfo != nullptr) {
    ensureBodyAnalyzed(*declarationInfo);
  }
  const FunctionReturnBorrowSummary concreteBorrow =
      functionReturnBorrowSummary(
          function.returnType, function.parameterTypes, function.ownerClass,
          function.staticMember, function.record().intrinsic,
          declarationInfo == nullptr
              ? std::nullopt
              : exactMovedReturnParameter(*declarationInfo));
  const bool declarationOwnsBorrow =
      declarationInfo != nullptr &&
      declarationInfo->returnBorrowOrigin != BorrowOriginKind::None;
  const bool candidateOwnsBorrow =
      function.returnBorrowOrigin != BorrowOriginKind::None;
  const BorrowOriginKind returnBorrowOrigin =
      declarationOwnsBorrow ? declarationInfo->returnBorrowOrigin
                            : (candidateOwnsBorrow ? function.returnBorrowOrigin
                                                   : concreteBorrow.origin);
  const std::size_t returnBorrowParameter =
      declarationOwnsBorrow
          ? declarationInfo->returnBorrowParameter
          : (candidateOwnsBorrow ? function.returnBorrowParameter
                                 : concreteBorrow.parameter);
  const AccessMode returnBorrowAccess =
      declarationOwnsBorrow ? declarationInfo->returnBorrowAccess
                            : (candidateOwnsBorrow ? function.returnBorrowAccess
                                                   : concreteBorrow.access);
  const std::optional<BorrowOriginPlace> returnBorrowPlace =
      declarationOwnsBorrow ? declarationInfo->returnBorrowPlace
                            : (candidateOwnsBorrow ? function.returnBorrowPlace
                                                   : concreteBorrow.place);
  std::vector<const Expr *> defaultArguments =
      function.declaration == nullptr
          ? std::vector<const Expr *>{}
          : omittedDefaultArguments(function.declaration->parameters(),
                                    call.arguments().size() +
                                        (argumentReceiver != nullptr));
  recordDefaultArgumentDependencies(defaultArguments);
  ResolvedCallInfo resolved{
      .function = function.id,
      .declaration = function.declaration,
      .returnType = function.returnType,
      .parameterTypes = function.parameterTypes,
      .defaultArguments = std::move(defaultArguments),
      .typeArguments = std::move(typeArguments),
      .valueArguments = std::move(valueArguments),
      .requirements = function.requirements,
      .synchronization =
          {.kind = function.record().cThreadOperation ==
                           CThreadRuntimeOperation::Create
                       ? SynchronizationOperationKind::ThreadSpawn
                   : function.record().cThreadOperation ==
                           CThreadRuntimeOperation::Join
                       ? SynchronizationOperationKind::ThreadJoin
                   : (function.record().cThreadOperation ==
                          CThreadRuntimeOperation::MutexLock ||
                      function.record().cThreadOperation ==
                          CThreadRuntimeOperation::MutexTryLock ||
                      function.record().cThreadOperation ==
                          CThreadRuntimeOperation::MutexTimedLock)
                       ? SynchronizationOperationKind::MutexLock
                   : function.record().cThreadOperation ==
                           CThreadRuntimeOperation::MutexUnlock
                       ? SynchronizationOperationKind::MutexUnlock
                   : (function.record().cThreadOperation ==
                          CThreadRuntimeOperation::ConditionWait ||
                      function.record().cThreadOperation ==
                          CThreadRuntimeOperation::ConditionTimedWait)
                       ? SynchronizationOperationKind::ConditionWait
                   : (function.record().cThreadOperation ==
                          CThreadRuntimeOperation::ConditionSignal ||
                      function.record().cThreadOperation ==
                          CThreadRuntimeOperation::ConditionBroadcast)
                       ? SynchronizationOperationKind::ConditionNotify
                       : SynchronizationOperationKind::None},
      .borrowOrigin = returnBorrowOrigin,
      .borrowArgument = returnBorrowParameter,
      .borrowAccess = returnBorrowAccess,
      .borrowArguments = declarationOwnsBorrow
                             ? declarationInfo->returnBorrowParameters
                         : candidateOwnsBorrow ? function.returnBorrowParameters
                                               : concreteBorrow.parameters,
      .borrowFields = declarationOwnsBorrow
                          ? declarationInfo->returnBorrowFields
                      : candidateOwnsBorrow ? function.returnBorrowFields
                                            : concreteBorrow.fields,
      .borrowPlace = returnBorrowPlace,
      .dispatch =
          function.virtualMethod ? CallDispatch::Virtual : CallDispatch::Static,
      .dispatchOwner = function.dispatchOwner,
      .receiverMode = receiverMode,
      .ephemeralBorrowArguments = std::move(ephemeralBorrowArguments),
      .callableArguments = std::move(callableArguments),
      .argumentReceiver = argumentReceiver};
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
}

auto SemanticVisitor::Impl::callExpressionType(const SemanticType &returnType)
    -> SemanticType {
  if (returnType.kind == SemanticType::Reference &&
      returnType.arguments.size() == 1) {
    return returnType.arguments[0];
  }
  return returnType;
}

auto SemanticVisitor::Impl::reportOverloadResolutionFailure(
    const Call &call, const Symbol &overloadSet,
    const std::vector<SemanticType> &argumentTypes,
    const std::vector<const CallableCandidate *> &exactMatches,
    std::span<const ConstraintFailure> constraintFailures) -> void {
  const std::string name = callableSpelling(call.callee());
  Diagnostic diagnostic;
  if (exactMatches.empty()) {
    std::string arguments;
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      if (index != 0) {
        arguments += ", ";
      }
      arguments += typeSpelling(argumentTypes[index]);
    }
    diagnostic = makeDiagnostic(
        "GTI-S2012", DiagnosticPhase::Semantics, expressionToken(call.callee()),
        "No overload of '" + name + "' exactly matches argument types (" +
            arguments + ").");
    if (constraintFailures.empty()) {
      const bool acceptsCount = std::any_of(
          overloadSet.overloads.begin(), overloadSet.overloads.end(),
          [&](const CallableCandidate &candidate) {
            return (isCompilerTrustedRequester(call.paren()) ||
                    !candidate.compilerPrivate) &&
                   acceptsArgumentShape(candidate, argumentTypes);
          });
      diagnostic.hints.emplace_back(
          acceptsCount
              ? "Match the exact parameter types and reference/ownership "
                "requirements of an available overload."
              : "No available overload accepts this argument count. Check the "
                "arguments and include the declaration of the intended "
                "overload.");
      if (!acceptsCount &&
          (call.callee()->exprKind() == ExprKind::Variable ||
           call.callee()->exprKind() == ExprKind::QualifiedName)) {
        diagnostic.missingIncludeName = expressionToken(call.callee()).lexeme;
      }
    }
    for (const CallableCandidate &candidate : overloadSet.overloads) {
      if (candidate.declaration != nullptr &&
          (isCompilerTrustedRequester(call.paren()) ||
           !candidate.compilerPrivate)) {
        diagnostic.related.push_back(
            {tokenSpan(candidate.declaration->name()),
             "Candidate: " + functionSignatureSpelling(candidate)});
      }
    }
    for (const ConstraintFailure &failure : constraintFailures) {
      std::vector<Diagnostic> explanations;
      SemanticDiagnosticReporter detailReporter(semanticModel, explanations);
      detailReporter.reportConstraintFailure(call.paren(), failure);
      for (const Diagnostic &explanation : explanations) {
        diagnostic.related.push_back(
            {failure.candidate ? tokenSpan(*failure.candidate)
                               : explanation.primary,
             "Candidate rejected: " + explanation.message});
        diagnostic.related.insert(diagnostic.related.end(),
                                  explanation.related.begin(),
                                  explanation.related.end());
        for (const std::string &hint : explanation.hints) {
          if (std::find(diagnostic.hints.begin(), diagnostic.hints.end(),
                        hint) == diagnostic.hints.end()) {
            diagnostic.hints.push_back(hint);
          }
        }
      }
    }
  } else {
    diagnostic = makeDiagnostic(
        "GTI-S2013", DiagnosticPhase::Semantics, call.paren(),
        "Call to '" + name + "' is ambiguous; " +
            std::to_string(exactMatches.size()) + " overloads exactly match.");
    for (const CallableCandidate *candidate : exactMatches) {
      if (candidate != nullptr && candidate->declaration != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(candidate->declaration->name()),
             "Exact candidate: " + functionSignatureSpelling(*candidate)});
      }
    }
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::callConversionRank(const SemanticType &parameter,
                                               const SemanticType &argument)
    -> int {
  if (parameter == argument) {
    return 0;
  }
  if (parameter == SemanticType::CString) {
    if (argument == SemanticType::StringView) {
      return 1;
    }
    if (argument == SemanticType::NullPtr) {
      return 2;
    }
    return 1000;
  }
  if (parameter.kind != SemanticType::RawPointer) {
    return 1000;
  }
  if (argument == SemanticType::NullPtr) {
    return 2;
  }
  if (argument.kind == SemanticType::RawPointer &&
      parameter.arguments.size() == 1 && argument.arguments.size() == 1 &&
      parameter.arguments.front() == argument.arguments.front() &&
      parameter.pointerAccess == AccessMode::ReadOnly &&
      argument.pointerAccess == AccessMode::Mutable) {
    return 1;
  }
  return 1000;
}

auto SemanticVisitor::Impl::callConversionIsBetter(
    std::span<const SemanticType> leftParameters,
    std::span<const SemanticType> rightParameters,
    std::span<const SemanticType> arguments) -> bool {
  bool strictlyBetter = false;
  const std::size_t count = std::min(
      {leftParameters.size(), rightParameters.size(), arguments.size()});
  for (std::size_t index = 0; index < count; ++index) {
    const int left =
        callConversionRank(leftParameters[index], arguments[index]);
    const int right =
        callConversionRank(rightParameters[index], arguments[index]);
    if (left > right) {
      return false;
    }
    strictlyBetter = strictlyBetter || left < right;
  }
  return strictlyBetter;
}

} // namespace lang
