#include "semantic_analyzer_impl.h"

namespace lang {

namespace {

// Merely projecting an existing reference does not invoke, copy or erase the
// callable's invocation contract. Its normal checked borrow is the contract.
[[nodiscard]] bool isCheckedReferenceRelay(const FunctionInfo &function,
                                           std::size_t parameter) {
  return function.declaration != nullptr &&
         function.bodyDeclaration()->body() != nullptr &&
         function.intrinsic == IntrinsicKind::None &&
         function.returnType.kind == SemanticType::Reference &&
         function.returnBorrowOrigin == BorrowOriginKind::Argument &&
         function.returnBorrowParameter == parameter &&
         function.returnBorrowParameters.empty() &&
         function.returnBorrowFields.empty() &&
         parameter < function.parameterTypes.size() &&
         function.parameterTypes[parameter].kind == SemanticType::Reference;
}

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

auto SemanticVisitor::Impl::currentReturnBorrowContract() const
    -> std::optional<ReturnBorrowContract> {
  if (currentLambdaReturn) {
    const LambdaReturnContext &context = *currentLambdaReturn;
    return ReturnBorrowContract{
        .origin = context.origin,
        .parameter = context.parameter,
        .access = context.access,
        .parameters =
            context.declaration ? &context.declaration->parameters() : nullptr,
        .parameterTypes = &context.parameterTypes,
        .lambda = true,
        .deferred = context.deferred};
  }
  const FunctionInfo *function = currentFunctionInfo();
  if (function == nullptr) {
    return std::nullopt;
  }
  return ReturnBorrowContract{
      .origin = function->returnBorrowOrigin,
      .parameter = function->returnBorrowParameter,
      .access = function->returnBorrowAccess,
      .place = function->returnBorrowPlace,
      .parameters = function->declaration != nullptr &&
                            function->bodyDeclaration() != nullptr
                        ? &function->bodyDeclaration()->parameters()
                        : nullptr,
      .parameterTypes = &function->parameterTypes,
      .lambda = false};
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
      std::find_if(function->bodyDeclaration()->parameters().begin(),
                   function->bodyDeclaration()->parameters().end(),
                   [&](const Parameter &candidate) {
                     return &candidate == symbol->parameterDeclaration;
                   });
  if (parameter == function->bodyDeclaration()->parameters().end()) {
    return nullptr;
  }
  const std::size_t index = static_cast<std::size_t>(std::distance(
      function->bodyDeclaration()->parameters().begin(), parameter));
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
        stored != nullptr && stored->receiverBinding != 0) {
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
      receiverSymbol != nullptr &&
      (receiverSymbol->kind == SymbolKind::Field ||
       receiverSymbol->kind == SymbolKind::LocalVariable) &&
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
                             (storedReceiver ? "binding '" : "parameter '") +
                             expressionToken(call.callee()).lexeme +
                             "' cannot be inferred with 'auto'.");
      diagnostic.hints.emplace_back(
          "Use an explicit result binding, assignment target, condition, or "
          "enclosing return type so the exact callable result is known.");
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    } else if (returnType.kind == SemanticType::Reference &&
               returnType.referenceAccess == AccessMode::ReadOnly &&
               currentReturnBorrowContract() &&
               (currentReturnBorrowContract()->origin ==
                    BorrowOriginKind::Argument ||
                currentReturnBorrowContract()->deferred)) {
      // A read-only reference result relays the selected callable's own
      // result-origin contract. The enclosing body names the parameter the
      // relayed result borrows; each concrete instance verifies that the
      // selected callable's origin reaches exactly that parameter.
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
           "A deferred callable must be a named generic parameter, local "
           "binding, or field whose exact type is checked at instantiation.",
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
            .receiverBinding = receiverSymbol->id,
            .inferResultFromInstantiation = inferResultFromInstantiation});
    currentType = callExpressionType(returnType);
    return;
  }

  const auto found = std::find_if(
      function->bodyDeclaration()->parameters().begin(),
      function->bodyDeclaration()->parameters().end(),
      [&](const Parameter &candidate) { return &candidate == parameter; });
  if (found == function->bodyDeclaration()->parameters().end()) {
    currentType = SemanticType::Unknown;
    return;
  }
  const std::size_t parameterIndex = static_cast<std::size_t>(
      std::distance(function->bodyDeclaration()->parameters().begin(), found));
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
  currentType = callExpressionType(returnType);
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
    // A reference result relays through the selected callable's own
    // result-origin contract: the call result borrows that argument, and
    // the enclosing body's return summary decides whether it may escape.
    if (returnType.kind == SemanticType::Reference &&
        returnType.referenceAccess == AccessMode::ReadOnly &&
        callableResultOriginContract(call))
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
      deferred && deferred->receiverBinding != 0
          ? semanticModel.database().findSymbol(deferred->receiverBinding)
          : nullptr;
  // The stored callable is a class field or a closure capture; name the
  // binding by what it is.
  const std::string storedKind = field == nullptr ? "Callable parameter '"
                                 : field->kind == SymbolKind::LambdaCapture
                                     ? "Callable capture '"
                                     : "Callable field '";
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2046", DiagnosticPhase::Semantics, location,
                     storedKind + location.lexeme + "' must return '" +
                         typeSpelling(requirement->returnType) +
                         "' for this invocation, "
                         "but the selected callable returns '" +
                         typeSpelling(returnType) + "'.");
  if (field) {
    diagnostic.related.push_back(
        {field->nameSpan, field->kind == SymbolKind::LambdaCapture
                              ? "Callable capture declared here."
                              : "Callable field declared here."});
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
  const auto access =
      lambda->capability == CallableInvocationCapability::Mutable
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  if (access == AccessMode::Mutable && !isMutableObject(call.callee())) {
    report(expressionToken(call.callee()),
           "A callback with an exclusive mutable capture requires mutable "
           "access to the callback.",
           "GTI-S2046");
    valid = false;
  }
  const auto receiver = loanPlace(call.callee());
  reportLoanPlaceConflict(
      expressionToken(call.callee()), receiver, access,
      "Callback invocation overlaps a borrow that may still be live.",
      accessLoan(call.callee()));
  if (access == AccessMode::Mutable)
    recordMutableBindingUse(receiver.root);
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
                                   call.paren(), receiver, access);
  valid = validateCallableReturn(call, lambda->returnType) && valid;
  valid = validateCallableCapability(call, lambda->capability) && valid;
  if (valid) {
    semanticModel.recordLambdaCall(
        call,
        ResolvedLambdaCallInfo{.lambda = lambda->id,
                               .returnType = lambda->returnType,
                               .parameterTypes = lambda->parameterTypes,
                               .capability = lambda->capability,
                               .boundary = boundary,
                               .borrowOrigin = lambda->returnBorrowOrigin,
                               .borrowArgument = lambda->returnBorrowParameter,
                               .borrowAccess = lambda->returnBorrowAccess});
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
  semanticModel.clearBorrowDependencySet(call);
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

auto SemanticVisitor::Impl::satisfiesAtomicConstraint(
    const SemanticType &argument, GenericConstraintKind constraint) const
    -> bool {
  if (argument == SemanticType::Unknown ||
      constraint == GenericConstraintKind::None ||
      constraint == GenericConstraintKind::Invalid) {
    return true;
  }
  switch (constraint) {
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
  case GenericConstraintKind::Boolean:
    return argument == SemanticType::Bool;
  case GenericConstraintKind::ObjectLayout:
    return runtimeObjectLayoutType(argument);
  case GenericConstraintKind::BorrowFree:
    return argument.kind != SemanticType::Reference &&
           !typeTraits(argument).containsBorrowedState;
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
    if (requirement.syntax && requirement.syntax->predicate) {
      const auto capability = predicateCapabilityFor(
          *requirement.syntax, requirement.arguments,
          requirement.lookupScope ? &*requirement.lookupScope : nullptr);
      if (capability &&
          capability->structural != StructuralConstraintKind::None)
        result.push_back({.kind = capability->structural,
                          .arguments = capability->arguments,
                          .application = requirement.syntax});
      return result;
    }
    if (requirement.syntax && requirement.syntax->memberRequirement) {
      if (requirement.syntax->memberRequirement->isExpressionRequirement()) {
        if (auto fact = expressionRequirementFact(
                *requirement.syntax->memberRequirement,
                requirement.arguments)) {
          fact->application = requirement.syntax;
          result.push_back(std::move(*fact));
        }
        return result;
      }
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
          .requiresUnsafe = pattern.requiresUnsafe,
          .borrowSource = pattern.borrowSource};
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
        result.emplace_back(std::move(expanded));
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
           left.borrowSource == right.borrowSource &&
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
  // An inferred result is a weaker description of the same operation, not a
  // second overload. Refine it only with identical inputs/access and a stronger
  // result contract; unrelated origins or unsafe obligations never disappear.
  std::vector<bool> superseded(result.size(), false);
  bool any = false;
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto &broad = result[index];
    if (broad.kind != StructuralConstraintKind::ReadMember ||
        broad.arguments.size() < 2)
      continue;
    const auto &value = broad.arguments[1];
    const bool inferredReference =
        value.kind == SemanticType::Reference && value.arguments.size() == 1 &&
        value.arguments.front() == SemanticType::Unknown;
    if (value != SemanticType::Unknown && !inferredReference)
      continue;
    for (const auto &narrow : result) {
      if (narrow.kind != broad.kind || narrow.memberName != broad.memberName ||
          narrow.mutableReceiver != broad.mutableReceiver ||
          (narrow.requiresUnsafe && !broad.requiresUnsafe) ||
          narrow.arguments.size() != broad.arguments.size() ||
          narrow.arguments[0] != broad.arguments[0] ||
          !std::equal(narrow.arguments.begin() + 2, narrow.arguments.end(),
                      broad.arguments.begin() + 2) ||
          narrow.arguments[1] == value ||
          narrow.arguments[1] == SemanticType::Unknown)
        continue;
      const auto &exact = narrow.arguments[1];
      if (inferredReference &&
          (exact.kind != SemanticType::Reference ||
           exact.referenceAccess != value.referenceAccess ||
           narrow.borrowSource != broad.borrowSource))
        continue;
      superseded[index] = any = true;
      break;
    }
  }
  if (any) {
    std::vector<ExpandedStructuralRequirement> refined;
    refined.reserve(result.size());
    for (std::size_t index = 0; index < result.size(); ++index)
      if (!superseded[index])
        refined.push_back(std::move(result[index]));
    return refined;
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
          if (requirement.syntax && requirement.syntax->predicate) {
            const auto capability = predicateCapabilityFor(
                *requirement.syntax, requirement.arguments,
                requirement.lookupScope ? &*requirement.lookupScope : nullptr);
            return capability &&
                           capability->atomic != GenericConstraintKind::None &&
                           capability->arguments.size() == 1 &&
                           capability->arguments.front() == type
                       ? constraintBit(capability->atomic)
                       : leaf;
          }
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
      return node.kind == ConstraintExpressionKind::Conjunction
                 ? left | right
                 : impliedConstraints(left) & impliedConstraints(right);
    };
    result |= atomicForNode(expression->root);
  }
  return result;
}

auto SemanticVisitor::Impl::structuralRequirementAvailable(
    StructuralConstraintKind kind,
    std::span<const SemanticType> arguments) const -> bool {
  const std::vector<ExpandedStructuralRequirement> active =
      activeStructuralRequirements();
  return std::any_of(active.begin(), active.end(),
                     [&](const auto &requirement) {
                       if (requirement.arguments.size() != arguments.size() ||
                           !std::equal(requirement.arguments.begin(),
                                       requirement.arguments.end(),
                                       arguments.begin(), arguments.end())) {
                         return false;
                       }
                       return requirement.kind == kind;
                     });
}

auto SemanticVisitor::Impl::activeRequirementsImply(
    std::span<const AppliedConceptRequirement> requirements,
    const ConstraintExpression &expression, std::size_t root) const -> bool {
  // Retain alternatives as propositions: intersecting their common unary
  // consequences alone loses the fact that one of the alternatives holds.
  // This is bounded, conservative implication, not predicate execution or a
  // solver for arbitrary constexpr code. Failure to prove grants no capability.
  struct Proof {
    ConstraintExpressionKind kind = ConstraintExpressionKind::Application;
    std::size_t left = 0;
    std::size_t right = 0;
    AppliedConceptRequirement leaf;
    std::optional<PredicateCapability> capability;
  };
  std::vector<Proof> proofs(1);
  std::size_t budget = 8192;
  bool exhausted = false;
  const auto append = [&](Proof proof) -> std::size_t {
    if (proofs.size() >= 2048) {
      exhausted = true;
      return 0;
    }
    proofs.emplace_back(std::move(proof));
    return proofs.size() - 1;
  };
  const auto combine = [&](ConstraintExpressionKind kind, std::size_t left,
                           std::size_t right) -> std::size_t {
    if (!left || !right)
      return 0;
    return append(Proof{.kind = kind, .left = left, .right = right});
  };
  std::function<std::size_t(const AppliedConceptRequirement &, unsigned)> leaf;
  std::function<std::size_t(std::span<const AppliedConceptRequirement>,
                            const ConstraintExpression &, std::size_t,
                            unsigned)>
      tree;
  tree = [&](std::span<const AppliedConceptRequirement> set,
             const ConstraintExpression &shape, std::size_t index,
             unsigned depth) -> std::size_t {
    if (depth > 64 || budget == 0) {
      exhausted = true;
      return 0;
    }
    --budget;
    if (index >= shape.nodes.size())
      return 0;
    const auto &node = shape.nodes[index];
    if (node.kind == ConstraintExpressionKind::Application) {
      const auto found =
          std::find_if(set.begin(), set.end(), [&](const auto &item) {
            return item.expression == &shape &&
                   item.expressionApplication == node.application;
          });
      return found == set.end() ? 0 : leaf(*found, depth + 1);
    }
    const auto left = tree(set, shape, node.left, depth + 1);
    const auto right = tree(set, shape, node.right, depth + 1);
    return combine(node.kind, left, right);
  };
  leaf = [&](const AppliedConceptRequirement &item,
             unsigned depth) -> std::size_t {
    if (depth > 64 || budget == 0) {
      exhausted = true;
      return 0;
    }
    --budget;
    if (item.conceptId && item.conceptId <= concepts.size() &&
        !(item.syntax &&
          (item.syntax->predicate || item.syntax->memberRequirement))) {
      const auto &conceptInfo = concepts[item.conceptId - 1];
      if (!conceptInfo.protocol && conceptInfo.definitionExpression &&
          !conceptInfo.definitionRequirements.empty()) {
        const auto substitution =
            conceptSubstitution(conceptInfo, item.arguments);
        std::vector<AppliedConceptRequirement> expanded;
        for (const auto &pattern : conceptInfo.definitionRequirements) {
          AppliedConceptRequirement applied{
              .conceptId = pattern.conceptId,
              .syntax = pattern.syntax,
              .expression = conceptInfo.definitionExpression,
              .expressionApplication = pattern.expressionApplication,
              .lookupScope = conceptInfo.namespaceScope};
          for (const auto &argument : pattern.arguments) {
            if (!argument.parameter)
              applied.arguments.push_back(
                  substituteType(argument.concrete, substitution));
            else if (*argument.parameter < item.arguments.size())
              applied.arguments.push_back(item.arguments[*argument.parameter]);
            else
              return 0;
          }
          expanded.emplace_back(std::move(applied));
        }
        return tree(expanded, *conceptInfo.definitionExpression,
                    conceptInfo.definitionExpression->root, depth + 1);
      }
    }
    Proof proof{.leaf = item};
    if (item.syntax && item.syntax->predicate)
      proof.capability = predicateCapabilityFor(
          *item.syntax, item.arguments,
          item.lookupScope ? &*item.lookupScope : nullptr);
    return append(std::move(proof));
  };
  const auto required = tree(requirements, expression, root, 0);
  std::function<bool(std::size_t, std::size_t)> implies =
      [&](std::size_t provided, std::size_t wanted) -> bool {
    if (!provided || !wanted || exhausted || budget == 0)
      return false;
    --budget;
    const auto &a = proofs[provided];
    const auto &b = proofs[wanted];
    // A required conjunction needs both guarantees. Every possible provided
    // branch must entail the goal, even when the goal is also a disjunction.
    if (b.kind == ConstraintExpressionKind::Conjunction)
      return implies(provided, b.left) && implies(provided, b.right);
    if (a.kind == ConstraintExpressionKind::Disjunction)
      return implies(a.left, wanted) && implies(a.right, wanted);
    // A conjunct can preserve a complete disjunction. Splitting the wanted
    // disjunction first would incorrectly require one particular alternative
    // from `(A || B) && C`. If neither conjunct proves it alone, still try each
    // wanted alternative against the complete conjunction below (A and C may
    // be supplied by different conjuncts).
    if (a.kind == ConstraintExpressionKind::Conjunction &&
        (implies(a.left, wanted) || implies(a.right, wanted)))
      return true;
    if (b.kind == ConstraintExpressionKind::Disjunction)
      return implies(provided, b.left) || implies(provided, b.right);
    if (a.kind == ConstraintExpressionKind::Conjunction)
      return false;
    if (a.capability && b.capability &&
        a.capability->atomic != GenericConstraintKind::None &&
        b.capability->atomic != GenericConstraintKind::None &&
        a.capability->arguments == b.capability->arguments)
      return constraintImplies(constraintBit(a.capability->atomic),
                               b.capability->atomic);
    // Opaque predicates and member queries retain declaration identity. Equal
    // spelling, or equal results on one concrete type, is not a proof.
    const auto special = [](const AppliedConceptRequirement &item) {
      return item.syntax &&
             (item.syntax->predicate || item.syntax->memberRequirement);
    };
    return a.leaf.arguments == b.leaf.arguments &&
           (special(a.leaf) || special(b.leaf)
                ? a.leaf.syntax == b.leaf.syntax
                : a.leaf.conceptId != 0 &&
                      a.leaf.conceptId == b.leaf.conceptId);
  };
  for (const auto &scope : requirementScopes) {
    const auto shaped =
        std::find_if(scope.begin(), scope.end(),
                     [](const auto &item) { return item.expression; });
    if (shaped != scope.end()) {
      const auto provided =
          tree(scope, *shaped->expression, shaped->expression->root, 0);
      if (implies(provided, required))
        return true;
    } else {
      for (const auto &item : scope) {
        const auto provided = leaf(item, 0);
        if (implies(provided, required))
          return true;
      }
    }
  }
  return false;
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
            if (leftRequirement && rightRequirement &&
                ((leftRequirement->syntax &&
                  (leftRequirement->syntax->predicate ||
                   (leftRequirement->syntax->memberRequirement &&
                    leftRequirement->syntax->memberRequirement
                        ->isExpressionRequirement()))) ||
                 (rightRequirement->syntax &&
                  (rightRequirement->syntax->predicate ||
                   (rightRequirement->syntax->memberRequirement &&
                    rightRequirement->syntax->memberRequirement
                        ->isExpressionRequirement())))))
              return leftRequirement->syntax == rightRequirement->syntax &&
                     leftRequirement->arguments == rightRequirement->arguments;
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
    if (activeRequirementsImply(requirements, *expression, nodeIndex)) {
      return std::nullopt;
    }
    return ConstraintFailure{.parameter = left->parameter,
                             .argument = left->argument,
                             .disjunction = &node.operation,
                             .alternatives = {*left, *right}};
  };
  auto failure = failureFor(expression->root);
  // A source concept can imply another through nested conjunctions, including
  // opaque predicate leaves whose declaration identity is preserved. Do not
  // re-evaluate those predicates on an unknown type and discard an existing
  // proof merely because the requested expression has no disjunction.
  if (failure &&
      activeRequirementsImply(requirements, *expression, expression->root))
    return std::nullopt;
  return failure;
}

auto SemanticVisitor::Impl::preambleConceptProofApplies(
    const AppliedConceptRequirement &requirement) const -> bool {
  if (adoptedPreamble == nullptr || adoptedPreamble->state == nullptr ||
      requirement.conceptId > adoptedPreamble->state->concepts.size())
    return false;
  if (requirement.syntax == nullptr) {
    // A named concept leaf: caller-independent by construction.
    if (requirement.conceptId == 0 || requirement.expression != nullptr ||
        requirement.lookupScope || requirement.trustedMemberAccess)
      return false;
  } else {
    // Written syntax is caller-independent only as a declaration query, and
    // its node identity is stable only in the preamble's own source.
    const SourceUnit *unit = sourceGraph == nullptr
                                 ? nullptr
                                 : sourceGraph->findUnit(sourceUnitFor(
                                       requirement.syntax->name.last()));
    if (!requirement.lookupScope || unit == nullptr || !trustedUnit(unit->id) ||
        unit->declarationStart + unit->declarationCount >
            adoptedPrefixDeclarations)
      return false;
  }
  return std::all_of(requirement.arguments.begin(), requirement.arguments.end(),
                     [&](const SemanticType &argument) {
                       return preambleClosedType(argument);
                     });
}

auto SemanticVisitor::Impl::preambleClosedType(
    const SemanticType &candidate) const -> bool {
  if (adoptedPreamble == nullptr || adoptedPreamble->state == nullptr)
    return false;
  const Impl &preamble = *adoptedPreamble->state;
  const auto preambleType = [&](const auto &self,
                                const SemanticType &type) -> bool {
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
    case SemanticType::Void:
    case SemanticType::StringView:
    case SemanticType::CString:
    case SemanticType::NullPtr:
      break;
    case SemanticType::Class:
      if (type.classId == 0 || type.classId > preamble.classes.size())
        return false;
      break;
    case SemanticType::Enum:
      if (type.enumId == 0 || type.enumId > preamble.enums.size())
        return false;
      break;
    case SemanticType::Reference:
    case SemanticType::RawPointer:
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
      break;
    case SemanticType::Array:
      if (type.arrayLengthParameterId != 0)
        return false;
      break;
    default:
      return false;
    }
    return std::all_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  return preambleType(preambleType, candidate);
}

auto SemanticVisitor::Impl::firstConceptApplicationFailure(
    const AppliedConceptRequirement &requirement) const
    -> std::optional<ConstraintFailure> {
  // A concept proved of the adopted preamble's own types holds for every
  // analysis adopting it, so an editor reuses it across snapshots.
  const bool shared = preambleConceptProofApplies(requirement);
  const auto proofKey = [&] {
    return SemanticPreamble::ConceptProofKey{
        .conceptId = requirement.conceptId,
        .syntax = requirement.syntax,
        .expression = requirement.expression,
        .expressionApplication = requirement.expressionApplication,
        .trustedMemberAccess = requirement.trustedMemberAccess,
        .lookupScope = requirement.lookupScope,
        .arguments = requirement.arguments};
  };
  if (shared && adoptedPreamble->provedConcept(proofKey())) {
    if (workMetrics != nullptr)
      ++workMetrics->semanticConceptProofReuses;
    return std::nullopt;
  }
  if (shared) {
    // An active requirement over closed arguments (a substituted instance's
    // constraint, say) could satisfy the query by assumption rather than
    // proof. Open assumptions cannot speak about these closed arguments, so a
    // proof is recorded only from an evaluation without closed assumptions:
    // when some are active, evaluate once without any. Assumptions only add
    // satisfactions, so a failure here falls back to the ordinary query.
    const auto openAssumption = [](const AppliedConceptRequirement &active) {
      return std::any_of(active.arguments.begin(), active.arguments.end(),
                         containsOpenTypeParameter);
    };
    const bool closedAssumptions = std::any_of(
        requirementScopes.begin(), requirementScopes.end(),
        [&](const auto &scope) {
          return !std::all_of(scope.begin(), scope.end(), openAssumption);
        });
    std::optional<ConstraintFailure> proof;
    if (closedAssumptions) {
      struct RestoreScopes {
        std::vector<std::vector<AppliedConceptRequirement>> &scopes;
        std::vector<std::vector<AppliedConceptRequirement>> held;
        ~RestoreScopes() { scopes = std::move(held); }
      } restore{requirementScopes, std::move(requirementScopes)};
      requirementScopes.clear();
      proof = evaluateConceptApplication(requirement);
    } else {
      proof = evaluateConceptApplication(requirement);
    }
    if (!proof) {
      adoptedPreamble->recordConceptProof(proofKey());
      if (workMetrics != nullptr)
        ++workMetrics->semanticConceptProofRecords;
      return std::nullopt;
    }
    if (!closedAssumptions)
      return proof;
  }
  return evaluateConceptApplication(requirement);
}

auto SemanticVisitor::Impl::evaluateConceptApplication(
    const AppliedConceptRequirement &requirement) const
    -> std::optional<ConstraintFailure> {
  if (requirement.syntax && requirement.syntax->predicate) {
    std::string reason;
    if (constraintPredicateSatisfied(
            *requirement.syntax, requirement.arguments, &reason,
            requirement.lookupScope ? &*requirement.lookupScope : nullptr))
      return std::nullopt;
    ConstraintFailure failure{.parameter = requirement.syntax->name.last(),
                              .argument = requirement.arguments.empty()
                                              ? SemanticType::Unknown
                                              : requirement.arguments.front(),
                              .application = requirement.syntax,
                              .leafApplication = requirement.syntax,
                              .applicationArgumentIndex =
                                  requirement.arguments.size() == 1
                                      ? std::optional<std::size_t>(0)
                                      : std::nullopt,
                              .expressionFailure = std::move(reason)};
    // Preserve the fact's existing diagnostic without deriving a subject
    // from coincidentally equal arguments in a multi-parameter predicate.
    if (requirement.arguments.size() == 1) {
      const auto capability = predicateCapabilityFor(
          *requirement.syntax, requirement.arguments,
          requirement.lookupScope ? &*requirement.lookupScope : nullptr);
      if (capability && capability->arguments == requirement.arguments)
        failure.failed = capability->atomic;
    }
    return failure;
  }
  if (requirement.syntax && requirement.syntax->memberRequirement) {
    if (requirement.syntax->memberRequirement->isExpressionRequirement()) {
      std::string reason;
      if (expressionRequirementSatisfied(
              *requirement.syntax->memberRequirement, requirement.arguments,
              &reason,
              requirement.lookupScope ? &*requirement.lookupScope : nullptr))
        return std::nullopt;
      const auto &query = *requirement.syntax->memberRequirement;
      const auto operation =
          expressionRequirementFact(query, requirement.arguments);
      // An arbitrary expression may ignore its first synthetic parameter,
      // mention several types, or fail only because of its required result.
      // Retain an input origin only for a supported operation whose sole
      // synthetic parameter is its actual borrowed receiver, and whose result
      // cannot introduce an independent generic subject.
      bool unaryReceiver = false;
      if (query.parameters().size() == 1 && requirement.arguments.size() == 2 &&
          operation && operation->memberName != "$construct") {
        const auto &result = query.result();
        const bool fixedBuiltinResult =
            result.name.segments.size() == 1 &&
            isTypeKeywordToken(result.name.last().kind) &&
            result.name.last().kind != TokenKind::AUTO &&
            result.arguments.empty() && !result.qualifiedOwner &&
            result.pointers.empty() && result.arrayExtents.empty() &&
            !result.reference && !result.isNativeFunction() &&
            !result.isFunctionSignature() && !result.packExpansion &&
            !result.genericValueExpression;
        TypeRef receiverSubject = query.parameters().front();
        receiverSubject.reference.reset();
        receiverSubject.referenceMutability.reset();
        unaryReceiver =
            fixedBuiltinResult ||
            typeRefSpelling(receiverSubject) == typeRefSpelling(result);
      }
      return ConstraintFailure{
          .parameter = requirement.syntax->memberRequirement->keyword(),
          .argument =
              !unaryReceiver ? SemanticType::Unknown
              : requirement.arguments.front().kind == SemanticType::Reference &&
                      requirement.arguments.front().arguments.size() == 1
                  ? requirement.arguments.front().arguments.front()
                  : requirement.arguments.front(),
          .application = requirement.syntax,
          .leafApplication = requirement.syntax,
          .applicationArgumentIndex =
              unaryReceiver ? std::optional<std::size_t>(0) : std::nullopt,
          .expressionFailure = std::move(reason)};
    }
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
  if (conceptInfo.protocol) {
    return protocolConformanceFailure(requirement);
  }
  if (std::any_of(requirement.arguments.begin(), requirement.arguments.end(),
                  containsOpenTypeParameter)) {
    // Inline generic annotations have no enclosing ConstraintExpression.
    // Prove their named application through the same bounded source formula
    // path as a trailing clause, including aliases and opaque predicate leaves.
    // The temporary shape grants no facts of its own and never escapes this
    // query. Closed arguments still undergo concrete admission below.
    ConstraintExpression shape;
    shape.nodes.push_back(
        {.kind = ConstraintExpressionKind::Application, .application = 0});
    shape.root = 0;
    AppliedConceptRequirement wanted = requirement;
    wanted.expression = &shape;
    wanted.expressionApplication = 0;
    if (activeRequirementsImply(std::span(&wanted, 1), shape, shape.root))
      return std::nullopt;
  }
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
          .expressionApplication = pattern.expressionApplication,
          .lookupScope = conceptInfo.namespaceScope};
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
            (failure->expressionFailure && !outerArgument) ||
                    requirement.syntax->arguments.empty()
                ? requirement.syntax->name.last()
                : requirement.syntax->arguments.front().name.last();
      }
      // The concept the requirement names states what satisfies it, ahead of
      // any concept it is defined through.
      if (std::optional<std::string> explanation = conceptInfo.explanation())
        failure->conceptExplanation = std::move(explanation);
      return failure;
    }
    return std::nullopt;
  }

  const auto applicationContext = [&] {
    std::vector<SemanticConstraintContext> context;
    if (conceptInfo.compilerPrivate)
      return context;
    SemanticConstraintContext entry;
    entry.name = conceptInfo.qualifiedName;
    entry.arguments = requirement.arguments;
    if (requirement.syntax != nullptr)
      entry.location = requirement.syntax->name.last();
    else
      entry.location = conceptInfo.declaration->name();
    context.emplace_back(std::move(entry));
    return context;
  };
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
          .context = applicationContext(),
          .leafApplication = requirement.syntax,
          .applicationArgumentIndex = index,
          .conceptExplanation = conceptInfo.explanation()};
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
          .context = applicationContext(),
          .leafApplication = requirement.syntax,
          .conceptExplanation = conceptInfo.explanation()};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::hasConstraintFailure(
    const ConstraintFailure &failure) -> bool {
  return failure.failed != GenericConstraintKind::None ||
         failure.structural != StructuralConstraintKind::None ||
         failure.expressionFailure.has_value() || !failure.alternatives.empty();
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
    GenericParameterId *conflictingParameter,
    bool *conflictingValueParameter) const -> bool {
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

  const auto definitionFamily = [&](ClassId id) {
    if (id == 0 || id > classes.size())
      return id;
    const auto primary = classInfo(id).definitionFamilyPrimary;
    return primary == 0 ? id : primary;
  };
  // Deduce from a family's public arguments. Concrete substitution still
  // selects an exact definition and ordinary argument matching checks it;
  // distinct definitions are not interchangeable object representations.
  if (pattern.kind != argument.kind ||
      pattern.nativeCScalar != argument.nativeCScalar ||
      definitionFamily(pattern.classId) != definitionFamily(argument.classId) ||
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
            dynamicTextReturnBorrowSummary(concrete, resolved.dispatchOwner)) {
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

auto SemanticVisitor::Impl::containsExactType(
    const SemanticType &type, const SemanticType &candidate) const -> bool {
  if (candidate != SemanticType::Unknown && type == candidate) {
    return true;
  }
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size() &&
      classInfo(type.classId).compilerType ==
          CompilerTypeKind::MetaCallableReturnType) {
    // A query's operand is not stored in its result. Concrete substitution
    // rewrites the projection before escape/use checks; an open projection
    // must not falsely claim to return the callable environment itself.
    const SemanticType resolved = rewriteCompilerType(type);
    return resolved != type && containsExactType(resolved, candidate);
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsExactType(argument, candidate);
                     });
}

auto SemanticVisitor::Impl::callableReturnSubjectUnsupported(
    const SemanticType &subject) -> bool {
  return subject != SemanticType::Unknown &&
         !containsOpenTypeParameter(subject) && !subject.hasLambdaShape() &&
         !subject.hasFunctionItemShape() && !subject.hasNativeFunctionShape() &&
         subject.kind != SemanticType::FunctionSignature;
}

auto SemanticVisitor::Impl::containsUnsupportedCallableReturn(
    const SemanticType &type) const -> bool {
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size() &&
      classInfo(type.classId).compilerType ==
          CompilerTypeKind::MetaCallableReturnType &&
      type.arguments.size() == 1 &&
      callableReturnSubjectUnsupported(type.arguments.front())) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsUnsupportedCallableReturn(argument);
                     });
}

auto SemanticVisitor::Impl::signatureReflectsUnsupportedCallable(
    const CallableCandidate &function) const -> bool {
  const auto unsupported = [&](const SemanticType &type) {
    return containsUnsupportedCallableReturn(type);
  };
  return unsupported(function.returnType) ||
         std::any_of(function.parameterTypes.begin(),
                     function.parameterTypes.end(), unsupported) ||
         std::any_of(function.requirements.begin(), function.requirements.end(),
                     [&](const AppliedConceptRequirement &requirement) {
                       return std::any_of(requirement.arguments.begin(),
                                          requirement.arguments.end(),
                                          unsupported);
                     });
}

auto SemanticVisitor::Impl::reportUnsupportedCallableReturn(
    const CallableCandidate &declared, const CallableCandidate &instantiated,
    const Token &site) -> bool {
  if (signatureReflectsUnsupportedCallable(declared) ||
      !signatureReflectsUnsupportedCallable(instantiated)) {
    return false;
  }
  // This instantiation's signature names no result type, so the call is
  // rejected here rather than by every use of that type inside the callee.
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2023", DiagnosticPhase::Semantics, site,
      "Callable return-type reflection requires a function signature, a "
      "selected function item, a native function pointer, or a lambda type; "
      "it does not select an overloaded operator().");
  if (instantiated.declaration != nullptr)
    diagnostic.related.push_back(
        {tokenSpan(instantiated.declaration->name()),
         "The selected function's signature reflects its callable's result."});
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::containsDisallowedCallableCaptureState(
    const SemanticType &type, std::unordered_set<ClassId> &visiting,
    bool reviewedRawState, bool checkedBorrows) const -> bool {
  if (type.kind == SemanticType::FunctionItem) {
    // Parameters and results describe the selected target; they are not
    // stored state in the zero-state function-item value.
    return false;
  }
  if (type.kind == SemanticType::RawPointer)
    return !reviewedRawState;
  if (type.kind == SemanticType::Reference ||
      type.kind == SemanticType::StringView) {
    return !checkedBorrows;
  }
  if (type.kind == SemanticType::Lambda) {
    return type.hasLambdaShape() &&
           std::any_of(type.lambdaCaptureTypes().begin(),
                       type.lambdaCaptureTypes().end(),
                       [&](const SemanticType &capture) {
                         return containsDisallowedCallableCaptureState(
                             capture, visiting, reviewedRawState,
                             checkedBorrows);
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
          substituteType(base->type, substitution), visiting, reviewedRawState,
          checkedBorrows);
    }
    for (const FieldInfo &field : owner.fields) {
      if (found || field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end()) {
        found = containsDisallowedCallableCaptureState(
            substituteType(member->second.symbol.type, substitution), visiting,
            reviewedRawState, checkedBorrows);
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
    return std::any_of(
        concrete->enumerators.begin(), concrete->enumerators.end(),
        [&](const auto &entry) {
          return std::any_of(
              entry.payloadTypes.begin(), entry.payloadTypes.end(),
              [&](const SemanticType &payload) {
                return containsDisallowedCallableCaptureState(
                    payload, visiting, reviewedRawState, checkedBorrows);
              });
        });
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsDisallowedCallableCaptureState(
                           argument, visiting, reviewedRawState,
                           checkedBorrows);
                     });
}

auto SemanticVisitor::Impl::containsDisallowedCallableCaptureState(
    const SemanticType &type, bool checkedBorrows) const -> bool {
  std::unordered_set<ClassId> visiting;
  return containsDisallowedCallableCaptureState(type, visiting, false,
                                                checkedBorrows);
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
  for (std::size_t index = 0;
       index < function.bodyDeclaration()->parameters().size(); ++index) {
    const BindingInfo *binding = semanticModel.findBinding(
        function.bodyDeclaration()->parameters()[index]);
    if (binding != nullptr && binding->symbol == symbol) {
      return index;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::exactMovedReturnParameter(
    const FunctionInfo &function) const -> std::optional<std::size_t> {
  if (function.declaration == nullptr ||
      function.bodyDeclaration()->body() == nullptr) {
    return std::nullopt;
  }
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.bodyDeclaration()->body().get(),
                                 returns);
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
        function.bodyDeclaration()->body() == nullptr ||
        !finalizesFunction(function)) {
      continue;
    }
    const ReturnStmt *soleReturn =
        function.bodyDeclaration()->body()->statements().size() == 1
            ? stmtAs<ReturnStmt>(function.bodyDeclaration()
                                     ->body()
                                     ->statements()
                                     .front()
                                     .get())
            : nullptr;
    const std::optional<std::size_t> exactReturn =
        exactMovedReturnParameter(function);
    for (std::size_t index = 0;
         index < function.parameterTypes.size() &&
         index < function.bodyDeclaration()->parameters().size();
         ++index) {
      const Parameter &parameter =
          function.bodyDeclaration()->parameters()[index];
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
    if (function.declaration == nullptr ||
        !function.bodyDeclaration()->body() || !finalizesFunction(function))
      continue;
    for (std::size_t index = 0; index < function.parameterTypes.size();
         ++index) {
      if (index >= function.bodyDeclaration()->parameters().size())
        continue;
      const auto &parameter = function.bodyDeclaration()->parameters()[index];
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
                         target->bodyDeclaration()->parameters().size() - 1)
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
            {tokenSpan(target->bodyDeclaration()
                           ->parameters()[declarationIndex]
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
      // Moving an exact carrier transfers its checked dependency facts; it
      // does not turn those dependencies into owned storage. Unbounded thread
      // tasks and erased owning wrappers retain their distinct owning rules.
      const bool checkedCarrier =
          !threadTask && isDirectStoredReferenceType(concreteArgumentType);
      if (!consumed) {
        failure = "requires an explicit std::move ownership transfer";
      } else if (!traits.movable) {
        failure = "is not movable";
      } else if ((traits.containsBorrowedState && !checkedCarrier) ||
                 (!threadTask && containsDisallowedCallableCaptureState(
                                     concreteArgumentType, checkedCarrier))) {
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
          {tokenSpan(target->bodyDeclaration()
                         ->parameters()[declarationIndex]
                         .sourceAnchor()),
           "Exact owned callable parameter declared here."});
      diagnostic.hints.emplace_back(
          "Move an exact movable callable; any checked dependencies must "
          "outlive its destination. Unbounded thread tasks must own their "
          "state. Raw implementation state "
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
          (callableParameterContract(target->id, argument.parameterIndex) !=
               nullptr ||
           isCheckedReferenceRelay(*target, argument.parameterIndex));
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
          argument.parameterIndex <
              target->bodyDeclaration()->parameters().size()) {
        diagnostic.related.push_back(
            {tokenSpan(target->bodyDeclaration()
                           ->parameters()[argument.parameterIndex]
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
  // A fresh function item borrowed for one call may be matched before the
  // target's contract is final, as a lambda is; the target must then prove
  // the parameter confined.
  for (const auto &[call, resolved] : semanticModel.calls) {
    if (call == nullptr || resolved.ephemeralBorrowArguments.empty() ||
        !finalizesCall(call)) {
      continue;
    }
    const FunctionInfo *target = semanticModel.findFunction(resolved.function);
    for (const std::size_t index : resolved.ephemeralBorrowArguments) {
      if (index >= call->arguments().size()) {
        continue;
      }
      const ExpressionInfo *info =
          semanticModel.findExpression(*call->arguments()[index]);
      if (info == nullptr || info->type.kind != SemanticType::FunctionItem) {
        continue;
      }
      const CallableParameterContract *contract =
          target == nullptr ? nullptr
                            : callableParameterContract(target->id, index);
      if (contract != nullptr &&
          contract->boundary == CallableBoundary::Confined) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics,
          expressionToken(call->arguments()[index]),
          "Function item argument " + std::to_string(index + 1) +
              " cannot be borrowed for the call because the selected target "
              "parameter is not proven confined.");
      if (target != nullptr && target->declaration != nullptr &&
          index < target->bodyDeclaration()->parameters().size()) {
        diagnostic.related.push_back(
            {tokenSpan(target->bodyDeclaration()->parameters()[index].name),
             "Selected generic parameter declared here."});
      }
      diagnostic.hints.emplace_back(
          "Pass the function to a generic reference parameter that it only "
          "invokes or forwards to another confined parameter.");
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
          contract.parameterIndex <
                  function.bodyDeclaration()->parameters().size()
              ? &function.bodyDeclaration()
                     ->parameters()[contract.parameterIndex]
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
                    source->bodyDeclaration()->parameters().size()
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
                    source->bodyDeclaration()->parameters().size()
            ? &source->declaration
                   ->parameters()[forwarding.sourceParameterIndex]
            : nullptr;
    const FunctionInfo *target = semanticModel.findFunction(forwarding.target);
    const Parameter *targetParameter =
        target != nullptr && target->declaration != nullptr &&
                forwarding.targetParameterIndex <
                    target->bodyDeclaration()->parameters().size()
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
    if (function.declaration == nullptr ||
        !function.bodyDeclaration()->body() || !finalizesFunction(function)) {
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
              function.bodyDeclaration()->parameters().size()) {
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
          function.bodyDeclaration()->parameters()[contract.parameterIndex];
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
        collectBorrowReturnExpressions(function.bodyDeclaration()->body().get(),
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
    if (const FunctionInfo *target = semanticModel.findFunction(candidate.id);
        targetContract == nullptr && target != nullptr &&
        isCheckedReferenceRelay(*target, index))
      continue;
    const std::size_t formalIndex =
        candidate.parameterPack && !candidate.parameterTypes.empty()
            ? std::min(index, candidate.parameterTypes.size() - 1)
            : index;
    const Parameter *parameter =
        candidate.declaration != nullptr &&
                formalIndex < candidate.bodyDeclaration()->parameters().size()
            ? &candidate.bodyDeclaration()->parameters()[formalIndex]
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
          candidate.declaration == nullptr ||
          !candidate.bodyDeclaration()->body() || targetContract == nullptr) {
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
                !candidate.bodyDeclaration()->body())) {
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
    const SemanticType &type, std::unordered_set<ClassId> &visiting,
    bool includeAtomics) const -> bool {
  if (std::any_of(type.arguments.begin(), type.arguments.end(),
                  [&](const SemanticType &argument) {
                    return typeRequiresThreads(argument, visiting,
                                               includeAtomics);
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
    return includeAtomics;
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
                        substituteType(base.type, substitution), visiting,
                        includeAtomics);
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
                       visiting, includeAtomics);
          });
  visiting.erase(owner->id);
  return required;
}

auto SemanticVisitor::Impl::typeRequiresThreads(const SemanticType &type,
                                                bool includeAtomics) const
    -> bool {
  std::unordered_set<ClassId> visiting;
  return typeRequiresThreads(type, visiting, includeAtomics);
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
    const auto *binding = semanticModel.findBinding(
        function->bodyDeclaration()->parameters()[index]);
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
    const Token &location, std::string_view surface,
    bool requiresConcurrentProfile) -> bool {
  if (!requiresConcurrentProfile ||
      (target.executionProfile == ExecutionProfile::Concurrent &&
       target.runtimeCapabilities.threads)) {
    return true;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2084", DiagnosticPhase::Semantics, location,
      requiresConcurrentProfile &&
              target.executionProfile != ExecutionProfile::Concurrent
          ? std::string(surface) + " requires the concurrent execution profile."
          : std::string(surface) +
                " is unavailable because the selected target does not "
                "provide thread-capable concurrency support.");
  diagnostic.hints.emplace_back(
      requiresConcurrentProfile &&
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
  if (constructor.requiresUnsafe)
    result = "unsafe " + result;
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

auto SemanticVisitor::Impl::consumingResultCarriesBorrow(
    const CallableCandidate &function) const -> bool {
  return function.receiverMutability == ReceiverMutability::Consuming &&
         !receiverTextFieldResult(function.returnType,
                                  function.returnBorrowOrigin,
                                  function.returnBorrowFields) &&
         !conditionalReferenceReturn(function.returnType,
                                     function.parameterTypes,
                                     function.dispatchOwner) &&
         (function.returnType.kind == SemanticType::Reference ||
          function.returnBorrowOrigin != BorrowOriginKind::None ||
          typeTraits(function.returnType).containsBorrowedState);
}

auto SemanticVisitor::Impl::reportConsumingBorrowedResult(
    const CallableCandidate &function, const Token &paren) -> void {
  report(paren,
         "A consuming method cannot return a reference or tracked borrowed "
         "state; this call returns '" +
             typeSpelling(function.returnType) + "'.",
         "GTI-S2022");
}

auto SemanticVisitor::Impl::validateSelectedFunction(
    const CallableCandidate &function, const ExprPtr &callee,
    const Token &paren, std::span<const ExprPtr> arguments,
    CallReceiverMode receiverMode) -> bool {
  if (consumingResultCarriesBorrow(function)) {
    reportConsumingBorrowedResult(function, paren);
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
    const bool requiresProfile =
        typeRequiresThreads(ownerType, false) ||
        typeRequiresThreads(function.returnType, false) ||
        std::any_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(),
                    [&](const SemanticType &type) {
                      return typeRequiresThreads(type, false);
                    });
    if (requiresThreads &&
        !validateConcurrentSurfaceAvailability(paren, "This concurrency API",
                                               requiresProfile)) {
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
  const auto *member = exprAs<Get>(callee.get());
  const SemanticType accessReceiver =
      function.staticMember ? SemanticType::Unknown
      : member              ? semanticModel.typeOf(*member->object())
      : currentClass        ? openClassType(*currentClass)
                            : SemanticType::Unknown;
  if (!canAccessMember(function.access, function.ownerClass, accessReceiver,
                       function.dispatchOwner)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, paren,
        "Method '" + function.declaration->name().lexeme + "' of '" +
            classInfo(function.ownerClass).name.lexeme + "' is " +
            std::string(accessModifierSpelling(function.access)) + ".");
    diagnostic.related.push_back(
        {tokenSpan(function.declaration->name()), "Method declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }

  validateMutableCallArgumentLoans(function.parameterTypes, arguments, paren);
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
      if (temporaryReceiver ||
          function.receiverMutability == ReceiverMutability::Consuming) {
        // Neither mutating a materialized carrier shell nor consuming a
        // fresh carrier upgrades a stored shared dependency into exclusive
        // access to its owner.
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
  const auto receiverOrigins =
      member != nullptr && !function.staticMember && !temporaryReceiver
          ? loanPlaces(member->object(), true)
          : (receiverPlace ? std::vector<SemanticLoanPlace>{*receiverPlace}
                           : std::vector<SemanticLoanPlace>{});
  validateCallPlaceExclusivity(function.parameterTypes, arguments, paren,
                               receiverPlace, callReceiverAccess,
                               transientReceiver, receiverOrigins);
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
    for (const auto &origin : receiverOrigins)
      recordMutableBindingUse(origin.root);
  }

  if (function.staticMember) {
    // Object-qualified static access is diagnosed while resolving Get.
    return true;
  }

  if (member != nullptr && receiverMode == CallReceiverMode::Value) {
    const ExpressionInfo *receiver =
        semanticModel.findExpression(*member->object());
    // A reference to a temporary's text has no result loan to carry it yet.
    const bool textReference =
        function.returnType.kind == SemanticType::Reference &&
        function.returnType.arguments.size() == 1 &&
        (function.returnType.arguments.front().kind ==
             SemanticType::StringView ||
         function.returnType.arguments.front().kind == SemanticType::CString);
    if (receiver != nullptr && receiver->category == ValueCategory::Value &&
        receiver->type.kind == SemanticType::Class &&
        (receiver->traits.containsBorrowedState || textReference)) {
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
    reportLoanPlaceConflicts(
        paren, receiverOrigins, callReceiverAccess,
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

auto SemanticVisitor::Impl::consultsResultUse(const Expr *expression) -> bool {
  if (const auto *binary = exprAs<Binary>(expression))
    return binary->oper().kind == TokenKind::COMMA;
  return exprAs<Call>(expression) || exprAs<Get>(expression) ||
         exprAs<Index>(expression) || exprAs<Grouping>(expression) ||
         exprAs<ConditionalExpr>(expression) || exprAs<Unary>(expression);
}

auto SemanticVisitor::Impl::resultUseFor(const Expr &expression) const
    -> std::optional<ResultUse> {
  if (contextualResultUse && contextualResultUse->expression == &expression)
    return contextualResultUse;
  return std::nullopt;
}

auto SemanticVisitor::Impl::prependResultUseStep(
    ResultUseStep step, const std::optional<ResultUse> &parent)
    -> std::vector<ResultUseStep> {
  std::vector<ResultUseStep> steps{std::move(step)};
  if (parent)
    steps.insert(steps.end(), parent->steps.begin(), parent->steps.end());
  return steps;
}

auto SemanticVisitor::Impl::accessorResultUse(const ResultUse &use,
                                              SemanticType type) const
    -> ResultUseAccess {
  for (const ResultUseStep &step : use.steps) {
    // SemanticType assignment from its own argument is not alias-safe.
    while (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
      SemanticType referent = type.arguments.front();
      type = std::move(referent);
    }
    if (step.kind == ResultUseStep::Kind::Bind) {
      // A reference target needs its own access. A value target copies the
      // result; a conversion from another class could need more.
      std::optional<ResultUseAccess> bound;
      for (const SemanticType &target : step.targets) {
        ResultUseAccess access = ResultUseAccess::Unknown;
        if (target.kind == SemanticType::Reference)
          access = target.referenceAccess == AccessMode::Mutable
                       ? ResultUseAccess::Mutable
                       : ResultUseAccess::ReadOnly;
        else if (target == type || containsOpenTypeParameter(target) ||
                 (classInfo(target) == nullptr && classInfo(type) == nullptr))
          access = ResultUseAccess::ReadOnly;
        if (bound && *bound != access)
          return ResultUseAccess::Unknown;
        bound = access;
      }
      return bound.value_or(ResultUseAccess::Unknown);
    }
    const ClassInfo *owner = classInfo(type);
    if (step.kind == ResultUseStep::Kind::Field) {
      const auto found = owner == nullptr ? decltype(owner->members.end()){}
                                          : owner->members.find(step.name);
      if (owner == nullptr || found == owner->members.end() ||
          found->second.symbol.type == SemanticType::Function ||
          found->second.symbol.staticMember)
        return ResultUseAccess::Unknown;
      type = substituteSymbol(found->second.symbol, type).type;
      continue;
    }
    if (owner == nullptr) {
      if (step.kind == ResultUseStep::Kind::Member)
        return ResultUseAccess::Unknown;
      // A built-in operator reads its operands, including a raw pointer it
      // dereferences or indexes. A fixed array's element is part of the
      // array, so the element's use is the array's.
      if (step.operation == OverloadedOperator::Subscript &&
          type.kind == SemanticType::Array && type.arguments.size() == 1) {
        SemanticType element = type.arguments.front();
        type = std::move(element);
        continue;
      }
      return ResultUseAccess::ReadOnly;
    }
    const auto found = owner->members.find(
        step.kind == ResultUseStep::Kind::Member
            ? step.name
            : std::string(operatorFunctionName(step.operation)));
    if (found == owner->members.end() ||
        found->second.symbol.type != SemanticType::Function)
      return ResultUseAccess::Unknown;
    std::vector<CallableCandidate> candidates =
        substituteSymbol(found->second.symbol, type).overloads;
    std::erase_if(candidates, [](const CallableCandidate &candidate) {
      return candidate.staticMember;
    });
    if (candidates.empty())
      return ResultUseAccess::Unknown;
    const auto has = [&](ReceiverMutability mutability) {
      return std::any_of(candidates.begin(), candidates.end(),
                         [&](const CallableCandidate &candidate) {
                           return candidate.receiverMutability == mutability;
                         });
    };
    if (has(ReceiverMutability::Consuming))
      return ResultUseAccess::Unknown;
    // An exclusive borrow carrier, such as a mutex guard, may hold mutable
    // capability over the receiver's state through a read-only member, so
    // its use is not classified.
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](const CallableCandidate &candidate) {
                      if (candidate.returnType.kind == SemanticType::Reference)
                        return false;
                      const SemanticTypeTraits traits =
                          typeTraits(candidate.returnType);
                      return traits.containsBorrowedState && !traits.copyable;
                    }))
      return ResultUseAccess::Unknown;
    if (!has(ReceiverMutability::Mutable))
      return ResultUseAccess::ReadOnly;
    if (!has(ReceiverMutability::ReadOnly))
      return ResultUseAccess::Mutable;
    // An accessor pair: its own selection follows the outer use, so the
    // walk continues through the referent both forms return.
    const SemanticType referent =
        callExpressionType(candidates.front().returnType);
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](const CallableCandidate &candidate) {
                      return candidate.returnType.kind !=
                                 SemanticType::Reference ||
                             callExpressionType(candidate.returnType) !=
                                 referent;
                    }))
      return ResultUseAccess::Unknown;
    type = referent;
  }
  return use.top;
}

auto SemanticVisitor::Impl::readOnlyAccessorOperator(
    const std::optional<ResultUse> &use, const SemanticType &receiver,
    OverloadedOperator operation) const -> bool {
  const ClassInfo *owner = use ? classInfo(receiver) : nullptr;
  if (owner == nullptr)
    return false;
  const auto found =
      owner->members.find(std::string(operatorFunctionName(operation)));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function)
    return false;
  const std::vector<CallableCandidate> candidates =
      substituteSymbol(found->second.symbol, receiver).overloads;
  const auto form = [](const CallableCandidate &candidate,
                       ReceiverMutability receiverMutability,
                       AccessMode access) {
    return !candidate.staticMember &&
           candidate.receiverMutability == receiverMutability &&
           candidate.returnType.kind == SemanticType::Reference &&
           candidate.returnType.referenceAccess == access;
  };
  const auto readOnly = std::find_if(
      candidates.begin(), candidates.end(), [&](const auto &candidate) {
        return form(candidate, ReceiverMutability::ReadOnly,
                    AccessMode::ReadOnly);
      });
  return readOnly != candidates.end() &&
         std::any_of(candidates.begin(), candidates.end(),
                     [&](const CallableCandidate &candidate) {
                       return form(candidate, ReceiverMutability::Mutable,
                                   AccessMode::Mutable) &&
                              callExpressionType(candidate.returnType) ==
                                  callExpressionType(readOnly->returnType);
                     }) &&
         accessorResultUse(*use, readOnly->returnType) ==
             ResultUseAccess::ReadOnly;
}

auto SemanticVisitor::Impl::argumentResultUses(
    const Call &expr, const SemanticType &calleeType,
    const std::optional<Symbol> &callee) const
    -> std::vector<std::optional<ResultUseStep>> {
  // Explicit type arguments can make a written `T` parameter a reference.
  if (!expr.typeArguments().empty())
    return {};
  struct Signature {
    std::span<const SemanticType> parameters;
    std::size_t required = 0;
    bool pack = false;
  };
  std::vector<Signature> signatures;
  if (calleeType.kind == SemanticType::Lambda) {
    const auto parameters = calleeType.lambdaParameterTypes();
    signatures.push_back({parameters, parameters.size()});
  } else if (calleeType.kind == SemanticType::FunctionItem) {
    const auto parameters = calleeType.functionItemParameterTypes();
    signatures.push_back({parameters, parameters.size()});
  } else if (calleeType.kind == SemanticType::NativeFunction) {
    const auto parameters = calleeType.nativeFunctionParameterTypes();
    signatures.push_back({parameters, parameters.size()});
  } else if (callee) {
    for (const CallableCandidate &candidate : callee->overloads)
      signatures.push_back({candidate.parameterTypes,
                            candidate.requiredParameterCount,
                            candidate.parameterPack});
  }
  const std::size_t count = expr.arguments().size();
  std::erase_if(signatures, [&](const Signature &signature) {
    return count < signature.required ||
           (count > signature.parameters.size() &&
            !(signature.pack && !signature.parameters.empty()));
  });
  if (signatures.empty())
    return {};
  std::vector<std::optional<ResultUseStep>> uses(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::vector<SemanticType> targets;
    for (const Signature &signature : signatures)
      targets.push_back(signature.pack &&
                                index + 1 >= signature.parameters.size()
                            ? signature.parameters.back()
                            : signature.parameters[index]);
    uses[index] = bindResultUseStep(std::move(targets));
  }
  return uses;
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
  // A nominal generic class supplies a declaration-owned member set. Ordinary
  // substitution exposes it with exact types even when its payload or the
  // required signature names enclosing parameters, so an unproved open
  // relation on a class receiver is inspected below like a concrete one. A
  // bare parameter still needs an explicit enclosing requirement; do not
  // infer members on arbitrary unknown types.
  if (containsOpenTypeParameter(receiver) ||
      containsOpenTypeParameter(result) ||
      std::any_of(parameters.begin(), parameters.end(),
                  containsOpenTypeParameter)) {
    for (const auto &requirement : activeStructuralRequirements()) {
      if (requirement.kind == StructuralConstraintKind::ReadMember &&
          requirement.memberName == member && !requirement.borrowSource &&
          requirement.mutableReceiver == mutableReceiver &&
          (requiresUnsafe || !requirement.requiresUnsafe) &&
          requirement.arguments.size() == parameters.size() + 2 &&
          requirement.arguments[0] == receiver &&
          requirement.arguments[1] == result &&
          std::equal(parameters.begin(), parameters.end(),
                     requirement.arguments.begin() + 2))
        return true;
    }
    if (receiver.kind != SemanticType::Class)
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
  for (const ExpandedStructuralRequirement &requirement :
       activeStructuralRequirements()) {
    if (requirement.arguments.empty() ||
        requirement.arguments.front() != rangeType) {
      continue;
    }
    switch (requirement.kind) {
    case StructuralConstraintKind::Iteration:
      readable = !requirement.mutableReceiver || readable;
      writable = requirement.mutableReceiver || writable;
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
  if (member == "end" && (readable || writable)) {
    return symbolicRangeCarrierType(rangeType, SymbolicRangeCarrier::Sentinel);
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
  // An open relation is first proved by an active requirement with the same
  // identity. Failing that, a bare subject has no declaration to inspect, and
  // an unexpanded pack has no exact arity. A dependent compound subject, such
  // as a class instance or lambda whose signature names enclosing parameters,
  // keeps its declaration: the checks below inspect it through ordinary
  // substitution and exact type equality, as for a concrete instance. A bare
  // argument that itself needs member lookup has no declaration and fails
  // closed, and concrete reanalysis of every instance remains the final check.
  const auto bareParameter = [](const SemanticType &type) {
    return type.kind == SemanticType::TypeParameter ||
           type.kind == SemanticType::TypePack;
  };
  if (std::any_of(arguments.begin(), arguments.end(), bareParameter)) {
    if (structuralRequirementAvailable(kind, arguments)) {
      return true;
    }
    if (bareParameter(arguments.front()) ||
        std::any_of(arguments.begin(), arguments.end(),
                    [](const SemanticType &type) {
                      return type.kind == SemanticType::TypePack;
                    })) {
      return false;
    }
  }

  switch (kind) {
  case StructuralConstraintKind::EnumUnderlying:
    return arguments.size() == 2 && arguments[0].kind == SemanticType::Enum &&
           satisfiesAtomicConstraint(arguments[0],
                                     GenericConstraintKind::IntegralEnum) &&
           enums[arguments[0].enumId - 1].underlyingType == arguments[1];
  case StructuralConstraintKind::None:
    return true;
  case StructuralConstraintKind::ReadMember:
  case StructuralConstraintKind::Iteration:
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
    case StructuralConstraintKind::Iteration: {
      if (!rangeCarrier || requirement.arguments.empty() ||
          requirement.arguments.front() != rangeCarrier->first) {
        break;
      }
      const bool mutableRange = requirement.mutableReceiver;
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
    case StructuralConstraintKind::EnumUnderlying:
      // Enum representation classification grants no numeric operators.
      break;

    case StructuralConstraintKind::ReadMember:
      if (requirement.memberName == operatorFunctionName(kind) &&
          requirement.arguments.size() == argumentTypes.size() + 2 &&
          requirement.arguments[0] == receiverType) {
        bool same = true;
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          SemanticType parameter = requirement.arguments[index + 2];
          if (parameter.kind == SemanticType::Reference &&
              parameter.arguments.size() == 1) {
            SemanticType value = parameter.arguments.front();
            parameter = std::move(value);
          }
          same = same && parameter == argumentTypes[index];
        }
        if (same) {
          candidate.returnType = requirement.arguments[1];
          candidate.parameterTypes.assign(requirement.arguments.begin() + 2,
                                          requirement.arguments.end());
          candidate.receiverMutability = requirement.mutableReceiver
                                             ? ReceiverMutability::Mutable
                                             : ReceiverMutability::ReadOnly;
          candidate.requiresUnsafeFact = requirement.requiresUnsafe;
          if (requirement.borrowSource) {
            candidate.returnBorrowOrigin = *requirement.borrowSource == 0
                                               ? BorrowOriginKind::Receiver
                                               : BorrowOriginKind::Argument;
            candidate.returnBorrowParameter =
                *requirement.borrowSource == 0 ? 0
                                               : *requirement.borrowSource - 2;
            candidate.returnBorrowAccess = candidate.returnType.referenceAccess;
          }
          matches = true;
        }
      }
      break;
    case StructuralConstraintKind::None:
      break;
    }
    if (matches) {
      candidate.access = AccessModifier::Public;
      result.emplace_back(std::move(candidate));
    }
  }

  // An exact-referent dereference or subscript requirement refines the
  // unknown-referent form from
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
                 existing.receiverMutability == candidate.receiverMutability &&
                 existing.returnBorrowOrigin == candidate.returnBorrowOrigin &&
                 existing.returnBorrowParameter ==
                     candidate.returnBorrowParameter &&
                 existing.returnBorrowAccess == candidate.returnBorrowAccess &&
                 existing.requiresUnsafeFact == candidate.requiresUnsafeFact;
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
  if (receiverType.kind == SemanticType::TypeParameter &&
      argumentTypes.size() == 1 && argumentTypes.front() == receiverType) {
    const bool equality = kind == OverloadedOperator::Equal ||
                          kind == OverloadedOperator::NotEqual;
    const bool ordering = kind == OverloadedOperator::Less ||
                          kind == OverloadedOperator::LessEqual ||
                          kind == OverloadedOperator::Greater ||
                          kind == OverloadedOperator::GreaterEqual;
    // A primitive comparison remains a value operation. A source query using
    // reference bindings must not turn it into a hypothetical member call,
    // which would incorrectly reject temporary operands or invent loans.
    if ((equality || ordering) && isNumeric(receiverType))
      return false;
    if (equality && constraintImplies(constraintOf(receiverType),
                                      GenericConstraintKind::AtomicScalar))
      return false;
  }
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
  if (selected.requiresUnsafeFact && unsafeDepth == 0) {
    report(token, "The required operator needs an unsafe block.", "GTI-S2029");
    return std::nullopt;
  }
  const AccessMode receiverAccess =
      receiverAllowsMutation(selected.receiverMutability)
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  const SemanticLoanPlace receiverPlace =
      receiverState ? loanPlace(receiverState->place) : loanPlace(receiver);
  const auto receiverOrigins =
      receiverState ? loanPlaces(receiverState->place) : loanPlaces(receiver);
  reportLoanPlaceConflicts(
      token, receiverOrigins, receiverAccess,
      selected.receiverMutability == ReceiverMutability::Mutable
          ? "Mutable constrained operator receiver overlaps a borrow that "
            "may still be live."
          : "Read-only constrained operator receiver overlaps a mutable "
            "borrow that may still be live.",
      receiverState ? accessLoan(receiverState->place) : accessLoan(receiver));
  validateMutableCallArgumentLoans(selected.parameterTypes, arguments, token);
  validateCallPlaceExclusivity(selected.parameterTypes, arguments, token,
                               receiverPlace, receiverAccess, false,
                               receiverOrigins);
  if (receiverAccess == AccessMode::Mutable) {
    for (const auto &origin : receiverOrigins)
      recordMutableBindingUse(origin.root);
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
    auto check = captureProgramStorageBorrows(tokenSpan(token));
    const AccessMode access =
        resolved.receiverMutability == ReceiverMutability::ReadOnly
            ? AccessMode::ReadOnly
            : AccessMode::Mutable;
    if (receiver)
      appendProgramStorageBorrowInput(check, receiver, access);
    else if (receiverState)
      for (const auto &place : loanPlaces(receiverState->place))
        appendProgramStorageBorrowProtection(check, place, access,
                                             tokenSpan(token));
    const auto owner = currentProgramEffectOwner;
    const auto edgeStart = programEffects[owner].edges.size();
    const auto unknownStart = programEffects[owner].unknown.size();
    recordProgramOperatorTargetEffects(site, resolved);
    if (owner.valid())
      publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                       unknownStart);
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
    bool preferReadOnlyReceiver, ContextualCallArguments *contextualArguments,
    std::optional<SemanticType> conversionTarget, bool implicitOnly)
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
  // A conversion's diagnostics name its target; other operators name their
  // spelling.
  const std::string subject =
      conversionTarget
          ? "Conversion to '" + typeSpelling(*conversionTarget) + "'"
          : std::string(operatorSourceSpelling(kind));
  const std::string object =
      conversionTarget
          ? "the conversion to '" + typeSpelling(*conversionTarget) + "'"
          : std::string(operatorSourceSpelling(kind));
  const bool consumingReceiver = consumingReceiverValue(receiver) &&
                                 (kind == OverloadedOperator::Call ||
                                  kind == OverloadedOperator::Conversion ||
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
    // A conversion selects exactly its requested target; a declaration
    // considers only conversions declared implicit.
    if ((conversionTarget && candidate.returnType != *conversionTarget) ||
        (implicitOnly && (candidate.declaration == nullptr ||
                          !candidate.declaration->implicitKeyword()))) {
      continue;
    }
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
      reportCompilerPrivateAccess(token, object);
    } else if (viable.empty() && rejectedConsumingReceiver) {
      const auto *call =
          kind == OverloadedOperator::Call ? exprAs<Call>(&site) : nullptr;
      if (call != nullptr && boundary) {
        (void)validateCallableCapability(*call,
                                         CallableInvocationCapability::Once);
      } else {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2022", DiagnosticPhase::Semantics, token,
            std::string(conversionTarget ? "A consuming conversion operator"
                                         : "A consuming operator") +
                " requires an owned temporary or an explicitly moved "
                "receiver.");
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
                                     subject);
      } else {
        report(token, subject + " requires a mutable receiver.", "GTI-S2022");
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
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2022", DiagnosticPhase::Semantics, token,
                         "No exact overload of " + object + received + ".");
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
      report(token, "Use of " + object + " is ambiguous.", "GTI-S2022");
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
  if (!canAccessMember(selected.access, selected.ownerClass, receiverType,
                       selected.dispatchOwner)) {
    const std::string access(accessModifierSpelling(selected.access));
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, token,
        conversionTarget
            ? "The conversion of '" + owner->name.lexeme + "' to '" +
                  typeSpelling(*conversionTarget) + "' is " + access + "."
            : std::string(operatorSourceSpelling(kind)) + " of '" +
                  owner->name.lexeme + "' is " + access + ".");
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
  const auto receiverOrigins = receiverState ? loanPlaces(receiverState->place)
                               : !temporaryReceiver
                                   ? loanPlaces(receiver, true)
                                   : std::vector<SemanticLoanPlace>{};
  if (receiverPlace) {
    reportLoanPlaceConflicts(
        token, receiverOrigins, selectedReceiverAccess,
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
  validateCallPlaceExclusivityImpl(
      ordinaryParameters, ordinaryArguments, token, receiverPlace,
      callReceiverAccess, receiverProduction.has_value(), receiverOrigins);
  if (receiverPlace && callReceiverAccess == AccessMode::Mutable) {
    for (const auto &origin : receiverOrigins)
      recordMutableBindingUse(origin.root);
  }

  // A generic-result operator with an explicit borrowed_from contract
  // resolves its origin per substitution, exactly as its instance records.
  BorrowOriginKind operatorBorrowOrigin = selected.returnBorrowOrigin;
  std::size_t operatorBorrowParameter = selected.returnBorrowParameter;
  AccessMode operatorBorrowAccess = selected.returnBorrowAccess;
  if (selected.declaration != nullptr &&
      selected.returnBorrowOrigin == BorrowOriginKind::None &&
      selected.returnType.kind == SemanticType::Reference &&
      selected.declaration->hasFunctionAttribute("borrowed_from")) {
    const GenericReturnBorrowContract contract = genericReturnBorrowContract(
        *selected.declaration, selected.returnType, selected.parameterTypes,
        selected.ownerClass, selected.staticMember);
    operatorBorrowOrigin = contract.origin;
    operatorBorrowParameter = contract.parameter;
    operatorBorrowAccess = contract.access;
  }
  // A generic operator whose result this call's types make text relays as
  // its instance's own analysis derives, as an ordinary method call does
  // (L-OWN-02).
  std::vector<std::size_t> operatorBorrowParameters =
      selected.returnBorrowParameters;
  std::vector<BorrowedFieldOrigin> operatorBorrowFields =
      selected.returnBorrowFields;
  if (operatorBorrowOrigin == BorrowOriginKind::None &&
      (selected.returnType.kind == SemanticType::StringView ||
       selected.returnType.kind == SemanticType::CString)) {
    const FunctionDemandScope pauseDemand(semanticModel, nullptr);
    const FunctionInfo *declaration = semanticModel.findFunction(selected.id);
    if (declaration == nullptr && instanceBaseModel != nullptr)
      declaration = instanceBaseModel->findFunction(selected.id);
    if (declaration != nullptr && !declaration->explicitReturnBorrowOrigin &&
        declaration->returnType != selected.returnType) {
      FunctionInfo concrete = *declaration;
      concrete.returnType = selected.returnType;
      concrete.parameterTypes = selected.parameterTypes;
      if (const auto text = dynamicTextReturnBorrowSummary(
              concrete, selected.dispatchOwner.kind == SemanticType::Class
                            ? selected.dispatchOwner
                            : receiverType)) {
        operatorBorrowOrigin = text->origin;
        operatorBorrowParameter = text->parameter;
        operatorBorrowAccess = text->access;
        if (!text->fields.empty()) {
          operatorBorrowParameters = text->parameters;
          operatorBorrowFields = text->fields;
        }
      }
    }
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
      .borrowOrigin = operatorBorrowOrigin,
      .borrowArgument = operatorBorrowParameter,
      .borrowAccess = operatorBorrowAccess,
      .borrowArguments = std::move(operatorBorrowParameters),
      .borrowFields = std::move(operatorBorrowFields),
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
  // The operator's reference relay for this receiver's class, and its
  // receiver's and operands' relays (L-OWN-02).
  if (selected.returnType.kind == SemanticType::Reference) {
    const FunctionDemandScope pauseDemand(semanticModel, nullptr);
    const FunctionInfo *declaration = semanticModel.findFunction(selected.id);
    if (declaration == nullptr && instanceBaseModel != nullptr)
      declaration = instanceBaseModel->findFunction(selected.id);
    if (declaration != nullptr) {
      FunctionInfo concrete = *declaration;
      concrete.returnType = selected.returnType;
      concrete.parameterTypes = selected.parameterTypes;
      resolved.returnedTextReference =
          textReferenceReturnSummary(concrete, receiverType);
    }
  }
  if (!receiverState && receiver)
    recordTextFieldOperator(receiver, arguments, selected);
  if (contextual) {
    // The site's own analysis recorded the effects of computing its value;
    // the conversion applied to that value is a further call.
    recordProgramOperatorTargetEffects(site, resolved);
    semanticModel.recordContextualConversion(site, std::move(resolved));
  } else if (projection) {
    semanticModel.recordProjectionOperator(site, std::move(resolved));
  } else {
    semanticModel.recordOperator(site, std::move(resolved));
  }
  return selected;
}

auto SemanticVisitor::Impl::conversionOperatorCandidates(
    const SemanticType &source, const SemanticType &target, bool implicitOnly)
    -> std::vector<CallableCandidate> {
  std::vector<CallableCandidate> candidates;
  if (source.kind != SemanticType::Class || target == SemanticType::Unknown ||
      source == target) {
    return candidates;
  }
  const ClassInfo *owner = classInfo(source);
  if (owner == nullptr) {
    return candidates;
  }
  const auto found = owner->members.find(
      std::string(operatorFunctionName(conversionOperatorKind(target))));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function) {
    return candidates;
  }
  // Every declared conversion to exactly this target participates, whatever
  // access or receiver it then requires: a rejected candidate never lets the
  // conversion fall back to another path.
  for (const CallableCandidate &candidate :
       substituteSymbol(found->second.symbol, source).overloads) {
    if (candidate.returnType == target &&
        (!implicitOnly || (candidate.declaration != nullptr &&
                           candidate.declaration->implicitKeyword()))) {
      candidates.push_back(candidate);
    }
  }
  return candidates;
}

auto SemanticVisitor::Impl::conversionConstructorCandidates(
    const Expr &site, const ExprPtr &argument, const SemanticType &argumentType,
    const SemanticType &target, const Token &token, bool implicitOnly)
    -> std::vector<const ConstructorInfo *> {
  std::vector<const ConstructorInfo *> candidates;
  if (target.kind != SemanticType::Class || target.classId == 0 ||
      target.classId > classes.size()) {
    return candidates;
  }
  const SemanticType enclosingType = currentType;
  analyzeConstructorCallArguments(
      site, target.classId, target.arguments, target.valueArguments,
      {{.type = argumentType, .expression = &argument}}, token, false,
      implicitOnly, &candidates);
  currentType = enclosingType;
  return candidates;
}

auto SemanticVisitor::Impl::reportAmbiguousConversion(
    const Token &token, const SemanticType &source, const SemanticType &target,
    std::span<const CallableCandidate> operators,
    std::span<const ConstructorInfo *const> constructors, bool declaration)
    -> void {
  const std::string sourceName = typeSpelling(source);
  const std::string targetName = typeSpelling(target);
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2122", DiagnosticPhase::Semantics, token,
      declaration
          ? "Initialization of '" + targetName + "' from '" + sourceName +
                "' is ambiguous: an implicit constructor of '" + targetName +
                "' and an implicit conversion operator of '" + sourceName +
                "' both apply."
          : "Conversion of '" + sourceName + "' to '" + targetName +
                "' is ambiguous: a constructor of '" + targetName +
                "' and a conversion operator of '" + sourceName +
                "' both apply.");
  for (const CallableCandidate &candidate : operators) {
    if (candidate.declaration != nullptr &&
        candidate.declaration->operatorName()) {
      diagnostic.related.push_back(
          {tokenSpan(candidate.declaration->operatorName()->keyword),
           "Conversion operator declared here."});
    }
  }
  for (const ConstructorInfo *constructor : constructors) {
    if (constructor != nullptr && constructor->declaration != nullptr) {
      diagnostic.related.push_back({tokenSpan(constructor->declaration->name()),
                                    "Constructor declared here."});
    }
  }
  diagnostic.hints.emplace_back(
      "GTI does not rank a constructor against a conversion operator. Brace "
      "construction '" +
      targetName +
      "{...}' always selects a constructor; remove one of the conversions or "
      "give one a named method to choose the other.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::reportMissingConversionOperator(
    const Token &token, const SemanticType &source, const SemanticType &target)
    -> void {
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2014", DiagnosticPhase::Semantics, token,
                     "Type '" + typeSpelling(source) +
                         "' declares no conversion operator to '" +
                         typeSpelling(target) + "'.");
  diagnostic.hints.emplace_back(
      target == SemanticType::Bool
          ? "Declare 'operator bool()' in '" + typeSpelling(source) +
                "' to define this conversion."
          : "Declare 'operator " + typeSpelling(target) + "()' in '" +
                typeSpelling(source) + "' to define this conversion.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::selectConversionOperator(
    const Expr &site, const ExprPtr &source, const SemanticType &sourceType,
    const SemanticType &target, const Token &token, bool declaration)
    -> std::optional<CallableCandidate> {
  // Declarations already reject these targets. A generic instance can still
  // reach one through a substituted target.
  const std::vector<CallableCandidate> candidates =
      conversionOperatorCandidates(sourceType, target, declaration);
  if (conversionTargetCarriesBorrow(target) ||
      std::any_of(candidates.begin(), candidates.end(),
                  [](const CallableCandidate &candidate) {
                    return candidate.returnBorrowOrigin !=
                           BorrowOriginKind::None;
                  })) {
    report(token,
           "A conversion operator cannot produce borrowed state yet; '" +
               typeSpelling(target) +
               "' carries a borrow. Call a named method returning the view.",
           "GTI-S2122");
    return std::nullopt;
  }
  return resolveOperator(site, conversionOperatorKind(target), source,
                         sourceType, token, {}, {}, declaration, std::nullopt,
                         std::nullopt, false, false, nullptr, target,
                         declaration);
}

auto SemanticVisitor::Impl::analyzeExplicitConversion(
    const Expr &site, const ExprPtr &source, const SemanticType &sourceType,
    const SemanticType &target, const Token &token) -> bool {
  if (!source) {
    return false;
  }
  const std::vector<CallableCandidate> operators =
      conversionOperatorCandidates(sourceType, target, false);
  if (operators.empty()) {
    return false;
  }
  // One explicit conversion has exactly one path: a constructor of the target
  // that takes the source, or a conversion operator of the source.
  if (const std::vector<const ConstructorInfo *> constructors =
          conversionConstructorCandidates(site, source, sourceType, target,
                                          token, false);
      !constructors.empty()) {
    reportAmbiguousConversion(token, sourceType, target, operators,
                              constructors, false);
  } else {
    (void)selectConversionOperator(site, source, sourceType, target, token,
                                   false);
  }
  currentType = target;
  return true;
}

auto SemanticVisitor::Impl::arrowTargetType(const Expr &site,
                                            const ExprPtr &receiver,
                                            const SemanticType &receiverType,
                                            const Token &token, bool projection,
                                            bool preferReadOnlyStep)
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
    // A step whose result is only read selects a read-only and mutable
    // `operator->` pair's read-only form, as an accessor call does.
    const std::optional<CallableCandidate> selected = resolveOperator(
        site, OverloadedOperator::Arrow, receiver, receiverType, token, {}, {},
        false, std::nullopt, std::nullopt, projection, preferReadOnlyStep);
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
    if (isCheckedReferenceRelay(*targetInfo, index))
      continue;
    if (index >= targetInfo->bodyDeclaration()->parameters().size() ||
        callableType(targetInfo->parameterTypes[index]).kind !=
            SemanticType::TypeParameter) {
      continue;
    }
    const Parameter &targetParameter =
        targetInfo->bodyDeclaration()->parameters()[index];
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
        std::find_if(sourceInfo->bodyDeclaration()->parameters().begin(),
                     sourceInfo->bodyDeclaration()->parameters().end(),
                     [&](const Parameter &candidate) {
                       return &candidate == symbol->parameterDeclaration;
                     });
    if (sourceParameter == sourceInfo->bodyDeclaration()->parameters().end()) {
      continue;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(std::distance(
        sourceInfo->bodyDeclaration()->parameters().begin(), sourceParameter));
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
  // A statement template has a source-level selection for editor queries,
  // not an executable call/parameter boundary. Its expanded calls own effects.
  const FunctionInfo *target = semanticModel.findFunction(resolved.function);
  if (target == nullptr || !target->macroExpansion)
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
  if (!instanceAnalysisActive && function.declaration != nullptr &&
      (function.record().intrinsic == IntrinsicKind::AtomicWait ||
       function.record().intrinsic == IntrinsicKind::AtomicNotifyOne ||
       function.record().intrinsic == IntrinsicKind::AtomicNotifyAll)) {
    requestSession.cThreadCallUses.push_back(
        {.caller = currentFunctionDeclaration,
         .callee = function.declaration,
         .location = call.paren(),
         .trusted = isCompilerTrustedRequester(call.paren())});
  }
  semanticModel.record(call, resolved);
  recordSelectedCallOccurrence(call, std::move(resolved));
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
      declaration == nullptr || function.bodyDeclaration()->body() != nullptr ||
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

auto SemanticVisitor::Impl::recordSelectedCallSafety(
    const Call &call, const CallableCandidate &function) -> void {
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
}

auto SemanticVisitor::Impl::recordResolvedCall(
    const Call &call, const CallableCandidate &function,
    std::vector<SemanticType> typeArguments,
    std::vector<CompileTimeValue> valueArguments,
    std::vector<std::size_t> ephemeralBorrowArguments,
    std::vector<CallableArgumentBoundary> callableArguments,
    CallReceiverMode receiverMode, const ExprPtr *argumentReceiver) -> void {
  requireFunctionDefinition(function, expressionToken(call.callee()));
  recordSelectedCallSafety(call, function);
  recordCallableForwardings(call, function);
  recordOwnerlessTextCall(call, function);
  recordTextFieldCall(call, function);
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
  // The receiver's concrete class, which decides a generic class method's
  // text relays.
  SemanticType textRelayOwner = function.dispatchOwner;
  if (const auto *member = exprAs<Get>(call.callee().get());
      member != nullptr && member->object() && function.ownerClass != 0)
    if (const SemanticType *object = semanticModel.findType(*member->object());
        object != nullptr && object->kind == SemanticType::Class &&
        object->classId == function.ownerClass)
      textRelayOwner = *object;
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
    } else if (const auto text = dynamicTextReturnBorrowSummary(
                   *concreteDeclaration, textRelayOwner)) {
      if (!text->fields.empty()) {
        // A generic class method relays a receiver field that this call's
        // types make text (L-OWN-02).
        concreteDeclaration->returnBorrowOrigin = text->origin;
        concreteDeclaration->returnBorrowParameter = text->parameter;
        concreteDeclaration->returnBorrowAccess = text->access;
        concreteDeclaration->returnBorrowParameters = text->parameters;
        concreteDeclaration->returnBorrowFields = text->fields;
        concreteDeclaration->returnBorrowPlace = text->place;
        declarationInfo = &*concreteDeclaration;
      } else if (text->origin != BorrowOriginKind::None &&
                 !concreteDeclaration->explicitReturnBorrowOrigin) {
        // A result type this call's types make text relays an argument or
        // the receiver as the instance's own analysis derives (L-OWN-02).
        concreteDeclaration->returnBorrowOrigin = text->origin;
        concreteDeclaration->returnBorrowParameter = text->parameter;
        concreteDeclaration->returnBorrowAccess = text->access;
        concreteDeclaration->returnBorrowPlace = text->place;
        declarationInfo = &*concreteDeclaration;
      }
    }
    concreteDeclaration->returnedTextReference =
        textReferenceReturnSummary(*concreteDeclaration, textRelayOwner);
  }
  // Whether the callee returns a moved parameter is a body-derived fact; ask
  // for the body first so the answer is the same whatever the source order.
  if (declarationInfo != nullptr) {
    ensureBodyAnalyzed(*declarationInfo);
  }
  // Restoration authority is a checked-body fact, not part of the sealed
  // overload signature. Refresh it after an on-demand body, and materialize
  // the selected concrete referent's field dependencies at this call.
  const FunctionInfo *restoreDeclaration =
      semanticModel.findFunction(function.id);
  if (restoreDeclaration == nullptr && instanceBaseModel != nullptr)
    restoreDeclaration = instanceBaseModel->findFunction(function.id);
  bool restoresReferenceResult = false;
  std::vector<RestoredBorrowField> restoredFields;
  if (!function.virtualMethod && restoreDeclaration != nullptr &&
      restoreDeclaration->restoresReferenceResult &&
      function.returnType.kind == SemanticType::Reference &&
      function.returnType.arguments.size() == 1) {
    if (auto fields =
            restoredReferenceFields(function.returnType.arguments.front())) {
      restoresReferenceResult = true;
      restoredFields = std::move(*fields);
    }
  }
  FunctionReturnBorrowSummary concreteBorrow = functionReturnBorrowSummary(
      function.returnType, function.parameterTypes, function.ownerClass,
      function.staticMember, function.record().intrinsic,
      declarationInfo == nullptr ? std::nullopt
                                 : exactMovedReturnParameter(*declarationInfo));
  // A generic-result declaration with an explicit borrowed_from contract
  // decides its origin per substitution; the selected instance's contract
  // binds this call exactly as its own reanalysis records it.
  bool genericContract = false;
  if (declarationInfo != nullptr && declarationInfo->declaration != nullptr &&
      declarationInfo->bodyDeclaration() != nullptr &&
      containsOpenTypeParameter(declarationInfo->returnType) &&
      declarationInfo->bodyDeclaration()->hasFunctionAttribute(
          "borrowed_from")) {
    const GenericReturnBorrowContract contract = genericReturnBorrowContract(
        *declarationInfo->bodyDeclaration(), function.returnType,
        function.parameterTypes, function.ownerClass, function.staticMember);
    concreteBorrow = {};
    concreteBorrow.origin = contract.origin;
    concreteBorrow.parameter = contract.parameter;
    concreteBorrow.access = contract.access;
    genericContract = true;
  }
  const bool declarationOwnsBorrow =
      !genericContract && declarationInfo != nullptr &&
      declarationInfo->returnBorrowOrigin != BorrowOriginKind::None;
  const bool candidateOwnsBorrow =
      !genericContract && function.returnBorrowOrigin != BorrowOriginKind::None;
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
  if (declarationInfo != nullptr) {
    // A concrete lambda may have received a provisional confined-callable
    // descriptor before selection. A checked reference relay does not invoke
    // or own its referent; retain its ordinary borrow contract, not a callable
    // permission absent from the selected function's body contract.
    std::erase_if(callableArguments,
                  [&](const CallableArgumentBoundary &argument) {
                    return isCheckedReferenceRelay(*declarationInfo,
                                                   argument.parameterIndex) &&
                           callableParameterContract(
                               function.id, argument.parameterIndex) == nullptr;
                  });
  }
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
      .restoresReferenceResult = restoresReferenceResult,
      .restoredBorrowFields = std::move(restoredFields),
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
      .returnedTextReference = concreteDeclaration
                                   ? concreteDeclaration->returnedTextReference
                                   : function.returnedTextReference,
      .dispatch =
          function.virtualMethod ? CallDispatch::Virtual : CallDispatch::Static,
      .dispatchOwner = function.dispatchOwner,
      .receiverMode = receiverMode,
      .ephemeralBorrowArguments = std::move(ephemeralBorrowArguments),
      .callableArguments = std::move(callableArguments),
      .argumentReceiver = argumentReceiver};
  semanticModel.record(call, resolved);
  // A callee that returns an argument in a text field of its fresh result
  // publishes that argument's owner at this call (L-OWN-02). Its relays are
  // body facts, read after the body demand above.
  if (restoreDeclaration != nullptr && !function.virtualMethod) {
    publishTextCallResult(call, resolved, *restoreDeclaration);
    publishTextReturnResult(call, resolved, *restoreDeclaration);
  }
  // Complete dependency facts are body-derived, not overload properties.
  // Read the exact declaration after the order-independent body demand above;
  // never reuse an open primary's facts for a different concrete instance.
  if (function.returnType.kind == SemanticType::Enum ||
      (function.returnType.kind == SemanticType::Class &&
       typeTraits(function.returnType).containsBorrowedState) ||
      conditionalReferenceReturn(function.returnType, function.parameterTypes,
                                 function.dispatchOwner)) {
    const auto complete = completeFunctionReturnBorrow(
        function, resolved.typeArguments, resolved.valueArguments,
        tokenSpan(expressionToken(call.callee())));
    bool noNormalResult = false;
    const auto instantiated =
        complete ? instantiateCompleteCallBorrow(call, resolved, *complete,
                                                 &noNormalResult)
                 : std::nullopt;
    // The exact instance's complete contract names the result's fields
    // whether or not this call's inputs instantiate its dependency set.
    const bool contractFields =
        complete && (function.returnType.kind == SemanticType::Class ||
                     (function.returnType.kind == SemanticType::Reference &&
                      function.dispatchOwner.kind == SemanticType::Class));
    // A reference selected from a receiver's alternative-guarded stored path
    // is relayed only from a named or fresh receiver. Reaching that carrier
    // through a field, element or dereference of another value needs a
    // composed receiver-prefix proof that verified MIR does not yet carry.
    if (instantiated && function.returnType.kind == SemanticType::Reference &&
        std::any_of(complete->dependencies.begin(),
                    complete->dependencies.end(),
                    [](const BorrowedFieldOrigin &mapping) {
                      return mapping.origin == BorrowOriginKind::Receiver &&
                             !mapping.sourceConditions.empty();
                    }))
      if (const auto *member = exprAs<Get>(call.callee().get())) {
        const Expr *receiver = member->object().get();
        while (const auto *grouping = exprAs<Grouping>(receiver))
          receiver = grouping->expression().get();
        const auto *dereference = exprAs<Unary>(receiver);
        if (exprAs<Get>(receiver) || exprAs<Index>(receiver) ||
            (dereference && dereference->oper().kind == TokenKind::STAR))
          report(member->name(),
                 "A reference selected from a conditional borrowed carrier "
                 "cannot yet be returned through a field, element or "
                 "dereference of another value; call it on a named local, "
                 "parameter or temporary carrier.",
                 "GTI-S2045");
      }
    if (instantiated) {
      if (contractFields)
        resolved.borrowFields = complete->dependencies;
      // A result whose contract names only static storage has no caller
      // input to borrow from: it borrows that exact global place.
      if (resolved.borrowOrigin == BorrowOriginKind::None)
        if (const auto global = std::find_if(
                complete->dependencies.begin(), complete->dependencies.end(),
                [](const BorrowedFieldOrigin &mapping) {
                  return mapping.origin == BorrowOriginKind::Global &&
                         mapping.globalRoot != 0;
                });
            global != complete->dependencies.end()) {
          BorrowOriginPlace place{.root = global->globalRoot};
          for (const auto field : global->sourceFields)
            place.projections.push_back(
                {.kind = PlaceProjectionKind::Field, .field = field});
          resolved.borrowOrigin = BorrowOriginKind::Global;
          resolved.borrowAccess = global->access;
          resolved.borrowPlace = std::move(place);
        }
      semanticModel.record(call, resolved);
      semanticModel.recordBorrowDependencySet(call, *instantiated);
    } else if (noNormalResult &&
               function.returnType.kind == SemanticType::Reference) {
      // The exact input alternative domain excludes every normal reference
      // result. Keep this call-site fact distinct from declaration noreturn;
      // no referent or empty dependency set is manufactured for its result.
      resolved.noNormalBorrowResult = true;
      resolved.neverReturns = true;
      resolved.borrowFields = complete->dependencies;
      semanticModel.record(call, resolved);
      semanticModel.clearBorrowDependencySet(call);
    } else {
      if (contractFields) {
        resolved.borrowFields = complete->dependencies;
        semanticModel.record(call, resolved);
      }
      semanticModel.clearBorrowDependencySet(call);
    }
  }
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
