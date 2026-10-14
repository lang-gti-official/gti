#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::restoredReferenceFields(
    const SemanticType &referent)
    -> std::optional<std::vector<RestoredBorrowField>> {
  std::vector<RestoredBorrowField> result;
  for (const auto &path : storedReferencePaths(referent)) {
    auto fields = storedFieldSymbols(referent, path.fields);
    if (fields.size() != path.fields.size())
      return std::nullopt;
    result.push_back(
        {.fields = std::move(fields), .referenceType = path.reference});
  }
  return result;
}

auto SemanticVisitor::Impl::recordRestoredReferenceResult(
    const FunctionInfo &function) -> void {
  if (function.returnType.kind != SemanticType::Reference ||
      function.returnType.arguments.size() != 1 ||
      function.returnBorrowOrigin != BorrowOriginKind::Argument ||
      !function.returnBorrowParameters.empty() ||
      !function.returnBorrowFields.empty() ||
      function.returnBorrowParameter >= function.parameterTypes.size() ||
      function.bodyDeclaration() == nullptr ||
      function.returnBorrowParameter >=
          function.bodyDeclaration()->parameters().size() ||
      function.bodyDeclaration()->body() == nullptr)
    return;
  const auto &input = function.parameterTypes[function.returnBorrowParameter];
  const auto *owner =
      input.kind == SemanticType::Reference && input.arguments.size() == 1
          ? classInfo(input.arguments.front())
          : nullptr;
  if (owner == nullptr ||
      owner->compilerCapability != CompilerCapabilityTypeKind::ErasedObject)
    return;
  const auto *parameter = semanticModel.findBinding(
      function.bodyDeclaration()->parameters()[function.returnBorrowParameter]);
  if (parameter == nullptr)
    return;

  // Resolve lexical reference aliases by declaration identity. Rebinding a
  // reference is not assignment in GTI; its initializer remains its origin.
  // This work is restricted to opaque-reference return helpers, not every body.
  std::unordered_map<SymbolId, const Expr *> aliases;
  for (const auto &[declaration, binding] : semanticModel.variableBindings) {
    if (binding.type.kind == SemanticType::Reference &&
        declaration->initializer())
      aliases.emplace(binding.symbol, declaration->initializer().get());
  }
  const auto unalias = [&](const Expr *expression) {
    std::unordered_set<SymbolId> visited;
    while (expression != nullptr) {
      if (const auto *group = exprAs<Grouping>(expression)) {
        expression = group->expression().get();
        continue;
      }
      if (exprAs<Variable>(expression) == nullptr)
        break;
      const auto symbol = semanticModel.findResolvedSymbol(*expression);
      const auto alias = aliases.find(symbol);
      if (alias == aliases.end() || !visited.insert(symbol).second)
        break;
      expression = alias->second;
    }
    return expression;
  };
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.bodyDeclaration()->body().get(),
                                 returns);
  if (returns.empty())
    return;
  for (const auto *returned : returns) {
    const auto *expression =
        unalias(returned == nullptr ? nullptr : returned->get());
    const auto *call = exprAs<Call>(expression);
    const auto *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    if (resolved == nullptr || !resolved->restoresReferenceResult ||
        resolved->returnType != function.returnType ||
        resolved->borrowOrigin != BorrowOriginKind::Argument)
      return;
    const auto *argument =
        resolvedCallArgument(*call, resolved, resolved->borrowArgument);
    if (argument == nullptr)
      return;
    const auto *witness = unalias(argument->get());
    if (witness == nullptr ||
        semanticModel.findResolvedSymbol(*witness) != parameter->symbol)
      return;
  }
  auto fields = restoredReferenceFields(function.returnType.arguments.front());
  if (!fields)
    return;
  FunctionInfo updated = function;
  updated.restoresReferenceResult = true;
  updated.restoredBorrowFields = std::move(*fields);
  semanticModel.record(*function.declaration, std::move(updated));
}

auto SemanticVisitor::Impl::analyzeReferenceErasureCall(const Call &expr,
                                                        IntrinsicKind intrinsic)
    -> void {
  std::vector<SemanticType> arguments;
  for (const auto &argument : expr.arguments())
    arguments.push_back(analyze(argument));
  for (const auto &argument : expr.typeArguments())
    validateType(argument);
  const bool restoring = intrinsic == IntrinsicKind::RestoreReference ||
                         intrinsic == IntrinsicKind::RestoreReferenceMut;
  const auto access = intrinsic == IntrinsicKind::EraseReferenceMut ||
                              intrinsic == IntrinsicKind::RestoreReferenceMut
                          ? AccessMode::Mutable
                          : AccessMode::ReadOnly;
  if (restoring)
    requireUnsafe(expr, UnsafeOperationKind::ErasedReferenceRestore,
                  expr.paren(),
                  "Restoring the type of an erased checked reference");
  const auto opaque =
      std::find_if(classes.begin(), classes.end(), [](const auto &owner) {
        return owner.compilerCapability ==
               CompilerCapabilityTypeKind::ErasedObject;
      });
  if (arguments.size() != 1 || opaque == classes.end() ||
      (restoring ? expr.typeArguments().size() != 1
                 : !expr.typeArguments().empty())) {
    report(expr.paren(),
           restoring ? "Restoring a checked reference requires one erased "
                       "reference and its original referent type."
                     : "Erasing a checked reference requires one stable object "
                       "place and infers its type.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  const auto erased = SemanticType::classType(opaque->id);
  const auto result = restoring ? typeOf(expr.typeArguments().front()) : erased;
  if (restoring && arguments.front() != erased) {
    report(expr.paren(),
           "Restoration requires an erased checked reference, "
           "not a raw pointer or unrelated object.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  if (result == SemanticType::Void || result.kind == SemanticType::Reference ||
      result.kind == SemanticType::FunctionSignature ||
      result.kind == SemanticType::Function ||
      result.kind == SemanticType::TypeName) {
    report(expr.paren(),
           "An erased checked reference must denote an object "
           "referent type.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  const auto parameter = SemanticType::referenceTo(arguments.front(), access);
  validateReferenceReferent(parameter, expr.paren());
  validateReferenceReferent(SemanticType::referenceTo(result, access),
                            expr.paren());
  validateReferenceBinding(parameter, arguments.front(),
                           expr.arguments().front());
  const std::array parameters{parameter};
  validateCallPlaceExclusivity(parameters, expr.arguments(), expr.paren());
  std::vector<RestoredBorrowField> restoredFields;
  if (restoring) {
    auto fields = restoredReferenceFields(result);
    if (!fields) {
      report(expr.paren(),
             "Restoration requires concrete checked field "
             "dependencies for its original referent type.",
             "GTI-S2017");
      currentType = SemanticType::Unknown;
      return;
    }
    restoredFields = std::move(*fields);
  }
  currentType = result;
  semanticModel.record(
      expr, ResolvedCallInfo{
                .returnType = SemanticType::referenceTo(result, access),
                .parameterTypes = {parameter},
                .typeArguments = {restoring ? result : arguments.front()},
                .intrinsic = intrinsic,
                .restoresReferenceResult = restoring,
                .restoredBorrowFields = std::move(restoredFields),
                .borrowOrigin = BorrowOriginKind::Argument,
                .borrowArgument = 0,
                .borrowAccess = access});
}

auto SemanticVisitor::Impl::analyzeFunctionPointerCall(const Call &expr,
                                                       IntrinsicKind intrinsic)
    -> void {
  std::vector<SemanticType> arguments;
  for (const auto &argument : expr.arguments())
    arguments.push_back(analyze(argument));
  for (const auto &argument : expr.typeArguments())
    validateType(argument);
  const bool forming = intrinsic == IntrinsicKind::FunctionPointerFromItem;
  SemanticType signature;
  SemanticType pointer;
  if (forming && expr.typeArguments().size() == 1 && arguments.size() == 1) {
    signature = typeOf(expr.typeArguments().front());
    const auto capability =
        std::find_if(classes.begin(), classes.end(), [](const auto &owner) {
          return owner.compilerCapability ==
                 CompilerCapabilityTypeKind::FunctionPointer;
        });
    if (capability != classes.end())
      pointer = SemanticType::classType(capability->id, {signature});
  } else if (!forming && expr.typeArguments().empty() && !arguments.empty()) {
    pointer = arguments.front();
    const auto *owner = classInfo(pointer);
    if (owner != nullptr &&
        owner->compilerCapability ==
            CompilerCapabilityTypeKind::FunctionPointer &&
        pointer.arguments.size() == 1)
      signature = pointer.arguments.front();
  }
  if (signature.kind != SemanticType::FunctionSignature ||
      signature.arguments.empty() ||
      signature.signatureReceiver != ReceiverMutability::ReadOnly) {
    report(expr.paren(),
           "GTI invocation pointers require one unqualified "
           "function signature and an exact stateless target.",
           "GTI-S2093");
    currentType = SemanticType::Unknown;
    return;
  }
  const auto &result = signature.arguments.front();
  if (result.kind == SemanticType::Reference ||
      (!containsOpenTypeParameter(result) &&
       typeTraits(result).containsBorrowedState)) {
    report(expr.paren(),
           "A signature-erased invocation result cannot borrow "
           "until its signature carries a result-origin contract.",
           "GTI-S2093");
    currentType = SemanticType::Unknown;
    return;
  }
  std::vector<SemanticType> parameters;
  for (const auto &type :
       std::span<const SemanticType>(signature.arguments).subspan(1))
    SemanticType::appendExpandedArgument(parameters, type);
  if (forming) {
    const auto &target = arguments.front();
    FunctionId function = 0;
    LambdaId lambda = 0;
    const SemanticType *targetResult = nullptr;
    std::vector<SemanticType> targetParameters;
    if (target.hasFunctionItemShape()) {
      function = target.functionId;
      const auto *declaration = semanticModel.findFunction(function);
      if (declaration == nullptr && instanceBaseModel != nullptr)
        declaration = instanceBaseModel->findFunction(function);
      if (declaration == nullptr || declaration->requiresUnsafe ||
          declaration->tracksCaller || declaration->entryPoint ||
          declaration->ownerClass != 0 ||
          declaration->linkage != LanguageLinkage::Gti ||
          declaration->bodyDeclaration() == nullptr ||
          declaration->bodyDeclaration()->body() == nullptr) {
        report(expressionToken(expr.arguments().front()),
               "A safe GTI invocation pointer requires an ordinary defined "
               "namespace function without unsafe or caller-location effects.",
               "GTI-S2093");
        currentType = SemanticType::Unknown;
        return;
      }
      targetResult = target.functionItemReturnType();
      for (const auto &type : target.functionItemParameterTypes())
        SemanticType::appendExpandedArgument(targetParameters, type);
    } else if (target.hasLambdaShape() && target.lambdaCaptureTypes().empty()) {
      lambda = target.lambdaId;
      targetResult = target.lambdaReturnType();
      for (const auto &type : target.lambdaParameterTypes())
        SemanticType::appendExpandedArgument(targetParameters, type);
    }
    if (!containsOpenTypeParameter(target) &&
        (targetResult == nullptr || *targetResult != result ||
         targetParameters != parameters)) {
      report(expressionToken(expr.arguments().front()),
             "GTI invocation pointer construction requires an exact-signature "
             "namespace function item or a lambda with no captures.",
             "GTI-S2093");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = pointer;
    semanticModel.record(expr,
                         ResolvedCallInfo{.returnType = pointer,
                                          .parameterTypes = arguments,
                                          .typeArguments = {signature, target},
                                          .intrinsic = intrinsic,
                                          .functionPointerFunction = function,
                                          .functionPointerLambda = lambda});
    return;
  }
  const auto expanded = concreteCallArguments(arguments, expr.arguments());
  currentType = result;
  if (!expanded)
    return;
  parameters.insert(parameters.begin(),
                    SemanticType::referenceTo(pointer, AccessMode::ReadOnly));
  bool valid = expanded->size() == parameters.size();
  if (!valid)
    report(expr.paren(),
           "GTI invocation pointer argument count does not match "
           "its exact signature.",
           "GTI-S2005");
  std::vector<SemanticType> ordinaryParameters;
  std::vector<const ExprPtr *> ordinaryArguments;
  for (std::size_t index = 0;
       index < std::min(expanded->size(), parameters.size()); ++index) {
    const auto &argument = (*expanded)[index];
    const auto &expression = *argument.expression;
    if (!argument.forwardedPackElement) {
      ordinaryParameters.push_back(parameters[index]);
      ordinaryArguments.push_back(argument.expression);
      validateMutableCallArgumentLoans(
          std::span<const SemanticType>(&parameters[index], 1),
          std::span<const ExprPtr>(&expression, 1), expr.paren());
    }
    if (!(argument.forwardedPackElement
              ? forwardedPackArgumentMatches(parameters[index], argument.type)
              : callArgumentMatches(parameters[index], argument.type,
                                    expression))) {
      reportCallArgumentMismatch(index, parameters[index], argument.type,
                                 expression, "GTI invocation pointer");
      valid = false;
    }
  }
  validateCallPlaceExclusivityImpl(ordinaryParameters, ordinaryArguments,
                                   expr.paren());
  if (valid)
    semanticModel.record(expr, ResolvedCallInfo{.returnType = result,
                                                .parameterTypes = parameters,
                                                .typeArguments = {signature},
                                                .intrinsic = intrinsic});
  else
    currentType = SemanticType::Unknown;
}

} // namespace lang
