#include "semantic_analyzer_impl.h"

namespace lang {

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

auto SemanticVisitor::Impl::reportStorageArity(const Call &expr,
                                               IntrinsicKind intrinsic,
                                               std::size_t expected) -> void {
  report(expr.paren(),
         storageIntrinsicName(intrinsic) + " expects " +
             std::to_string(expected) + " argument" +
             (expected == 1 ? "." : "s."),
         "GTI-S2019");
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
  case IntrinsicKind::UniqueOwnerUpcast:
    return "unique_owner_upcast";
  default:
    return "unique-owner operation";
  }
}

auto SemanticVisitor::Impl::storageIntrinsicName(IntrinsicKind intrinsic)
    -> std::string {
  switch (intrinsic) {
  case IntrinsicKind::AllocateStorage:
    return "allocate_storage";
  case IntrinsicKind::StorageTryAllocate:
    return "storage_try_allocate";
  case IntrinsicKind::StorageCapacity:
    return "storage_capacity";
  case IntrinsicKind::StorageInitialized:
    return "storage_initialized";
  case IntrinsicKind::StorageConstruct:
    return "storage_construct";
  case IntrinsicKind::StorageRead:
    return "storage_read";
  case IntrinsicKind::StorageReadMut:
    return "storage_read_mut";
  case IntrinsicKind::StorageDestroy:
    return "storage_destroy";
  case IntrinsicKind::StorageRelocate:
    return "storage_relocate";
  case IntrinsicKind::StorageShiftRight:
    return "storage_shift_right";
  case IntrinsicKind::StorageShiftLeft:
    return "storage_shift_left";
  default:
    return "storage operation";
  }
}

auto SemanticVisitor::Impl::typeAliasForCallee(const ExprPtr &callee) const
    -> std::optional<TypeAliasId> {
  if (callee && callee->exprKind() == ExprKind::Variable) {
    const auto *variable = static_cast<const Variable *>(callee.get());
    return resolveTypeAliasPath(NamePath(variable->name()), currentNamespace);
  }
  if (callee && callee->exprKind() == ExprKind::QualifiedName) {
    const auto *qualified = static_cast<const QualifiedName *>(callee.get());
    return resolveTypeAliasPath(qualified->name(), currentNamespace);
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

auto SemanticVisitor::Impl::rejectConsumingActiveCleanup(
    const Token &location, const SemanticType &receiverType,
    std::optional<CallableBoundary> boundary) -> bool {
  if (!requiresActiveCleanupFor(receiverType)) {
    return false;
  }
  Diagnostic diagnostic = makeDiagnostic(
      boundary ? "GTI-S2046" : "GTI-S2022", DiagnosticPhase::Semantics,
      location,
      "Cannot consume callable receiver of type '" +
          typeSpelling(receiverType) +
          "' because it requires active cleanup and GTI cannot yet "
          "guarantee that cleanup at the enclosing full-expression "
          "boundary.");
  if (const std::optional<ActiveCleanupCause> cause =
          activeCleanupCause(receiverType);
      cause && cause->source != nullptr) {
    diagnostic.related.push_back({tokenSpan(*cause->source), cause->message});
  }
  diagnostic.hints.emplace_back(
      "Use a read-callable or mut-callable operation, or keep the callable "
      "receiver free of cleanup-owning state until consuming "
      "full-expression storage is supported.");
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
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
  bool valid = true;
  SemanticType returnType = SemanticType::Void;
  if (contextualCallableResult && contextualCallableResult->call == &call) {
    returnType = contextualCallableResult->type;
    if (returnType == SemanticType::Unknown) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2046", DiagnosticPhase::Semantics,
                         expressionToken(call.callee()),
                         "The result type of generic callable parameter '" +
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
          "Confined generic callable results must be exact non-reference "
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
  if (variable == nullptr || parameter == nullptr || function == nullptr ||
      function->declaration == nullptr || parameter->type.reference ||
      parameter->pack || calleeType.kind != SemanticType::TypeParameter) {
    report(call.paren(),
           "Only a direct by-value generic function parameter can be used "
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
  }
  if (!valid) {
    currentType = SemanticType::Unknown;
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
      call, DeferredCallableCallInfo{.genericParameter =
                                         calleeType.genericParameterId,
                                     .returnType = returnType,
                                     .parameterTypes = argumentTypes,
                                     .access = access,
                                     .capability = capability});
  semanticModel.recordCallableRequirement(
      *function->declaration,
      CallableParameterContract{.parameterIndex = parameterIndex,
                                .genericParameter =
                                    calleeType.genericParameterId,
                                .callableType = calleeType,
                                .access = access,
                                .signatures = {{.source = &call,
                                                .returnType = returnType,
                                                .parameterTypes = argumentTypes,
                                                .capability = capability}}});
  currentType = returnType;
}

auto SemanticVisitor::Impl::validateCallableReturn(
    const Call &call, const SemanticType &returnType) -> bool {
  const std::optional<CallableSignatureRequirement> requirement =
      callableSignatureRequirement(call);
  if (!requirement || requirement->returnType == returnType ||
      returnType == SemanticType::Unknown) {
    return true;
  }
  const Variable *variable = directCallableParameterVariable(call.callee());
  const Token location =
      variable == nullptr ? expressionToken(call.callee()) : variable->name();
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2046", DiagnosticPhase::Semantics, location,
      "Callable parameter '" + location.lexeme + "' must return '" +
          typeSpelling(requirement->returnType) +
          "' for this invocation, "
          "but the selected callable returns '" +
          typeSpelling(returnType) + "'.");
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
  if (argumentTypes.size() != lambda->parameterTypes.size()) {
    report(call.paren(),
           "Lambda expects " + std::to_string(lambda->parameterTypes.size()) +
               " argument" + (lambda->parameterTypes.size() == 1 ? "" : "s") +
               " but received " + std::to_string(argumentTypes.size()) + ".",
           "GTI-S2005");
    valid = false;
  }
  const std::size_t count =
      std::min(argumentTypes.size(), lambda->parameterTypes.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (argumentTypes[index].kind == SemanticType::Lambda) {
      report(expressionToken(call.arguments()[index]),
             "Lambda values cannot be passed to another lambda yet.",
             "GTI-S2027");
      valid = false;
      continue;
    }
    if (argumentTypes[index] != SemanticType::Unknown &&
        lambda->parameterTypes[index] != SemanticType::Unknown &&
        !callArgumentMatches(lambda->parameterTypes[index],
                             argumentTypes[index], call.arguments()[index])) {
      reportCallArgumentMismatch(index, lambda->parameterTypes[index],
                                 argumentTypes[index], call.arguments()[index],
                                 "Lambda");
      valid = false;
    }
  }
  validateMutableCallArgumentLoans(lambda->parameterTypes, call.arguments(),
                                   call.paren());
  validateCallPlaceExclusivity(lambda->parameterTypes, call.arguments(),
                               call.paren());
  valid = validateCallableReturn(call, lambda->returnType) && valid;
  valid =
      validateCallableCapability(call, CallableInvocationCapability::Read) &&
      valid;
  if (valid && isExplicitMoveReceiver(call.callee()) &&
      rejectConsumingActiveCleanup(call.paren(), calleeType, boundary)) {
    valid = false;
  }
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
    std::vector<AnalyzedCallArgument> &result) const -> bool {
  if (type.kind != SemanticType::TypePack) {
    result.push_back({.type = type, .expression = &expression});
    return true;
  }
  if (!type.concretePack) {
    return false;
  }
  for (const SemanticType &element : type.arguments) {
    if (element.kind == SemanticType::TypePack) {
      if (!appendConcreteCallArgument(element, expression, result)) {
        return false;
      }
    } else {
      result.push_back({.type = element,
                        .expression = &expression,
                        .forwardedPackElement = true});
    }
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
    return !hasPackExpansion(arguments) &&
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
      invalidConstraintSet(constraints)) {
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
  std::size_t argumentIndex = 0;
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.value) {
      continue;
    }
    const std::size_t end =
        parameter.pack ? arguments.size() : argumentIndex + 1;
    while (argumentIndex < end && argumentIndex < arguments.size()) {
      const std::optional<GenericConstraintKind> failed =
          firstUnsatisfiedConstraint(arguments[argumentIndex],
                                     parameter.constraints);
      if (failed) {
        return ConstraintFailure{.parameter = parameter.name,
                                 .argument = arguments[argumentIndex],
                                 .constraints = parameter.constraints,
                                 .failed = *failed,
                                 .constraintName = parameter.constraintName};
      }
      ++argumentIndex;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::expandStructuralRequirements(
    std::span<const AppliedConceptRequirement> requirements) const
    -> std::vector<ExpandedStructuralRequirement> {
  std::vector<ExpandedStructuralRequirement> result;
  for (const AppliedConceptRequirement &requirement : requirements) {
    if (requirement.conceptId == 0 || requirement.conceptId > concepts.size()) {
      continue;
    }
    const RegisteredConcept &conceptInfo = concepts[requirement.conceptId - 1];
    for (const RegisteredConcept::StructuralPattern &pattern :
         conceptInfo.structuralConstraints) {
      ExpandedStructuralRequirement expanded{.kind = pattern.kind,
                                             .application = requirement.syntax};
      bool valid = true;
      for (const RegisteredConcept::StructuralPattern::Argument &argument :
           pattern.arguments) {
        if (!argument.parameter) {
          expanded.arguments.emplace_back(argument.concrete);
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
  }
  return result;
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
    for (const AppliedConceptRequirement &requirement : scope) {
      if (requirement.conceptId == 0 ||
          requirement.conceptId > concepts.size()) {
        continue;
      }
      const RegisteredConcept &conceptInfo =
          concepts[requirement.conceptId - 1];
      const std::size_t count =
          std::min(requirement.arguments.size(),
                   conceptInfo.parameterConstraints.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (requirement.arguments[index] == type) {
          result |= conceptInfo.parameterConstraints[index];
        }
      }
    }
  }
  return result;
}

auto SemanticVisitor::Impl::structuralRequirementAvailable(
    StructuralConstraintKind kind,
    std::span<const SemanticType> arguments) const -> bool {
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
  for (const AppliedConceptRequirement &requirement : requirements) {
    if (requirement.conceptId == 0 || requirement.conceptId > concepts.size() ||
        requirement.syntax == nullptr) {
      continue;
    }
    const RegisteredConcept &conceptInfo = concepts[requirement.conceptId - 1];
    const std::size_t count = std::min(requirement.arguments.size(),
                                       conceptInfo.parameterConstraints.size());
    for (std::size_t index = 0; index < count; ++index) {
      if (const std::optional<GenericConstraintKind> failed =
              firstUnsatisfiedConstraint(
                  requirement.arguments[index],
                  conceptInfo.parameterConstraints[index])) {
        return ConstraintFailure{
            .parameter = requirement.syntax->arguments[index].name.last(),
            .argument = requirement.arguments[index],
            .constraints = conceptInfo.parameterConstraints[index],
            .failed = *failed,
            .constraintName = requirement.syntax->name,
            .application = requirement.syntax};
      }
    }
    for (const ExpandedStructuralRequirement &structural :
         expandStructuralRequirements(
             std::span<const AppliedConceptRequirement>(&requirement, 1))) {
      if (!structuralConstraintSatisfied(structural.kind,
                                         structural.arguments)) {
        return ConstraintFailure{
            .parameter =
                requirement.syntax->arguments.empty()
                    ? requirement.syntax->name.last()
                    : requirement.syntax->arguments.front().name.last(),
            .argument = structural.arguments.empty()
                            ? SemanticType::Unknown
                            : structural.arguments.front(),
            .constraintName = requirement.syntax->name,
            .structural = structural.kind,
            .application = requirement.syntax};
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::hasConstraintFailure(
    const ConstraintFailure &failure) -> bool {
  return failure.failed != GenericConstraintKind::None ||
         failure.structural != StructuralConstraintKind::None;
}

auto SemanticVisitor::Impl::reportConstraintFailure(
    const Token &site, const ConstraintFailure &failure) -> void {
  reporter.reportConstraintFailure(site, failure);
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
           "Variadic function expects at least " +
               std::to_string(fixedParameterCount(function)) +
               " fixed argument" +
               (fixedParameterCount(function) == 1 ? "" : "s") +
               " before its parameter pack.",
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
    } else if (substitution.types.at(parameter.id) == SemanticType::Void) {
      if (explicitTypeArguments.empty()) {
        report(paren, "Generic type arguments cannot be void.", "GTI-S2023");
      }
      valid = false;
    }
  }

  if (!explicitTypeArguments.empty() &&
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
  return valid;
}

auto SemanticVisitor::Impl::inferTypeArguments(
    const SemanticType &pattern, const SemanticType &argument,
    const std::vector<GenericParameterInfo> &parameters,
    GenericSubstitution &substitution, const Token &argumentToken,
    const ExprPtr *expression) -> bool {
  if (pattern.kind == SemanticType::Reference &&
      pattern.arguments.size() == 1) {
    return inferTypeArguments(pattern.arguments[0], argument, parameters,
                              substitution, argumentToken, expression);
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
      report(argumentToken, "Conflicting types inferred for generic type "
                            "parameter '" +
                                typeParameter->name.lexeme + "'.");
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
    if (pattern.arrayLengthParameterId != 0) {
      const GenericParameterInfo *parameter =
          findGenericParameter(parameters, pattern.arrayLengthParameterId);
      if (parameter != nullptr && parameter->value && inferredLength) {
        const CompileTimeValue value = *inferredLength;
        const auto found =
            substitution.values.find(pattern.arrayLengthParameterId);
        if (found == substitution.values.end()) {
          substitution.values.emplace(pattern.arrayLengthParameterId, value);
        } else if (found->second != value) {
          report(argumentToken,
                 "Conflicting lengths inferred for generic value parameter '" +
                     parameter->name.lexeme + "'.",
                 "GTI-S2026");
          return false;
        }
      }
    }
    if (argument.kind == SemanticType::Array && pattern.arguments.size() == 1 &&
        argument.arguments.size() == 1) {
      return inferTypeArguments(pattern.arguments.front(),
                                argument.arguments.front(), parameters,
                                substitution, argumentToken, nullptr);
    }
    return true;
  }

  if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
      pattern.arrayLength != argument.arrayLength ||
      pattern.arrayLengthParameterId != argument.arrayLengthParameterId ||
      pattern.valueArguments != argument.valueArguments ||
      pattern.arguments.size() != argument.arguments.size()) {
    return true;
  }
  bool valid = true;
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    valid =
        inferTypeArguments(pattern.arguments[index], argument.arguments[index],
                           parameters, substitution, argumentToken, nullptr) &&
        valid;
  }
  return valid;
}

auto SemanticVisitor::Impl::applyFunctionTypeArguments(
    CallableCandidate &function,
    const std::vector<SemanticType> &explicitTypeArguments,
    const std::vector<CompileTimeValue> &explicitValueArguments,
    const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
    const Token &paren) -> bool {
  if (function.parameterPack) {
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
  for (std::size_t index = 0; index < count; ++index) {
    valid = inferTypeArguments(
                function.parameterTypes[index], argumentTypes[index],
                function.record().genericParameters, substitution,
                expressionToken(arguments[index]), &arguments[index]) &&
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
                      ? "' from a fixed-array argument extent and no default "
                        "is declared; provide it explicitly."
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
  if (std::any_of(resolvedTypeArguments.begin(), resolvedTypeArguments.end(),
                  [](const SemanticType &argument) {
                    return argument == SemanticType::Void;
                  })) {
    if (explicitTypeArguments.empty()) {
      report(paren, "Generic type arguments cannot be void.", "GTI-S2023");
    }
    valid = false;
  }
  if (resolvedTypeArguments.size() ==
      genericTypeParameterCount(function.record().genericParameters)) {
    if (const std::optional<ConstraintFailure> failure = firstConstraintFailure(
            function.record().genericParameters, resolvedTypeArguments)) {
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
    if (const std::optional<ConstraintFailure> failure =
            firstRequirementFailure(function.requirements)) {
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
    return tryInferTypeArguments(pattern.arguments[0], argument, parameters,
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

  if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
      pattern.arrayLength != argument.arrayLength ||
      pattern.arrayLengthParameterId != argument.arrayLengthParameterId ||
      pattern.valueArguments != argument.valueArguments ||
      pattern.arguments.size() != argument.arguments.size()) {
    return true;
  }
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    if (!tryInferTypeArguments(pattern.arguments[index],
                               argument.arguments[index], parameters,
                               substitution, nullptr, conflictingParameter,
                               conflictingValueParameter)) {
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
  if (candidate.record().parameterPack != (pack != nullptr) ||
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
  if (pack != nullptr) {
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
    resolvedTypeArguments.insert(resolvedTypeArguments.end(), packTypes.begin(),
                                 packTypes.end());
  }
  if (std::any_of(resolvedTypeArguments.begin(), resolvedTypeArguments.end(),
                  [](const SemanticType &argument) {
                    return argument == SemanticType::Void;
                  })) {
    return false;
  }
  if (const std::optional<ConstraintFailure> failure = firstConstraintFailure(
          candidate.record().genericParameters, resolvedTypeArguments)) {
    if (constraintFailure != nullptr) {
      *constraintFailure = *failure;
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
    if (const std::optional<FunctionReturnBorrowSummary> dynamicView =
            dynamicStringViewReturnBorrowSummary(concrete)) {
      resolved.returnBorrowOrigin = dynamicView->origin;
      resolved.returnBorrowParameter = dynamicView->parameter;
      resolved.returnBorrowAccess = dynamicView->access;
      resolved.returnBorrowPlace = dynamicView->place;
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
    const SemanticType &type, std::unordered_set<ClassId> &visiting) const
    -> bool {
  if (type.kind == SemanticType::Reference ||
      type.kind == SemanticType::RawPointer ||
      type.kind == SemanticType::StringView) {
    return true;
  }
  if (type.kind == SemanticType::Lambda) {
    return type.hasLambdaShape() &&
           std::any_of(type.lambdaCaptureTypes().begin(),
                       type.lambdaCaptureTypes().end(),
                       [&](const SemanticType &capture) {
                         return containsDisallowedCallableCaptureState(
                             capture, visiting);
                       });
  }
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size()) {
    if (!visiting.insert(type.classId).second) {
      return false;
    }
    const ClassInfo &owner = classInfo(type.classId);
    const GenericSubstitution substitution = classSubstitution(type);
    bool found = false;
    if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
      found = containsDisallowedCallableCaptureState(
          substituteType(base->type, substitution), visiting);
    }
    for (const FieldInfo &field : owner.fields) {
      if (found || field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end()) {
        found = containsDisallowedCallableCaptureState(
            substituteType(member->second.symbol.type, substitution), visiting);
      }
    }
    visiting.erase(type.classId);
    return found;
  }
  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size() && enums[type.enumId - 1].payload) {
    return std::any_of(
        enums[type.enumId - 1].enumerators.begin(),
        enums[type.enumId - 1].enumerators.end(), [&](const auto &entry) {
          return std::any_of(entry.second.payloadTypes.begin(),
                             entry.second.payloadTypes.end(),
                             [&](const SemanticType &payload) {
                               return containsDisallowedCallableCaptureState(
                                   payload, visiting);
                             });
        });
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsDisallowedCallableCaptureState(argument,
                                                                     visiting);
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
        function.declaration->body() == nullptr) {
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

auto SemanticVisitor::Impl::ownedTransportMatchesConcrete(
    const FunctionInfo &function, const CallableParameterContract &contract,
    const ResolvedCallInfo &call) -> bool {
  if (!contract.ownedTransport ||
      contract.parameterIndex >= call.parameterTypes.size()) {
    return false;
  }
  GenericSubstitution substitution;
  if (contract.ownedTransport->kind == CallableOwnedTransportKind::ThreadTask &&
      function.ownerClass != 0 &&
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
  const SemanticType &callableType =
      call.parameterTypes[contract.parameterIndex];
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
    if (call == nullptr) {
      continue;
    }
    const FunctionInfo *target = semanticModel.findFunction(resolved.function);
    if (target == nullptr || target->declaration == nullptr) {
      continue;
    }
    for (const CallableParameterContract &contract :
         target->callableParameters) {
      const bool threadTask =
          contract.ownedTransport && contract.ownedTransport->kind ==
                                         CallableOwnedTransportKind::ThreadTask;
      const SemanticType *argumentType =
          contract.parameterIndex < resolved.parameterTypes.size()
              ? &resolved.parameterTypes[contract.parameterIndex]
              : nullptr;
      const bool callableTransport =
          contract.ownedTransport && argumentType != nullptr &&
          (threadTask || !contract.signatures.empty() ||
           containsLambdaType(*argumentType));
      if (!callableTransport ||
          contract.parameterIndex >= call->arguments().size() ||
          contract.parameterIndex >= resolved.parameterTypes.size()) {
        continue;
      }
      const ExprPtr &argument = call->arguments()[contract.parameterIndex];
      const SemanticType &concreteArgumentType =
          resolved.parameterTypes[contract.parameterIndex];
      const SemanticTypeTraits traits = typeTraits(concreteArgumentType);
      const ExpressionInfo *argumentInfo =
          semanticModel.findExpression(*argument);
      const bool consumed = explicitMoveSource(argument) != nullptr ||
                            (argumentInfo != nullptr &&
                             argumentInfo->category == ValueCategory::Value);
      if (threadTask && consumed &&
          !ownedTransportMatchesConcrete(*target, contract, resolved)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2084", DiagnosticPhase::Semantics, expressionToken(argument),
            contract.ownedTransport->destinationType == SemanticType::Void
                ? "A managed thread requires one consumed, transfer-safe "
                  "exact void() task."
                : "A managed thread requires one consumed, transfer-safe "
                  "exact task whose return type matches its transferable "
                  "result.");
        diagnostic.related.push_back(
            {tokenSpan(
                 target->declaration->parameters()[contract.parameterIndex]
                     .name),
             "Managed task parameter declared here."});
        diagnostic.hints.emplace_back(
            "Pass a fresh task value or std::move(task); captures must own "
            "their state and cannot contain references, tracked borrows, "
            "or raw pointers.");
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
          {tokenSpan(
               target->declaration->parameters()[contract.parameterIndex].name),
           "Exact owned callable parameter declared here."});
      diagnostic.hints.emplace_back(
          "Move an exact movable callable whose stored state contains no "
          "references, tracked borrowed state, or raw pointers.");
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
    if (call == nullptr || resolved.callableArguments.empty()) {
      continue;
    }
    const FunctionInfo *target = semanticModel.findFunction(resolved.function);
    for (const CallableArgumentBoundary &argument :
         resolved.callableArguments) {
      const bool provenBoundary =
          target != nullptr &&
          std::any_of(target->callableParameters.begin(),
                      target->callableParameters.end(),
                      [&](const CallableParameterContract &contract) {
                        return contract.parameterIndex ==
                               argument.parameterIndex;
                      });
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
              : "Pass a lambda only to a direct by-value generic parameter "
                "whose visible body proves confined use or exact owned "
                "transport.");
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
    if (function.declaration == nullptr) {
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
            {tokenSpan(parameter->name),
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
          {tokenSpan(sourceParameter->name),
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
  for (const SemanticModel::PendingCallableForwarding &forwarding :
       semanticModel.pendingCallableForwardings) {
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
          {tokenSpan(sourceParameter->name),
           "Confined callable parameter declared here."});
    }
    if (targetParameter != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(targetParameter->name),
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
    if (function.declaration == nullptr || !function.declaration->body()) {
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
            {tokenSpan(parameter.name),
             exactReturnTransport
                 ? "Exact-return callable parameter declared here."
                 : "Confined callable parameter declared here."});
        diagnostic.hints.emplace_back(
            exactReturnTransport
                ? "Invoke the callable directly and return it only through "
                  "the declaration's exact std::move transport."
                : "Invoke the callable directly or forward it to a direct "
                  "by-value generic parameter with a proven confined "
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
              {tokenSpan(parameter.name),
               exactReturnTransport
                   ? "Exact-return callable parameter declared here."
                   : "Confined callable parameter declared here."});
          diagnostic.hints.emplace_back(
              exactReturnTransport
                  ? "Invoke the callable directly and return it only "
                    "through the declaration's exact std::move transport."
                  : "Invoke the callable directly or forward it to a direct "
                    "by-value generic parameter with a proven confined "
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
    const bool lexicalClosure = containsLambdaType(argumentTypes[index]);
    const bool provenBoundary =
        (sourceContract != nullptr &&
         (sourceContract->boundary == CallableBoundary::Confined ||
          sourceContract->boundary == CallableBoundary::Owned)) ||
        (lexicalClosure && targetContract != nullptr);
    if (!lexicalClosure && !provenBoundary) {
      continue;
    }
    const Parameter *parameter =
        candidate.declaration != nullptr &&
                index < candidate.declaration->parameters().size()
            ? &candidate.declaration->parameters()[index]
            : nullptr;
    const bool directByValueGeneric =
        index < candidate.parameterTypes.size() && parameter != nullptr &&
        candidate.parameterTypes[index].kind == SemanticType::TypeParameter &&
        !parameter->type.reference && !parameter->pack;
    if (instanceAnalysisActive) {
      if (!directByValueGeneric || candidate.declaration == nullptr ||
          !candidate.declaration->body() || targetContract == nullptr) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2046", DiagnosticPhase::Semantics,
            expressionToken(arguments[index]),
            "Callable argument " + std::to_string(index + 1) +
                " cannot be forwarded because the selected parameter is "
                "not proven confined or owned.");
        diagnostic.hints.emplace_back(
            "Forward closures only through a direct by-value generic "
            "parameter whose visible body proves confined use or exact "
            "owned transport.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
    } else if (lexicalClosure &&
               (!directByValueGeneric || candidate.declaration == nullptr ||
                !candidate.declaration->body())) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2046", DiagnosticPhase::Semantics,
          expressionToken(arguments[index]),
          "Lambda argument " + std::to_string(index + 1) +
              " must bind to a direct by-value generic parameter with a "
              "visible function body.");
      diagnostic.hints.emplace_back(
          "Use a generic value parameter such as 'Operation operation'; "
          "callable references and owning type erasure are not available.");
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
      owner->compilerCapability ==
          CompilerCapabilityTypeKind::MutexLockToken) {
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
            "use std::thread or atomic synchronization."
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
  if (!isCompilerTrustedRequester(paren)) {
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
  const AccessMode selectedReceiverAccess =
      receiverAllowsMutation(function.receiverMutability)
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  AccessMode callReceiverAccess = selectedReceiverAccess;
  std::optional<SemanticLoanPlace> receiverPlace;
  SemanticLoanId receiverLoan = 0;
  bool transientReceiver = false;
  const bool temporaryReceiver =
      receiverMode == CallReceiverMode::FreshTemporaryRead ||
      receiverMode == CallReceiverMode::FreshTemporaryMutable;
  if (member != nullptr && !function.staticMember && !temporaryReceiver) {
    std::optional<TransientBorrowProduction> production =
        transientBorrowProduction(member->object());
    if (production) {
      transientReceiver = true;
      receiverPlace = std::move(production->place);
      if (production->access == AccessMode::Mutable) {
        callReceiverAccess = AccessMode::Mutable;
      }
    } else {
      receiverPlace = loanPlace(member->object());
    }
    receiverLoan = accessLoan(member->object());
  } else if (member == nullptr && function.ownerClass != 0 &&
             !function.staticMember) {
    receiverPlace = loanPlace(
        SemanticPlace{.root = receiverStateSymbol(), .receiver = true});
  }
  validateCallPlaceExclusivity(function.parameterTypes, arguments, paren,
                               receiverPlace, callReceiverAccess,
                               transientReceiver);

  if (function.staticMember) {
    // Object-qualified static access is diagnosed while resolving Get.
    return true;
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
        paren, *receiverPlace, selectedReceiverAccess,
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
  const auto hasDereference = [&](const SemanticType &iterator,
                                  const SemanticType *referent) {
    const std::optional<CallableCandidate> candidate =
        selectedDereference(iterator);
    if (!candidate || candidate->returnType.kind != SemanticType::Reference ||
        candidate->returnType.arguments.size() != 1) {
      return false;
    }
    return referent == nullptr ||
           candidate->returnType.arguments.front() == *referent;
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
                                     CallableInvocationCapability required) {
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
        if (!readableCallableParameter(candidate.parameterTypes[index],
                                       parameters[index])) {
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
    const SemanticType parameter =
        SemanticType::referenceTo(sentinel, AccessMode::ReadOnly);
    const std::optional<CallableCandidate> comparison =
        selectStructuralOperator(iterator, OverloadedOperator::NotEqual,
                                 std::span<const SemanticType>(&parameter, 1),
                                 false);
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
    const SemanticType parameter =
        SemanticType::referenceTo(sentinel, AccessMode::ReadOnly);
    const std::optional<CallableCandidate> difference =
        selectStructuralOperator(iterator, OverloadedOperator::Minus,
                                 std::span<const SemanticType>(&parameter, 1),
                                 false);
    return difference && difference->returnType == SemanticType::Int64 &&
           difference->receiverMutability == ReceiverMutability::ReadOnly;
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
    if (!dereference ||
        dereference->returnType.kind != SemanticType::Reference ||
        dereference->returnType.arguments.size() != 1) {
      return false;
    }
    const SemanticType &element = dereference->returnType.arguments.front();
    const SemanticType &result =
        kind == StructuralConstraintKind::IndirectPredicate ? SemanticType::Bool
                                                            : arguments[2];
    return callableSatisfies(arguments[0],
                             std::span<const SemanticType>(&element, 1), result,
                             CallableInvocationCapability::Mutable);
  }
  case StructuralConstraintKind::None:
    return true;
  }
  return false;
}

auto SemanticVisitor::Impl::symbolicOperatorRequirements(
    OverloadedOperator kind, const SemanticType &receiverType,
    std::span<const SemanticType> argumentTypes) const
    -> std::vector<CallableCandidate> {
  std::vector<CallableCandidate> result;
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
      // Callable requirements constrain ordinary invocation rather than a
      // member operator expression synthesized by this table.
      break;
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
    bool preferReadOnlyReceiver) -> std::optional<CallableCandidate> {
  if (receiverType.kind == SemanticType::TypeParameter) {
    return resolveSymbolicOperator(site, kind, receiver, receiverType, token,
                                   argumentTypes, arguments, contextual,
                                   boundary, receiverState, projection);
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
  const bool consumingReceiver =
      kind == OverloadedOperator::Call && isExplicitMoveReceiver(receiver);
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
  for (const CallableCandidate &candidate : overloadSet.overloads) {
    if (candidate.parameterTypes.size() != argumentTypes.size() ||
        arguments.size() != argumentTypes.size()) {
      continue;
    }
    bool exact = true;
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      if (argumentTypes[index] != SemanticType::Unknown &&
          candidate.parameterTypes[index] != SemanticType::Unknown &&
          !exactOperatorArgumentMatches(candidate.parameterTypes[index],
                                        argumentTypes[index],
                                        arguments[index])) {
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
                                    consumingReceiver);
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
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2022", DiagnosticPhase::Semantics, token,
            "operator() && requires an explicitly consumed receiver.");
        diagnostic.hints.emplace_back(
            "Invoke a named local or by-value parameter with "
            "std::move(value)().");
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
      diagnostic.hints.emplace_back(
          "GTI operator calls do not perform implicit conversions, rank "
          "candidates, reverse operands, or use ADL; convert the operand "
          "explicitly when that conversion is intentional.");
      for (const CallableCandidate &candidate : overloadSet.overloads) {
        if (candidate.declaration != nullptr &&
            (isCompilerTrustedRequester(token) || !candidate.compilerPrivate)) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->operatorName()->keyword),
               "Candidate: " + functionSignatureSpelling(candidate)});
        }
      }
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
  const CallReceiverMode selectedReceiverMode = viable.front().receiverMode;
  if (consumingReceiver &&
      rejectConsumingActiveCleanup(token, receiverType, boundary)) {
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
  validateMutableCallArgumentLoans(selected.parameterTypes, arguments, token);
  validateCallPlaceExclusivity(selected.parameterTypes, arguments, token,
                               receiverPlace, callReceiverAccess,
                               receiverProduction.has_value());

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
  for (std::size_t index = 0; index < count; ++index) {
    if (index >= targetInfo->declaration->parameters().size() ||
        targetInfo->parameterTypes[index].kind != SemanticType::TypeParameter) {
      continue;
    }
    const Parameter &targetParameter =
        targetInfo->declaration->parameters()[index];
    if (targetParameter.type.reference || targetParameter.pack) {
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
        sourceInfo->parameterTypes[sourceIndex].kind !=
            SemanticType::TypeParameter ||
        sourceParameter->type.reference || sourceParameter->pack) {
      continue;
    }

    semanticModel.recordCallableForwarding(
        *sourceInfo->declaration, sourceIndex,
        sourceInfo->parameterTypes[sourceIndex].genericParameterId,
        sourceInfo->parameterTypes[sourceIndex],
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
  const Token token = callableToken(call.callee());
  if (token.generated) {
    return;
  }
  const SymbolId symbol = resolved.declaration == nullptr
                              ? 0
                              : recordFunctionSymbol(*resolved.declaration);
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

auto SemanticVisitor::Impl::recordResolvedCall(
    const Call &call, const CallableCandidate &function,
    std::vector<SemanticType> typeArguments,
    std::vector<CompileTimeValue> valueArguments,
    std::vector<std::size_t> ephemeralBorrowArguments,
    std::vector<CallableArgumentBoundary> callableArguments,
    CallReceiverMode receiverMode) -> void {
  const bool pointerBearingCFunction =
      function.declaration != nullptr && function.declaration->hasCLinkage() &&
      (containsCAbiRawPointer(function.returnType) ||
       std::any_of(function.parameterTypes.begin(),
                   function.parameterTypes.end(),
                   [this](const SemanticType &parameter) {
                     return containsCAbiRawPointer(parameter);
                   }));
  const bool missingUnsafe = pointerBearingCFunction && unsafeDepth == 0;
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
  const FunctionInfo *declarationInfo = semanticModel.findFunction(function.id);
  if (declarationInfo == nullptr && instanceBaseModel != nullptr) {
    declarationInfo = instanceBaseModel->findFunction(function.id);
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
                                    call.arguments().size());
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
      .borrowOrigin = returnBorrowOrigin,
      .borrowArgument = returnBorrowParameter,
      .borrowAccess = returnBorrowAccess,
      .borrowPlace = returnBorrowPlace,
      .dispatch =
          function.virtualMethod ? CallDispatch::Virtual : CallDispatch::Static,
      .dispatchOwner = function.dispatchOwner,
      .receiverMode = receiverMode,
      .ephemeralBorrowArguments = std::move(ephemeralBorrowArguments),
      .callableArguments = std::move(callableArguments)};
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
    const std::vector<const CallableCandidate *> &exactMatches) -> void {
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
        "GTI-S2012", DiagnosticPhase::Semantics, call.paren(),
        "No overload of '" + name + "' exactly matches argument types (" +
            arguments + ").");
    diagnostic.hints.emplace_back(
        "Function calls do not perform implicit conversions; convert an "
        "argument explicitly with syntax such as 'uint64_t(value)'.");
    for (const CallableCandidate &candidate : overloadSet.overloads) {
      if (candidate.declaration != nullptr &&
          (isCompilerTrustedRequester(call.paren()) ||
           !candidate.compilerPrivate)) {
        diagnostic.related.push_back(
            {tokenSpan(candidate.declaration->name()),
             "Candidate: " + functionSignatureSpelling(candidate)});
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
